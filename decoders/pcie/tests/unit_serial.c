// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The serial line decoders through the plugin ABI (SPEC.md §11): manifests,
// parameters, the line-level transactions of §11.8 and how they combine
// with the PIPE and DLL output. Streams are built with pcie_8b10b_encode
// (checked against the protocol notes in unit_8b10b.c) and CRCs with
// pcie_crc (checked in unit_crc.c).

#include <string.h>

#include "pcie_8b10b.h"
#include "pcie_crc.h"
#include "pcie_line.h"
#include "pcie_test_util.h"
#include "wcx/manifest.h"

// ── a serial driver ────────────────────────────────────────────────────────

#define MAX_BITS 8192u

typedef struct sdrv {
    WcDecoderDef def;
    WcDecoderHandle h;
    txlog log;
    uint8_t buf[64];
    bool bit[MAX_BITS];
    size_t n;
    bool rd_pos;
    uint64_t t;      // time of the next bit
    uint64_t ui;
    bool have_last;  // a sample has been fed
    bool last;       // its level
} sdrv;

static bool sopen(sdrv *d, wcx_test *t, const char *id, const char *params_json, uint64_t ui) {
    memset(d, 0, sizeof *d);
    d->def = abi_find(t, id);
    d->ui = ui;
    d->t = 1000000;
    char cfg_json[512] = {0};
    WCX_IGNORE(wcx_str_format(cfg_json, sizeof cfg_json,
                              "{\"decoder_id\":\"%s\",\"signal_bindings\":{\"line\":\"0\"},"
                              "\"parameters\":%s}",
                              id, params_json != NULL ? params_json : "{}"));
    if (d->def.create == NULL) {
        return false;
    }
    d->h = d->def.create(cfg_json);
    return d->h != NULL;
}

static void sfeed(sdrv *d, uint64_t ts, bool unknown, bool level) {
    memset(d->buf, 0, sizeof d->buf);
    d->buf[0] = (uint8_t)((level ? 1u : 0u) | (unknown ? 2u : 0u));
    WcSample s;
    memset(&s, 0, sizeof s);
    s.timestamp_fs = ts;
    s.bits_ptr = d->buf;
    s.bit_width = 1;
    (void)abi_call(&d->def, d->h, &s, &d->log);
}

static void sbits(sdrv *d, uint16_t code, unsigned width) {
    for (unsigned i = 0; i < width && d->n < MAX_BITS; i++) {
        d->bit[d->n++] = (((unsigned)code >> (width - 1u - i)) & 1u) != 0u;
    }
}

static void ssym(sdrv *d, uint8_t byte, bool k) {
    uint16_t code = 0;
    bool after = false;
    if (pcie_8b10b_encode(byte, k, d->rd_pos, &code, &after)) {
        sbits(d, code, 10u);
        d->rd_pos = after;
    }
}

// Puts the queued bits on the line (a sample at every change) and a closing
// transition one UI after the last bit.
static void send(sdrv *d) {
    for (size_t i = 0; i < d->n; i++) {
        if (!d->have_last || d->bit[i] != d->last) {
            sfeed(d, d->t, false, d->bit[i]);
            d->have_last = true;
            d->last = d->bit[i];
        }
        d->t += d->ui;
    }
    d->n = 0;
    sfeed(d, d->t, false, !d->last);
    d->last = !d->last;
    d->t += d->ui;
}

static void sclose(sdrv *d) {
    if (d->h != NULL) {
        d->def.destroy(d->h);
        d->h = NULL;
    }
    txlog_free(&d->log);
}

static void ts1(sdrv *d) {
    static const uint8_t k_ts1[16] = {0xBC, 0xF7, 0xF7, 24,   0x02, 0x00, 0x4A, 0x4A,
                                      0x4A, 0x4A, 0x4A, 0x4A, 0x4A, 0x4A, 0x4A, 0x4A};
    for (unsigned i = 0; i < 16u; i++) {
        ssym(d, k_ts1[i], i < 3u);
    }
}

static void ack(sdrv *d, unsigned seq) {
    const uint8_t body[4] = {0x00, 0x00, (uint8_t)((seq >> 8u) & 0x0Fu), (uint8_t)(seq & 0xFFu)};
    uint8_t crc[2] = {0, 0};
    pcie_crc16_wire(pcie_crc16(body, 4), crc);
    ssym(d, 0x5C, true); // SDP
    for (unsigned i = 0; i < 4u; i++) {
        ssym(d, body[i], false);
    }
    ssym(d, crc[0], false);
    ssym(d, crc[1], false);
    ssym(d, 0xFD, true); // END
}

static bool has_label(const txlog *l, const char *label) {
    for (size_t i = 0; i < l->n; i++) {
        if (strcmp(l->items[i].label, label) == 0) {
            return true;
        }
    }
    return false;
}

// ── manifests (§11.1) ──────────────────────────────────────────────────────

WCX_TEST(serial_manifests_declare_line_and_parameters) {
    const char *ids[2] = {"ferrite.pcie_serial", "ferrite.pcie_serial_dll"};
    for (size_t i = 0; i < 2u; i++) {
        const WcDecoderDef def = abi_find(t, ids[i]);
        WCX_REQUIRE(def.manifest_json != NULL);
        wcx_manifest m;
        wcx_error err = {{0}};
        WCX_REQUIRE(wcx_manifest_parse(&m, def.manifest_json, &err));
        WCX_REQUIRE(m.signal_count == 1u);
        char name[16] = {0};
        WCX_IGNORE(wcx_manifest_text(&m, m.signals[0].name, name, sizeof name));
        WCX_CHECK_STR_EQ(name, "line");
        WCX_CHECK_EQ_U64(m.signals[0].bit_width, 1);
        WCX_CHECK(!m.signals[0].optional);
        WCX_CHECK_EQ_U64(m.param_count, i == 0u ? 5u : 3u);
        const size_t rate = wcx_manifest_find_param(&m, "rate");
        const size_t inv = wcx_manifest_find_param(&m, "invert");
        const size_t scr = wcx_manifest_find_param(&m, "scrambling");
        WCX_REQUIRE(rate != WCX_JSON_NONE && inv != WCX_JSON_NONE && scr != WCX_JSON_NONE);
        WCX_CHECK_EQ_U64(m.params[rate].kind, WCX_PARAM_ENUM);
        WCX_CHECK(m.params[rate].enum_labels != WCX_JSON_NONE);
        WCX_CHECK(wcx_jdoc_string_equals(&m.doc, m.params[rate].default_value, "auto"));
        WCX_CHECK_EQ_U64(m.params[inv].kind, WCX_PARAM_BOOL);
        WCX_CHECK_EQ_U64(wcx_jdoc_type(&m.doc, m.params[inv].default_value), WCX_JT_FALSE);
        WCX_CHECK(wcx_jdoc_string_equals(&m.doc, m.params[scr].default_value, "auto"));
        WCX_CHECK_EQ_U64(wcx_manifest_find_param(&m, "coalesce") != WCX_JSON_NONE, i == 0u);
        WCX_CHECK_EQ_U64(wcx_manifest_find_param(&m, "sample_point"), WCX_JSON_NONE);
        wcx_manifest_free(&m);
    }
}

// ── the two views of one stream ────────────────────────────────────────────

static void training_then_ack(sdrv *d) {
    ts1(d);
    ts1(d);
    ssym(d, 0xBC, true); // SKP OS
    ssym(d, 0x1C, true);
    ssym(d, 0x1C, true);
    ssym(d, 0x1C, true);
    ack(d, 5);
    ssym(d, 0x00, false);
    send(d);
}

WCX_TEST(symbols_view_shows_lock_ordered_sets_and_frames) {
    sdrv d;
    WCX_REQUIRE(sopen(&d, t, "ferrite.pcie_serial", "{\"scrambling\":\"off\"}", PCIE_UI_GEN1_FS));
    training_then_ack(&d);
    (void)abi_call(&d.def, d.h, NULL, &d.log);
    WCX_REQUIRE(d.log.n >= 4u);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Symbol lock (Gen1, 2.5 GT/s)");
    WCX_CHECK_STR_EQ(d.log.items[0].fields, "{\"type\":\"symbol_lock\",\"rate\":\"gen1\"}");
    WCX_CHECK_EQ_U64(d.log.items[0].start, 1000000);
    WCX_CHECK_EQ_U64(d.log.items[0].end, 1000000u + 10u * PCIE_UI_GEN1_FS);
    WCX_CHECK(has_label(&d.log, "TS1 \xC3\x97" "2 link=PAD lane=PAD n_fts=24 gen1"));
    WCX_CHECK(has_label(&d.log, "SKP"));
    WCX_CHECK(has_label(&d.log, "DLLP frame (6 symbols)"));
    sclose(&d);
}

WCX_TEST(dll_view_shows_the_dllp_only) {
    sdrv d;
    WCX_REQUIRE(
        sopen(&d, t, "ferrite.pcie_serial_dll", "{\"scrambling\":\"off\"}", PCIE_UI_GEN1_FS));
    training_then_ack(&d);
    (void)abi_call(&d.def, d.h, NULL, &d.log);
    WCX_REQUIRE(d.log.n == 1u);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Ack seq=5");
    WCX_CHECK(!d.log.items[0].is_error);
    sclose(&d);
}

WCX_TEST(gen2_with_auto_rate_and_auto_scrambling) {
    sdrv d;
    WCX_REQUIRE(sopen(&d, t, "ferrite.pcie_serial", NULL, PCIE_UI_GEN2_FS));
    training_then_ack(&d);
    (void)abi_call(&d.def, d.h, NULL, &d.log);
    WCX_REQUIRE(d.log.n >= 1u);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Symbol lock (Gen2, 5.0 GT/s)");
    WCX_CHECK(has_label(&d.log, "Scrambling: off (detected)"));
    sclose(&d);
}

// ── line errors (§11.8) ────────────────────────────────────────────────────

WCX_TEST(code_and_disparity_errors_in_the_dll_view) {
    sdrv d;
    WCX_REQUIRE(
        sopen(&d, t, "ferrite.pcie_serial_dll", "{\"scrambling\":\"off\"}", PCIE_UI_GEN1_FS));
    ts1(&d);
    sbits(&d, 0x3FFu, 10u); // 1111111111: not a code group; EDB is fed
    const bool rd = d.rd_pos;
    d.rd_pos = !rd; // D0.0 at the wrong disparity
    ssym(&d, 0x00, false);
    d.rd_pos = rd;
    ssym(&d, 0x00, false);
    send(&d);
    (void)abi_call(&d.def, d.h, NULL, &d.log);
    const size_t ce = txlog_find(&d.log, "8b/10b code error");
    WCX_REQUIRE(ce != SIZE_MAX);
    WCX_CHECK(d.log.items[ce].is_error);
    WCX_CHECK_STR_EQ(d.log.items[ce].fields,
                     "{\"type\":\"code_error\",\"code\":\"1111111111\","
                     "\"error\":\"Not a valid 8b/10b code group.\"}");
    // The substituted EDB has no packet open.
    WCX_CHECK(has_label(&d.log, "EDB without start"));
    const size_t de = txlog_find(&d.log, "Disparity error D0.0");
    WCX_REQUIRE(de != SIZE_MAX);
    WCX_CHECK(strstr(d.log.items[de].fields, "\"symbol\":\"0x00\"") != NULL);
    WCX_CHECK(strstr(d.log.items[de].fields, "\"type\":\"disparity_error\"") != NULL);
    sclose(&d);
}

WCX_TEST(realign_label_counts_bits) {
    sdrv d;
    WCX_REQUIRE(sopen(&d, t, "ferrite.pcie_serial_dll", "{\"scrambling\":\"off\",\"rate\":\"gen1\"}",
                      PCIE_UI_GEN1_FS));
    ts1(&d);
    sbits(&d, 0x0u, 3u); // three slipped bits
    ts1(&d);
    send(&d);
    (void)abi_call(&d.def, d.h, NULL, &d.log);
    const size_t ra = txlog_find(&d.log, "Symbol lock moved by 3 bits");
    WCX_REQUIRE(ra != SIZE_MAX);
    WCX_CHECK(strstr(d.log.items[ra].fields, "\"shift\":3") != NULL);
    sclose(&d);
}

WCX_TEST(realign_by_one_bit_is_singular) {
    sdrv d;
    WCX_REQUIRE(sopen(&d, t, "ferrite.pcie_serial", "{\"scrambling\":\"off\",\"rate\":\"gen1\"}",
                      PCIE_UI_GEN1_FS));
    ts1(&d);
    sbits(&d, 0x1u, 1u);
    ts1(&d);
    send(&d);
    (void)abi_call(&d.def, d.h, NULL, &d.log);
    WCX_CHECK(txlog_find(&d.log, "Symbol lock moved by 1 bit") != SIZE_MAX);
    sclose(&d);
}

// ── electrical idle (§11.3, §11.8) ─────────────────────────────────────────

WCX_TEST(electrical_idle_truncates_and_is_shown_until_the_end) {
    sdrv d;
    WCX_REQUIRE(sopen(&d, t, "ferrite.pcie_serial", "{\"scrambling\":\"off\"}", PCIE_UI_GEN1_FS));
    ts1(&d);
    ssym(&d, 0xFB, true); // STP and two body bytes, then the line goes to X
    ssym(&d, 0x00, false);
    ssym(&d, 0x01, false);
    send(&d);
    const uint64_t idle_at = d.t;
    sfeed(&d, idle_at, true, false);
    sfeed(&d, idle_at + 5000000u, true, true); // still X
    (void)abi_call(&d.def, d.h, NULL, &d.log);
    WCX_CHECK(has_label(&d.log, "Packet truncated by electrical idle"));
    const size_t ei = txlog_find(&d.log, "Electrical idle");
    WCX_REQUIRE(ei != SIZE_MAX);
    WCX_CHECK_EQ_U64(d.log.items[ei].start, idle_at);
    WCX_CHECK_EQ_U64(d.log.items[ei].end, idle_at + 5000000u); // the last sample
    WCX_CHECK_STR_EQ(d.log.items[ei].fields, "{\"type\":\"electrical_idle\"}");
    WCX_CHECK(!d.log.items[ei].is_error);
    sclose(&d);
}

WCX_TEST(dll_view_does_not_show_idle_or_lock) {
    sdrv d;
    WCX_REQUIRE(
        sopen(&d, t, "ferrite.pcie_serial_dll", "{\"scrambling\":\"off\"}", PCIE_UI_GEN1_FS));
    ts1(&d);
    send(&d);
    sfeed(&d, d.t, true, false);
    sfeed(&d, d.t + 5000000u, true, true);
    (void)abi_call(&d.def, d.h, NULL, &d.log);
    WCX_CHECK_EQ_U64(d.log.n, 0);
    sclose(&d);
}

// ── parameters ─────────────────────────────────────────────────────────────

WCX_TEST(bad_rate_is_an_error_transaction) {
    sdrv d;
    WCX_REQUIRE(sopen(&d, t, "ferrite.pcie_serial", "{\"rate\":\"gen3\"}", PCIE_UI_GEN1_FS));
    (void)abi_call(&d.def, d.h, NULL, &d.log);
    WCX_REQUIRE(d.log.n == 1u);
    WCX_CHECK(d.log.items[0].is_error);
    WCX_CHECK(strstr(d.log.items[0].label, "rate") != NULL);
    sclose(&d);
}

WCX_TEST(invert_and_coalesce_parameters_are_read) {
    sdrv d;
    WCX_REQUIRE(sopen(&d, t, "ferrite.pcie_serial",
                      "{\"scrambling\":\"off\",\"invert\":true,\"coalesce\":false}",
                      PCIE_UI_GEN1_FS));
    ts1(&d);
    ts1(&d);
    for (size_t i = 0; i < d.n; i++) {
        d.bit[i] = !d.bit[i];
    }
    send(&d);
    (void)abi_call(&d.def, d.h, NULL, &d.log);
    // Uncoalesced: two TS1 transactions, both decoded through the inversion.
    size_t ts = 0;
    for (size_t i = 0; i < d.log.n; i++) {
        ts += strncmp(d.log.items[i].label, "TS1 link=PAD", 12) == 0 ? 1u : 0u;
    }
    WCX_CHECK_EQ_U64(ts, 2);
    sclose(&d);
}

WCX_TEST(bad_invert_is_an_error_transaction) {
    sdrv d;
    WCX_REQUIRE(sopen(&d, t, "ferrite.pcie_serial_dll", "{\"invert\":3}", PCIE_UI_GEN1_FS));
    (void)abi_call(&d.def, d.h, NULL, &d.log);
    WCX_REQUIRE(d.log.n == 1u);
    WCX_CHECK(d.log.items[0].is_error);
    sclose(&d);
}

WCX_TEST(bad_show_idle_is_an_error_transaction) {
    sdrv d;
    WCX_REQUIRE(sopen(&d, t, "ferrite.pcie_serial", "{\"show_idle\":\"yes\"}", PCIE_UI_GEN1_FS));
    (void)abi_call(&d.def, d.h, NULL, &d.log);
    WCX_REQUIRE(d.log.n == 1u);
    WCX_CHECK(d.log.items[0].is_error);
    sclose(&d);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, serial_manifests_declare_line_and_parameters);
    WCX_RUN(&t, symbols_view_shows_lock_ordered_sets_and_frames);
    WCX_RUN(&t, dll_view_shows_the_dllp_only);
    WCX_RUN(&t, gen2_with_auto_rate_and_auto_scrambling);
    WCX_RUN(&t, code_and_disparity_errors_in_the_dll_view);
    WCX_RUN(&t, realign_label_counts_bits);
    WCX_RUN(&t, realign_by_one_bit_is_singular);
    WCX_RUN(&t, electrical_idle_truncates_and_is_shown_until_the_end);
    WCX_RUN(&t, dll_view_does_not_show_idle_or_lock);
    WCX_RUN(&t, bad_rate_is_an_error_transaction);
    WCX_RUN(&t, invert_and_coalesce_parameters_are_read);
    WCX_RUN(&t, bad_invert_is_an_error_transaction);
    WCX_RUN(&t, bad_show_idle_is_an_error_transaction);
    return wcx_test_finish(&t);
}
