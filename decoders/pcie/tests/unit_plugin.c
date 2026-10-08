// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The plugin through its ABI entry points, as WaveCrux drives it: the
// eight registrations and manifests (SPEC.md §1-§3), symbol times (§4.2),
// byte lanes, gating (§4.3), X/Z runs (§4.4), RxStatus (§4.6) and the
// sample point. Expected values are worked out by hand from those sections
// in the comments.

#include <string.h>

#include "pcie_test_util.h"
#include "wcx/manifest.h"

// ── registration and manifests (§1-§3) ─────────────────────────────────────

typedef struct expected_class {
    const char *id;
    const char *name;
    unsigned lanes;
    bool dll;
} expected_class;

static const expected_class k_expected[8] = {
    {"ferrite.pcie_pipe_w8", "PCIe PIPE (8-bit)", 1, false},
    {"ferrite.pcie_pipe_w16", "PCIe PIPE (16-bit)", 2, false},
    {"ferrite.pcie_pipe_w32", "PCIe PIPE (32-bit)", 4, false},
    {"ferrite.pcie_pipe_w64", "PCIe PIPE (64-bit)", 8, false},
    {"ferrite.pcie_dll_w8", "PCIe Data Link Layer (8-bit)", 1, true},
    {"ferrite.pcie_dll_w16", "PCIe Data Link Layer (16-bit)", 2, true},
    {"ferrite.pcie_dll_w32", "PCIe Data Link Layer (32-bit)", 4, true},
    {"ferrite.pcie_dll_w64", "PCIe Data Link Layer (64-bit)", 8, true},
};

WCX_TEST(eight_decoders_register_in_spec_order) {
    WCX_CHECK_EQ_U64(WAVECRUX_DECODER_ABI_GET_MAJOR(wavecrux_decoder_abi_version()), 1);
    WCX_CHECK_STR_EQ(wavecrux_decoder_plugin_name(), "PCIe decoders (PIPE, Data Link Layer)");
    WCX_CHECK_STR_EQ(wavecrux_decoder_plugin_description(),
                     "Open source, Apache-2.0. Ferrite Engineering, "
                     "github.com/Ferrite-Engineering/wavecrux-decoders");
    size_t n = 0;
    WCX_CHECK_EQ_I64(wavecrux_decoder_register(NULL, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_U64(n, 8);
    WcDecoderDef defs[8];
    memset(defs, 0, sizeof defs);
    n = 8;
    WCX_REQUIRE(wavecrux_decoder_register(defs, &n) == WC_DECODER_OK);
    WCX_REQUIRE(n == 8);
    for (size_t i = 0; i < 8; i++) {
        WCX_CHECK_STR_EQ(defs[i].id, k_expected[i].id);
        WCX_CHECK_STR_EQ(defs[i].display_name, k_expected[i].name);
        WCX_CHECK(defs[i].create != NULL && defs[i].feed != NULL && defs[i].flush != NULL &&
                  defs[i].destroy != NULL);
        WCX_CHECK(defs[i]._reserved0 == NULL && defs[i]._reserved1 == 0);
    }
}

WCX_TEST(manifests_declare_the_spec_signals_and_parameters) {
    WcDecoderDef defs[8];
    size_t n = 8;
    WCX_REQUIRE(wavecrux_decoder_register(defs, &n) == WC_DECODER_OK);
    static const char *const sig_names[6] = {"pclk",    "data",       "datak",
                                             "rxvalid", "txelecidle", "rxstatus"};
    for (size_t i = 0; i < 8; i++) {
        wcx_manifest m;
        wcx_error err = {{0}};
        WCX_REQUIRE(wcx_manifest_parse(&m, defs[i].manifest_json, &err));
        WCX_CHECK_EQ_U64(m.signal_count, 6);
        for (size_t s = 0; s < 6 && s < m.signal_count; s++) {
            char name[32] = {0};
            WCX_IGNORE(wcx_manifest_text(&m, m.signals[s].name, name, sizeof name));
            WCX_CHECK_STR_EQ(name, sig_names[s]);
            WCX_CHECK_EQ_U64(m.signals[s].optional, s >= 3u);
        }
        // §2 widths: data 8N, datak N, rxstatus 3, the rest 1.
        const unsigned lanes = k_expected[i].lanes;
        WCX_CHECK_EQ_U64(m.signals[0].bit_width, 1);
        WCX_CHECK_EQ_U64(m.signals[1].bit_width, 8u * lanes);
        WCX_CHECK_EQ_U64(m.signals[2].bit_width, lanes);
        WCX_CHECK_EQ_U64(m.signals[3].bit_width, 1);
        WCX_CHECK_EQ_U64(m.signals[4].bit_width, 1);
        WCX_CHECK_EQ_U64(m.signals[5].bit_width, 3);
        // §3 parameters: both have scrambling and sample_point (enums with
        // labels); only the PIPE decoder has coalesce and show_idle (bools).
        WCX_CHECK_EQ_U64(m.param_count, k_expected[i].dll ? 2u : 4u);
        const size_t scr = wcx_manifest_find_param(&m, "scrambling");
        const size_t sp = wcx_manifest_find_param(&m, "sample_point");
        WCX_REQUIRE(scr != WCX_JSON_NONE && sp != WCX_JSON_NONE);
        WCX_CHECK_EQ_U64(m.params[scr].kind, WCX_PARAM_ENUM);
        WCX_CHECK(m.params[scr].enum_values != WCX_JSON_NONE);
        WCX_CHECK(m.params[scr].enum_labels != WCX_JSON_NONE);
        WCX_CHECK(wcx_jdoc_string_equals(&m.doc, m.params[scr].default_value, "auto"));
        WCX_CHECK_EQ_U64(m.params[sp].kind, WCX_PARAM_ENUM);
        WCX_CHECK(wcx_jdoc_string_equals(&m.doc, m.params[sp].default_value, "before_edge"));
        const size_t co = wcx_manifest_find_param(&m, "coalesce");
        const size_t si = wcx_manifest_find_param(&m, "show_idle");
        if (k_expected[i].dll) {
            WCX_CHECK(co == WCX_JSON_NONE && si == WCX_JSON_NONE);
        } else {
            WCX_REQUIRE(co != WCX_JSON_NONE && si != WCX_JSON_NONE);
            WCX_CHECK_EQ_U64(m.params[co].kind, WCX_PARAM_BOOL);
            WCX_CHECK_EQ_U64(wcx_jdoc_type(&m.doc, m.params[co].default_value), WCX_JT_TRUE);
            WCX_CHECK_EQ_U64(wcx_jdoc_type(&m.doc, m.params[si].default_value), WCX_JT_FALSE);
        }
        wcx_manifest_free(&m);
    }
}

// ── symbol times (§4.2) ────────────────────────────────────────────────────

WCX_TEST(symbol_times_use_the_measured_period) {
    // 16-bit: edges at 10000, 18000, 26000 fs (P = 8000, two lanes of 4000).
    abi_drv d;
    const abi_cfg cfg = {2, false, false, false};
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_pipe_w16", &cfg, "{\"scrambling\":\"off\"}", 10000, 8000));
    static const tsym w1[2] = {KS(0x9C), IDLE};     // first edge: P = 0
    static const tsym w2[2] = {IDLE, KS(0x9C)};     // lane 1
    static const tsym w3[2] = {KS(0x9C), KS(0x9C)}; // both lanes
    abi_word w = abi_word_from(w1, 2);
    abi_edge(&d, &w);
    w = abi_word_from(w2, 2);
    abi_edge(&d, &w);
    w = abi_word_from(w3, 2);
    abi_edge(&d, &w);
    WCX_REQUIRE(d.log.n == 4);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Invalid K symbol 0x9C");
    WCX_CHECK_EQ_U64(d.log.items[0].start, 10000); // P_1 = 0: zero duration
    WCX_CHECK_EQ_U64(d.log.items[0].end, 10000);
    WCX_CHECK_EQ_U64(d.log.items[1].start, 18000 + 4000); // lane 1 of edge 2
    WCX_CHECK_EQ_U64(d.log.items[1].end, 18000 + 8000);
    WCX_CHECK_EQ_U64(d.log.items[2].start, 26000);
    WCX_CHECK_EQ_U64(d.log.items[2].end, 30000);
    WCX_CHECK_EQ_U64(d.log.items[3].start, 30000);
    WCX_CHECK_EQ_U64(d.log.items[3].end, 34000);
    abi_close(&d);
}

WCX_TEST(lane_times_floor_an_uneven_period) {
    // 64-bit with P = 100 fs: lane j starts at t + floor(100 j / 8) =
    // 0, 12, 25, 37, 50, 62, 75, 87 and lasts floor(100 / 8) = 12.
    abi_drv d;
    const abi_cfg cfg = {8, false, false, false};
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_pipe_w64", &cfg, "{\"scrambling\":\"off\"}", 900, 100));
    tsym idle[8];
    tsym bad[8];
    for (unsigned j = 0; j < 8u; j++) {
        idle[j] = (tsym)IDLE;
        bad[j] = (tsym)KS(0x9C);
    }
    abi_word w = abi_word_from(idle, 8);
    abi_edge(&d, &w); // t = 900, P = 0
    w = abi_word_from(bad, 8);
    abi_edge(&d, &w); // t = 1000, P = 100
    WCX_REQUIRE(d.log.n == 8);
    static const uint64_t starts[8] = {0, 12, 25, 37, 50, 62, 75, 87};
    for (unsigned j = 0; j < 8u; j++) {
        WCX_CHECK_EQ_U64(d.log.items[j].start, 1000 + starts[j]);
        WCX_CHECK_EQ_U64(d.log.items[j].end, 1000 + starts[j] + 12);
    }
    abi_close(&d);
}

// ── byte lanes ─────────────────────────────────────────────────────────────

// protocol-notes.md §6: InitFC1-P 40 08 03 F0 -> 35 BC.
static const tsym k_dllp[8] = {KS(0x5C), DS(0x40), DS(0x08), DS(0x03),
                               DS(0xF0), DS(0x35), DS(0xBC), KS(0xFD)};

WCX_TEST(packets_start_at_every_byte_lane) {
    static const char *const ids[3] = {"ferrite.pcie_pipe_w16", "ferrite.pcie_pipe_w32",
                                       "ferrite.pcie_pipe_w64"};
    static const unsigned lanes[3] = {2, 4, 8};
    for (unsigned v = 0; v < 3u; v++) {
        for (unsigned j = 0; j < lanes[v]; j++) {
            abi_drv d;
            const abi_cfg cfg = {lanes[v], false, false, false};
            // Edges at 10000 (P = 0, an idle word), then 18000, 26000, ...
            // with P = 8000: each symbol lasts 8000 / N and the stream is
            // contiguous in time across words.
            WCX_REQUIRE(abi_open(&d, t, ids[v], &cfg, "{\"scrambling\":\"off\"}", 10000, 8000));
            tsym idle[8];
            for (unsigned k = 0; k < 8u; k++) {
                idle[k] = (tsym)IDLE;
            }
            abi_word w = abi_word_from(idle, lanes[v]);
            abi_edge(&d, &w);
            tsym stream[8 + 8];
            for (unsigned k = 0; k < j; k++) {
                stream[k] = (tsym)IDLE;
            }
            memcpy(stream + j, k_dllp, sizeof k_dllp);
            abi_stream(&d, stream, j + 8u);
            abi_flush(&d);
            WCX_REQUIRE(d.log.n == 1);
            WCX_CHECK_STR_EQ(d.log.items[0].label, "DLLP frame (6 symbols)");
            const uint64_t sym = 8000u / lanes[v];
            WCX_CHECK_EQ_U64(d.log.items[0].start, 18000 + j * sym);
            WCX_CHECK_EQ_U64(d.log.items[0].end, 18000 + j * sym + 8u * sym);
            abi_close(&d);
        }
    }
}

// ── gating (§4.3) ──────────────────────────────────────────────────────────

WCX_TEST(rxvalid_and_txelecidle_gate_edges) {
    abi_drv d;
    const abi_cfg cfg = {2, true, true, false};
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_pipe_w16", &cfg, "{\"scrambling\":\"off\"}", 10000, 8000));
    static const tsym stp[2] = {KS(0xFB), DS(0x00)};
    static const tsym junk[2] = {KS(0x9C), KS(0x9C)};
    abi_word w = abi_word_from(stp, 2); // edge 1 (t 10000): STP opens, body 1 symbol
    abi_edge(&d, &w);
    w = abi_word_from(junk, 2); // edge 2 (t 18000): rxvalid 0, the junk is ignored
    w.rxvalid = false;
    abi_edge(&d, &w);
    w = abi_word_from(stp, 2); // edge 3: txelecidle 1 is gating too
    w.txelecidle = true;
    abi_edge(&d, &w);
    w = abi_word_from(stp, 2); // edge 4 (t 34000): rxvalid X is not "0": decoded
    w.rxvalid_x = true;
    abi_edge(&d, &w);
    w = abi_word_from(junk, 2); // edge 5: both gating; rxvalid names the reason
    w.rxvalid = false;
    w.txelecidle = true;
    abi_edge(&d, &w);
    abi_flush(&d);
    WCX_REQUIRE(d.log.n == 2);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Packet truncated by rxvalid");
    WCX_CHECK_STR_EQ(d.log.items[0].fields,
                     "{\"type\":\"truncated\",\"packet\":\"tlp\",\"symbols\":1,\"error\":\"The "
                     "packet ended without END or EDB.\"}");
    WCX_CHECK_EQ_U64(d.log.items[0].start, 10000);
    WCX_CHECK_EQ_U64(d.log.items[0].end, 10000); // P_1 = 0
    WCX_CHECK_STR_EQ(d.log.items[1].label, "Packet truncated by rxvalid");
    WCX_CHECK_EQ_U64(d.log.items[1].start, 34000);
    WCX_CHECK_EQ_U64(d.log.items[1].end, 42000);
    abi_close(&d);
    // txelecidle alone.
    const abi_cfg cfg2 = {2, false, true, false};
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_dll_w16", &cfg2, "{\"scrambling\":\"off\"}", 10000, 8000));
    w = abi_word_from(stp, 2);
    abi_edge(&d, &w);
    w.txelecidle = true;
    abi_edge(&d, &w);
    WCX_REQUIRE(d.log.n == 1);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Packet truncated by txelecidle");
    abi_close(&d);
}

// ── X/Z (§4.4) ─────────────────────────────────────────────────────────────

WCX_TEST(xz_edges_form_one_error_per_run) {
    abi_drv d;
    const abi_cfg cfg = {2, false, false, false};
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_pipe_w16", &cfg, "{\"scrambling\":\"off\"}", 10000, 8000));
    static const tsym stp[2] = {KS(0xFB), DS(0x00)};
    static const tsym idle[2] = {IDLE, IDLE};
    abi_word w = abi_word_from(stp, 2); // edge 1: a packet opens
    abi_edge(&d, &w);
    w = abi_word_from(idle, 2); // edges 2-4 (18000, 26000, 34000): X on data
    w.data_x = 0x00FFu;
    abi_edge(&d, &w);
    abi_edge(&d, &w);
    w.data_x = 0;
    w.datak_x = 0x2u; // X on datak alone counts as well
    abi_edge(&d, &w);
    w = abi_word_from(idle, 2); // edge 5 (42000): clean, the run ends
    abi_edge(&d, &w);
    w = abi_word_from(idle, 2); // edge 6: another run of one
    w.data_x = 1;
    abi_edge(&d, &w);
    abi_flush(&d);             // ends the second run
    WCX_REQUIRE(d.log.n == 2); // the open packet was dropped silently
    WCX_CHECK_STR_EQ(d.log.items[0].label, "X/Z on data");
    WCX_CHECK_STR_EQ(d.log.items[0].fields,
                     "{\"type\":\"unknown_value\",\"edges\":3,\"error\":\"The "
                     "data or datak bus carries X or Z.\"}");
    WCX_CHECK(d.log.items[0].is_error);
    WCX_CHECK_EQ_U64(d.log.items[0].start, 18000);
    WCX_CHECK_EQ_U64(d.log.items[0].end, 34000 + 8000); // lane 1 end of the last X edge
    WCX_CHECK_STR_EQ(d.log.items[1].fields,
                     "{\"type\":\"unknown_value\",\"edges\":1,\"error\":\"The "
                     "data or datak bus carries X or Z.\"}");
    WCX_CHECK_EQ_U64(d.log.items[1].start, 50000);
    WCX_CHECK_EQ_U64(d.log.items[1].end, 58000);
    abi_close(&d);
}

// ── RxStatus (§4.6) ────────────────────────────────────────────────────────

WCX_TEST(rxstatus_codes) {
    static const struct {
        unsigned code;
        const char *label;
        const char *error; // NULL: not an error
    } codes[7] = {
        {1, "RxStatus: SKP added", NULL},
        {2, "RxStatus: SKP removed", NULL},
        {3, "RxStatus: receiver detected", NULL},
        {4, "RxStatus: 8b/10b decode error", "8b/10b decode error reported by the PHY."},
        {5, "RxStatus: elastic buffer overflow", "Elastic buffer overflow reported by the PHY."},
        {6, "RxStatus: elastic buffer underflow", "Elastic buffer underflow reported by the PHY."},
        {7, "RxStatus: disparity error", "Disparity error reported by the PHY."},
    };
    abi_drv d;
    const abi_cfg cfg = {1, true, false, true};
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_pipe_w8", &cfg, "{\"scrambling\":\"off\"}", 4000, 4000));
    static const tsym idle[1] = {IDLE};
    abi_word w = abi_word_from(idle, 1);
    abi_edge(&d, &w); // edge 1: code 0, P = 0
    for (unsigned i = 0; i < 7u; i++) {
        w.rxstatus = codes[i].code; // edges 2..8 at 8000 + 4000 i
        abi_edge(&d, &w);
    }
    abi_edge(&d, &w); // edge 9: 7 again: no event
    w.rxstatus = 0;   // edge 10: back to 0: no event
    abi_edge(&d, &w);
    w.rxstatus = 4; // edge 11 (44000): event
    abi_edge(&d, &w);
    w.rxvalid = false; // edge 12: gated, code 0 (still the "previous edge")
    w.rxstatus = 0;
    abi_edge(&d, &w);
    w.rxvalid = true; // edge 13 (52000): 4 differs from the gated edge's 0: event
    w.rxstatus = 4;
    abi_edge(&d, &w);
    w.data_x = 0xFFu; // edge 14 (56000): X/Z edge, still ungated: event for 5
    w.rxstatus = 5;
    abi_edge(&d, &w);
    abi_flush(&d);
    // 7 codes + 4 at edge 11 + 4 at edge 13 + 5 at edge 14 + the X/Z run.
    WCX_REQUIRE(d.log.n == 11);
    for (unsigned i = 0; i < 7u; i++) {
        WCX_CHECK_STR_EQ(d.log.items[i].label, codes[i].label);
        char fields[160] = {0};
        if (codes[i].error != NULL) {
            WCX_IGNORE(
                wcx_str_format(fields, sizeof fields,
                               "{\"type\":\"rxstatus\",\"code\":\"0b%u%u%u\",\"error\":\"%s\"}",
                               (codes[i].code >> 2u) & 1u, (codes[i].code >> 1u) & 1u,
                               codes[i].code & 1u, codes[i].error));
        } else {
            WCX_IGNORE(wcx_str_format(
                fields, sizeof fields, "{\"type\":\"rxstatus\",\"code\":\"0b%u%u%u\"}",
                (codes[i].code >> 2u) & 1u, (codes[i].code >> 1u) & 1u, codes[i].code & 1u));
        }
        WCX_CHECK_STR_EQ(d.log.items[i].fields, fields);
        WCX_CHECK_EQ_U64(d.log.items[i].is_error, codes[i].error != NULL);
        WCX_CHECK_EQ_U64(d.log.items[i].start, 8000 + 4000 * i);
        WCX_CHECK_EQ_U64(d.log.items[i].end, 12000 + 4000 * i);
    }
    WCX_CHECK_STR_EQ(d.log.items[7].label, "RxStatus: 8b/10b decode error");
    WCX_CHECK_EQ_U64(d.log.items[7].start, 44000);
    WCX_CHECK_STR_EQ(d.log.items[8].label, "RxStatus: 8b/10b decode error");
    WCX_CHECK_EQ_U64(d.log.items[8].start, 52000);
    WCX_CHECK_STR_EQ(d.log.items[9].label, "RxStatus: elastic buffer overflow");
    WCX_CHECK_EQ_U64(d.log.items[9].start, 56000);
    WCX_CHECK_STR_EQ(d.log.items[10].label, "X/Z on data");
    abi_close(&d);
    // The Data Link Layer decoder declares rxstatus and ignores it.
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_dll_w8", &cfg, "{\"scrambling\":\"off\"}", 4000, 4000));
    w = abi_word_from(idle, 1);
    w.rxstatus = 4;
    abi_edge(&d, &w);
    abi_edge(&d, &w);
    abi_flush(&d);
    WCX_CHECK_EQ_U64(d.log.n, 0);
    abi_close(&d);
}

WCX_TEST(rxstatus_with_unknown_bits_is_no_event) {
    // SPEC.md §4.6 speaks of rxstatus's value; an X or Z value is treated as
    // 000: no event, and not the "previous value" either.
    abi_drv d;
    const abi_cfg cfg = {1, false, false, true};
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_pipe_w8", &cfg, "{\"scrambling\":\"off\"}", 4000, 4000));
    static const tsym idle[1] = {IDLE};
    abi_word w = abi_word_from(idle, 1);
    w.rxstatus = 7;
    w.rxstatus_x = true;
    abi_edge(&d, &w);
    abi_edge(&d, &w);
    w.rxstatus_x = false; // 7 after "0": an event at the third edge (12000)
    abi_edge(&d, &w);
    abi_flush(&d);
    WCX_REQUIRE(d.log.n == 1);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "RxStatus: disparity error");
    WCX_CHECK_EQ_U64(d.log.items[0].start, 12000);
    abi_close(&d);
}

// ── sample point (§4.1) ────────────────────────────────────────────────────

WCX_TEST(sample_point_before_or_at_the_edge) {
    // The sample before the edge holds 9C K on lane 0 (an invalid K); the
    // sample at the edge holds DC K. before_edge reports 0x9C, at_edge 0xDC.
    static const char *const points[2] = {"before_edge", "at_edge"};
    static const unsigned expect[2] = {0x9Cu, 0xDCu};
    for (unsigned v = 0; v < 2u; v++) {
        abi_drv d;
        const abi_cfg cfg = {1, false, false, false};
        char params[96] = {0};
        WCX_IGNORE(wcx_str_format(params, sizeof params,
                                  "{\"scrambling\":\"off\",\"sample_point\":\"%s\"}", points[v]));
        WCX_REQUIRE(abi_open(&d, t, "ferrite.pcie_pipe_w8", &cfg, params, 4000, 4000));
        static const tsym before[1] = {KS(0x9C)};
        static const tsym at[1] = {KS(0xDC)};
        abi_word wb = abi_word_from(before, 1);
        abi_word wa = abi_word_from(at, 1);
        WcSample s = abi_pack(d.buf, &cfg, 2000, false, &wb);
        (void)abi_call(&d.def, d.h, &s, &d.log);
        s = abi_pack(d.buf, &cfg, 4000, true, &wa);
        (void)abi_call(&d.def, d.h, &s, &d.log);
        WCX_REQUIRE(d.log.n == 1);
        char label[32] = {0};
        WCX_IGNORE(wcx_str_format(label, sizeof label, "Invalid K symbol 0x%02X", expect[v]));
        WCX_CHECK_STR_EQ(d.log.items[0].label, label);
        abi_close(&d);
    }
}

// ── parameters ─────────────────────────────────────────────────────────────

WCX_TEST(parameters_are_checked_and_plumbed) {
    abi_drv d;
    const abi_cfg cfg = {1, false, false, false};
    // An unknown enum value is one error transaction from the base, then silence.
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_pipe_w8", &cfg, "{\"scrambling\":\"maybe\"}", 4000, 4000));
    static const tsym idle[1] = {IDLE};
    abi_word w = abi_word_from(idle, 1);
    abi_edge(&d, &w);
    abi_edge(&d, &w);
    abi_flush(&d);
    WCX_REQUIRE(d.log.n == 1);
    WCX_CHECK(d.log.items[0].is_error);
    WCX_CHECK(strstr(d.log.items[0].label, "scrambling") != NULL);
    abi_close(&d);
    // show_idle shows idle runs; coalesce=false separates ordered sets.
    static const tsym ts1[16] = {KS(0xBC), KS(0xF7), KS(0xF7), DS(0x18), DS(0x02), DS(0x00),
                                 DS(0x4A), DS(0x4A), DS(0x4A), DS(0x4A), DS(0x4A), DS(0x4A),
                                 DS(0x4A), DS(0x4A), DS(0x4A), DS(0x4A)};
    WCX_REQUIRE(abi_open(&d, t, "ferrite.pcie_pipe_w8", &cfg,
                         "{\"scrambling\":\"off\",\"show_idle\":true,\"coalesce\":false}", 4000,
                         4000));
    abi_edge(&d, &w);
    abi_edge(&d, &w);
    ABI_STREAM(&d, ts1);
    ABI_STREAM(&d, ts1);
    abi_flush(&d);
    WCX_REQUIRE(d.log.n == 3);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Idle " PCIE_TIMES "2");
    WCX_CHECK_STR_EQ(d.log.items[1].label, "TS1 link=PAD lane=PAD n_fts=24 gen1");
    WCX_CHECK_STR_EQ(d.log.items[2].label, "TS1 link=PAD lane=PAD n_fts=24 gen1");
    abi_close(&d);
    // The DLL decoder does not take the PIPE-only parameters, but WaveCrux
    // never sends them either; a stray one is simply ignored by the base.
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_dll_w8", &cfg, "{\"scrambling\":\"on\"}", 4000, 4000));
    abi_edge(&d, &w);
    abi_flush(&d);
    WCX_CHECK_EQ_U64(d.log.n, 0);
    abi_close(&d);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, eight_decoders_register_in_spec_order);
    WCX_RUN(&t, manifests_declare_the_spec_signals_and_parameters);
    WCX_RUN(&t, symbol_times_use_the_measured_period);
    WCX_RUN(&t, lane_times_floor_an_uneven_period);
    WCX_RUN(&t, packets_start_at_every_byte_lane);
    WCX_RUN(&t, rxvalid_and_txelecidle_gate_edges);
    WCX_RUN(&t, xz_edges_form_one_error_per_run);
    WCX_RUN(&t, rxstatus_codes);
    WCX_RUN(&t, rxstatus_with_unknown_bits_is_no_event);
    WCX_RUN(&t, sample_point_before_or_at_the_edge);
    WCX_RUN(&t, parameters_are_checked_and_plumbed);
    return wcx_test_finish(&t);
}
