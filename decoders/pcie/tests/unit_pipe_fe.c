// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The PIPE front end (SPEC.md §6, §7, §10) through a real pipeline into a
// buffered sink: labels and fields_json exactly as the specification writes
// them. Symbol i spans [1000 + 10 i, 1010 + 10 i) fs.

#include <string.h>

#include "pcie_fe_common.h"
#include "pcie_pipe_fe.h"
#include "pcie_test_util.h"

#define T(i)   (1000u + 10u * (i))
#define END(i) (1010u + 10u * (i))

typedef struct rig {
    test_sink s;
    pcie_pipe_fe fe;
    pcie_pipeline p;
} rig;

static bool rig_init(rig *r, bool coalesce, bool show_idle) {
    if (!test_sink_init(&r->s, 64)) {
        return false;
    }
    pcie_pipe_fe_init(&r->fe, &r->s.sink, coalesce, show_idle);
    pcie_pipeline_init(&r->p, false, pcie_pipe_fe_event, &r->fe);
    return true;
}

static void rig_free(rig *r) {
    test_sink_free(&r->s);
}

#define TX(r, i) ((r).s.buf.items[(i)])

#define TS_BODY(id)                                                                               \
    DS(0x18), DS(0x02), DS(0x00), DS(id), DS(id), DS(id), DS(id), DS(id), DS(id), DS(id), DS(id), \
        DS(id), DS(id)
static const tsym k_ts1[16] = {KS(0xBC), KS(0xF7), KS(0xF7), TS_BODY(0x4A)};

// protocol-notes.md §6/§7 known-answer packets.
static const tsym k_dllp[8] = {KS(0x5C), DS(0x40), DS(0x08), DS(0x03),
                               DS(0xF0), DS(0x35), DS(0xBC), KS(0xFD)};
static const tsym k_tlp[20] = {KS(0xFB), DS(0x00), DS(0x05), DS(0x00), DS(0x00), DS(0x00), DS(0x01),
                               DS(0x01), DS(0x00), DS(0x00), DS(0x0F), DS(0x00), DS(0x00), DS(0x10),
                               DS(0x00), DS(0x9A), DS(0xE8), DS(0xF8), DS(0xC2), KS(0xFD)};

WCX_TEST(ts1_label_and_fields) {
    rig r;
    WCX_REQUIRE(rig_init(&r, true, false));
    FEED(&r.p, k_ts1, 1000, 10);
    WCX_CHECK_EQ_U64(r.s.buf.count, 0); // coalescing: pending until the run ends
    pcie_pipeline_break(&r.p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(r.s.buf.count == 1);
    // §7: ×N only when N > 1; link/lane "PAD"; n_fts 0x18 = 24; rate 0x02 = gen1.
    WCX_CHECK_STR_EQ(TX(r, 0).label, "TS1 link=PAD lane=PAD n_fts=24 gen1");
    WCX_CHECK_STR_EQ(TX(r, 0).fields,
                     "{\"type\":\"ts1\",\"count\":1,\"link\":\"PAD\",\"lane\":\"PAD\","
                     "\"n_fts\":24,\"rate_id\":\"0x02\",\"training_control\":\"0x00\","
                     "\"hot_reset\":false,\"disable_link\":false,\"loopback\":false,"
                     "\"disable_scrambling\":false,\"compliance_receive\":false}");
    WCX_CHECK(!TX(r, 0).is_error);
    WCX_CHECK_EQ_U64(TX(r, 0).start_fs, T(0));
    WCX_CHECK_EQ_U64(TX(r, 0).end_fs, END(15));
    rig_free(&r);
}

WCX_TEST(ts2_with_numbers_gen2_and_every_control_bit) {
    // Link 3, Lane 0, N_FTS 255, rate 0x06 (5 GT/s capable), control 0x1F.
    static const tsym ts2[16] = {KS(0xBC), DS(0x03), DS(0x00), DS(0xFF), DS(0x06), DS(0x1F),
                                 DS(0x45), DS(0x45), DS(0x45), DS(0x45), DS(0x45), DS(0x45),
                                 DS(0x45), DS(0x45), DS(0x45), DS(0x45)};
    rig r;
    WCX_REQUIRE(rig_init(&r, false, false));
    FEED(&r.p, ts2, 1000, 10);
    WCX_REQUIRE(r.s.buf.count == 1); // coalesce off: emitted at once
    WCX_CHECK_STR_EQ(TX(r, 0).label, "TS2 link=3 lane=0 n_fts=255 gen2");
    WCX_CHECK_STR_EQ(TX(r, 0).fields,
                     "{\"type\":\"ts2\",\"count\":1,\"link\":3,\"lane\":0,\"n_fts\":255,"
                     "\"rate_id\":\"0x06\",\"training_control\":\"0x1F\",\"hot_reset\":true,"
                     "\"disable_link\":true,\"loopback\":true,\"disable_scrambling\":true,"
                     "\"compliance_receive\":true}");
    rig_free(&r);
}

WCX_TEST(coalescing_rules) {
    rig r;
    WCX_REQUIRE(rig_init(&r, true, false));
    // Three identical TS1, a TS1 with a different N_FTS, then a TS2.
    tsym other[16];
    memcpy(other, k_ts1, sizeof other);
    other[3] = (tsym)DS(0x19);
    static const tsym ts2[16] = {KS(0xBC), KS(0xF7), KS(0xF7), TS_BODY(0x45)};
    FEED(&r.p, k_ts1, 1000, 10);
    FEED(&r.p, k_ts1, 1160, 10);
    FEED(&r.p, k_ts1, 1320, 10);
    FEED(&r.p, other, 1480, 10);
    FEED(&r.p, ts2, 1640, 10);
    WCX_REQUIRE(r.s.buf.count == 2);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "TS1 " PCIE_TIMES "3 link=PAD lane=PAD n_fts=24 gen1");
    WCX_CHECK(strstr(TX(r, 0).fields, "\"count\":3,") != NULL);
    WCX_CHECK_EQ_U64(TX(r, 0).start_fs, 1000);
    WCX_CHECK_EQ_U64(TX(r, 0).end_fs, 1320 + 160); // end of the third set
    WCX_CHECK_STR_EQ(TX(r, 1).label, "TS1 link=PAD lane=PAD n_fts=25 gen1");
    pcie_pipeline_break(&r.p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(r.s.buf.count == 3);
    WCX_CHECK_STR_EQ(TX(r, 2).label, "TS2 link=PAD lane=PAD n_fts=24 gen1");
    rig_free(&r);
    // Anything between two sets ends the run (§7): a SKP OS here.
    static const tsym skp[4] = {KS(0xBC), KS(0x1C), KS(0x1C), KS(0x1C)};
    WCX_REQUIRE(rig_init(&r, true, false));
    FEED(&r.p, k_ts1, 1000, 10);
    FEED(&r.p, skp, 1160, 10);
    FEED(&r.p, k_ts1, 1200, 10);
    pcie_pipeline_break(&r.p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(r.s.buf.count == 3);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "TS1 link=PAD lane=PAD n_fts=24 gen1");
    WCX_CHECK_STR_EQ(TX(r, 1).label, "SKP");
    WCX_CHECK_STR_EQ(TX(r, 2).label, "TS1 link=PAD lane=PAD n_fts=24 gen1");
    rig_free(&r);
    // coalesce = false: two transactions, count 1 each, no ×1.
    WCX_REQUIRE(rig_init(&r, false, false));
    FEED(&r.p, k_ts1, 1000, 10);
    FEED(&r.p, k_ts1, 1160, 10);
    WCX_REQUIRE(r.s.buf.count == 2);
    WCX_CHECK_STR_EQ(TX(r, 1).label, "TS1 link=PAD lane=PAD n_fts=24 gen1");
    WCX_CHECK(strstr(TX(r, 1).fields, "\"count\":1,") != NULL);
    rig_free(&r);
}

WCX_TEST(skp_fts_eios_eieos_labels) {
    static const tsym skp3[] = {KS(0xBC), KS(0x1C), KS(0x1C), KS(0x1C)};
    static const tsym skp2[] = {KS(0xBC), KS(0x1C), KS(0x1C), KS(0xBC), KS(0x3C),
                                KS(0x3C), KS(0x3C), KS(0xBC), KS(0x3C), KS(0x3C),
                                KS(0x3C), KS(0xBC), KS(0x7C), KS(0x7C), KS(0x7C)};
    rig r;
    WCX_REQUIRE(rig_init(&r, true, false));
    FEED(&r.p, skp3, 1000, 10);
    FEED(&r.p, skp2, 1040, 10);
    tsym eieos[16] = {KS(0xBC)};
    for (unsigned i = 1; i <= 14u; i++) {
        eieos[i] = (tsym)KS(0xFC);
    }
    eieos[15] = (tsym)DS(0x4A);
    FEED(&r.p, eieos, 1190, 10);
    pcie_pipeline_break(&r.p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(r.s.buf.count == 5);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "SKP"); // 3 SKPs: the nominal count
    WCX_CHECK_STR_EQ(TX(r, 0).fields, "{\"type\":\"skp\",\"skp_symbols\":3}");
    WCX_CHECK_EQ_U64(TX(r, 0).end_fs, END(3));
    WCX_CHECK_STR_EQ(TX(r, 1).label, "SKP " PCIE_TIMES "2");
    WCX_CHECK_STR_EQ(TX(r, 1).fields, "{\"type\":\"skp\",\"skp_symbols\":2}");
    WCX_CHECK_STR_EQ(TX(r, 2).label, "FTS " PCIE_TIMES "2"); // two FTS sets coalesced
    WCX_CHECK_STR_EQ(TX(r, 2).fields, "{\"type\":\"fts\",\"count\":2}");
    WCX_CHECK_EQ_U64(TX(r, 2).start_fs, 1070);
    WCX_CHECK_EQ_U64(TX(r, 2).end_fs, 1150);
    WCX_CHECK_STR_EQ(TX(r, 3).label, "EIOS");
    WCX_CHECK_STR_EQ(TX(r, 3).fields, "{\"type\":\"eios\"}");
    WCX_CHECK_STR_EQ(TX(r, 4).label, "EIEOS");
    WCX_CHECK_STR_EQ(TX(r, 4).fields, "{\"type\":\"eieos\",\"count\":1}");
    rig_free(&r);
    // A lone FTS set: "FTS" with count 1 (§10.3).
    static const tsym fts[] = {KS(0xBC), KS(0x3C), KS(0x3C), KS(0x3C)};
    WCX_REQUIRE(rig_init(&r, true, false));
    FEED(&r.p, fts, 1000, 10);
    pcie_pipeline_break(&r.p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(r.s.buf.count == 1);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "FTS");
    WCX_CHECK_STR_EQ(TX(r, 0).fields, "{\"type\":\"fts\",\"count\":1}");
    rig_free(&r);
}

WCX_TEST(idle_shown_only_on_request) {
    static const tsym syms[] = {IDLE, IDLE, IDLE, DS(0x12), IDLE};
    rig r;
    WCX_REQUIRE(rig_init(&r, true, false));
    FEED(&r.p, syms, 1000, 10);
    pcie_pipeline_break(&r.p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(r.s.buf.count == 1); // only the stray byte
    WCX_CHECK_STR_EQ(TX(r, 0).label, "Data outside a packet");
    rig_free(&r);
    WCX_REQUIRE(rig_init(&r, true, true));
    FEED(&r.p, syms, 1000, 10);
    pcie_pipeline_break(&r.p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(r.s.buf.count == 3);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "Idle " PCIE_TIMES "3");
    WCX_CHECK_STR_EQ(TX(r, 0).fields, "{\"type\":\"idle\",\"symbols\":3}");
    WCX_CHECK_EQ_U64(TX(r, 0).start_fs, T(0));
    WCX_CHECK_EQ_U64(TX(r, 0).end_fs, END(2));
    WCX_CHECK_STR_EQ(TX(r, 2).label, "Idle"); // §10.3: no ×1
    WCX_CHECK_STR_EQ(TX(r, 2).fields, "{\"type\":\"idle\",\"symbols\":1}");
    rig_free(&r);
}

WCX_TEST(frames) {
    rig r;
    WCX_REQUIRE(rig_init(&r, true, false));
    FEED(&r.p, k_dllp, 1000, 10);
    FEED(&r.p, k_tlp, 1080, 10);
    WCX_REQUIRE(r.s.buf.count == 2);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "DLLP frame (6 symbols)");
    WCX_CHECK_STR_EQ(TX(r, 0).fields, "{\"type\":\"dllp_frame\",\"symbols\":6,\"end\":\"END\"}");
    WCX_CHECK_EQ_U64(TX(r, 0).start_fs, T(0));
    WCX_CHECK_EQ_U64(TX(r, 0).end_fs, END(7));
    WCX_CHECK_STR_EQ(TX(r, 1).label, "TLP frame seq=5 (18 symbols)"); // body 00 05 ...
    WCX_CHECK_STR_EQ(TX(r, 1).fields,
                     "{\"type\":\"tlp_frame\",\"seq\":5,\"symbols\":18,\"end\":\"END\"}");
    WCX_CHECK_EQ_U64(TX(r, 1).start_fs, 1080);
    WCX_CHECK_EQ_U64(TX(r, 1).end_fs, 1080 + 200);
    rig_free(&r);
    // EDB: a TLP frame says so; a DLLP ended by EDB is the dllp_edb error (§10.7).
    tsym edb[20];
    memcpy(edb, k_tlp, sizeof edb);
    edb[19] = (tsym)KS(0xFE);
    static const tsym dllp_edb[] = {KS(0x5C), DS(0x40), DS(0x08), DS(0x03),
                                    DS(0xF0), DS(0x35), DS(0xBC), KS(0xFE)};
    // A one-byte TLP body has no sequence number; a 12-bit seq masks the
    // reserved high nibble (0xF5 0x67 -> seq 0x567 = 1383).
    static const tsym tiny[] = {KS(0xFB), DS(0xAA), KS(0xFD)};
    static const tsym seq12[] = {KS(0xFB), DS(0xF5), DS(0x67), KS(0xFD)};
    WCX_REQUIRE(rig_init(&r, true, false));
    FEED(&r.p, edb, 1000, 10);
    FEED(&r.p, dllp_edb, 1200, 10);
    FEED(&r.p, tiny, 1280, 10);
    FEED(&r.p, seq12, 1310, 10);
    WCX_REQUIRE(r.s.buf.count == 4);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "TLP frame seq=5 (18 symbols, EDB)");
    WCX_CHECK_STR_EQ(TX(r, 0).fields,
                     "{\"type\":\"tlp_frame\",\"seq\":5,\"symbols\":18,\"end\":\"EDB\"}");
    WCX_CHECK_STR_EQ(TX(r, 1).label, "DLLP ended by EDB");
    WCX_CHECK_STR_EQ(TX(r, 1).fields, "{\"type\":\"dllp_edb\",\"symbols\":6,\"error\":\"A DLLP "
                                      "cannot be ended by EDB.\"}");
    WCX_CHECK(TX(r, 1).is_error);
    WCX_CHECK_STR_EQ(TX(r, 2).label, "TLP frame (1 symbols)");
    WCX_CHECK_STR_EQ(TX(r, 2).fields, "{\"type\":\"tlp_frame\",\"symbols\":1,\"end\":\"END\"}");
    WCX_CHECK_STR_EQ(TX(r, 3).label, "TLP frame seq=1383 (2 symbols)");
    rig_free(&r);
}

WCX_TEST(every_common_error_label_and_sentence) {
    // §6 and §10.1, one of each: invalid K, END without start, EDB without
    // start, unexpected K, stray data, unknown ordered set, K in packet,
    // truncation by STP and by rxvalid.
    static const tsym syms[] = {
        KS(0x9C),                               // 0 invalid_k
        KS(0xFD),                               // 1 unmatched_end
        KS(0xFE),                               // 2 unmatched_end (EDB)
        KS(0x3C),                               // 3 unexpected_k
        DS(0x12), DS(0x34),                     // 4-5 stray run
        KS(0xBC), KS(0x5C),                     // 6-7 unknown_os
        KS(0xFB), DS(0x00), DS(0x01), KS(0x1C), // 8-11 k_in_packet (packet dropped)
        KS(0x5C), DS(0x40), KS(0xFB), DS(0x00), // 12-15 truncated by STP, then a TLP
    };
    rig r;
    WCX_REQUIRE(rig_init(&r, true, false));
    FEED(&r.p, syms, 1000, 10);
    pcie_pipeline_break(&r.p, PCIE_BREAK_RXVALID);
    WCX_REQUIRE(r.s.buf.count == 9);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "Invalid K symbol 0x9C");
    WCX_CHECK_STR_EQ(TX(r, 0).fields,
                     "{\"type\":\"invalid_k\",\"symbol\":\"0x9C\",\"error\":\"Not a PCIe control "
                     "symbol.\"}");
    WCX_CHECK_STR_EQ(TX(r, 1).label, "END without start");
    WCX_CHECK_STR_EQ(TX(r, 1).fields, "{\"type\":\"unmatched_end\",\"error\":\"END or EDB arrived "
                                      "with no packet open.\"}");
    WCX_CHECK_STR_EQ(TX(r, 2).label, "EDB without start");
    WCX_CHECK_STR_EQ(TX(r, 3).label, "Unexpected K symbol 0x3C");
    WCX_CHECK_STR_EQ(TX(r, 3).fields,
                     "{\"type\":\"unexpected_k\",\"symbol\":\"0x3C\",\"error\":\"A control symbol "
                     "appeared outside an ordered set or packet.\"}");
    WCX_CHECK_STR_EQ(TX(r, 4).label, "Data outside a packet");
    WCX_CHECK_STR_EQ(TX(r, 4).fields,
                     "{\"type\":\"stray_data\",\"symbols\":2,\"first\":\"0x12\",\"error\":\"Data "
                     "outside a packet or ordered set; logical idle is 0x00.\"}");
    WCX_CHECK_EQ_U64(TX(r, 4).start_fs, T(4));
    WCX_CHECK_EQ_U64(TX(r, 4).end_fs, END(5));
    WCX_CHECK_STR_EQ(TX(r, 5).label, "Unknown ordered set");
    WCX_CHECK_STR_EQ(TX(r, 5).fields,
                     "{\"type\":\"unknown_os\",\"symbol\":\"0x5C\",\"error\":\"COM was not "
                     "followed by a recognised ordered set.\"}");
    WCX_CHECK_EQ_U64(TX(r, 5).start_fs, T(6));
    WCX_CHECK_EQ_U64(TX(r, 5).end_fs, END(7));
    WCX_CHECK_STR_EQ(TX(r, 6).label, "Invalid K symbol in packet 0x1C");
    WCX_CHECK_STR_EQ(TX(r, 6).fields,
                     "{\"type\":\"k_in_packet\",\"symbol\":\"0x1C\",\"error\":\"A control symbol "
                     "other than END or EDB appeared inside a packet.\"}");
    WCX_CHECK_EQ_U64(TX(r, 6).start_fs, T(11));
    WCX_CHECK_STR_EQ(TX(r, 7).label, "Packet truncated by STP");
    WCX_CHECK_STR_EQ(TX(r, 7).fields,
                     "{\"type\":\"truncated\",\"packet\":\"dllp\",\"symbols\":1,\"error\":\"The "
                     "packet ended without END or EDB.\"}");
    WCX_CHECK_EQ_U64(TX(r, 7).start_fs, T(12));
    WCX_CHECK_EQ_U64(TX(r, 7).end_fs, END(13));
    WCX_CHECK_STR_EQ(TX(r, 8).label, "Packet truncated by rxvalid");
    WCX_CHECK_STR_EQ(TX(r, 8).fields,
                     "{\"type\":\"truncated\",\"packet\":\"tlp\",\"symbols\":1,\"error\":\"The "
                     "packet ended without END or EDB.\"}");
    for (size_t i = 0; i < 9; i++) {
        WCX_CHECK(TX(r, i).is_error);
    }
    rig_free(&r);
    // The other truncation labels.
    static const tsym by_com[] = {KS(0xFB), DS(0x00), KS(0xBC), KS(0x1C)};
    WCX_REQUIRE(rig_init(&r, true, false));
    FEED(&r.p, by_com, 1000, 10);
    WCX_REQUIRE(r.s.buf.count == 1);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "Packet truncated by COM");
    rig_free(&r);
    static const tsym by_sdp[] = {KS(0xFB), DS(0x00), KS(0x5C)};
    WCX_REQUIRE(rig_init(&r, true, false));
    FEED(&r.p, by_sdp, 1000, 10);
    pcie_pipeline_break(&r.p, PCIE_BREAK_TXELECIDLE);
    WCX_REQUIRE(r.s.buf.count == 2);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "Packet truncated by SDP");
    WCX_CHECK_STR_EQ(TX(r, 1).label, "Packet truncated by txelecidle");
    WCX_CHECK_STR_EQ(TX(r, 1).fields,
                     "{\"type\":\"truncated\",\"packet\":\"dllp\",\"symbols\":0,\"error\":\"The "
                     "packet ended without END or EDB.\"}");
    rig_free(&r);
    WCX_REQUIRE(rig_init(&r, true, false));
    FEED(&r.p, by_sdp, 1000, 10);
    pcie_pipeline_break(&r.p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(r.s.buf.count == 2);
    WCX_CHECK_STR_EQ(TX(r, 1).label, "Packet truncated by end of trace");
    rig_free(&r);
}

WCX_TEST(end_of_trace_emits_the_coalesced_run_before_the_open_packet) {
    static const tsym open[] = {KS(0xFB), DS(0x00), DS(0x07)};
    rig r;
    WCX_REQUIRE(rig_init(&r, true, false));
    FEED(&r.p, k_ts1, 1000, 10);
    FEED(&r.p, k_ts1, 1160, 10);
    FEED(&r.p, open, 1320, 10);
    WCX_CHECK_EQ_U64(r.s.buf.count, 0);
    pcie_pipeline_break(&r.p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(r.s.buf.count == 2);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "TS1 " PCIE_TIMES "2 link=PAD lane=PAD n_fts=24 gen1");
    WCX_CHECK_STR_EQ(TX(r, 1).label, "Packet truncated by end of trace");
    WCX_CHECK_EQ_U64(TX(r, 1).start_fs, 1320);
    WCX_CHECK_EQ_U64(TX(r, 1).end_fs, 1350);
    rig_free(&r);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, ts1_label_and_fields);
    WCX_RUN(&t, ts2_with_numbers_gen2_and_every_control_bit);
    WCX_RUN(&t, coalescing_rules);
    WCX_RUN(&t, skp_fts_eios_eieos_labels);
    WCX_RUN(&t, idle_shown_only_on_request);
    WCX_RUN(&t, frames);
    WCX_RUN(&t, every_common_error_label_and_sentence);
    WCX_RUN(&t, end_of_trace_emits_the_coalesced_run_before_the_open_packet);
    return wcx_test_finish(&t);
}
