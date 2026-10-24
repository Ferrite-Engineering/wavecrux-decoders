// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Multi-lane links through the ABI (SPEC.md §12): the binding rules
// (§12.1), the link decided at the end of a TS2 run or at the first SKP
// (§12.5), lock and loss (§12.6), striping (§12.7), framing on a row
// (§12.10), unaligned data (§12.11), lane mismatch and lane order (§12.9,
// §12.5) and RxStatus per lane (§12.8). Every expected value is worked out by
// hand from those sections, in the comments.

#include <string.h>

#include "pcie_test_util.h"

// ── a multi-lane driver ────────────────────────────────────────────────────

#define ML_MAX_LANES 16u
#define ML_MAX_SYMS  2048u

// Lane streams under construction: one symbol list per physical lane.
typedef struct ml_lanes {
    unsigned bound;
    size_t n[ML_MAX_LANES];
    tsym s[ML_MAX_LANES][ML_MAX_SYMS];
    bool x[ML_MAX_LANES][ML_MAX_SYMS]; // the edge carrying this symbol is X on that lane
} ml_lanes;

// An X/Z symbol position: the edge that holds it reads X on the lane.
static void ml_put_x(ml_lanes *m, unsigned lane) {
    if (m->n[lane] < ML_MAX_SYMS) {
        m->x[lane][m->n[lane]] = true;
        m->s[lane][m->n[lane]++] = (tsym)IDLE;
    }
}

static void ml_put(ml_lanes *m, unsigned lane, tsym s) {
    if (m->n[lane] < ML_MAX_SYMS) {
        m->s[lane][m->n[lane]++] = s;
    }
}

// A TS1/TS2 on every lane: Link# `link` (PAD when < 0) and Lane# lane_no[l]
// (PAD when < 0), N_FTS `nfts[l]`, rate 0x02, training control 0x00.
static void ml_ts(ml_lanes *m, bool ts2, int link, const int *lane_no, const unsigned *nfts) {
    for (unsigned l = 0; l < m->bound; l++) {
        ml_put(m, l, (tsym)KS(PCIE_K_COM));
        ml_put(m, l, link < 0 ? (tsym)KS(PCIE_K_PAD) : (tsym)DS(link));
        ml_put(m, l, lane_no[l] < 0 ? (tsym)KS(PCIE_K_PAD) : (tsym)DS(lane_no[l]));
        ml_put(m, l, (tsym)DS(nfts[l]));
        ml_put(m, l, (tsym)DS(0x02));
        ml_put(m, l, (tsym)DS(0x00));
        for (unsigned i = 0; i < 10u; i++) {
            ml_put(m, l, ts2 ? (tsym)DS(PCIE_D_TS2_ID) : (tsym)DS(PCIE_D_TS1_ID));
        }
    }
}

// A SKP OS on every lane: COM + skp[l] SKP symbols.
static void ml_skp(ml_lanes *m, const unsigned *skp) {
    for (unsigned l = 0; l < m->bound; l++) {
        ml_put(m, l, (tsym)KS(PCIE_K_COM));
        for (unsigned i = 0; i < skp[l]; i++) {
            ml_put(m, l, (tsym)KS(PCIE_K_SKP));
        }
    }
}

// Logical idle (D 0x00, unscrambled) on every lane.
static void ml_idle(ml_lanes *m, unsigned rows) {
    for (unsigned r = 0; r < rows; r++) {
        for (unsigned l = 0; l < m->bound; l++) {
            ml_put(m, l, (tsym)IDLE);
        }
    }
}

// Byte stream symbol i on logical lane i mod w, here physical = logical.
static void ml_stripe(ml_lanes *m, const tsym *syms, size_t n) {
    for (size_t i = 0; i < n; i++) {
        ml_put(m, (unsigned)(i % m->bound), syms[i]);
    }
}

typedef struct ml_drv {
    WcDecoderDef def;
    WcDecoderHandle h;
    unsigned n;     // symbols per clock per lane
    unsigned bound; // lanes bound
    bool rxstatus1; // lane 1's rxstatus is bound
    uint64_t t;
    uint64_t period;
    txlog log;
    uint8_t buf[1024];
} ml_drv;

static bool ml_open(ml_drv *d, wcx_test *t, const char *id, unsigned n, unsigned bound,
                    bool rxstatus1, const char *params) {
    memset(d, 0, sizeof *d);
    d->def = abi_find(t, id);
    d->n = n;
    d->bound = bound;
    d->rxstatus1 = rxstatus1;
    d->t = 10000;
    d->period = UINT64_C(4000) * n;
    char bind[1024] = {0};
    size_t at = 0;
    for (unsigned l = 1; l < bound; l++) {
        WCX_IGNORE(wcx_str_format(bind + at, sizeof bind - at,
                                  ",\"data%u\":\"d%u\",\"datak%u\":\"k%u\"", l, l, l, l));
        at += strlen(bind + at);
    }
    char cfg[2048] = {0};
    WCX_IGNORE(wcx_str_format(cfg, sizeof cfg,
                              "{\"decoder_id\":\"%s\",\"signal_bindings\":{\"pclk\":\"0\","
                              "\"data\":\"1\",\"datak\":\"2\"%s%s},\"parameters\":%s}",
                              id, bind, rxstatus1 ? ",\"rxstatus1\":\"s1\"" : "",
                              params != NULL ? params : "{}"));
    if (d->def.create == NULL) {
        return false;
    }
    d->h = d->def.create(cfg);
    return d->h != NULL;
}

// One edge: the n symbols of every lane at symbol position `at`; lane 1's
// rxstatus is `rxs1`.
static void ml_edge(ml_drv *d, const ml_lanes *m, size_t at, unsigned rxs1) {
    for (int phase = 0; phase < 2; phase++) {
        memset(d->buf, 0, sizeof d->buf);
        uint32_t bit = 0;
        abi_put(d->buf, &bit, phase == 1 ? 1u : 0u, 0, 1);
        for (unsigned l = 0; l < d->bound; l++) {
            uint64_t data = 0;
            uint64_t datak = 0;
            bool x = false;
            for (unsigned j = 0; j < d->n; j++) {
                const tsym s = at + j < m->n[l] ? m->s[l][at + j] : (tsym)IDLE;
                x = x || (at + j < m->n[l] && m->x[l][at + j]);
                data |= (uint64_t)s.byte << (8u * j);
                if (s.k) {
                    datak |= UINT64_C(1) << j;
                }
            }
            abi_put(d->buf, &bit, data, x ? UINT64_MAX : 0u, 8u * d->n);
            abi_put(d->buf, &bit, datak, 0, d->n);
            if (l == 1 && d->rxstatus1) {
                abi_put(d->buf, &bit, rxs1, 0, 3);
            }
        }
        WcSample s;
        memset(&s, 0, sizeof s);
        s.timestamp_fs = phase == 1 ? d->t : d->t - d->period / 2u;
        s.bits_ptr = d->buf;
        s.bit_width = bit;
        (void)abi_call(&d->def, d->h, &s, &d->log);
    }
    d->t += d->period;
}

// Feeds every lane to its end (the longest lane), then flushes.
static void ml_run(ml_drv *d, const ml_lanes *m) {
    size_t len = 0;
    for (unsigned l = 0; l < m->bound; l++) {
        len = m->n[l] > len ? m->n[l] : len;
    }
    for (size_t at = 0; at < len; at += d->n) {
        ml_edge(d, m, at, 0);
    }
    (void)abi_call(&d->def, d->h, NULL, &d->log);
}

static void ml_close(ml_drv *d) {
    if (d->h != NULL) {
        d->def.destroy(d->h);
        d->h = NULL;
    }
    txlog_free(&d->log);
}

static bool has_field(const txlog *l, size_t i, const char *frag) {
    return i < l->n && strstr(l->items[i].fields, frag) != NULL;
}

// The Ack seq=0 DLLP with its CRC-16 in wire order (the generated fixture
// dll_at_edge_w64_100fs carries it, decoded `Ack seq=0`, crc 0xB362).
static const tsym k_ack0[8] = {KS(PCIE_K_SDP), DS(0x00), DS(0x00), DS(0x00),
                               DS(0x00),       DS(0xB3), DS(0x62), KS(PCIE_K_END)};

// ── §12.1: binding ─────────────────────────────────────────────────────────

static void expect_binding_error(wcx_test *t, const char *bindings, const char *message) {
    const WcDecoderDef def = abi_find(t, "ferrite.pcie_pipe_w8");
    WCX_REQUIRE(def.create != NULL && def.destroy != NULL);
    char cfg[512] = {0};
    WCX_IGNORE(wcx_str_format(cfg, sizeof cfg,
                              "{\"decoder_id\":\"ferrite.pcie_pipe_w8\",\"signal_bindings\":{"
                              "\"pclk\":\"0\",\"data\":\"1\",\"datak\":\"2\"%s}}",
                              bindings));
    WcDecoderHandle h = def.create(cfg);
    WCX_REQUIRE(h != NULL);
    txlog log;
    memset(&log, 0, sizeof log);
    (void)abi_call(&def, h, NULL, &log);
    WCX_REQUIRE(log.n == 1u);
    WCX_CHECK(log.items[0].is_error);
    WCX_CHECK(strstr(log.items[0].label, message) != NULL);
    def.destroy(h);
    txlog_free(&log);
}

WCX_TEST(binding_rules_name_the_problem) {
    // A gap: lanes 0, 1 and 3.
    expect_binding_error(t, ",\"data1\":\"a\",\"datak1\":\"b\",\"data3\":\"c\",\"datak3\":\"d\"",
                         "lanes bound: 0, 1, 3 (lane 2 is missing)");
    // Three lanes: not a link width.
    expect_binding_error(t, ",\"data1\":\"a\",\"datak1\":\"b\",\"data2\":\"c\",\"datak2\":\"d\"",
                         "lanes bound: 0, 1, 2 (x3 is not a PCIe link width)");
    // data2 without datak2.
    expect_binding_error(t, ",\"data1\":\"a\",\"datak1\":\"b\",\"data2\":\"c\"",
                         "datak2 must be bound together with data2");
    // rxvalid5 without data5.
    expect_binding_error(t, ",\"data1\":\"a\",\"datak1\":\"b\",\"rxvalid5\":\"c\"",
                         "rxvalid5 is bound but data5 is not");
}

// ── §12.5, §12.6, §12.7: training, lock, striping ──────────────────────────

WCX_TEST(x2_link_locks_at_the_end_of_the_ts2_run_and_stripes) {
    // Two TS2 with Link# 0 and Lane# 0/1 on lanes 0/1, idle, then Ack seq=0
    // striped: SDP 00 00 00 on lane 0, 00 B3 62 END on lane 1... byte i on
    // lane i mod 2. §12.5 rule 1 decides x2 on the last TS2 (the next item
    // on lane 0 is idle); the round is complete, so it locks with offsets
    // 0,0 (§12.6).
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 2;
    const int lane_no[2] = {0, 1};
    const unsigned nfts[2] = {24, 24};
    ml_ts(&m, true, 0, lane_no, nfts);
    ml_ts(&m, true, 0, lane_no, nfts);
    ml_idle(&m, 4);
    ml_stripe(&m, k_ack0, 8);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_dll_w8", 1, 2, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    // The DLL decoder emits no link_width/lanes_aligned (§12.12), only the Ack.
    WCX_CHECK_EQ_U64(d.log.n, 1);
    WCX_CHECK(d.log.n > 0 && strcmp(d.log.items[0].label, "Ack seq=0") == 0);
    ml_close(&d);

    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    const size_t w = txlog_find(&d.log, "Link width x2 (2 bound)");
    const size_t a = txlog_find(&d.log, "Lanes aligned x2 (offsets 0,0)");
    const size_t ts = txlog_find(&d.log, "TS2 " PCIE_TIMES "2 link=0 lanes=0,1 n_fts=24 gen1");
    const size_t f = txlog_find(&d.log, "DLLP frame (6 symbols)");
    WCX_CHECK(w != SIZE_MAX && a != SIZE_MAX && ts != SIZE_MAX && f != SIZE_MAX);
    WCX_CHECK(has_field(&d.log, w, "\"lanes\":\"0,1\",\"from\":\"ts2\""));
    WCX_CHECK(has_field(&d.log, ts, "\"links\":\"0,0\",\"lanes\":\"0,1\""));
    WCX_CHECK(txlog_find(&d.log, "Lane order") == SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(l0_start_gives_one_unaligned_row_then_locks_at_the_first_skp) {
    // Idle on both lanes (6 symbols each), then SKP OS: §12.5 rule 2 decides
    // x2 "from SKP"; the 6 idle symbols of lane 0 (the reference lane while
    // undecided) were withheld: one non-error unaligned row (§12.11).
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 2;
    const unsigned skp[2] = {3, 3};
    ml_idle(&m, 6);
    ml_skp(&m, skp);
    ml_stripe(&m, k_ack0, 8);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    const size_t u = txlog_find(&d.log, "Packet data before lane alignment");
    WCX_REQUIRE(u != SIZE_MAX);
    WCX_CHECK(!d.log.items[u].is_error);
    WCX_CHECK(has_field(&d.log, u, "\"symbols\":6"));
    WCX_CHECK(txlog_find(&d.log, "Link width x2 (2 bound, from SKP)") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Lanes aligned x2 (offsets 0,0)") != SIZE_MAX);
    // §12.9: both lanes carry 3 SKPs, so the label is plain "SKP".
    const size_t s = txlog_find(&d.log, "SKP");
    WCX_CHECK(s != SIZE_MAX && strcmp(d.log.items[s].label, "SKP") == 0);
    WCX_CHECK(has_field(&d.log, s, "\"skp_symbols\":\"3,3\""));
    WCX_CHECK(txlog_find(&d.log, "DLLP frame (6 symbols)") != SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(skp_counts_and_skew_per_lane) {
    // Lane 1 starts 3 symbols late (3 idle in front) and its second SKP OS
    // has 2 SKPs: offsets are 0,3 at COM (§12.6), and the 2-SKP round is
    // labelled "SKP ×3,2" (§12.9). The Ack after it still decodes: SKP counts
    // do not change COM offsets, and rows pair item by item.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 2;
    for (unsigned i = 0; i < 3u; i++) {
        ml_put(&m, 1, (tsym)IDLE);
    }
    const unsigned skp[2] = {3, 3};
    const unsigned skp2[2] = {3, 2};
    ml_skp(&m, skp);
    ml_idle(&m, 4);
    ml_skp(&m, skp2);
    ml_stripe(&m, k_ack0, 8);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_dll_w8", 1, 2, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "Ack seq=0") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Lanes not aligned") == SIZE_MAX);
    ml_close(&d);
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "Lanes aligned x2 (offsets 0,3)") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "SKP " PCIE_TIMES "3,2") != SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(a_missing_ordered_set_loses_alignment_and_the_next_round_relocks) {
    // Locked at the first SKP; then lane 1 sends idle where lane 0 sends a
    // SKP OS. Pairing rows after the first round, lane 0 is at its SKP (no
    // k-th item) while lane 1 still has idle: §12.6 loss. Two lanes tie, so
    // the norm is logical lane 0's kind (none) and lane 1 is named: `Lanes
    // not aligned (lane 1)`. Lane 0's later data is withheld as an error
    // row (§12.11), and the third SKP (on both lanes) locks again.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 2;
    const unsigned skp[2] = {3, 3};
    ml_skp(&m, skp);
    ml_idle(&m, 4);
    ml_put(&m, 0, (tsym)KS(PCIE_K_COM));
    for (unsigned i = 0; i < 3u; i++) {
        ml_put(&m, 0, (tsym)KS(PCIE_K_SKP));
    }
    for (unsigned i = 0; i < 4u; i++) {
        ml_put(&m, 1, (tsym)IDLE);
    }
    ml_idle(&m, 20);
    ml_skp(&m, skp);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    const size_t loss = txlog_find(&d.log, "Lanes not aligned (lane 1)");
    WCX_REQUIRE(loss != SIZE_MAX);
    WCX_CHECK(d.log.items[loss].is_error);
    const size_t after = txlog_find(&d.log, "Packet data after lane alignment lost");
    WCX_REQUIRE(after != SIZE_MAX);
    WCX_CHECK(d.log.items[after].is_error);
    size_t locks = 0;
    for (size_t i = 0; i < d.log.n; i++) {
        locks += strncmp(d.log.items[i].label, "Lanes aligned", 13) == 0 ? 1u : 0u;
    }
    WCX_CHECK_EQ_U64(locks, 2);
    ml_close(&d);
}

// ── §12.10: framing on a row ───────────────────────────────────────────────

WCX_TEST(x4_start_lane_and_misplaced_pad) {
    // x4 after a SKP lock. Row 1: idle, idle, SDP, 00 (an SDP on lane 2 is
    // `SDP on lane 2`, framed anyway). Then a row with PAD on lane 1 and no
    // END before it: `PAD outside a packet (lane 1)`.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 4;
    const unsigned skp[4] = {3, 3, 3, 3};
    ml_skp(&m, skp);
    ml_idle(&m, 2);
    static const tsym row1[4] = {IDLE, IDLE, KS(PCIE_K_SDP), DS(0x00)};
    static const tsym row2[4] = {DS(0x00), DS(0x00), DS(0x00), DS(0xB3)};
    static const tsym row3[4] = {DS(0x62), KS(PCIE_K_END), IDLE, IDLE};
    static const tsym row4[4] = {IDLE, KS(PCIE_K_PAD), IDLE, IDLE};
    ml_stripe(&m, row1, 4);
    ml_stripe(&m, row2, 4);
    ml_stripe(&m, row3, 4);
    ml_stripe(&m, row4, 4);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 4, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    const size_t fl = txlog_find(&d.log, "SDP on lane 2");
    WCX_REQUIRE(fl != SIZE_MAX);
    WCX_CHECK(d.log.items[fl].is_error);
    WCX_CHECK(has_field(&d.log, fl, "\"type\":\"framing_lane\",\"symbol\":\"0x5C\",\"lane\":2"));
    WCX_CHECK(txlog_find(&d.log, "DLLP frame (6 symbols)") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "PAD outside a packet (lane 1)") != SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(x8_pad_fill_after_end_is_not_an_error) {
    // x8 after a SKP lock: a 12-symbol TLP frame (STP + 10 body + END) ends
    // on lane 3 of its second row; lanes 4-7 carry PAD fill (§12.10).
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 8;
    const unsigned skp[8] = {3, 3, 3, 3, 3, 3, 3, 3};
    ml_skp(&m, skp);
    ml_idle(&m, 2);
    tsym tlp[16];
    tlp[0] = (tsym)KS(PCIE_K_STP);
    for (unsigned i = 1; i < 11u; i++) {
        tlp[i] = (tsym)DS(i);
    }
    tlp[11] = (tsym)KS(PCIE_K_END);
    for (unsigned i = 12; i < 16u; i++) {
        tlp[i] = (tsym)KS(PCIE_K_PAD);
    }
    ml_stripe(&m, tlp, 16);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 8, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "TLP frame seq=") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "PAD outside a packet") == SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Unexpected K symbol") == SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "STP on lane") == SIZE_MAX);
    ml_close(&d);
}

// ── §12.9, §12.5: mismatch and order; §12.8: RxStatus ──────────────────────

WCX_TEST(different_n_fts_is_a_mismatch_once_and_reversal_is_an_order) {
    // Two TS1 with N_FTS 24 on lane 0 and 20 on lane 1: one lane_mismatch,
    // not repeated for the second round (§12.9). Then TS2 ×2 with Lane# 1 on
    // lane 0 and 0 on lane 1: the link is x2 in order 1,0 (§12.5).
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 2;
    const int pad[2] = {-1, -1};
    const int rev[2] = {1, 0};
    const unsigned nfts_bad[2] = {24, 20};
    const unsigned nfts[2] = {24, 24};
    ml_ts(&m, false, -1, pad, nfts_bad);
    ml_ts(&m, false, -1, pad, nfts_bad);
    ml_ts(&m, true, 0, rev, nfts);
    ml_ts(&m, true, 0, rev, nfts);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    size_t mm = 0;
    for (size_t i = 0; i < d.log.n; i++) {
        mm += strncmp(d.log.items[i].label, "Lane 1 differs: n_fts=20, lane 0 n_fts=24", 41) == 0
                  ? 1u
                  : 0u;
    }
    WCX_CHECK_EQ_U64(mm, 1);
    WCX_CHECK(txlog_find(&d.log, "Lane order 1,0") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "TS2 " PCIE_TIMES "2 link=0 lanes=1,0") != SIZE_MAX);
    ml_close(&d);

    // lane_order=physical: no reordering, no lane_order transaction.
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false,
                        "{\"scrambling\":\"off\",\"lane_order\":\"physical\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "Lane order") == SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(rxstatus_names_its_lane) {
    // Lane 1's rxstatus goes 000 -> 001 at the third edge: one
    // `RxStatus: SKP added (lane 1)` with `lane` after `code` (§12.8).
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 2;
    ml_idle(&m, 4);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, true, "{\"scrambling\":\"off\"}"));
    for (size_t at = 0; at < 4u; at++) {
        ml_edge(&d, &m, at, at >= 2u ? 1u : 0u);
    }
    (void)abi_call(&d.def, d.h, NULL, &d.log);
    const size_t r = txlog_find(&d.log, "RxStatus: SKP added (lane 1)");
    WCX_REQUIRE(r != SIZE_MAX);
    WCX_CHECK(has_field(&d.log, r, "\"code\":\"0b001\",\"lane\":1"));
    ml_close(&d);
}

// ── more ordered sets, cuts and narrower links ─────────────────────────────

static void ml_os_all(ml_lanes *m, const tsym *os, size_t n) {
    for (unsigned l = 0; l < m->bound; l++) {
        for (size_t i = 0; i < n; i++) {
            ml_put(m, l, os[i]);
        }
    }
}

WCX_TEST(fts_eios_eieos_and_unknown_ordered_sets_on_every_lane) {
    // After a SKP lock on x2: two FTS OS (COM + 3 FTS) coalesce into
    // `FTS ×2` (§7), then EIOS, an EIEOS (COM + 14 EIE + D10.2), and COM
    // followed by STP, which fits no ordered set: `Unknown ordered set`
    // (§5.3, §12.3). All four are rounds of the same type on both lanes, so
    // no lane_mismatch.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 2;
    const unsigned skp[2] = {3, 3};
    ml_skp(&m, skp);
    ml_idle(&m, 2);
    static const tsym fts[4] = {KS(PCIE_K_COM), KS(PCIE_K_FTS), KS(PCIE_K_FTS), KS(PCIE_K_FTS)};
    static const tsym eios[4] = {KS(PCIE_K_COM), KS(PCIE_K_IDL), KS(PCIE_K_IDL), KS(PCIE_K_IDL)};
    static const tsym bad[2] = {KS(PCIE_K_COM), KS(PCIE_K_STP)};
    tsym eieos[16];
    eieos[0] = (tsym)KS(PCIE_K_COM);
    for (unsigned i = 1; i < 15u; i++) {
        eieos[i] = (tsym)KS(PCIE_K_EIE);
    }
    eieos[15] = (tsym)DS(PCIE_D_TS1_ID);
    ml_os_all(&m, fts, 4);
    ml_os_all(&m, fts, 4);
    ml_os_all(&m, eios, 4);
    ml_os_all(&m, eieos, 16);
    ml_os_all(&m, bad, 2);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "FTS " PCIE_TIMES "2") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "EIOS") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "EIEOS") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Unknown ordered set") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Lane 1 differs") == SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Lanes not aligned") == SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(misfits_inside_fts_eios_and_eieos_are_unknown_on_the_lane) {
    // COM FTS D, COM IDL D and COM EIE D on both lanes: each misfit ends an
    // `unknown` ordered set on its lane (§12.3), one round each, and the
    // pipeline reports three `Unknown ordered set`.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 2;
    const unsigned skp[2] = {3, 3};
    ml_skp(&m, skp);
    ml_idle(&m, 2);
    static const tsym f[3] = {KS(PCIE_K_COM), KS(PCIE_K_FTS), DS(0x11)};
    static const tsym i[3] = {KS(PCIE_K_COM), KS(PCIE_K_IDL), DS(0x22)};
    static const tsym e[3] = {KS(PCIE_K_COM), KS(PCIE_K_EIE), DS(0x33)};
    tsym e_end[16];
    e_end[0] = (tsym)KS(PCIE_K_COM);
    for (unsigned j = 1; j < 15u; j++) {
        e_end[j] = (tsym)KS(PCIE_K_EIE);
    }
    e_end[15] = (tsym)DS(0x44); // not D10.2
    ml_os_all(&m, f, 3);
    ml_os_all(&m, i, 3);
    ml_os_all(&m, e, 3);
    ml_os_all(&m, e_end, 16);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    size_t unknown = 0;
    for (size_t k = 0; k < d.log.n; k++) {
        unknown += strcmp(d.log.items[k].label, "Unknown ordered set") == 0 ? 1u : 0u;
    }
    WCX_CHECK_EQ_U64(unknown, 4);
    ml_close(&d);
}

WCX_TEST(xz_on_every_lane_cuts_an_ordered_set_then_an_incomplete_round_is_a_skew) {
    // Locked at a SKP. A TS1 starts on both lanes and both go X after 4
    // symbols: the cut TS is `unknown` on each lane (§12.3) and dropped by the
    // pipeline as on x1 (§10.8 item 1); the X edges on lane 0 are one
    // `X/Z on data` with `lane` 0 (§12.8), and the common gap unlocks
    // without an error (§12.6). The next SKP comes on lane 0 only: an
    // incomplete round while not aligned, with no lane_skew since the lock:
    // `Lanes not aligned (lane 1)`.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 2;
    const unsigned skp[2] = {3, 3};
    ml_skp(&m, skp);
    ml_idle(&m, 2);
    static const tsym ts_head[4] = {KS(PCIE_K_COM), KS(PCIE_K_PAD), KS(PCIE_K_PAD), DS(24)};
    ml_os_all(&m, ts_head, 4);
    for (unsigned k = 0; k < 4u; k++) {
        ml_put_x(&m, 0);
        ml_put_x(&m, 1);
    }
    ml_idle(&m, 4);
    ml_put(&m, 0, (tsym)KS(PCIE_K_COM));
    for (unsigned k = 0; k < 3u; k++) {
        ml_put(&m, 0, (tsym)KS(PCIE_K_SKP));
    }
    for (unsigned k = 0; k < 4u; k++) {
        ml_put(&m, 1, (tsym)IDLE);
    }
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    const size_t xz = txlog_find(&d.log, "X/Z on data");
    WCX_REQUIRE(xz != SIZE_MAX);
    WCX_CHECK(has_field(&d.log, xz, "\"edges\":4,\"lane\":0"));
    WCX_CHECK(txlog_find(&d.log, "Lanes not aligned (lane 1)") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "TS1") == SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(a_lone_lane_0_at_a_loss_is_named_by_the_norm) {
    // x4, locked at a SKP. Lane 0 then sends a SKP OS while lanes 1-3 send
    // idle: at the row, lane 0 has no item and three lanes have a row
    // symbol, so the norm is a row symbol and lane 0 is named (§12.6).
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 4;
    const unsigned skp[4] = {3, 3, 3, 3};
    ml_skp(&m, skp);
    ml_idle(&m, 2);
    ml_put(&m, 0, (tsym)KS(PCIE_K_COM));
    for (unsigned k = 0; k < 3u; k++) {
        ml_put(&m, 0, (tsym)KS(PCIE_K_SKP));
    }
    for (unsigned l = 1; l < 4u; l++) {
        for (unsigned k = 0; k < 4u; k++) {
            ml_put(&m, l, (tsym)IDLE);
        }
    }
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 4, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "Lanes not aligned (lane 0)") != SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(duplicate_lane_numbers_keep_physical_order_and_unused_lanes_are_ignored) {
    // x4 bound: TS2 with Link# 0 and Lane# 0, 0 on lanes 0-1, PAD on lanes
    // 2-3. The link is lanes 0-1 (§12.5 rule 1); their Lane#s are not
    // exactly 0..1, so the order is physical: no lane_order. Lanes 2-3 keep
    // sending idle and X afterwards and are ignored: no lane_skew, no X/Z.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 4;
    const int lane_no[4] = {0, 0, -1, -1};
    const unsigned nfts[4] = {24, 24, 24, 24};
    ml_ts(&m, true, 0, lane_no, nfts);
    ml_ts(&m, true, 0, lane_no, nfts);
    for (unsigned r = 0; r < 4u; r++) {
        ml_put(&m, 0, (tsym)IDLE);
        ml_put(&m, 1, (tsym)IDLE);
        ml_put_x(&m, 2);
        ml_put(&m, 3, (tsym)IDLE);
    }
    for (size_t i = 0; i < 8u; i++) {
        ml_put(&m, (unsigned)(i % 2u), k_ack0[i]);
    }
    for (unsigned r = 0; r < 30u; r++) {
        ml_put(&m, 0, (tsym)IDLE);
        ml_put(&m, 1, (tsym)IDLE);
    }
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 4, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "Link width x2 (4 bound)") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Lane order") == SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Lanes not aligned") == SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "X/Z on data") == SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "DLLP frame (6 symbols)") != SIZE_MAX);
    ml_close(&d);
}

// ── after the QA review ────────────────────────────────────────────────────

static size_t count_prefix(const txlog *l, const char *prefix) {
    size_t n = 0;
    for (size_t i = 0; i < l->n; i++) {
        n += strncmp(l->items[i].label, prefix, strlen(prefix)) == 0 ? 1u : 0u;
    }
    return n;
}

WCX_TEST(x16_with_max_skew_64_releases_a_burst_without_running_out_of_strings) {
    // x16, max_skew 64: after a SKP lock, 120 rows of STP on every lane are
    // held while each round settles (§12.4) and released together. Every
    // STP truncates the packet the previous one opened (§6); the decoder
    // must not stop for lack of string space.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 16;
    unsigned skp[16];
    for (unsigned l = 0; l < 16u; l++) {
        skp[l] = 3;
    }
    for (unsigned rep_i = 0; rep_i < 3u; rep_i++) {
        ml_skp(&m, skp);
        for (unsigned r = 0; r < 120u; r++) {
            for (unsigned l = 0; l < 16u; l++) {
                ml_put(&m, l, (tsym)KS(PCIE_K_STP));
            }
        }
    }
    ml_idle(&m, 100);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 16, false,
                        "{\"scrambling\":\"off\",\"max_skew\":64}"));
    ml_run(&d, &m);
    WCX_CHECK(count_prefix(&d.log, "the decoder ran out") == 0);
    WCX_CHECK(count_prefix(&d.log, "Packet truncated by STP") > 2000u);
    ml_close(&d);
}

WCX_TEST(a_partial_first_skp_widens_at_the_next_skp) {
    // A capture starting inside lane 3's first SKP: lane 3 has its SKPs but
    // not the COM. The first SKP round has lanes 0-2: x3 is cut to x2 with
    // `Lanes not aligned (lane 2)` (§12.5 width rule). The next SKP is on all
    // four lanes, a wider link: `Link width x4 (4 bound, from SKP)`.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 4;
    for (unsigned l = 0; l < 3u; l++) {
        ml_put(&m, l, (tsym)KS(PCIE_K_COM));
    }
    ml_put(&m, 3, (tsym)IDLE);
    for (unsigned k = 0; k < 3u; k++) {
        for (unsigned l = 0; l < 4u; l++) {
            ml_put(&m, l, (tsym)KS(PCIE_K_SKP));
        }
    }
    ml_idle(&m, 20);
    const unsigned skp[4] = {3, 3, 3, 3};
    ml_skp(&m, skp);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 4, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "Link width x2 (4 bound, from SKP)") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Lanes not aligned (lane 2)") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Link width x4 (4 bound, from SKP)") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Lanes aligned x4") != SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(a_loss_mid_packet_drops_the_packet_and_names_the_gap) {
    // x4, locked at a SKP. STP starts a packet on lane 0; one row later lane
    // 2 goes X for one edge while lanes 0, 1, 3 carry data. The norm is a row
    // symbol, so lane 2 is named, spanning its one-edge gap (4000 fs at
    // N = 1). The loss is a break of kind unknown (§12.6): the open packet
    // is dropped with no `Packet truncated`.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 4;
    const unsigned skp[4] = {3, 3, 3, 3};
    ml_skp(&m, skp);
    ml_idle(&m, 2);
    static const tsym row1[4] = {KS(PCIE_K_STP), DS(0x00), DS(0x01), DS(0x02)};
    ml_stripe(&m, row1, 4);
    ml_put(&m, 0, (tsym)DS(0x03));
    ml_put(&m, 1, (tsym)DS(0x04));
    ml_put_x(&m, 2);
    ml_put(&m, 3, (tsym)DS(0x05));
    ml_idle(&m, 10);
    ml_skp(&m, skp);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 4, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    const size_t sk = txlog_find(&d.log, "Lanes not aligned (lane 2)");
    WCX_REQUIRE(sk != SIZE_MAX);
    WCX_CHECK_EQ_U64(d.log.items[sk].end - d.log.items[sk].start, 4000);
    WCX_CHECK(count_prefix(&d.log, "Packet truncated") == 0);
    WCX_CHECK(count_prefix(&d.log, "Lanes aligned x4") == 2);
    ml_close(&d);
}

WCX_TEST(the_span_of_a_loss_is_a_norm_item_when_the_named_lane_has_none) {
    // x4 locked. At the row, lane 0 is at a SKP OS (no item), lanes 1 and 2
    // carry data, lane 3 carries data too: the norm is a row symbol, lane 0
    // is named, and the span is lane 1's symbol (one symbol, 4000 fs).
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 4;
    const unsigned skp[4] = {3, 3, 3, 3};
    ml_skp(&m, skp);
    ml_idle(&m, 2);
    ml_put(&m, 0, (tsym)KS(PCIE_K_COM));
    for (unsigned k = 0; k < 3u; k++) {
        ml_put(&m, 0, (tsym)KS(PCIE_K_SKP));
    }
    for (unsigned l = 1; l < 4u; l++) {
        for (unsigned k = 0; k < 4u; k++) {
            ml_put(&m, l, (tsym)IDLE);
        }
    }
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 4, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    const size_t sk = txlog_find(&d.log, "Lanes not aligned (lane 0)");
    WCX_REQUIRE(sk != SIZE_MAX);
    WCX_CHECK_EQ_U64(d.log.items[sk].end - d.log.items[sk].start, 4000);
    ml_close(&d);
}

WCX_TEST(ts_coalescing_ignores_what_other_lanes_sent_instead) {
    // x4 undecided: lanes 0-2 send TS1 twice; lane 3 sends a SKP OS in the
    // first round and an FTS OS in the second. The links/lanes lists are the
    // same both times (lane 3 shows `-`), so the TS1 coalesce (§12.9); lane
    // 3 differs both times, with two different differences.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 4;
    for (unsigned r = 0; r < 2u; r++) {
        for (unsigned l = 0; l < 3u; l++) {
            static const tsym ts1[16] = {KS(PCIE_K_COM), KS(PCIE_K_PAD), KS(PCIE_K_PAD), DS(24),
                                         DS(0x02),       DS(0x00),       DS(0x4A),       DS(0x4A),
                                         DS(0x4A),       DS(0x4A),       DS(0x4A),       DS(0x4A),
                                         DS(0x4A),       DS(0x4A),       DS(0x4A),       DS(0x4A)};
            for (unsigned i = 0; i < 16u; i++) {
                ml_put(&m, l, ts1[i]);
            }
        }
        ml_put(&m, 3, (tsym)KS(PCIE_K_COM));
        for (unsigned i = 0; i < 3u; i++) {
            ml_put(&m, 3, r == 0 ? (tsym)KS(PCIE_K_SKP) : (tsym)KS(PCIE_K_FTS));
        }
        for (unsigned i = 0; i < 12u; i++) {
            ml_put(&m, 3, (tsym)IDLE);
        }
    }
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 4, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "TS1 " PCIE_TIMES "2 link=PAD lanes=PAD,PAD,PAD,-") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Lane 3 differs: SKP, lane 0 TS1") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Lane 3 differs: FTS, lane 0 TS1") != SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(the_round_that_shrinks_the_link_still_compares_the_lanes_it_drops) {
    // x4: TS2 ×2 with numbers on lanes 0-1 and PAD on lanes 2-3; only the
    // last TS2 on lanes 2-3 has N_FTS 99. That round decides x2, but the
    // comparison uses the lanes considered before it (§12.9): lanes 2 and 3
    // differ.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 4;
    const int lane_no[4] = {0, 1, -1, -1};
    const unsigned nfts[4] = {24, 24, 24, 24};
    const unsigned nfts99[4] = {24, 24, 99, 99};
    ml_ts(&m, true, 0, lane_no, nfts);
    ml_ts(&m, true, 0, lane_no, nfts99);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 4, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "Lane 2 differs: n_fts=99, lane 0 n_fts=24") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Lane 3 differs: n_fts=99, lane 0 n_fts=24") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Link width x2 (4 bound)") != SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(x12_stripes_and_pads_after_a_dllp) {
    // x12 locked at a SKP: one row holds the 8 symbols of Ack seq=0 on lanes
    // 0-7 and PAD fill on lanes 8-11 (§12.10).
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 12;
    unsigned skp[12];
    for (unsigned l = 0; l < 12u; l++) {
        skp[l] = 3;
    }
    ml_skp(&m, skp);
    ml_idle(&m, 2);
    tsym row[12];
    for (unsigned i = 0; i < 12u; i++) {
        row[i] = i < 8u ? k_ack0[i] : (tsym)KS(PCIE_K_PAD);
    }
    ml_stripe(&m, row, 12);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_dll_w8", 1, 12, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "Ack seq=0") != SIZE_MAX);
    WCX_CHECK(count_prefix(&d.log, "PAD outside") == 0);
    ml_close(&d);
}

WCX_TEST(lane_order_returns_to_physical) {
    // x2: TS2 run with Lane# 1,0 (order 1,0), idle, then Recovery TS2 with
    // Lane# 0,1: the order is physical again, so a second lane_order (§12.5).
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 2;
    const int rev[2] = {1, 0};
    const int fwd[2] = {0, 1};
    const unsigned nfts[2] = {24, 24};
    ml_ts(&m, true, 0, rev, nfts);
    ml_ts(&m, true, 0, rev, nfts);
    ml_idle(&m, 8);
    ml_ts(&m, true, 0, fwd, nfts);
    ml_ts(&m, true, 0, fwd, nfts);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "Lane order 1,0") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Lane order 0,1") != SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(max_skew_0_and_64) {
    // Lane 1 one symbol late. With max_skew 0 the SKPs never share a round:
    // the first SKP round has lane 0 only, an x1 link, and lane 1's own round
    // is not wider (§12.5 rule 2). With max_skew 64, a 20-symbol skew locks
    // with offsets 0,20.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 2;
    ml_put(&m, 1, (tsym)IDLE);
    const unsigned skp[2] = {3, 3};
    ml_skp(&m, skp);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false,
                        "{\"scrambling\":\"off\",\"max_skew\":0}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "Link width x1 (2 bound, from SKP)") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Link width x2") == SIZE_MAX);
    ml_close(&d);

    static ml_lanes m2;
    memset(&m2, 0, sizeof m2);
    m2.bound = 2;
    for (unsigned k = 0; k < 20u; k++) {
        ml_put(&m2, 1, (tsym)IDLE);
    }
    ml_skp(&m2, skp);
    ml_idle(&m2, 120);
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false,
                        "{\"scrambling\":\"off\",\"max_skew\":64}"));
    ml_run(&d, &m2);
    WCX_CHECK(txlog_find(&d.log, "Lanes aligned x2 (offsets 0,20)") != SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(width_cut_and_an_incomplete_row_at_the_end) {
    // x4: TS2 with numbers on lanes 0-2 and PAD on lane 3: x3 is cut to x2,
    // `Lanes not aligned (lane 2)` (§12.5). The trace then ends with lane 0
    // one symbol longer than lane 1: that row is dropped without an error
    // (§12.13).
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 4;
    const int lane_no[4] = {0, 1, 2, -1};
    const unsigned nfts[4] = {24, 24, 24, 24};
    ml_ts(&m, true, 0, lane_no, nfts);
    ml_ts(&m, true, 0, lane_no, nfts);
    ml_idle(&m, 30);
    ml_put(&m, 0, (tsym)IDLE);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 4, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "Link width x2 (4 bound)") != SIZE_MAX);
    WCX_CHECK_EQ_U64(count_prefix(&d.log, "Lanes not aligned"), 1);
    WCX_CHECK(txlog_find(&d.log, "Lanes not aligned (lane 2)") != SIZE_MAX);
    ml_close(&d);
}

WCX_TEST(a_late_reference_lane_still_joins_the_round) {
    // L0 start on x2 with lane 0 two symbols late: lane 1's SKP opens the
    // round at c, lane 0's SKP is at c + 2, behind two idle symbols with
    // indexes c and c + 1. Those are withheld before the round (§12.7), so
    // lane 0 is present: x2 from SKP, offsets 2,0, and 4 withheld symbols
    // on lane 0 (2 before c, 2 after).
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 2;
    for (unsigned k = 0; k < 4u; k++) {
        ml_put(&m, 0, (tsym)IDLE);
    }
    for (unsigned k = 0; k < 2u; k++) {
        ml_put(&m, 1, (tsym)IDLE);
    }
    const unsigned skp[2] = {3, 3};
    ml_skp(&m, skp);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(txlog_find(&d.log, "Link width x2 (2 bound, from SKP)") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Lanes aligned x2 (offsets 2,0)") != SIZE_MAX);
    const size_t u = txlog_find(&d.log, "Packet data before lane alignment");
    WCX_REQUIRE(u != SIZE_MAX);
    WCX_CHECK(has_field(&d.log, u, "\"symbols\":4"));
    ml_close(&d);

    // The same with lane 0 six symbols late (within max_skew 8): offsets
    // 6,0, and 8 withheld symbols on lane 0.
    static ml_lanes m6;
    memset(&m6, 0, sizeof m6);
    m6.bound = 2;
    for (unsigned k = 0; k < 8u; k++) {
        ml_put(&m6, 0, (tsym)IDLE);
    }
    for (unsigned k = 0; k < 2u; k++) {
        ml_put(&m6, 1, (tsym)IDLE);
    }
    ml_skp(&m6, skp);
    ml_idle(&m6, 30);
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 2, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m6);
    WCX_CHECK(txlog_find(&d.log, "Lanes aligned x2 (offsets 6,0)") != SIZE_MAX);
    const size_t u6 = txlog_find(&d.log, "Packet data before lane alignment");
    WCX_REQUIRE(u6 != SIZE_MAX);
    WCX_CHECK(has_field(&d.log, u6, "\"symbols\":8"));
    ml_close(&d);
}

WCX_TEST(an_outside_lane_too_late_to_join_does_not_fill_its_queue) {
    // L0 start, x4 bound, max_skew 8: lanes 2-3 carry their first SKP 12
    // symbols after lanes 0-1, too late for the round lanes 0-1 open, so the
    // link is x2 from SKP. Their SKP can then join no round and is dropped
    // (§12.4) rather than holding 600 rows of idle behind it. The next SKP
    // comes on all four lanes at one index: a wider link, `Link width x4
    // (4 bound, from SKP)`, with two SKP transactions in all.
    static ml_lanes m;
    memset(&m, 0, sizeof m);
    m.bound = 4;
    for (unsigned l = 0; l < 2u; l++) {
        ml_put(&m, l, (tsym)KS(PCIE_K_COM));
        for (unsigned k = 0; k < 3u; k++) {
            ml_put(&m, l, (tsym)KS(PCIE_K_SKP));
        }
        for (unsigned k = 0; k < 12u; k++) {
            ml_put(&m, l, (tsym)IDLE);
        }
    }
    for (unsigned l = 2; l < 4u; l++) {
        for (unsigned k = 0; k < 12u; k++) {
            ml_put(&m, l, (tsym)IDLE);
        }
        ml_put(&m, l, (tsym)KS(PCIE_K_COM));
        for (unsigned k = 0; k < 3u; k++) {
            ml_put(&m, l, (tsym)KS(PCIE_K_SKP));
        }
    }
    ml_idle(&m, 600);
    const unsigned skp[4] = {3, 3, 3, 3};
    ml_skp(&m, skp);
    ml_idle(&m, 30);
    ml_drv d;
    WCX_REQUIRE(ml_open(&d, t, "ferrite.pcie_pipe_w8", 1, 4, false, "{\"scrambling\":\"off\"}"));
    ml_run(&d, &m);
    WCX_CHECK(count_prefix(&d.log, "internal decoder error") == 0);
    WCX_CHECK_EQ_U64(count_prefix(&d.log, "SKP"), 2);
    WCX_CHECK(txlog_find(&d.log, "Link width x2 (4 bound, from SKP)") != SIZE_MAX);
    WCX_CHECK(txlog_find(&d.log, "Link width x4 (4 bound, from SKP)") != SIZE_MAX);
    ml_close(&d);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, binding_rules_name_the_problem);
    WCX_RUN(&t, x2_link_locks_at_the_end_of_the_ts2_run_and_stripes);
    WCX_RUN(&t, l0_start_gives_one_unaligned_row_then_locks_at_the_first_skp);
    WCX_RUN(&t, skp_counts_and_skew_per_lane);
    WCX_RUN(&t, a_missing_ordered_set_loses_alignment_and_the_next_round_relocks);
    WCX_RUN(&t, x4_start_lane_and_misplaced_pad);
    WCX_RUN(&t, x8_pad_fill_after_end_is_not_an_error);
    WCX_RUN(&t, different_n_fts_is_a_mismatch_once_and_reversal_is_an_order);
    WCX_RUN(&t, rxstatus_names_its_lane);
    WCX_RUN(&t, fts_eios_eieos_and_unknown_ordered_sets_on_every_lane);
    WCX_RUN(&t, misfits_inside_fts_eios_and_eieos_are_unknown_on_the_lane);
    WCX_RUN(&t, xz_on_every_lane_cuts_an_ordered_set_then_an_incomplete_round_is_a_skew);
    WCX_RUN(&t, a_lone_lane_0_at_a_loss_is_named_by_the_norm);
    WCX_RUN(&t, duplicate_lane_numbers_keep_physical_order_and_unused_lanes_are_ignored);
    WCX_RUN(&t, x16_with_max_skew_64_releases_a_burst_without_running_out_of_strings);
    WCX_RUN(&t, a_partial_first_skp_widens_at_the_next_skp);
    WCX_RUN(&t, a_loss_mid_packet_drops_the_packet_and_names_the_gap);
    WCX_RUN(&t, the_span_of_a_loss_is_a_norm_item_when_the_named_lane_has_none);
    WCX_RUN(&t, ts_coalescing_ignores_what_other_lanes_sent_instead);
    WCX_RUN(&t, the_round_that_shrinks_the_link_still_compares_the_lanes_it_drops);
    WCX_RUN(&t, x12_stripes_and_pads_after_a_dllp);
    WCX_RUN(&t, lane_order_returns_to_physical);
    WCX_RUN(&t, max_skew_0_and_64);
    WCX_RUN(&t, width_cut_and_an_incomplete_row_at_the_end);
    WCX_RUN(&t, a_late_reference_lane_still_joins_the_round);
    WCX_RUN(&t, an_outside_lane_too_late_to_join_does_not_fill_its_queue);
    return wcx_test_finish(&t);
}
