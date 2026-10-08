// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The decoder base against the ABI contract (wavecrux_decoder.h), using a
// small decoder whose output count per sample is the sample's own data
// value, so every NEED_MORE_SLOTS path can be driven precisely.

#include <stdlib.h>
#include <string.h>

#include "wcx/arena.h"
#include "wcx/decoder.h"
#include "wcx/json_writer.h"
#include "wcx/str.h"
#include "wcx/test.h"

enum { SIG_CLK, SIG_N, SIG_COUNT };

static const wcx_signal_spec k_signals[SIG_COUNT] = {{"clk", 1, false}, {"n", 4, false}};

static const char k_manifest[] =
    "{\"signals\":[{\"name\":\"clk\"},{\"name\":\"n\",\"bit_width\":4}],"
    "\"parameters\":[{\"name\":\"mode\",\"kind\":\"int\",\"default\":0}]}";

enum {
    MODE_NORMAL,
    MODE_BAD_UTF8,
    MODE_PARAM_IN_SAMPLE,
    MODE_EXHAUST_ARENA,
    MODE_INIT_FALSE,
    MODE_FAIL_IN_SAMPLE,
    MODE_REPORT_DELIVERED // labels say what wcx_decoder_batch_delivered reported
};

typedef struct test_state {
    wcx_decoder *self;
    int64_t mode;
    unsigned samples;
    unsigned flushes;
    unsigned finis;
} test_state;

static bool td_init(wcx_decoder *d, void *state) {
    test_state *st = state;
    st->self = d;
    if (!wcx_param_int(d, "mode", 0, 0, 10, &st->mode)) {
        return false;
    }
    return st->mode != MODE_INIT_FALSE;
}

static void td_sample(wcx_decoder *d, void *state, const wcx_sample *s) {
    test_state *st = state;
    st->samples++;
    uint64_t n = 0;
    bool x = false;
    WCX_IGNORE(wcx_sample_read(s->layout, s->bits, SIG_N, &n, &x));
    wcx_arena *a = wcx_decoder_arena(d);
    if (st->mode == MODE_PARAM_IN_SAMPLE) {
        int64_t v = 0;
        WCX_IGNORE(wcx_param_int(d, "mode", 0, 0, 10, &v));
        return;
    }
    if (st->mode == MODE_EXHAUST_ARENA) {
        (void)wcx_emit(d, 0, 0, wcx_arena_alloc(a, 1u << 20), "{}", false);
        return;
    }
    if (st->mode == MODE_FAIL_IN_SAMPLE) {
        wcx_decoder_fail(d, "invariant %d broken", 7);
        (void)wcx_emit(d, 0, 0, "dropped", "{}", false);
        return;
    }
    for (uint64_t i = 0; i < n; i++) {
        const char *label = st->mode == MODE_BAD_UTF8 ? "bad\xFFlabel"
                                                      : wcx_arena_printf(a, "s%u.%llu", st->samples,
                                                                         (unsigned long long)i);
        if (st->mode == MODE_REPORT_DELIVERED) {
            label = wcx_decoder_batch_delivered(d) ? "delivered" : "pending";
        }
        wcx_json j;
        wcx_json_begin(&j, a);
        wcx_json_add_u64(&j, "i", i);
        if (!wcx_emit(d, s->timestamp_fs, s->timestamp_fs, label, wcx_json_end(&j), false)) {
            return;
        }
    }
}

static void td_flush(wcx_decoder *d, void *state) {
    test_state *st = state;
    st->flushes++;
    (void)wcx_emit(d, 0, wcx_decoder_now(d), "end", "{}", false);
    (void)wcx_emit(d, 0, wcx_decoder_now(d), "end2", "{}", false);
}

static void td_fini(wcx_decoder *d, void *state) {
    (void)d;
    test_state *st = state;
    st->finis++;
}

static WcDecoderHandle td_create(const char *config_json);

static const wcx_decoder_class k_class = {
    .id = "t.dec",
    .display_name = "Test decoder",
    .manifest_json = k_manifest,
    .signals = k_signals,
    .signal_count = SIG_COUNT,
    .state_size = sizeof(test_state),
    .max_transactions_per_call = 10,
    .arena_block_bytes = 256,
    .arena_max_bytes = 4096,
    .init = td_init,
    .on_sample = td_sample,
    .on_flush = td_flush,
    .fini = td_fini,
    .create = td_create,
};

static WcDecoderHandle td_create(const char *config_json) {
    return wcx_decoder_create(&k_class, config_json);
}

static const char k_cfg[] = "{\"decoder_id\":\"t.dec\",\"signal_bindings\":{\"clk\":\"0\",\"n\":"
                            "\"1\"},\"parameters\":{\"mode\":0},\"options\":{\"mode\":0}}";

// Packs clk=0 and n into a 32-byte buffer (clk at signal bit 0, n at 1..4).
static WcSample make_sample(uint8_t *buf, uint64_t ts, unsigned n) {
    memset(buf, 0, 32);
    for (unsigned i = 0; i < 4; i++) {
        if ((n >> i) & 1u) {
            const unsigned pos = (1u + i) * 2u;
            buf[pos / 8u] = (uint8_t)(buf[pos / 8u] | (1u << (pos % 8u)));
        }
    }
    const WcSample s = {ts, buf, 5, 0};
    return s;
}

WCX_TEST(feed_fits_and_strings_are_correct) {
    WcDecoderHandle h = td_create(k_cfg);
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    WcSample s = make_sample(buf, 100, 3);
    WcTransaction out[16];
    size_t n = 16;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, out, &n), WC_DECODER_OK);
    WCX_REQUIRE(n == 3);
    WCX_CHECK_STR_EQ(out[0].label, "s1.0");
    WCX_CHECK_STR_EQ(out[2].label, "s1.2");
    WCX_CHECK_STR_EQ(out[2].fields_json, "{\"i\":2}");
    WCX_CHECK_EQ_U64(out[1].start_fs, 100);
    wcx_decoder_destroy(h);
}

WCX_TEST(need_more_slots_retry_does_not_decode_twice) {
    WcDecoderHandle h = td_create(k_cfg);
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    WcSample s = make_sample(buf, 100, 5);
    WcTransaction small[2];
    size_t n = 2;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, small, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_U64(n, 5);
    // The loader reallocates and retries with the same sample.
    WcTransaction *big = calloc(n, sizeof *big);
    WCX_REQUIRE(big != NULL);
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, big, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 5);
    // "s1.*": on_sample ran once (a second run would label them "s2.*").
    WCX_CHECK_STR_EQ(big[0].label, "s1.0");
    WCX_CHECK_STR_EQ(big[4].label, "s1.4");
    // The next sample is decoded normally and numbered 2.
    WcSample s2 = make_sample(buf, 200, 1);
    n = 2;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s2, small, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 1);
    WCX_CHECK_STR_EQ(small[0].label, "s2.0");
    free(big);
    wcx_decoder_destroy(h);
}

WCX_TEST(slots_one_on_every_call_matches_a_big_buffer) {
    // A retry may itself be offered too few slots (a host that grows by
    // doubling); the batch stays pending until it fits.
    WcDecoderHandle h = td_create(k_cfg);
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    WcSample s = make_sample(buf, 5, 4);
    WcTransaction out[8];
    size_t n = 1;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, out, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_U64(n, 4);
    n = 2;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, out, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_U64(n, 4);
    n = 8;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 4);
    WCX_CHECK_STR_EQ(out[3].label, "s1.3");
    wcx_decoder_destroy(h);
}

WCX_TEST(host_that_moves_on_loses_nothing) {
    WcDecoderHandle h = td_create(k_cfg);
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    WcSample s = make_sample(buf, 10, 3);
    WcTransaction out[16];
    size_t n = 1;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, out, &n), WC_DECODER_NEED_MORE_SLOTS);
    uint8_t buf2[32];
    WcSample s2 = make_sample(buf2, 20, 2);
    n = 16;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s2, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 5);
    WCX_CHECK_STR_EQ(out[0].label, "s1.0");
    WCX_CHECK_STR_EQ(out[2].label, "s1.2");
    WCX_CHECK_STR_EQ(out[3].label, "s2.0");
    wcx_decoder_destroy(h);
}

WCX_TEST(batch_delivered_tracks_the_host) {
    // The first call has nothing pending: delivered. After a NEED_MORE_SLOTS
    // that the host answers by moving on (emit.h), the next call sees the
    // batch pending and its output is appended; once that bigger batch has
    // been copied, the call after it sees delivered again.
    WcDecoderHandle h =
        td_create("{\"signal_bindings\":{\"clk\":\"0\",\"n\":\"1\"},\"parameters\":{\"mode\":6}}");
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    WcTransaction out[8];
    WcSample s = make_sample(buf, 100, 3);
    size_t n = 1;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, out, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_U64(n, 3);
    uint8_t buf2[32];
    WcSample s2 = make_sample(buf2, 200, 1); // a different sample: no retry
    n = 8;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s2, out, &n), WC_DECODER_OK);
    WCX_REQUIRE(n == 4);
    WCX_CHECK_STR_EQ(out[0].label, "delivered"); // from the first call
    WCX_CHECK_STR_EQ(out[2].label, "delivered");
    WCX_CHECK_STR_EQ(out[3].label, "pending"); // appended while the batch waited
    WcSample s3 = make_sample(buf, 300, 1);
    n = 8;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s3, out, &n), WC_DECODER_OK);
    WCX_REQUIRE(n == 1);
    WCX_CHECK_STR_EQ(out[0].label, "delivered");
    // A retry is served from the queue without running on_sample, so the
    // flag is not consulted; the call after a delivered retry sees delivered.
    WcSample s4 = make_sample(buf2, 400, 2);
    n = 1;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s4, out, &n), WC_DECODER_NEED_MORE_SLOTS);
    n = 8;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s4, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 2);
    WcSample s5 = make_sample(buf, 500, 1);
    n = 8;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s5, out, &n), WC_DECODER_OK);
    WCX_REQUIRE(n == 1);
    WCX_CHECK_STR_EQ(out[0].label, "delivered");
    WCX_CHECK(!wcx_decoder_batch_delivered(NULL));
    wcx_decoder_destroy(h);
}

WCX_TEST(flush_and_flush_retry) {
    WcDecoderHandle h = td_create(k_cfg);
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    WcSample s = make_sample(buf, 70, 0);
    WcTransaction out[4];
    size_t n = 4;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 0);
    n = 1;
    WCX_CHECK_EQ_I64(wcx_decoder_flush(h, out, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_U64(n, 2);
    n = 4;
    WCX_CHECK_EQ_I64(wcx_decoder_flush(h, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 2); // on_flush ran once
    WCX_CHECK_STR_EQ(out[1].label, "end2");
    WCX_CHECK_EQ_U64(out[1].end_fs, 70);
    // A second flush emits nothing; a feed after flush is an error.
    n = 4;
    WCX_CHECK_EQ_I64(wcx_decoder_flush(h, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 0);
    n = 4;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, out, &n), WC_DECODER_OK);
    WCX_REQUIRE(n == 1);
    WCX_CHECK(out[0].is_error != 0);
    WCX_CHECK_STR_EQ(out[0].label, "the host fed a sample after flush");
    wcx_decoder_destroy(h);
}

// Feeds `s`, expecting exactly one error transaction with `label`, then
// silence on the next feed and on flush.
static void expect_one_error(wcx_test *t, WcDecoderHandle h, const WcSample *s, const char *label) {
    WcTransaction out[16];
    size_t n = 16;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, s, out, &n), WC_DECODER_OK);
    WCX_REQUIRE(n == 1);
    WCX_CHECK(out[0].is_error != 0);
    WCX_CHECK_STR_EQ(out[0].label, label);
    WCX_CHECK(strstr(out[0].fields_json, "\"error\":") != NULL);
    uint8_t buf[32];
    WcSample next = make_sample(buf, s->timestamp_fs + 1000u, 3);
    n = 16;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &next, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 0);
    n = 16;
    WCX_CHECK_EQ_I64(wcx_decoder_flush(h, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 0);
}

WCX_TEST(wrong_bit_width_is_one_error_then_silence) {
    WcDecoderHandle h = td_create(k_cfg);
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    WcSample s = make_sample(buf, 1, 2);
    s.bit_width = 6;
    expect_one_error(t, h, &s,
                     "sample is 6 bits wide but the bound signals need 5 bits; "
                     "decoding stopped");
    wcx_decoder_destroy(h);

    h = td_create(k_cfg);
    WCX_REQUIRE(h != NULL);
    s.bit_width = UINT32_MAX;
    WcTransaction out[2];
    size_t n = 2;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 1);
    wcx_decoder_destroy(h);
}

WCX_TEST(timestamps_backwards_is_an_error) {
    WcDecoderHandle h = td_create(k_cfg);
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    WcSample s = make_sample(buf, 500, 0);
    WcTransaction out[4];
    size_t n = 4;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, out, &n), WC_DECODER_OK);
    WcSample back = make_sample(buf, 400, 0);
    expect_one_error(t, h, &back, "sample timestamps went backwards (400 fs after 500 fs)");
    wcx_decoder_destroy(h);
}

WCX_TEST(configuration_errors_become_one_error_transaction) {
    static const struct {
        const char *config;
        const char *label;
    } cases[] = {
        {"{", "configuration is not valid JSON (invalid JSON at byte 1: unexpected end of "
              "document)"},
        {"{\"signal_bindings\":{\"n\":\"1\"}}", "required signal \"clk\" is not bound"},
        {"{\"decoder_id\":\"other\"}", "configuration names a different decoder than \"t.dec\""},
        {"{\"signal_bindings\":{\"clk\":\"0\",\"n\":\"1\"},\"parameters\":{\"mode\":99}}",
         "parameter \"mode\" must be an integer from 0 to 10, got 99"},
        {"{\"signal_bindings\":{\"clk\":\"0\",\"n\":\"1\"},\"parameters\":{\"mode\":4}}",
         "the decoder could not start with this configuration"},
        {"\xFF", "configuration is not valid JSON (invalid JSON at byte 0: expected a value)"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        WcDecoderHandle h = td_create(cases[i].config);
        WCX_REQUIRE(h != NULL);
        uint8_t buf[32];
        WcSample s = make_sample(buf, 42, 1);
        expect_one_error(t, h, &s, cases[i].label);
        wcx_decoder_destroy(h);
    }
    // NULL config, and an error reported by flush when nothing was fed.
    WcDecoderHandle h = td_create(NULL);
    WCX_REQUIRE(h != NULL);
    WcTransaction out[2];
    size_t n = 2;
    WCX_CHECK_EQ_I64(wcx_decoder_flush(h, out, &n), WC_DECODER_OK);
    WCX_REQUIRE(n == 1);
    WCX_CHECK_STR_EQ(out[0].label, "no configuration was passed to create");
    wcx_decoder_destroy(h);
}

WCX_TEST(manifest_and_table_must_agree) {
    static const wcx_signal_spec wrong[] = {{"clk", 1, false}, {"n", 3, false}};
    wcx_decoder_class cls = k_class;
    cls.signals = wrong;
    WcDecoderHandle h = wcx_decoder_create(&cls, k_cfg);
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    WcSample s = make_sample(buf, 1, 1);
    s.bit_width = 4;
    expect_one_error(t, h, &s,
                     "internal decoder error: decoder signal table entry 1 does not "
                     "match manifest signal \"n\" (4 bits)");
    wcx_decoder_destroy(h);
}

WCX_TEST(decoder_misuse_is_contained) {
    static const struct {
        int mode;
        const char *label;
    } cases[] = {
        {MODE_PARAM_IN_SAMPLE, "internal decoder error: parameters can only be read in init"},
        {MODE_EXHAUST_ARENA, "the decoder ran out of string space (limit 4096 bytes per sample)"},
        {MODE_FAIL_IN_SAMPLE, "invariant 7 broken"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char cfg[160] = {0};
        WCX_IGNORE(wcx_str_format(cfg, sizeof cfg,
                                  "{\"signal_bindings\":{\"clk\":\"0\",\"n\":\"1\"},"
                                  "\"parameters\":{\"mode\":%d}}",
                                  cases[i].mode));
        WcDecoderHandle h = td_create(cfg);
        WCX_REQUIRE(h != NULL);
        uint8_t buf[32];
        WcSample s = make_sample(buf, 9, 1);
        expect_one_error(t, h, &s, cases[i].label);
        wcx_decoder_destroy(h);
    }
}

WCX_TEST(too_many_transactions_uses_the_reserved_slot) {
    // n = 15 > the class ceiling of 10.
    WcDecoderHandle h = td_create(k_cfg);
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    WcSample s = make_sample(buf, 3, 15);
    WcTransaction out[16];
    size_t n = 16;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, out, &n), WC_DECODER_OK);
    WCX_REQUIRE(n == 11);
    WCX_CHECK_STR_EQ(out[9].label, "s1.9");
    WCX_CHECK(out[10].is_error != 0);
    WCX_CHECK_STR_EQ(out[10].label, "more than 10 transactions from one sample; decoding stopped");
    wcx_decoder_destroy(h);
}

WCX_TEST(ill_formed_utf8_is_replaced) {
    WcDecoderHandle h =
        td_create("{\"signal_bindings\":{\"clk\":\"0\",\"n\":\"1\"},\"parameters\":{\"mode\":1}}");
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    WcSample s = make_sample(buf, 3, 1);
    WcTransaction out[4];
    size_t n = 4;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, &s, out, &n), WC_DECODER_OK);
    WCX_REQUIRE(n == 1);
    WCX_CHECK_STR_EQ(out[0].label, "bad\xEF\xBF\xBDlabel");
    wcx_decoder_destroy(h);
}

WCX_TEST(abi_edges) {
    WcTransaction out[1];
    size_t n = 1;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(NULL, NULL, out, &n), WC_DECODER_ERR);
    WCX_CHECK_EQ_I64(wcx_decoder_flush(NULL, out, &n), WC_DECODER_ERR);
    wcx_decoder_destroy(NULL);
    WcDecoderHandle h = td_create(k_cfg);
    WCX_REQUIRE(h != NULL);
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, NULL, out, NULL), WC_DECODER_ERR);
    WCX_CHECK_EQ_I64(wcx_decoder_flush(h, out, NULL), WC_DECODER_ERR);
    // A NULL sample is the host's error, reported once.
    n = 1;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(h, NULL, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 1);
    WCX_CHECK_STR_EQ(out[0].label, "the host passed no sample");
    wcx_decoder_destroy(h);
    WCX_CHECK(wcx_decoder_create(NULL, k_cfg) == NULL);
    // Accessors tolerate NULL.
    WCX_CHECK(wcx_decoder_layout(NULL) == NULL);
    WCX_CHECK(wcx_decoder_arena(NULL) == NULL);
    WCX_CHECK(!wcx_decoder_is_bound(NULL, 0));
    WCX_CHECK_EQ_U64(wcx_decoder_now(NULL), 0);
    WCX_CHECK(wcx_decoder_class_of(NULL) == NULL);
    WCX_CHECK(wcx_decoder_failed(NULL));
    WCX_CHECK(!wcx_emit(NULL, 0, 0, "x", "{}", false));
    wcx_decoder_fail(NULL, "ignored");
}

WCX_TEST(register_two_call_pattern) {
    const wcx_decoder_class *const classes[] = {&k_class, &k_class};
    size_t n = 0;
    WCX_CHECK_EQ_I64(wcx_register(classes, 2, NULL, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_U64(n, 2);
    WcDecoderDef defs[2];
    memset(defs, 0x5A, sizeof defs);
    n = 1;
    WCX_CHECK_EQ_I64(wcx_register(classes, 2, defs, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_U64(n, 2);
    WCX_CHECK_EQ_U64(defs[0]._reserved1, 0x5A5A5A5A5A5A5A5Au); // untouched
    n = 2;
    WCX_CHECK_EQ_I64(wcx_register(classes, 2, defs, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 2);
    WCX_CHECK_STR_EQ(defs[1].id, "t.dec");
    WCX_CHECK(defs[1].create == td_create);
    WCX_CHECK(defs[1].feed == wcx_decoder_feed);
    WCX_CHECK(defs[1]._reserved0 == NULL && defs[1]._reserved1 == 0);
    WCX_CHECK_EQ_I64(wcx_register(classes, 2, defs, NULL), WC_DECODER_ERR);
    wcx_decoder_class broken = k_class;
    broken.create = NULL;
    const wcx_decoder_class *const bad[] = {&broken};
    n = 1;
    WCX_CHECK_EQ_I64(wcx_register(bad, 1, defs, &n), WC_DECODER_ERR);
}

WCX_TEST(two_instances_are_independent) {
    WcDecoderHandle a = td_create(k_cfg);
    WcDecoderHandle b = td_create(k_cfg);
    WCX_REQUIRE(a != NULL && b != NULL);
    uint8_t buf[32];
    WcTransaction out[4];
    for (unsigned i = 0; i < 3; i++) {
        WcSample s = make_sample(buf, (uint64_t)10u * (i + 1u), 1);
        size_t n = 4;
        WCX_CHECK_EQ_I64(wcx_decoder_feed(a, &s, out, &n), WC_DECODER_OK);
        if (i == 2) {
            WCX_CHECK_STR_EQ(out[0].label, "s3.0");
        }
    }
    WcSample s = make_sample(buf, 10, 1);
    size_t n = 4;
    WCX_CHECK_EQ_I64(wcx_decoder_feed(b, &s, out, &n), WC_DECODER_OK);
    WCX_CHECK_STR_EQ(out[0].label, "s1.0");
    wcx_decoder_destroy(a);
    wcx_decoder_destroy(b);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, feed_fits_and_strings_are_correct);
    WCX_RUN(&t, need_more_slots_retry_does_not_decode_twice);
    WCX_RUN(&t, slots_one_on_every_call_matches_a_big_buffer);
    WCX_RUN(&t, host_that_moves_on_loses_nothing);
    WCX_RUN(&t, batch_delivered_tracks_the_host);
    WCX_RUN(&t, flush_and_flush_retry);
    WCX_RUN(&t, wrong_bit_width_is_one_error_then_silence);
    WCX_RUN(&t, timestamps_backwards_is_an_error);
    WCX_RUN(&t, configuration_errors_become_one_error_transaction);
    WCX_RUN(&t, manifest_and_table_must_agree);
    WCX_RUN(&t, decoder_misuse_is_contained);
    WCX_RUN(&t, too_many_transactions_uses_the_reserved_slot);
    WCX_RUN(&t, ill_formed_utf8_is_replaced);
    WCX_RUN(&t, abi_edges);
    WCX_RUN(&t, register_two_call_pattern);
    WCX_RUN(&t, two_instances_are_independent);
    return wcx_test_finish(&t);
}
