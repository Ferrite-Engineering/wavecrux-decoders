// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Scrambling detection (SPEC.md §5.5, §10.3, §10.5, §10.6) through the
// plugin's ABI, 8-bit PIPE: one symbol per clock, edges 4000 fs apart from
// 4000 fs, so the symbol captured at edge k (k >= 1) spans
// [4000 k, 4000 (k + 1)) and the first edge has a zero-length symbol.

#include <string.h>

#include "pcie_detector.h"
#include "pcie_test_util.h"

#define EDGE(k) ((uint64_t)4000u * (uint64_t)(k))

static const abi_cfg k_w8 = {1, false, false, false};
static const tsym k_idle[1] = {IDLE};

#define TS_BODY(id)                                                                               \
    DS(0x18), DS(0x02), DS(0x00), DS(id), DS(id), DS(id), DS(id), DS(id), DS(id), DS(id), DS(id), \
        DS(id), DS(id)
static const tsym k_ts1[16] = {KS(0xBC), KS(0xF7), KS(0xF7), TS_BODY(0x4A)};
static const tsym k_skp[4] = {KS(0xBC), KS(0x1C), KS(0x1C), KS(0x1C)};
// protocol-notes.md §6: 40 08 03 F0 -> 35 BC; Ack seq 0 -> B3 62.
static const tsym k_initfc[8] = {KS(0x5C), DS(0x40), DS(0x08), DS(0x03),
                                 DS(0xF0), DS(0x35), DS(0xBC), KS(0xFD)};
static const tsym k_ack[8] = {KS(0x5C), DS(0x00), DS(0x00), DS(0x00),
                              DS(0x00), DS(0xB3), DS(0x62), KS(0xFD)};

static void open8(abi_drv *d, wcx_test *t, const char *id) {
    WCX_REQUIRE(abi_open(d, t, id, &k_w8, NULL, EDGE(1), 4000));
    const abi_word w = abi_word_from(k_idle, 1); // edge 1: the zero-period edge
    abi_edge(d, &w);
}

WCX_TEST(off_wins_at_the_first_valid_packet) {
    abi_drv d;
    open8(&d, t, "ferrite.pcie_pipe_w8");
    // Symbols from edge 2: TS1 (2..17), TS1 (18..33), SKP OS (34..37),
    // idle (38, 39), InitFC1-P (40..47), Ack (48..55).
    ABI_STREAM(&d, k_ts1);
    ABI_STREAM(&d, k_ts1);
    ABI_STREAM(&d, k_skp);
    ABI_STREAM(&d, k_idle);
    ABI_STREAM(&d, k_idle);
    WCX_CHECK_EQ_U64(d.log.n, 0); // everything is buffered until the lock
    ABI_STREAM(&d, k_initfc);
    WCX_REQUIRE(d.log.n == 4); // the lock released the buffer, then the notice
    WCX_CHECK_STR_EQ(d.log.items[0].label, "TS1 " PCIE_TIMES "2 link=PAD lane=PAD n_fts=24 gen1");
    WCX_CHECK_EQ_U64(d.log.items[0].start, EDGE(2));
    WCX_CHECK_EQ_U64(d.log.items[0].end, EDGE(34));
    WCX_CHECK_STR_EQ(d.log.items[1].label, "SKP");
    WCX_CHECK_STR_EQ(d.log.items[2].label, "DLLP frame (6 symbols)");
    WCX_CHECK_EQ_U64(d.log.items[2].start, EDGE(40));
    WCX_CHECK_EQ_U64(d.log.items[2].end, EDGE(48));
    WCX_CHECK_STR_EQ(d.log.items[3].label, "Scrambling: off (detected)");
    WCX_CHECK_STR_EQ(d.log.items[3].fields, "{\"type\":\"scrambling_detected\",\"mode\":\"off\"}");
    WCX_CHECK(!d.log.items[3].is_error);
    WCX_CHECK_EQ_U64(d.log.items[3].start, EDGE(40)); // spans the winning packet
    WCX_CHECK_EQ_U64(d.log.items[3].end, EDGE(48));
    ABI_STREAM(&d, k_ack); // locked: straight through
    WCX_REQUIRE(d.log.n == 5);
    WCX_CHECK_STR_EQ(d.log.items[4].label, "DLLP frame (6 symbols)");
    abi_flush(&d);
    WCX_CHECK_EQ_U64(d.log.n, 5); // nothing more at flush
    abi_close(&d);
    // The Data Link Layer decoder on the same stream.
    open8(&d, t, "ferrite.pcie_dll_w8");
    ABI_STREAM(&d, k_ts1);
    ABI_STREAM(&d, k_skp);
    ABI_STREAM(&d, k_initfc);
    ABI_STREAM(&d, k_ack);
    abi_flush(&d);
    WCX_REQUIRE(d.log.n == 3);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "InitFC1-P VC0 H=32 D=1008");
    WCX_CHECK_STR_EQ(d.log.items[1].label, "Scrambling: off (detected)");
    WCX_CHECK_STR_EQ(d.log.items[2].label, "Ack seq=0");
    abi_close(&d);
}

WCX_TEST(edge_level_transactions_keep_their_place_before_the_lock) {
    abi_drv d;
    open8(&d, t, "ferrite.pcie_pipe_w8");
    ABI_STREAM(&d, k_skp); // edges 2..5
    abi_word w = abi_word_from(k_idle, 1);
    w.data_x = 0xFFu;
    abi_edge(&d, &w);         // edge 6: X/Z
    ABI_STREAM(&d, k_initfc); // edges 7..14 (the X/Z run ends at edge 7)
    WCX_REQUIRE(d.log.n == 4);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "SKP");
    WCX_CHECK_STR_EQ(d.log.items[1].label, "X/Z on data");
    WCX_CHECK_EQ_U64(d.log.items[1].start, EDGE(6));
    WCX_CHECK_EQ_U64(d.log.items[1].end, EDGE(7));
    WCX_CHECK_STR_EQ(d.log.items[2].label, "DLLP frame (6 symbols)");
    WCX_CHECK_STR_EQ(d.log.items[3].label, "Scrambling: off (detected)");
    abi_close(&d);
}

WCX_TEST(on_wins_with_a_scrambled_packet) {
    // COM SKP SKP SKP SDP then 40 08 03 F0 35 BC XOR k[1..6] (SDP took k[0]):
    // 57 C8 17 42 D2 BE, END (protocol-notes.md §3 keys 17 C0 14 B2 E7 02).
    static const tsym scrambled[12] = {KS(0xBC), KS(0x1C), KS(0x1C), KS(0x1C), KS(0x5C), DS(0x57),
                                       DS(0xC8), DS(0x17), DS(0x42), DS(0xD2), DS(0xBE), KS(0xFD)};
    abi_drv d;
    open8(&d, t, "ferrite.pcie_dll_w8");
    ABI_STREAM(&d, scrambled); // edges 2..13
    WCX_REQUIRE(d.log.n == 2);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "InitFC1-P VC0 H=32 D=1008");
    WCX_CHECK_EQ_U64(d.log.items[0].start, EDGE(6));
    WCX_CHECK_EQ_U64(d.log.items[0].end, EDGE(14));
    WCX_CHECK_STR_EQ(d.log.items[1].label, "Scrambling: on (detected)");
    WCX_CHECK_STR_EQ(d.log.items[1].fields, "{\"type\":\"scrambling_detected\",\"mode\":\"on\"}");
    WCX_CHECK_EQ_U64(d.log.items[1].start, EDGE(6));
    WCX_CHECK_EQ_U64(d.log.items[1].end, EDGE(14));
    // The `off` pipeline's view (a CRC-mismatched InitFC1-NP) was discarded.
    // Scrambled idles after the lock: END took k[7], so wire 72 6E = k[8],
    // k[9] -> idle, nothing.
    static const tsym idles[2] = {DS(0x72), DS(0x6E)};
    ABI_STREAM(&d, idles);
    abi_flush(&d);
    WCX_CHECK_EQ_U64(d.log.n, 2);
    abi_close(&d);
    // Forced `off` on the same stream decodes the wire bytes as they are:
    // 57 C8 17 42 -> type 0x50 VC 7; HdrFC = (0xC8 & 0x3F) << 2 | 0x17 >> 6
    // = 32; DataFC = (0x17 & 0x0F) << 8 | 0x42 = 0x742 = 1858.
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_dll_w8", &k_w8, "{\"scrambling\":\"off\"}", EDGE(1), 4000));
    ABI_STREAM(&d, scrambled);
    WCX_REQUIRE(d.log.n == 1);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "InitFC1-NP VC7 H=32 D=1858 CRC mismatch");
    abi_close(&d);
}

WCX_TEST(training_sets_lock_to_off) {
    // TS1 with Training Control bit 3; three of them coalesce, but the
    // notice spans only the first (§10.7).
    static const tsym ts1_ds[16] = {KS(0xBC), KS(0xF7), KS(0xF7), DS(0x18), DS(0x02), DS(0x08),
                                    DS(0x4A), DS(0x4A), DS(0x4A), DS(0x4A), DS(0x4A), DS(0x4A),
                                    DS(0x4A), DS(0x4A), DS(0x4A), DS(0x4A)};
    abi_drv d;
    open8(&d, t, "ferrite.pcie_pipe_w8");
    ABI_STREAM(&d, ts1_ds); // edges 2..17
    WCX_REQUIRE(d.log.n == 1);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Scrambling: off (training sets)");
    WCX_CHECK_STR_EQ(d.log.items[0].fields, "{\"type\":\"scrambling_detected\",\"mode\":\"off\"}");
    WCX_CHECK_EQ_U64(d.log.items[0].start, EDGE(2));
    WCX_CHECK_EQ_U64(d.log.items[0].end, EDGE(18));
    ABI_STREAM(&d, ts1_ds);
    ABI_STREAM(&d, ts1_ds);
    abi_flush(&d);
    WCX_REQUIRE(d.log.n == 2);
    WCX_CHECK_STR_EQ(d.log.items[1].label, "TS1 " PCIE_TIMES "3 link=PAD lane=PAD n_fts=24 gen1");
    WCX_CHECK(strstr(d.log.items[1].fields, "\"disable_scrambling\":true") != NULL);
    WCX_CHECK_EQ_U64(d.log.items[1].start, EDGE(2));
    WCX_CHECK_EQ_U64(d.log.items[1].end, EDGE(50));
    abi_close(&d);
    // The DLL decoder emits the notice alone; with scrambling forced on the
    // bit is ignored (no notice at all).
    open8(&d, t, "ferrite.pcie_dll_w8");
    ABI_STREAM(&d, ts1_ds);
    abi_flush(&d);
    WCX_REQUIRE(d.log.n == 1);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Scrambling: off (training sets)");
    abi_close(&d);
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_dll_w8", &k_w8, "{\"scrambling\":\"on\"}", EDGE(1), 4000));
    ABI_STREAM(&d, ts1_ds);
    abi_flush(&d);
    WCX_CHECK_EQ_U64(d.log.n, 0);
    abi_close(&d);
}

WCX_TEST(buffer_ceiling_locks_to_off_undetermined) {
    // Pairs "12 00": each pair is one stray_data transaction in the `off`
    // pipeline (the idle ends the run); the `on` pipeline discards every D
    // symbol before a COM and buffers nothing. Pair i occupies edges
    // 2 + 2i and 3 + 2i; the 4096th pair (i = 4095) spans edges 8192..8193.
    static const tsym pair[2] = {DS(0x12), IDLE};
    abi_drv d;
    open8(&d, t, "ferrite.pcie_pipe_w8");
    for (unsigned i = 0; i < 4095u; i++) {
        ABI_STREAM(&d, pair);
    }
    WCX_CHECK_EQ_U64(d.log.n, 0);
    ABI_STREAM(&d, pair); // the 4096th buffered transaction: lock
    WCX_REQUIRE(d.log.n == PCIE_AUTO_LOCK_TRANSACTIONS + 1u);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Data outside a packet");
    WCX_CHECK_EQ_U64(d.log.items[0].start, EDGE(2));
    WCX_CHECK_EQ_U64(d.log.items[0].end, EDGE(3));
    WCX_CHECK_STR_EQ(d.log.items[4095].label, "Data outside a packet");
    WCX_CHECK_EQ_U64(d.log.items[4095].start, EDGE(8192));
    WCX_CHECK_STR_EQ(d.log.items[4096].label, "Scrambling: off (undetermined)");
    WCX_CHECK_STR_EQ(d.log.items[4096].fields,
                     "{\"type\":\"scrambling_detected\",\"mode\":\"off\"}");
    WCX_CHECK_EQ_U64(d.log.items[4096].start, EDGE(8192)); // the last buffered one
    WCX_CHECK_EQ_U64(d.log.items[4096].end, EDGE(8193));
    ABI_STREAM(&d, pair); // locked: direct
    WCX_REQUIRE(d.log.n == 4098);
    WCX_CHECK_STR_EQ(d.log.items[4097].label, "Data outside a packet");
    abi_flush(&d);
    WCX_CHECK_EQ_U64(d.log.n, 4098);
    abi_close(&d);
}

WCX_TEST(a_break_or_an_edge_transaction_can_fill_the_buffer) {
    static const tsym pair[2] = {DS(0x12), IDLE};
    static const tsym stp[1] = {KS(0xFB)};
    abi_drv d;
    // 4095 stray pairs (edges 2..8191), then an STP at edge 8192 and a
    // gated edge: the truncation pushed at the break is the 4096th buffered
    // transaction, and the notice spans it (the STP alone, §10.2).
    const abi_cfg with_rxvalid = {1, true, false, false};
    WCX_REQUIRE(abi_open(&d, t, "ferrite.pcie_pipe_w8", &with_rxvalid, NULL, EDGE(1), 4000));
    abi_word w = abi_word_from(k_idle, 1);
    abi_edge(&d, &w);
    for (unsigned i = 0; i < 4095u; i++) {
        ABI_STREAM(&d, pair);
    }
    ABI_STREAM(&d, stp);
    WCX_CHECK_EQ_U64(d.log.n, 0);
    w = abi_word_from(k_idle, 1);
    w.rxvalid = false;
    abi_edge(&d, &w);
    WCX_REQUIRE(d.log.n == PCIE_AUTO_LOCK_TRANSACTIONS + 1u);
    WCX_CHECK_STR_EQ(d.log.items[4095].label, "Packet truncated by rxvalid");
    WCX_CHECK_EQ_U64(d.log.items[4095].start, EDGE(8192));
    WCX_CHECK_EQ_U64(d.log.items[4095].end, EDGE(8193));
    WCX_CHECK_STR_EQ(d.log.items[4096].label, "Scrambling: off (undetermined)");
    WCX_CHECK_EQ_U64(d.log.items[4096].start, EDGE(8192));
    WCX_CHECK_EQ_U64(d.log.items[4096].end, EDGE(8193));
    abi_close(&d);
    // The same with an X/Z edge at 8192: its run closes at the clean edge
    // 8193 and that error is the 4096th transaction.
    open8(&d, t, "ferrite.pcie_pipe_w8");
    for (unsigned i = 0; i < 4095u; i++) {
        ABI_STREAM(&d, pair);
    }
    w = abi_word_from(k_idle, 1);
    w.data_x = 0xFFu;
    abi_edge(&d, &w);
    WCX_CHECK_EQ_U64(d.log.n, 0);
    w.data_x = 0;
    abi_edge(&d, &w);
    WCX_REQUIRE(d.log.n == PCIE_AUTO_LOCK_TRANSACTIONS + 1u);
    WCX_CHECK_STR_EQ(d.log.items[4095].label, "X/Z on data");
    WCX_CHECK_EQ_U64(d.log.items[4095].start, EDGE(8192));
    WCX_CHECK_EQ_U64(d.log.items[4095].end, EDGE(8193));
    WCX_CHECK_STR_EQ(d.log.items[4096].label, "Scrambling: off (undetermined)");
    WCX_CHECK_EQ_U64(d.log.items[4096].start, EDGE(8192));
    abi_close(&d);
}

WCX_TEST(flush_locks_an_undecided_stream) {
    abi_drv d;
    // PIPE: two TS1 (edges 2..33) and nothing else. At flush the run is
    // buffered, then the notice spans it (the last `off` transaction).
    open8(&d, t, "ferrite.pcie_pipe_w8");
    ABI_STREAM(&d, k_ts1);
    ABI_STREAM(&d, k_ts1);
    WCX_CHECK_EQ_U64(d.log.n, 0);
    abi_flush(&d);
    WCX_REQUIRE(d.log.n == 2);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "TS1 " PCIE_TIMES "2 link=PAD lane=PAD n_fts=24 gen1");
    WCX_CHECK_STR_EQ(d.log.items[1].label, "Scrambling: off (undetermined)");
    WCX_CHECK_EQ_U64(d.log.items[1].start, EDGE(2));
    WCX_CHECK_EQ_U64(d.log.items[1].end, EDGE(34));
    abi_close(&d);
    // DLL on the same stream: nothing buffered, so the notice spans the
    // last symbol (edge 33).
    open8(&d, t, "ferrite.pcie_dll_w8");
    ABI_STREAM(&d, k_ts1);
    ABI_STREAM(&d, k_ts1);
    abi_flush(&d);
    WCX_REQUIRE(d.log.n == 1);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Scrambling: off (undetermined)");
    WCX_CHECK_EQ_U64(d.log.items[0].start, EDGE(33));
    WCX_CHECK_EQ_U64(d.log.items[0].end, EDGE(34));
    abi_close(&d);
    // No samples at all: the notice sits at the flush time (0 fs).
    WCX_REQUIRE(abi_open(&d, t, "ferrite.pcie_dll_w8", &k_w8, NULL, EDGE(1), 4000));
    abi_flush(&d);
    WCX_REQUIRE(d.log.n == 1);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Scrambling: off (undetermined)");
    WCX_CHECK_EQ_U64(d.log.items[0].start, 0);
    WCX_CHECK_EQ_U64(d.log.items[0].end, 0);
    abi_close(&d);
    // Open packet at flush in auto mode: truncated "by end of trace" is
    // buffered first (§10.6 step 3), then the notice spans it (step 5).
    static const tsym open[3] = {KS(0xFB), DS(0x00), DS(0x09)};
    open8(&d, t, "ferrite.pcie_dll_w8");
    ABI_STREAM(&d, open); // edges 2..4
    abi_flush(&d);
    WCX_REQUIRE(d.log.n == 2);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Packet truncated by end of trace");
    WCX_CHECK_EQ_U64(d.log.items[0].start, EDGE(2));
    WCX_CHECK_EQ_U64(d.log.items[0].end, EDGE(5));
    WCX_CHECK_STR_EQ(d.log.items[1].label, "Scrambling: off (undetermined)");
    WCX_CHECK_EQ_U64(d.log.items[1].start, EDGE(2));
    WCX_CHECK_EQ_U64(d.log.items[1].end, EDGE(5));
    abi_close(&d);
}

WCX_TEST(forced_modes_emit_no_notice) {
    abi_drv d;
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_pipe_w8", &k_w8, "{\"scrambling\":\"off\"}", EDGE(1), 4000));
    ABI_STREAM(&d, k_initfc);
    abi_flush(&d);
    WCX_REQUIRE(d.log.n == 1);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "DLLP frame (6 symbols)");
    abi_close(&d);
    // `on` before any COM discards data: the DLLP body is empty.
    WCX_REQUIRE(
        abi_open(&d, t, "ferrite.pcie_pipe_w8", &k_w8, "{\"scrambling\":\"on\"}", EDGE(1), 4000));
    ABI_STREAM(&d, k_initfc);
    abi_flush(&d);
    WCX_REQUIRE(d.log.n == 1);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "DLLP frame (0 symbols)");
    abi_close(&d);
}

// Feeds one sample through the ABI with exactly `slots` slots offered and
// no retry; copies what came back into `log`.
static int32_t feed_once(abi_drv *d, uint64_t ts, bool pclk, const abi_word *w, size_t slots,
                         txlog *log) {
    WcTransaction out[16];
    memset(out, 0, sizeof out);
    size_t count = slots;
    const WcSample s = abi_pack(d->buf, &d->cfg, ts, pclk, w);
    const int32_t rc = d->def.feed(d->h, &s, out, &count);
    if (rc == WC_DECODER_OK) {
        txlog_add(log, out, count);
    }
    return rc;
}

WCX_TEST(lock_burst_survives_a_host_that_moves_on_without_retrying) {
    // wcx/emit.h: a host that gets NEED_MORE_SLOTS may move on to the next
    // sample without retrying; the base then keeps the pending batch and
    // appends to it, so the strings of the lock burst must stay valid until
    // that later call has delivered them. (Regression: the detector freed
    // its arena at the start of the very next call.)
    abi_drv d;
    open8(&d, t, "ferrite.pcie_pipe_w8");
    ABI_STREAM(&d, k_skp); // buffered: "SKP" (edges 2..5)
    // InitFC1-P over edges 6..13; feed its first seven words normally and
    // the END word by hand so the lock burst is answered with one slot.
    abi_stream(&d, k_initfc, 7);
    WCX_CHECK_EQ_U64(d.log.n, 0);
    const abi_word end_word = abi_word_from(&k_initfc[7], 1);
    txlog got;
    memset(&got, 0, sizeof got);
    WCX_CHECK_EQ_I64(feed_once(&d, EDGE(13) - 2000u, false, &end_word, 1, &got), WC_DECODER_OK);
    WCX_CHECK_EQ_I64(feed_once(&d, EDGE(13), true, &end_word, 1, &got), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_U64(got.n, 0);
    // The non-conforming host moves on: a different sample (the next
    // falling edge, which decodes nothing), with room for everything.
    static const tsym bad_k[1] = {KS(0x9C)};
    const abi_word next = abi_word_from(bad_k, 1);
    WCX_CHECK_EQ_I64(feed_once(&d, EDGE(14) - 2000u, false, &next, 16, &got), WC_DECODER_OK);
    WCX_REQUIRE(got.n == 3); // the pending lock burst, delivered now
    // Every string must still be readable (ASan: no heap-use-after-free).
    WCX_CHECK_STR_EQ(got.items[0].label, "SKP");
    WCX_CHECK_STR_EQ(got.items[0].fields, "{\"type\":\"skp\",\"skp_symbols\":3}");
    WCX_CHECK_STR_EQ(got.items[1].label, "DLLP frame (6 symbols)");
    WCX_CHECK_STR_EQ(got.items[1].fields,
                     "{\"type\":\"dllp_frame\",\"symbols\":6,\"end\":\"END\"}");
    WCX_CHECK_STR_EQ(got.items[2].label, "Scrambling: off (detected)");
    WCX_CHECK_STR_EQ(got.items[2].fields, "{\"type\":\"scrambling_detected\",\"mode\":\"off\"}");
    // And decoding carries on: the invalid K at edge 14 comes out alone.
    WCX_CHECK_EQ_I64(feed_once(&d, EDGE(14), true, &next, 16, &got), WC_DECODER_OK);
    WCX_REQUIRE(got.n == 4);
    WCX_CHECK_STR_EQ(got.items[3].label, "Invalid K symbol 0x9C");
    txlog_free(&got);
    abi_close(&d);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, lock_burst_survives_a_host_that_moves_on_without_retrying);
    WCX_RUN(&t, off_wins_at_the_first_valid_packet);
    WCX_RUN(&t, edge_level_transactions_keep_their_place_before_the_lock);
    WCX_RUN(&t, on_wins_with_a_scrambled_packet);
    WCX_RUN(&t, training_sets_lock_to_off);
    WCX_RUN(&t, buffer_ceiling_locks_to_off_undetermined);
    WCX_RUN(&t, a_break_or_an_edge_transaction_can_fill_the_buffer);
    WCX_RUN(&t, flush_locks_an_undecided_stream);
    WCX_RUN(&t, forced_modes_emit_no_notice);
    return wcx_test_finish(&t);
}
