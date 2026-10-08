// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Lifecycle tests of the test plugin through its ABI entry points, the way
// a host sees it. Expected values are derived by hand in the comments.

#include <stdlib.h>
#include <string.h>

#include "wavecrux_decoder.h"
#include "wcx/test.h"

// clk at signal bit 0, data (8 bits) at 1..8, valid at 9 when bound.
static WcSample pack(uint8_t *buf, uint64_t ts, int clk, unsigned data, unsigned data_x_mask,
                     int with_valid, int valid) {
    memset(buf, 0, 32);
    const unsigned width = with_valid ? 10u : 9u;
    for (unsigned bit = 0; bit < width; bit++) {
        int level = 0;
        int unknown = 0;
        if (bit == 0) {
            level = clk;
        } else if (bit <= 8) {
            level = (int)((data >> (bit - 1u)) & 1u);
            unknown = (int)((data_x_mask >> (bit - 1u)) & 1u);
        } else {
            level = valid;
        }
        const unsigned pos = bit * 2u;
        if (level) {
            buf[pos / 8u] = (uint8_t)(buf[pos / 8u] | (1u << (pos % 8u)));
        }
        if (unknown) {
            buf[pos / 8u] = (uint8_t)(buf[pos / 8u] | (2u << (pos % 8u)));
        }
    }
    const WcSample s = {ts, buf, width, 0};
    return s;
}

static WcDecoderDef find(wcx_test *t, const char *id) {
    WcDecoderDef defs[4];
    memset(defs, 0, sizeof defs);
    size_t n = 0;
    WCX_CHECK_EQ_I64(wavecrux_decoder_register(NULL, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_U64(n, 2);
    n = 4;
    WCX_CHECK_EQ_I64(wavecrux_decoder_register(defs, &n), WC_DECODER_OK);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(defs[i].id, id) == 0) {
            return defs[i];
        }
    }
    WCX_FAIL("decoder %s not registered", id);
    WcDecoderDef none;
    memset(&none, 0, sizeof none);
    return none;
}

WCX_TEST(entry_points) {
    WCX_CHECK_EQ_U64(WAVECRUX_DECODER_ABI_GET_MAJOR(wavecrux_decoder_abi_version()), 1);
    WCX_CHECK_STR_EQ(wavecrux_decoder_plugin_name(), "WaveCrux decoders test plugin");
    WCX_CHECK(wavecrux_decoder_plugin_description() != NULL);
}

WCX_TEST(echo_reports_the_pre_edge_value) {
    const WcDecoderDef def = find(t, "wcx.test.echo");
    WCX_REQUIRE(def.create != NULL);
    WcDecoderHandle h = def.create("{\"decoder_id\":\"wcx.test.echo\",\"signal_bindings\":"
                                   "{\"clk\":\"0\",\"data\":\"1\"},\"parameters\":{}}");
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    WcTransaction out[16];
    size_t n = 16;
    WcSample s = pack(buf, 1000, 0, 0x5A, 0, 0, 0);
    WCX_CHECK_EQ_I64(def.feed(h, &s, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 0);
    // Zero-delay: data changes to 0xC3 on the same timestamp as the edge;
    // the flop captured 0x5A.
    s = pack(buf, 2000, 1, 0xC3, 0, 0, 0);
    n = 16;
    WCX_CHECK_EQ_I64(def.feed(h, &s, out, &n), WC_DECODER_OK);
    WCX_REQUIRE(n == 1);
    WCX_CHECK_STR_EQ(out[0].label, "D=0x5A");
    WCX_CHECK_STR_EQ(out[0].fields_json, "{\"data\":\"0x5A\",\"edge\":1,\"repeat\":0}");
    WCX_CHECK_EQ_U64(out[0].start_fs, 2000);
    WCX_CHECK_EQ_U64(out[0].end_fs, 2001);
    n = 16;
    WCX_CHECK_EQ_I64(def.flush(h, out, &n), WC_DECODER_OK);
    WCX_REQUIRE(n == 1);
    WCX_CHECK_STR_EQ(out[0].label, "edges=1");
    WCX_CHECK_STR_EQ(out[0].fields_json, "{\"edges\":1,\"samples\":2}");
    def.destroy(h);
}

WCX_TEST(echo_repeat_drives_need_more_slots) {
    const WcDecoderDef def = find(t, "wcx.test.echo");
    WCX_REQUIRE(def.create != NULL);
    WcDecoderHandle h = def.create(
        "{\"signal_bindings\":{\"clk\":\"0\",\"data\":\"1\"},\"parameters\":{\"repeat\":20,"
        "\"prefix\":\"q\\\"\"}}");
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    size_t n = 16;
    WcTransaction *out = calloc(16, sizeof *out);
    WCX_REQUIRE(out != NULL);
    WcSample s = pack(buf, 0, 0, 0x01, 0, 0, 0);
    WCX_CHECK_EQ_I64(def.feed(h, &s, out, &n), WC_DECODER_OK);
    s = pack(buf, 10, 1, 0x02, 0, 0, 0);
    n = 16;
    WCX_CHECK_EQ_I64(def.feed(h, &s, out, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_U64(n, 20);
    free(out);
    out = calloc(n, sizeof *out);
    WCX_REQUIRE(out != NULL);
    WCX_CHECK_EQ_I64(def.feed(h, &s, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 20);
    WCX_CHECK_STR_EQ(out[0].label, "q\"=0x01 #0");
    WCX_CHECK_STR_EQ(out[19].label, "q\"=0x01 #19");
    WCX_CHECK_STR_EQ(out[19].fields_json, "{\"data\":\"0x01\",\"edge\":1,\"repeat\":19}");
    free(out);
    def.destroy(h);
}

WCX_TEST(echo_valid_and_unknown_data) {
    const WcDecoderDef def = find(t, "wcx.test.echo");
    WCX_REQUIRE(def.create != NULL);
    WcDecoderHandle h = def.create("{\"signal_bindings\":{\"clk\":\"0\",\"data\":\"1\","
                                   "\"valid\":\"2\"},\"parameters\":{\"emit_summary\":false}}");
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32];
    WcTransaction out[4];
    size_t n = 4;
    WcSample s = pack(buf, 0, 0, 0x11, 0, 1, 0); // valid low
    WCX_CHECK_EQ_I64(def.feed(h, &s, out, &n), WC_DECODER_OK);
    s = pack(buf, 5, 1, 0x11, 0, 1, 1);
    n = 4;
    WCX_CHECK_EQ_I64(def.feed(h, &s, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 0); // pre-edge valid was 0: skipped
    s = pack(buf, 10, 0, 0x22, 0x04, 1, 1);
    n = 4;
    WCX_CHECK_EQ_I64(def.feed(h, &s, out, &n), WC_DECODER_OK);
    s = pack(buf, 15, 1, 0x22, 0x04, 1, 1);
    n = 4;
    WCX_CHECK_EQ_I64(def.feed(h, &s, out, &n), WC_DECODER_OK);
    WCX_REQUIRE(n == 1);
    WCX_CHECK(out[0].is_error != 0);
    WCX_CHECK_STR_EQ(out[0].label, "data is X or Z at the rising edge");
    WCX_CHECK_STR_EQ(out[0].fields_json, "{\"edge\":2}");
    n = 4;
    WCX_CHECK_EQ_I64(def.flush(h, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 0); // emit_summary false
    def.destroy(h);
}

WCX_TEST(count_decoder) {
    const WcDecoderDef def = find(t, "wcx.test.count");
    WCX_REQUIRE(def.create != NULL);
    WcDecoderHandle h = def.create("{\"signal_bindings\":{\"clk\":\"0\"}}");
    WCX_REQUIRE(h != NULL);
    uint8_t buf[32] = {0};
    WcTransaction out[2];
    for (unsigned i = 0; i < 6; i++) {
        buf[0] = (uint8_t)(i % 2u); // 0,1,0,1,0,1: three rising edges
        const WcSample s = {(uint64_t)i * 100u, buf, 1, 0};
        size_t n = 2;
        WCX_CHECK_EQ_I64(def.feed(h, &s, out, &n), WC_DECODER_OK);
        WCX_CHECK_EQ_U64(n, 0);
    }
    size_t n = 2;
    WCX_CHECK_EQ_I64(def.flush(h, out, &n), WC_DECODER_OK);
    WCX_REQUIRE(n == 1);
    WCX_CHECK_STR_EQ(out[0].label, "samples=6 rising=3");
    WCX_CHECK_EQ_U64(out[0].end_fs, 500);
    def.destroy(h);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, entry_points);
    WCX_RUN(&t, echo_reports_the_pre_edge_value);
    WCX_RUN(&t, echo_repeat_drives_need_more_slots);
    WCX_RUN(&t, echo_valid_and_unknown_data);
    WCX_RUN(&t, count_decoder);
    return wcx_test_finish(&t);
}
