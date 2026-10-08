// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The symbol pipeline (SPEC.md §5, §6, §10.2, §10.3, §10.6) as a stream of
// events. Symbol i of a FEED() spans [1000 + 10 i, 1010 + 10 i) fs, so
// every expected span below is read straight off the symbol index.

#include <string.h>

#include "pcie_crc.h"
#include "pcie_test_util.h"

#define T(i)   (1000u + 10u * (i)) // start of symbol i
#define END(i) (1010u + 10u * (i)) // end of symbol i

static void setup(pcie_pipeline *p, evlog *log, bool descramble) {
    memset(log, 0, sizeof *log);
    pcie_pipeline_init(p, descramble, evlog_fn, log);
}

// COM PAD PAD 0x18 0x02 0x00 then ten identifiers (protocol-notes.md §2).
#define TS_BODY(id)                                                                               \
    DS(0x18), DS(0x02), DS(0x00), DS(id), DS(id), DS(id), DS(id), DS(id), DS(id), DS(id), DS(id), \
        DS(id), DS(id)
static const tsym k_ts1[16] = {KS(0xBC), KS(0xF7), KS(0xF7), TS_BODY(0x4A)};

// ── ordered sets ───────────────────────────────────────────────────────────

WCX_TEST(ts1_with_pad_link_and_lane) {
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, k_ts1, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    const pcie_event *e = &log.ev[0];
    WCX_CHECK_EQ_U64(e->kind, PCIE_EV_TS);
    WCX_CHECK(!e->u.ts.ts2);
    WCX_CHECK_EQ_U64(e->start_fs, T(0));
    WCX_CHECK_EQ_U64(e->end_fs, END(15));
    WCX_CHECK_EQ_U64(e->u.ts.kmask, 0x0003u); // symbols 1 and 2 are PAD
    WCX_CHECK_EQ_U64(e->u.ts.syms[0], 0xF7u);
    WCX_CHECK_EQ_U64(e->u.ts.syms[2], 0x18u); // N_FTS
    WCX_CHECK_EQ_U64(e->u.ts.syms[3], 0x02u); // rate id
    WCX_CHECK_EQ_U64(e->u.ts.syms[5], 0x4Au); // identifier
    WCX_CHECK(!p.step_disable_scrambling);
}

WCX_TEST(ts2_with_numbers_and_disable_scrambling) {
    static const tsym ts2[16] = {KS(0xBC), DS(0x03), DS(0x00), DS(0xFF), DS(0x06), DS(0x08),
                                 DS(0x45), DS(0x45), DS(0x45), DS(0x45), DS(0x45), DS(0x45),
                                 DS(0x45), DS(0x45), DS(0x45), DS(0x45)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, ts2, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_TS);
    WCX_CHECK(log.ev[0].u.ts.ts2);
    WCX_CHECK_EQ_U64(log.ev[0].u.ts.kmask, 0);
    WCX_CHECK_EQ_U64(log.ev[0].u.ts.syms[0], 3u);
    // Training Control bit 3 (Disable Scrambling) is reported for the lock.
    WCX_CHECK(p.step_disable_scrambling);
    WCX_CHECK_EQ_U64(p.step_start_fs, T(0));
    WCX_CHECK_EQ_U64(p.step_end_fs, END(15));
}

WCX_TEST(ts_with_bad_identifier_is_unknown_os_over_16_symbols) {
    static const tsym bad[16] = {KS(0xBC), KS(0xF7), KS(0xF7), TS_BODY(0x55)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, bad, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_UNKNOWN_OS);
    WCX_CHECK_EQ_U64(log.ev[0].u.symbol, 0x55u); // symbol 6 did not fit
    WCX_CHECK_EQ_U64(log.ev[0].start_fs, T(0));
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(15));
    // A K symbol at position 6 cannot be an identifier either.
    static const tsym kid[16] = {KS(0xBC), KS(0xF7), KS(0xF7), DS(0x18), DS(0x02), DS(0x00),
                                 KS(0x4A), DS(0x4A), DS(0x4A), DS(0x4A), DS(0x4A), DS(0x4A),
                                 DS(0x4A), DS(0x4A), DS(0x4A), DS(0x4A)};
    setup(&p, &log, false);
    FEED(&p, kid, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_UNKNOWN_OS);
}

WCX_TEST(skp_ordered_sets_of_every_length) {
    // §5.3: 1 to 5 SKPs, stopping at the first non-SKP, which is then an
    // ordinary symbol (here idle).
    for (unsigned n = 1; n <= 5u; n++) {
        tsym syms[8] = {KS(0xBC)};
        for (unsigned i = 0; i < n; i++) {
            syms[1 + i] = (tsym)KS(0x1C);
        }
        syms[1 + n] = (tsym)IDLE;
        pcie_pipeline p;
        evlog log;
        setup(&p, &log, false);
        feed_syms(&p, syms, 2u + n, 1000, 10);
        pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
        WCX_REQUIRE(log.n == 3); // SKP, IDLE_RUN, BREAK
        WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_SKP);
        WCX_CHECK_EQ_U64(log.ev[0].u.count, n);
        WCX_CHECK_EQ_U64(log.ev[0].start_fs, T(0));
        WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(n)); // COM + n SKP
        WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_IDLE_RUN);
        WCX_CHECK_EQ_U64(log.ev[1].u.count, 1);
        WCX_CHECK_EQ_U64(log.ev[1].start_fs, T(1 + n));
    }
}

WCX_TEST(sixth_skp_is_unexpected_k) {
    // §10.2: the SKP OS completes at its fifth SKP; a sixth is a control
    // symbol outside any ordered set.
    static const tsym syms[] = {KS(0xBC), KS(0x1C), KS(0x1C), KS(0x1C),
                                KS(0x1C), KS(0x1C), KS(0x1C)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, syms, 1000, 10);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_SKP);
    WCX_CHECK_EQ_U64(log.ev[0].u.count, 5);
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(5));
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_UNEXPECTED_K);
    WCX_CHECK_EQ_U64(log.ev[1].u.symbol, 0x1Cu);
    WCX_CHECK_EQ_U64(log.ev[1].start_fs, T(6));
    WCX_CHECK_EQ_U64(log.ev[1].end_fs, END(6));
}

WCX_TEST(skp_followed_by_com_starts_the_next_set) {
    static const tsym syms[] = {KS(0xBC), KS(0x1C), KS(0x1C), KS(0xBC),
                                KS(0x3C), KS(0x3C), KS(0x3C)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, syms, 1000, 10);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_SKP);
    WCX_CHECK_EQ_U64(log.ev[0].u.count, 2);
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(2));
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_FTS);
    WCX_CHECK_EQ_U64(log.ev[1].start_fs, T(3));
    WCX_CHECK_EQ_U64(log.ev[1].end_fs, END(6));
}

WCX_TEST(fts_eios_eieos_and_their_malformed_forms) {
    pcie_pipeline p;
    evlog log;
    // COM + 3 IDL = EIOS.
    static const tsym eios[] = {KS(0xBC), KS(0x7C), KS(0x7C), KS(0x7C)};
    setup(&p, &log, false);
    FEED(&p, eios, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_EIOS);
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(3));
    // COM FTS FTS <D 0x12>: the data symbol did not fit, error spans all four.
    static const tsym bad_fts[] = {KS(0xBC), KS(0x3C), KS(0x3C), DS(0x12), IDLE};
    setup(&p, &log, false);
    FEED(&p, bad_fts, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 3);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_UNKNOWN_OS);
    WCX_CHECK_EQ_U64(log.ev[0].u.symbol, 0x12u);
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(3));
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_IDLE_RUN); // decoding resumed at symbol 4
    // COM + 14 EIE + D10.2 = EIEOS.
    tsym eieos[16] = {KS(0xBC)};
    for (unsigned i = 1; i <= 14u; i++) {
        eieos[i] = (tsym)KS(0xFC);
    }
    eieos[15] = (tsym)DS(0x4A);
    setup(&p, &log, false);
    FEED(&p, eieos, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_EIEOS);
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(15));
    // ... with a wrong last symbol, or a non-EIE before 14.
    eieos[15] = (tsym)DS(0x4B);
    setup(&p, &log, false);
    FEED(&p, eieos, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_UNKNOWN_OS);
    WCX_CHECK_EQ_U64(log.ev[0].u.symbol, 0x4Bu);
    static const tsym short_eie[] = {KS(0xBC), KS(0xFC), KS(0xFC), KS(0x3C)};
    setup(&p, &log, false);
    FEED(&p, short_eie, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_UNKNOWN_OS);
    WCX_CHECK_EQ_U64(log.ev[0].u.symbol, 0x3Cu);
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(3));
}

WCX_TEST(com_followed_by_a_misfit_consumes_it) {
    // COM STP: the STP did not fit; decoding resynchronises at the symbol
    // after it, so the bytes that follow are stray data, not a TLP.
    static const tsym syms[] = {KS(0xBC), KS(0xFB), DS(0x12), DS(0x34)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, syms, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 3);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_UNKNOWN_OS);
    WCX_CHECK_EQ_U64(log.ev[0].u.symbol, 0xFBu);
    WCX_CHECK_EQ_U64(log.ev[0].start_fs, T(0));
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(1));
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_BREAK);
    WCX_CHECK_EQ_U64(log.ev[2].kind, PCIE_EV_STRAY_RUN); // §10.6 step 4
    WCX_CHECK_EQ_U64(log.ev[2].u.stray.count, 2);
    WCX_CHECK_EQ_U64(log.ev[2].u.stray.first, 0x12u);
    // COM COM: the second COM is the misfit (and still resets the LFSR,
    // tested below).
    static const tsym twice[] = {KS(0xBC), KS(0xBC), KS(0x9C)};
    setup(&p, &log, false);
    FEED(&p, twice, 1000, 10);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_UNKNOWN_OS);
    WCX_CHECK_EQ_U64(log.ev[0].u.symbol, 0xBCu);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_INVALID_K);
    WCX_CHECK_EQ_U64(log.ev[1].u.symbol, 0x9Cu);
}

// ── packets ────────────────────────────────────────────────────────────────

// protocol-notes.md §6: InitFC1-P 40 08 03 F0 -> CRC 35 BC.
static const tsym k_dllp[8] = {KS(0x5C), DS(0x40), DS(0x08), DS(0x03),
                               DS(0xF0), DS(0x35), DS(0xBC), KS(0xFD)};
// protocol-notes.md §7: seq 0 MRd32 -> LCRC 9A E8 F8 C2.
static const tsym k_tlp[20] = {KS(0xFB), DS(0x00), DS(0x00), DS(0x00), DS(0x00), DS(0x00), DS(0x01),
                               DS(0x01), DS(0x00), DS(0x00), DS(0x0F), DS(0x00), DS(0x00), DS(0x10),
                               DS(0x00), DS(0x9A), DS(0xE8), DS(0xF8), DS(0xC2), KS(0xFD)};

WCX_TEST(dllp_closes_with_crc_valid) {
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, k_dllp, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    const pcie_packet *k = &log.ev[0].u.packet;
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_PACKET);
    WCX_CHECK(!k->tlp);
    WCX_CHECK(!k->edb);
    WCX_CHECK_EQ_U64(k->length, 6);
    WCX_CHECK_EQ_U64(k->head[0], 0x40u);
    WCX_CHECK_EQ_U64(k->head[5], 0xBCu);
    WCX_CHECK(k->crc_valid);
    WCX_CHECK_EQ_U64(log.ev[0].start_fs, T(0)); // SDP
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(7)); // END inclusive (§10.2)
    WCX_CHECK(p.step_valid_packet);
    WCX_CHECK_EQ_U64(p.step_start_fs, T(0));
    WCX_CHECK_EQ_U64(p.step_end_fs, END(7));
}

WCX_TEST(dllp_with_bad_crc_or_edb_is_not_valid) {
    tsym bad[8];
    memcpy(bad, k_dllp, sizeof bad);
    bad[6] = (tsym)DS(0xBD);
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, bad, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK(!log.ev[0].u.packet.crc_valid);
    WCX_CHECK(!p.step_valid_packet);
    memcpy(bad, k_dllp, sizeof bad);
    bad[7] = (tsym)KS(0xFE); // EDB: an error for a DLLP (§10.2), never a lock
    setup(&p, &log, false);
    FEED(&p, bad, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK(log.ev[0].u.packet.edb);
    WCX_CHECK(!log.ev[0].u.packet.crc_valid);
}

WCX_TEST(tlp_closes_with_running_lcrc) {
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, k_tlp, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    const pcie_packet *k = &log.ev[0].u.packet;
    WCX_CHECK(k->tlp);
    WCX_CHECK_EQ_U64(k->length, 18);
    const uint8_t tail[4] = {0x9A, 0xE8, 0xF8, 0xC2};
    WCX_CHECK_MEM_EQ(k->tail, tail, 4);
    // The register covers body[0..14): the LCRC the transmitter sent.
    uint8_t want[4];
    pcie_crc32_wire(k->crc32_before_tail, want);
    WCX_CHECK_MEM_EQ(want, tail, 4);
    WCX_CHECK(k->crc_valid);
    WCX_CHECK_EQ_U64(log.ev[0].start_fs, T(0));
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(19));
}

WCX_TEST(tlp_variants_that_do_not_lock) {
    tsym v[20];
    pcie_pipeline p;
    evlog log;
    // Nullified: inverted LCRC + EDB (protocol-notes.md §7: 65 17 07 3D).
    memcpy(v, k_tlp, sizeof v);
    v[15] = (tsym)DS(0x65);
    v[16] = (tsym)DS(0x17);
    v[17] = (tsym)DS(0x07);
    v[18] = (tsym)DS(0x3D);
    v[19] = (tsym)KS(0xFE);
    setup(&p, &log, false);
    FEED(&p, v, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK(log.ev[0].u.packet.edb);
    WCX_CHECK(!log.ev[0].u.packet.crc_valid);
    // Corrupted LCRC.
    memcpy(v, k_tlp, sizeof v);
    v[18] = (tsym)DS(0xC3);
    setup(&p, &log, false);
    FEED(&p, v, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK(!log.ev[0].u.packet.crc_valid);
    // Body of 17 symbols (one payload byte missing): length invalid.
    tsym shortv[19];
    memcpy(shortv, k_tlp, 14 * sizeof shortv[0]);
    memcpy(shortv + 14, k_tlp + 15, 5 * sizeof shortv[0]);
    setup(&p, &log, false);
    FEED(&p, shortv, 1000, 10);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK_EQ_U64(log.ev[0].u.packet.length, 17);
    WCX_CHECK(!log.ev[0].u.packet.crc_valid);
}

WCX_TEST(back_to_back_packets_and_idle_between) {
    tsym syms[8 + 20 + 2 + 8];
    memcpy(syms, k_dllp, sizeof k_dllp);
    memcpy(syms + 8, k_tlp, sizeof k_tlp);
    syms[28] = (tsym)IDLE;
    syms[29] = (tsym)IDLE;
    memcpy(syms + 30, k_dllp, sizeof k_dllp);
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, syms, 1000, 10);
    WCX_REQUIRE(log.n == 4);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_PACKET);
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(7));
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_PACKET);
    WCX_CHECK_EQ_U64(log.ev[1].start_fs, T(8)); // STP right after END
    WCX_CHECK_EQ_U64(log.ev[1].end_fs, END(27));
    WCX_CHECK_EQ_U64(log.ev[2].kind, PCIE_EV_IDLE_RUN);
    WCX_CHECK_EQ_U64(log.ev[2].u.count, 2);
    WCX_CHECK_EQ_U64(log.ev[2].start_fs, T(28));
    WCX_CHECK_EQ_U64(log.ev[2].end_fs, END(29));
    WCX_CHECK_EQ_U64(log.ev[3].kind, PCIE_EV_PACKET);
    WCX_CHECK_EQ_U64(log.ev[3].start_fs, T(30));
}

// ── errors (§6, §10.2) ─────────────────────────────────────────────────────

WCX_TEST(truncation_by_stp_sdp_and_com) {
    // SDP 40 08 STP ...: the DLLP is truncated after 2 body symbols and the
    // TLP opens at the STP; then COM truncates the TLP after 1 body symbol
    // and starts a SKP OS.
    static const tsym syms[] = {KS(0x5C), DS(0x40), DS(0x08), KS(0xFB), DS(0x00), KS(0xBC),
                                KS(0x1C), KS(0x1C), KS(0x1C), KS(0xFB), KS(0x5C), KS(0xFD)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, syms, 1000, 10);
    WCX_REQUIRE(log.n == 5);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_TRUNCATED);
    WCX_CHECK(!log.ev[0].u.truncated.tlp);
    WCX_CHECK_EQ_U64(log.ev[0].u.truncated.symbols, 2);
    WCX_CHECK_EQ_U64(log.ev[0].u.truncated.by, PCIE_TRUNC_STP);
    WCX_CHECK_EQ_U64(log.ev[0].start_fs, T(0));
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(2)); // last body symbol
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_TRUNCATED);
    WCX_CHECK(log.ev[1].u.truncated.tlp);
    WCX_CHECK_EQ_U64(log.ev[1].u.truncated.symbols, 1);
    WCX_CHECK_EQ_U64(log.ev[1].u.truncated.by, PCIE_TRUNC_COM);
    WCX_CHECK_EQ_U64(log.ev[1].start_fs, T(3));
    WCX_CHECK_EQ_U64(log.ev[1].end_fs, END(4));
    WCX_CHECK_EQ_U64(log.ev[2].kind, PCIE_EV_SKP);
    // STP immediately followed by SDP: no body, the span is the STP alone.
    WCX_CHECK_EQ_U64(log.ev[3].kind, PCIE_EV_TRUNCATED);
    WCX_CHECK_EQ_U64(log.ev[3].u.truncated.by, PCIE_TRUNC_SDP);
    WCX_CHECK_EQ_U64(log.ev[3].u.truncated.symbols, 0);
    WCX_CHECK_EQ_U64(log.ev[3].start_fs, T(9));
    WCX_CHECK_EQ_U64(log.ev[3].end_fs, END(9));
    // The SDP opened a DLLP that END then closes with an empty body.
    WCX_CHECK_EQ_U64(log.ev[4].kind, PCIE_EV_PACKET);
    WCX_CHECK(!log.ev[4].u.packet.tlp);
    WCX_CHECK_EQ_U64(log.ev[4].u.packet.length, 0);
    WCX_CHECK(!log.ev[4].u.packet.crc_valid);
    WCX_CHECK_EQ_U64(log.ev[4].start_fs, T(10));
    WCX_CHECK_EQ_U64(log.ev[4].end_fs, END(11));
}

WCX_TEST(unmatched_end_and_invalid_k_outside) {
    static const tsym syms[] = {KS(0xFD), KS(0xFE), KS(0x9C), KS(0xDC)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, syms, 1000, 10);
    WCX_REQUIRE(log.n == 4);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_UNMATCHED_END);
    WCX_CHECK_EQ_U64(log.ev[0].u.symbol, 0xFDu);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_UNMATCHED_END);
    WCX_CHECK_EQ_U64(log.ev[1].u.symbol, 0xFEu);
    WCX_CHECK_EQ_U64(log.ev[1].start_fs, T(1));
    WCX_CHECK_EQ_U64(log.ev[1].end_fs, END(1));
    WCX_CHECK_EQ_U64(log.ev[2].kind, PCIE_EV_INVALID_K);
    WCX_CHECK_EQ_U64(log.ev[2].u.symbol, 0x9Cu);
    WCX_CHECK_EQ_U64(log.ev[3].kind, PCIE_EV_INVALID_K);
    WCX_CHECK_EQ_U64(log.ev[3].u.symbol, 0xDCu);
}

WCX_TEST(unexpected_k_outside_everything) {
    static const tsym syms[] = {KS(0xF7), KS(0x1C), KS(0x3C), KS(0x7C), KS(0xFC)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, syms, 1000, 10);
    WCX_REQUIRE(log.n == 5);
    for (size_t i = 0; i < 5; i++) {
        WCX_CHECK_EQ_U64(log.ev[i].kind, PCIE_EV_UNEXPECTED_K);
        WCX_CHECK_EQ_U64(log.ev[i].u.symbol, syms[i].byte);
        WCX_CHECK_EQ_U64(log.ev[i].start_fs, T(i));
    }
}

WCX_TEST(k_inside_a_packet_drops_it) {
    // §10.2: STP 00 05 SKP 00 05 END -> k_in_packet at the SKP, the packet
    // is dropped, the bytes after it are stray data and END is unmatched.
    static const tsym syms[] = {KS(0xFB), DS(0x00), DS(0x05), KS(0x1C),
                                DS(0x00), DS(0x05), KS(0xFD)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, syms, 1000, 10);
    WCX_REQUIRE(log.n == 4);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_K_IN_PACKET);
    WCX_CHECK_EQ_U64(log.ev[0].u.symbol, 0x1Cu);
    WCX_CHECK_EQ_U64(log.ev[0].start_fs, T(3));
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(3));
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_IDLE_RUN);  // the 0x00 after it
    WCX_CHECK_EQ_U64(log.ev[2].kind, PCIE_EV_STRAY_RUN); // the 0x05
    WCX_CHECK_EQ_U64(log.ev[2].u.stray.first, 0x05u);
    WCX_CHECK_EQ_U64(log.ev[3].kind, PCIE_EV_UNMATCHED_END);
    // An invalid K inside a packet is invalid_k, and drops it too.
    static const tsym inv[] = {KS(0x5C), DS(0x40), KS(0x9C), KS(0xFD)};
    setup(&p, &log, false);
    FEED(&p, inv, 1000, 10);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_INVALID_K);
    WCX_CHECK_EQ_U64(log.ev[0].u.symbol, 0x9Cu);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_UNMATCHED_END);
}

WCX_TEST(stray_data_runs_end_at_idle_or_control) {
    static const tsym syms[] = {DS(0x12), DS(0x34), IDLE, IDLE, DS(0x56), KS(0xBC), KS(0x1C)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, syms, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    // stray(12 34), idle(2), stray(56), then at flush: SKP (step 1), BREAK.
    WCX_REQUIRE(log.n == 5);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_STRAY_RUN);
    WCX_CHECK_EQ_U64(log.ev[0].u.stray.count, 2);
    WCX_CHECK_EQ_U64(log.ev[0].u.stray.first, 0x12u);
    WCX_CHECK_EQ_U64(log.ev[0].start_fs, T(0));
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(1));
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_IDLE_RUN);
    WCX_CHECK_EQ_U64(log.ev[1].u.count, 2);
    WCX_CHECK_EQ_U64(log.ev[2].kind, PCIE_EV_STRAY_RUN);
    WCX_CHECK_EQ_U64(log.ev[2].u.stray.count, 1);
    WCX_CHECK_EQ_U64(log.ev[2].u.stray.first, 0x56u);
    WCX_CHECK_EQ_U64(log.ev[3].kind, PCIE_EV_SKP);
    WCX_CHECK_EQ_U64(log.ev[3].u.count, 1);
    WCX_CHECK_EQ_U64(log.ev[4].kind, PCIE_EV_BREAK);
}

// ── breaks (§4.3, §4.4, §10.6) ─────────────────────────────────────────────

WCX_TEST(gating_truncates_an_open_packet) {
    static const tsym open[] = {KS(0xFB), DS(0x00), DS(0x05), DS(0x40)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, false);
    FEED(&p, open, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_RXVALID);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_TRUNCATED);
    WCX_CHECK_EQ_U64(log.ev[0].u.truncated.by, PCIE_TRUNC_RXVALID);
    WCX_CHECK_EQ_U64(log.ev[0].u.truncated.symbols, 3);
    WCX_CHECK_EQ_U64(log.ev[0].start_fs, T(0));
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(3));
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_BREAK);
    setup(&p, &log, false);
    FEED(&p, open, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_TXELECIDLE);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].u.truncated.by, PCIE_TRUNC_TXELECIDLE);
    // X/Z: dropped silently (§4.4); so is an incomplete ordered set.
    setup(&p, &log, false);
    FEED(&p, open, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_UNKNOWN);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_BREAK);
    static const tsym half_ts[] = {KS(0xBC), KS(0xF7), KS(0xF7), DS(0x18)};
    setup(&p, &log, false);
    FEED(&p, half_ts, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_UNKNOWN);
    WCX_REQUIRE(log.n == 1);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_BREAK);
    // Framing restarts cleanly after a break.
    FEED(&p, k_dllp, 2000, 10);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_PACKET);
    WCX_CHECK(log.ev[1].u.packet.crc_valid);
    // Gating reports an incomplete ordered set, as it reports a packet.
    setup(&p, &log, false);
    FEED(&p, half_ts, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_RXVALID);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_UNKNOWN_OS);
    WCX_CHECK_EQ_U64(log.ev[0].u.symbol, 0x18u);
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(3));
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_BREAK);
    // A SKP ordered set is complete as soon as it has one SKP (§5.3): every
    // kind of break emits it.
    static const tsym skp2[] = {KS(0xBC), KS(0x1C), KS(0x1C)};
    setup(&p, &log, false);
    FEED(&p, skp2, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_UNKNOWN);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_SKP);
    WCX_CHECK_EQ_U64(log.ev[0].u.count, 2);
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(2));
}

WCX_TEST(end_of_trace_order) {
    pcie_pipeline p;
    evlog log;
    // §10.6 step 1: a SKP OS with two SKPs is complete; step 3: nothing open.
    static const tsym skp2[] = {KS(0xBC), KS(0x1C), KS(0x1C)};
    setup(&p, &log, false);
    FEED(&p, skp2, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_SKP);
    WCX_CHECK_EQ_U64(log.ev[0].u.count, 2);
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(2));
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_BREAK);
    // An incomplete ordered set is unknown_os with its last symbol (§10.7).
    static const tsym half_ts[] = {KS(0xBC), KS(0xF7), KS(0xF7), DS(0x18)};
    setup(&p, &log, false);
    FEED(&p, half_ts, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_UNKNOWN_OS);
    WCX_CHECK_EQ_U64(log.ev[0].u.symbol, 0x18u);
    WCX_CHECK_EQ_U64(log.ev[0].start_fs, T(0));
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(3));
    static const tsym lone_com[] = {KS(0xBC)};
    setup(&p, &log, false);
    FEED(&p, lone_com, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_UNKNOWN_OS);
    WCX_CHECK_EQ_U64(log.ev[0].u.symbol, 0xBCu);
    WCX_CHECK_EQ_U64(log.ev[0].end_fs, END(0));
    static const tsym two_fts[] = {KS(0xBC), KS(0x3C), KS(0x3C)};
    setup(&p, &log, false);
    FEED(&p, two_fts, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_UNKNOWN_OS);
    WCX_CHECK_EQ_U64(log.ev[0].u.symbol, 0x3Cu);
    // Step 2 (idle run) before BREAK, step 3 (open packet) after it.
    static const tsym idle_then_pkt[] = {IDLE, IDLE, KS(0xFB), DS(0x00), DS(0x07)};
    setup(&p, &log, false);
    FEED(&p, idle_then_pkt, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 3); // the idle run ended at STP already
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_IDLE_RUN);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_BREAK);
    WCX_CHECK_EQ_U64(log.ev[2].kind, PCIE_EV_TRUNCATED);
    WCX_CHECK_EQ_U64(log.ev[2].u.truncated.by, PCIE_TRUNC_END_OF_TRACE);
    WCX_CHECK_EQ_U64(log.ev[2].u.truncated.symbols, 2);
    WCX_CHECK_EQ_U64(log.ev[2].start_fs, T(2));
    WCX_CHECK_EQ_U64(log.ev[2].end_fs, END(4));
    // An idle run still open at flush comes before BREAK; a stray run after.
    static const tsym idle_open[] = {IDLE, IDLE, IDLE};
    setup(&p, &log, false);
    FEED(&p, idle_open, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_IDLE_RUN);
    WCX_CHECK_EQ_U64(log.ev[0].u.count, 3);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_BREAK);
    static const tsym stray_open[] = {DS(0xAB)};
    setup(&p, &log, false);
    FEED(&p, stray_open, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_BREAK);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_STRAY_RUN);
}

// ── descrambling (§5.2, §10.3) ─────────────────────────────────────────────

// protocol-notes.md §3: keys k[0..] = FF 17 C0 14 B2 E7 02 82 72 6E 28 A6
// BE 6D BF 8D ... for the symbols after COM. A scrambled idle is the key
// itself on the wire.

WCX_TEST(descrambler_discards_data_before_the_first_com) {
    static const tsym syms[] = {DS(0xFF), DS(0x17), KS(0xBC), KS(0x1C), KS(0x1C), KS(0x1C)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, true);
    FEED(&p, syms, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    // No stray data, no idle: the two D symbols produced nothing at all.
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_SKP);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_BREAK);
}

WCX_TEST(descrambled_idles_follow_the_key_sequence) {
    // COM then the 16 wire bytes equal to the keys: all decode to 0x00.
    static const tsym syms[] = {KS(0xBC), DS(0xFF), DS(0x17), DS(0xC0), DS(0x14), DS(0xB2),
                                DS(0xE7), DS(0x02), DS(0x82), DS(0x72), DS(0x6E), DS(0x28),
                                DS(0xA6), DS(0xBE), DS(0x6D), DS(0xBF), DS(0x8D)};
    // ... except that COM + D is a TS: feed a SKP OS first so the idles are
    // outside any ordered set.
    static const tsym skp[] = {KS(0xBC), KS(0x1C), KS(0x1C), KS(0x1C)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, true);
    FEED(&p, skp, 1000, 10);
    feed_syms(&p, syms + 1, 16, 2000, 10); // keys after the (non-advancing) SKPs
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 3);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_SKP);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_IDLE_RUN);
    WCX_CHECK_EQ_U64(log.ev[1].u.count, 16);
    // One wrong byte is stray data of exactly that value.
    static const tsym wrong[] = {KS(0xBC), KS(0x1C), DS(0xFF), DS(0x16)}; // 0x16 ^ 0x17 = 0x01
    setup(&p, &log, true);
    FEED(&p, wrong, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 4);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_IDLE_RUN);
    WCX_CHECK_EQ_U64(log.ev[1].u.count, 1);
    WCX_CHECK_EQ_U64(log.ev[3].kind, PCIE_EV_STRAY_RUN);
    WCX_CHECK_EQ_U64(log.ev[3].u.stray.first, 0x01u);
}

WCX_TEST(every_symbol_but_skp_advances_the_lfsr) {
    pcie_pipeline p;
    evlog log;
    // STP consumes k[0]; the first body byte uses k[1] = 0x17.
    static const tsym stp[] = {KS(0xBC), KS(0x1C), KS(0xFB), DS(0x17), DS(0xC0), KS(0xFD)};
    setup(&p, &log, true);
    FEED(&p, stp, 1000, 10);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_PACKET);
    WCX_CHECK_EQ_U64(log.ev[1].u.packet.head[0], 0x00u);
    WCX_CHECK_EQ_U64(log.ev[1].u.packet.head[1], 0x00u);
    // A K other than SKP after COM (here END, an unmatched end) also uses
    // up a key: PAD outside an OS is unexpected_k and consumes k[0]... so
    // use a SKP OS then an unexpected PAD: COM SKP SKP SKP PAD <wire 0x17>.
    static const tsym pad[] = {KS(0xBC), KS(0x1C), KS(0x1C), KS(0x1C), KS(0xF7), DS(0x17)};
    setup(&p, &log, true);
    FEED(&p, pad, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 4);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_UNEXPECTED_K);
    WCX_CHECK_EQ_U64(log.ev[2].kind, PCIE_EV_IDLE_RUN); // 0x17 ^ k[1] = 0x00
}

WCX_TEST(ordered_set_symbols_bypass_the_scrambler) {
    pcie_pipeline p;
    evlog log;
    // TS1 symbols 1-15 arrive unscrambled; the next symbol uses k[15] = 0x8D.
    tsym syms[17];
    memcpy(syms, k_ts1, sizeof k_ts1);
    syms[16] = (tsym)DS(0x8D);
    setup(&p, &log, true);
    FEED(&p, syms, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 3);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_TS);
    WCX_CHECK_EQ_U64(log.ev[0].u.ts.syms[2], 0x18u); // N_FTS as sent
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_IDLE_RUN);
    // EIEOS: the final D10.2 is also unscrambled (§10.3), and the symbol
    // after the set uses k[15].
    tsym eieos[17] = {KS(0xBC)};
    for (unsigned i = 1; i <= 14u; i++) {
        eieos[i] = (tsym)KS(0xFC);
    }
    eieos[15] = (tsym)DS(0x4A);
    eieos[16] = (tsym)DS(0x8D);
    setup(&p, &log, true);
    FEED(&p, eieos, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 3);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_EIEOS);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_IDLE_RUN);
    // COM resets the LFSR every time: a second COM makes k[0] = 0xFF current.
    static const tsym twice[] = {KS(0xBC), KS(0x1C), KS(0xBC), KS(0x1C), DS(0xFF)};
    setup(&p, &log, true);
    FEED(&p, twice, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 4);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_SKP);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_SKP);
    WCX_CHECK_EQ_U64(log.ev[2].kind, PCIE_EV_IDLE_RUN);
}

WCX_TEST(ts_data_symbols_are_taken_as_sent_when_descrambling) {
    // §10.3: a TS2 whose Link# (symbol 1, right after COM) and Lane# are
    // data symbols, with N_FTS 0xFF: all 15 arrive unscrambled, and the
    // symbol after the set uses k[15] = 0x8D.
    static const tsym ts2[17] = {KS(0xBC), DS(0x03), DS(0x00), DS(0xFF), DS(0x06), DS(0x00),
                                 DS(0x45), DS(0x45), DS(0x45), DS(0x45), DS(0x45), DS(0x45),
                                 DS(0x45), DS(0x45), DS(0x45), DS(0x45), DS(0x8D)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, true);
    FEED(&p, ts2, 1000, 10);
    pcie_pipeline_break(&p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(log.n == 3);
    WCX_CHECK_EQ_U64(log.ev[0].kind, PCIE_EV_TS);
    WCX_CHECK(log.ev[0].u.ts.ts2);
    WCX_CHECK_EQ_U64(log.ev[0].u.ts.syms[0], 0x03u); // not 0x03 ^ 0xFF
    WCX_CHECK_EQ_U64(log.ev[0].u.ts.syms[1], 0x00u);
    WCX_CHECK_EQ_U64(log.ev[0].u.ts.syms[2], 0xFFu);
    WCX_CHECK_EQ_U64(log.ev[0].u.ts.kmask, 0);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_IDLE_RUN);
}

WCX_TEST(scrambled_dllp_decodes_with_descrambling_on) {
    // COM SKP SKP SKP SDP, then 40 08 03 F0 35 BC XORed with k[1..6]
    // (SDP used k[0]): 57 C8 17 42 D2 BE, then END.
    static const tsym syms[] = {KS(0xBC), KS(0x1C), KS(0x1C), KS(0x1C), KS(0x5C), DS(0x57),
                                DS(0xC8), DS(0x17), DS(0x42), DS(0xD2), DS(0xBE), KS(0xFD)};
    pcie_pipeline p;
    evlog log;
    setup(&p, &log, true);
    FEED(&p, syms, 1000, 10);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[1].kind, PCIE_EV_PACKET);
    const uint8_t want[6] = {0x40, 0x08, 0x03, 0xF0, 0x35, 0xBC};
    WCX_CHECK_MEM_EQ(log.ev[1].u.packet.head, want, 6);
    WCX_CHECK(log.ev[1].u.packet.crc_valid);
    // Without descrambling the same bytes are a DLLP with a wrong CRC.
    setup(&p, &log, false);
    FEED(&p, syms, 1000, 10);
    WCX_REQUIRE(log.n == 2);
    WCX_CHECK_EQ_U64(log.ev[1].u.packet.head[0], 0x57u);
    WCX_CHECK(!log.ev[1].u.packet.crc_valid);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, ts1_with_pad_link_and_lane);
    WCX_RUN(&t, ts2_with_numbers_and_disable_scrambling);
    WCX_RUN(&t, ts_with_bad_identifier_is_unknown_os_over_16_symbols);
    WCX_RUN(&t, skp_ordered_sets_of_every_length);
    WCX_RUN(&t, sixth_skp_is_unexpected_k);
    WCX_RUN(&t, skp_followed_by_com_starts_the_next_set);
    WCX_RUN(&t, fts_eios_eieos_and_their_malformed_forms);
    WCX_RUN(&t, com_followed_by_a_misfit_consumes_it);
    WCX_RUN(&t, dllp_closes_with_crc_valid);
    WCX_RUN(&t, dllp_with_bad_crc_or_edb_is_not_valid);
    WCX_RUN(&t, tlp_closes_with_running_lcrc);
    WCX_RUN(&t, tlp_variants_that_do_not_lock);
    WCX_RUN(&t, back_to_back_packets_and_idle_between);
    WCX_RUN(&t, truncation_by_stp_sdp_and_com);
    WCX_RUN(&t, unmatched_end_and_invalid_k_outside);
    WCX_RUN(&t, unexpected_k_outside_everything);
    WCX_RUN(&t, k_inside_a_packet_drops_it);
    WCX_RUN(&t, stray_data_runs_end_at_idle_or_control);
    WCX_RUN(&t, gating_truncates_an_open_packet);
    WCX_RUN(&t, end_of_trace_order);
    WCX_RUN(&t, descrambler_discards_data_before_the_first_com);
    WCX_RUN(&t, descrambled_idles_follow_the_key_sequence);
    WCX_RUN(&t, every_symbol_but_skp_advances_the_lfsr);
    WCX_RUN(&t, ordered_set_symbols_bypass_the_scrambler);
    WCX_RUN(&t, ts_data_symbols_are_taken_as_sent_when_descrambling);
    WCX_RUN(&t, scrambled_dllp_decodes_with_descrambling_on);
    return wcx_test_finish(&t);
}
