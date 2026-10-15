// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The serial line front end (SPEC.md §11.2-§11.7) on hand-built bit
// streams. Streams are encoded with pcie_8b10b_encode, whose output
// unit_8b10b.c checks against the protocol notes' known answers; expected
// times are worked out here from the §11.5 formula.

#include <stdlib.h>
#include <string.h>

#include "pcie_line.h"
#include "wcx/test.h"

// ── recording the events ───────────────────────────────────────────────────

#define MAX_EVENTS 512u

typedef struct evlog {
    pcie_line_event ev[MAX_EVENTS];
    size_t n;
} evlog;

static void record(void *ctx, const pcie_line_event *ev) {
    evlog *log = ctx;
    if (log->n < MAX_EVENTS) {
        log->ev[log->n++] = *ev;
    }
}

static size_t count(const evlog *log, pcie_line_event_kind kind) {
    size_t n = 0;
    for (size_t i = 0; i < log->n; i++) {
        n += log->ev[i].kind == kind ? 1u : 0u;
    }
    return n;
}

// The which-th event of a kind, or an all-zero event when there are fewer
// (never NULL; the tests check the counts first).
static const pcie_line_event *nth(const evlog *log, pcie_line_event_kind kind, size_t which) {
    static const pcie_line_event k_none = {0};
    for (size_t i = 0; i < log->n; i++) {
        if (log->ev[i].kind == kind && which-- == 0u) {
            return &log->ev[i];
        }
    }
    return &k_none;
}

static size_t index_of(const evlog *log, pcie_line_event_kind kind) {
    for (size_t i = 0; i < log->n; i++) {
        if (log->ev[i].kind == kind) {
            return i;
        }
    }
    return SIZE_MAX;
}

// ── building a line ────────────────────────────────────────────────────────

#define MAX_BITS 4096u

typedef struct wire {
    bool bit[MAX_BITS];
    size_t n;
    bool rd_pos;
} wire;

static void put_bits(wire *w, uint16_t code, unsigned width) {
    for (unsigned i = 0; i < width && w->n < MAX_BITS; i++) {
        w->bit[w->n++] = (((unsigned)code >> (width - 1u - i)) & 1u) != 0u;
    }
}

static void put_sym(wire *w, uint8_t byte, bool k) {
    uint16_t code = 0;
    bool after = false;
    if (pcie_8b10b_encode(byte, k, w->rd_pos, &code, &after)) {
        put_bits(w, code, 10u);
        w->rd_pos = after;
    }
}

// Feeds the wire from `t0`, one bit per `ui`: a sample at t0 and at every
// change, then one closing transition so the last run is decoded (§11.2).
// Returns the time of the closing sample.
static uint64_t drive(pcie_line *l, const wire *w, uint64_t t0, uint64_t ui) {
    if (w->n == 0u) {
        return t0;
    }
    pcie_line_sample(l, t0, false, w->bit[0]);
    for (size_t i = 1; i < w->n; i++) {
        if (w->bit[i] != w->bit[i - 1u]) {
            pcie_line_sample(l, t0 + i * ui, false, w->bit[i]);
        }
    }
    const uint64_t end = t0 + w->n * ui;
    pcie_line_sample(l, end, false, !w->bit[w->n - 1u]);
    return end;
}

static const uint8_t k_ts1[16] = {0xBC, 0xF7, 0xF7, 24,   0x02, 0x00, 0x4A, 0x4A,
                                  0x4A, 0x4A, 0x4A, 0x4A, 0x4A, 0x4A, 0x4A, 0x4A};

static void put_ts1(wire *w) {
    for (unsigned i = 0; i < 16u; i++) {
        put_sym(w, k_ts1[i], i == 0u || i == 1u || i == 2u);
    }
}

// ── tests ──────────────────────────────────────────────────────────────────

WCX_TEST(gen1_lock_and_symbol_times) {
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_GEN1, false, record, &log);
    wire w = {0};
    put_bits(&w, 0x2Au, 7u); // pre-lock noise: produces nothing (§11.6)
    put_ts1(&w);
    drive(l, &w, 1000000, PCIE_UI_GEN1_FS);
    WCX_REQUIRE(count(&log, PCIE_LINE_LOCK) == 1u);
    const pcie_line_event *lock = nth(&log, PCIE_LINE_LOCK, 0);
    WCX_CHECK(!lock->u.gen2);
    // The COM's first bit is bit 7: 1000000 + 7 * 400000.
    WCX_CHECK_EQ_U64(lock->start_fs, 1000000u + 7u * PCIE_UI_GEN1_FS);
    WCX_CHECK_EQ_U64(lock->end_fs, 1000000u + 17u * PCIE_UI_GEN1_FS);
    WCX_REQUIRE(count(&log, PCIE_LINE_SYMBOL) == 16u);
    for (size_t i = 0; i < 16u; i++) {
        const pcie_line_event *s = nth(&log, PCIE_LINE_SYMBOL, i);
        WCX_CHECK_EQ_U64(s->u.sym.byte, k_ts1[i]);
        WCX_CHECK_EQ_U64(s->u.sym.k, i < 3u);
        WCX_CHECK_EQ_U64(s->u.sym.start_fs, 1000000u + (7u + 10u * i) * PCIE_UI_GEN1_FS);
        WCX_CHECK_EQ_U64(s->u.sym.end_fs, 1000000u + (17u + 10u * i) * PCIE_UI_GEN1_FS);
    }
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_CODE_ERROR) + count(&log, PCIE_LINE_DISPARITY), 0);
    // The lock is reported before the COM it found.
    WCX_CHECK(index_of(&log, PCIE_LINE_LOCK) < index_of(&log, PCIE_LINE_SYMBOL));
    free(l);
}

WCX_TEST(com_of_either_disparity_locks_with_its_disparity) {
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_GEN1, false, record, &log);
    wire w = {0};
    w.rd_pos = true; // COM 1100000101, then data at the matching disparity
    put_sym(&w, 0xBC, true);
    put_sym(&w, 0x00, false);
    put_sym(&w, 0x00, false);
    drive(l, &w, 0, PCIE_UI_GEN1_FS);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_LOCK), 1);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_SYMBOL), 3);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_DISPARITY), 0);
    free(l);
}

WCX_TEST(auto_rate_gen2_from_a_short_run) {
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_AUTO, false, record, &log);
    wire w = {0};
    put_ts1(&w);
    drive(l, &w, 0, PCIE_UI_GEN2_FS);
    WCX_REQUIRE(count(&log, PCIE_LINE_LOCK) == 1u);
    WCX_CHECK(nth(&log, PCIE_LINE_LOCK, 0)->u.gen2);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_SYMBOL), 16);
    WCX_CHECK_EQ_U64(nth(&log, PCIE_LINE_SYMBOL, 1)->u.sym.start_fs, 10u * PCIE_UI_GEN2_FS);
    free(l);
}

WCX_TEST(auto_rate_gen1_after_forty_runs) {
    // 0011 repeated: every run is two Gen1 bits (800 ps), never under 300 ps.
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_AUTO, false, record, &log);
    wire w = {0};
    for (unsigned i = 0; i < 21u; i++) {
        put_bits(&w, 0x3u, 4u);
    }
    // 42 runs held so far; then a TS1 at Gen1 (its 1-bit runs are 400 ps).
    w.rd_pos = false;
    put_ts1(&w);
    drive(l, &w, 0, PCIE_UI_GEN1_FS);
    WCX_REQUIRE(count(&log, PCIE_LINE_LOCK) == 1u);
    WCX_CHECK(!nth(&log, PCIE_LINE_LOCK, 0)->u.gen2);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_SYMBOL), 16);
    free(l);
}

WCX_TEST(auto_rate_gen1_when_the_burst_ends_first) {
    // A COM at Gen1 has five runs; X ends the burst before 40: Gen1, and the
    // held runs are converted before the idle begins (§11.4 rule 3).
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_AUTO, false, record, &log);
    wire w = {0};
    put_sym(&w, 0xBC, true);
    const uint64_t end = drive(l, &w, 0, PCIE_UI_GEN1_FS);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_SYMBOL), 0);
    // The closing run is one Gen1 bit; a shorter one would decide Gen2.
    pcie_line_sample(l, end + PCIE_UI_GEN1_FS, true, false);
    WCX_REQUIRE(count(&log, PCIE_LINE_SYMBOL) == 1u);
    WCX_CHECK(!nth(&log, PCIE_LINE_LOCK, 0)->u.gen2);
    WCX_CHECK(index_of(&log, PCIE_LINE_SYMBOL) < index_of(&log, PCIE_LINE_IDLE_BEGIN));
    free(l);
}

WCX_TEST(flush_converts_held_runs_as_gen1) {
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_AUTO, false, record, &log);
    wire w = {0};
    put_sym(&w, 0xBC, true);
    drive(l, &w, 0, PCIE_UI_GEN1_FS);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_SYMBOL), 0);
    pcie_line_flush_runs(l);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_SYMBOL), 1);
    pcie_line_flush_runs(l); // nothing held any more
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_SYMBOL), 1);
    free(l);
}

WCX_TEST(bit_count_rounds_half_up_and_times_floor) {
    // §11.5 with UI = 400000: D = 3*UI + 7 holds 3 bits at t0, t0 + floor(D/3),
    // t0 + floor(2D/3); D = 1.5 UI holds 2; D = 1.49 UI holds 1; a 1 fs
    // glitch still holds 1.
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_GEN1, false, record, &log);
    // Lock first on a clean COM (RD-: 0011111010) ending at level 0.
    wire w = {0};
    put_sym(&w, 0xBC, true);
    uint64_t t0 = 0;
    pcie_line_sample(l, t0, false, w.bit[0]);
    for (size_t i = 1; i < w.n; i++) {
        if (w.bit[i] != w.bit[i - 1u]) {
            pcie_line_sample(l, i * PCIE_UI_GEN1_FS, false, w.bit[i]);
        }
    }
    // The last COM bit (0) is still open at 9 UI; jittered runs follow.
    t0 = 9u * PCIE_UI_GEN1_FS;
    const uint64_t d3 = 3u * PCIE_UI_GEN1_FS + 7u;
    pcie_line_sample(l, t0 + PCIE_UI_GEN1_FS, false, true); // the 0 bit ends: COM complete
    WCX_REQUIRE(count(&log, PCIE_LINE_SYMBOL) == 1u);
    const uint64_t a = t0 + PCIE_UI_GEN1_FS;
    pcie_line_sample(l, a + d3, false, false);                         // 3 ones
    pcie_line_sample(l, a + d3 + 600000u, false, true);                // 1.5 UI: 2 zeros
    pcie_line_sample(l, a + d3 + 600000u + 596000u, false, false);     // 1.49 UI: 1 one
    pcie_line_sample(l, a + d3 + 600000u + 596000u + 1u, false, true); // glitch: 1 zero
    pcie_line_sample(l, a + d3 + 600000u + 596000u + 1u + 3u * PCIE_UI_GEN1_FS, false, false);
    // 3 + 2 + 1 + 1 + 3 = 10 bits: 1110010111 is not a code group.
    WCX_REQUIRE(count(&log, PCIE_LINE_CODE_ERROR) == 1u);
    const pcie_line_event *ce = nth(&log, PCIE_LINE_CODE_ERROR, 0);
    WCX_CHECK_EQ_U64(ce->u.code, 0x397u);
    WCX_CHECK_EQ_U64(ce->start_fs, a);
    WCX_CHECK_EQ_U64(ce->end_fs, a + d3 + 600000u + 596000u + 1u + 3u * PCIE_UI_GEN1_FS);
    // The EDB substituted for it has the same span.
    const pcie_line_event *edb = nth(&log, PCIE_LINE_SYMBOL, 1);
    WCX_CHECK_EQ_U64(edb->u.sym.byte, 0xFE);
    WCX_CHECK(edb->u.sym.k);
    WCX_CHECK_EQ_U64(edb->u.sym.start_fs, a);
    free(l);
}

WCX_TEST(bit_times_inside_a_run_floor) {
    // A run of 3 bits over D = 3 UI + 7: the second symbol bit starts at
    // floor(D/3) into the run. Checked through the symbol after it.
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_GEN1, false, record, &log);
    // COM RD- = 0011111010: runs 00 | 11111 | 0 | 1 | 0. Start the 5-run of
    // ones at 800000 and stretch it to 5 UI + 9; COM spans its 10 bits.
    pcie_line_sample(l, 0, false, false);
    pcie_line_sample(l, 800000u, false, true);
    const uint64_t d5 = 5u * PCIE_UI_GEN1_FS + 9u;
    uint64_t t0 = 800000u + d5;
    pcie_line_sample(l, t0, false, false);
    pcie_line_sample(l, t0 + PCIE_UI_GEN1_FS, false, true);
    pcie_line_sample(l, t0 + 2u * PCIE_UI_GEN1_FS, false, false);
    pcie_line_sample(l, t0 + 3u * PCIE_UI_GEN1_FS, false, true);
    WCX_REQUIRE(count(&log, PCIE_LINE_SYMBOL) == 1u);
    const pcie_line_event *com = nth(&log, PCIE_LINE_SYMBOL, 0);
    WCX_CHECK_EQ_U64(com->u.sym.byte, 0xBC);
    WCX_CHECK_EQ_U64(com->u.sym.start_fs, 0);
    WCX_CHECK_EQ_U64(com->u.sym.end_fs, t0 + 3u * PCIE_UI_GEN1_FS);
    free(l);
}

WCX_TEST(realign_on_a_com_off_the_boundary) {
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_GEN1, false, record, &log);
    wire w = {0};
    put_sym(&w, 0xBC, true);
    put_sym(&w, 0x00, false);
    put_bits(&w, 0x0u, 1u); // one slipped bit
    put_sym(&w, 0xBC, true);
    put_sym(&w, 0x4A, false);
    drive(l, &w, 0, PCIE_UI_GEN1_FS);
    WCX_REQUIRE(count(&log, PCIE_LINE_REALIGN) == 1u);
    const pcie_line_event *ra = nth(&log, PCIE_LINE_REALIGN, 0);
    WCX_CHECK_EQ_U64(ra->u.shift, 1);
    WCX_CHECK_EQ_U64(ra->start_fs, 21u * PCIE_UI_GEN1_FS); // the COM after the slipped bit
    WCX_CHECK_EQ_U64(ra->end_fs, 31u * PCIE_UI_GEN1_FS);
    // Symbols: COM, D0.0, the misaligned group, the realigned COM, D10.2.
    WCX_REQUIRE(count(&log, PCIE_LINE_SYMBOL) == 5u);
    WCX_CHECK_EQ_U64(nth(&log, PCIE_LINE_SYMBOL, 3)->u.sym.byte, 0xBC);
    WCX_CHECK_EQ_U64(nth(&log, PCIE_LINE_SYMBOL, 3)->u.sym.start_fs, 21u * PCIE_UI_GEN1_FS);
    WCX_CHECK_EQ_U64(nth(&log, PCIE_LINE_SYMBOL, 4)->u.sym.byte, 0x4A);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_LOCK), 1);
    free(l);
}

WCX_TEST(disparity_error_decodes_the_symbol) {
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_GEN1, false, record, &log);
    wire w = {0};
    put_sym(&w, 0xBC, true); // RD now +
    w.rd_pos = false;        // D0.0 sent as if RD were -
    put_sym(&w, 0x00, false);
    drive(l, &w, 0, PCIE_UI_GEN1_FS);
    WCX_REQUIRE(count(&log, PCIE_LINE_DISPARITY) == 1u);
    const pcie_line_event *de = nth(&log, PCIE_LINE_DISPARITY, 0);
    WCX_CHECK_EQ_U64(de->u.disp.byte, 0x00);
    WCX_CHECK(!de->u.disp.k);
    WCX_CHECK_EQ_U64(de->u.disp.code, 0x274u); // 1001110100
    WCX_CHECK_EQ_U64(nth(&log, PCIE_LINE_SYMBOL, 1)->u.sym.byte, 0x00);
    free(l);
}

WCX_TEST(invert_flips_every_bit) {
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_GEN1, true, record, &log);
    wire w = {0};
    put_ts1(&w);
    for (size_t i = 0; i < w.n; i++) {
        w.bit[i] = !w.bit[i];
    }
    drive(l, &w, 0, PCIE_UI_GEN1_FS);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_SYMBOL), 16);
    WCX_CHECK_EQ_U64(nth(&log, PCIE_LINE_SYMBOL, 6)->u.sym.byte, 0x4A);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_DISPARITY), 0);
    free(l);
}

WCX_TEST(long_run_is_idle_and_ends_at_the_next_bits) {
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_GEN1, false, record, &log);
    wire w = {0};
    put_ts1(&w);
    const uint64_t end = drive(l, &w, 0, PCIE_UI_GEN1_FS);
    // `drive` left the line at the opposite level from `end`: hold 9 ns.
    const uint64_t back = end + 9000000u;
    pcie_line_sample(l, back, false, w.bit[w.n - 1u]);
    WCX_REQUIRE(count(&log, PCIE_LINE_IDLE_BEGIN) == 1u);
    WCX_CHECK_EQ_U64(nth(&log, PCIE_LINE_IDLE_BEGIN, 0)->start_fs, end);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_IDLE_PERIOD), 0); // not over yet
    // A second burst: the idle period ends where its first run of bits starts.
    wire w2 = {0};
    put_ts1(&w2);
    const bool lvl = w.bit[w.n - 1u];
    // Make the second burst start with a transition from `lvl`.
    for (size_t i = 0; i < w2.n; i++) {
        w2.bit[i] = w2.bit[i] != (w2.bit[0] == lvl);
    }
    (void)drive(l, &w2, back + 2000000u, PCIE_UI_GEN1_FS);
    WCX_REQUIRE(count(&log, PCIE_LINE_IDLE_PERIOD) == 1u);
    const pcie_line_event *ip = nth(&log, PCIE_LINE_IDLE_PERIOD, 0);
    WCX_CHECK_EQ_U64(ip->start_fs, end);
    WCX_CHECK_EQ_U64(ip->end_fs, back);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_LOCK), 2);
    free(l);
}

WCX_TEST(idle_before_any_bits_is_not_reported) {
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_GEN1, false, record, &log);
    pcie_line_sample(l, 0, true, false); // X at the start
    pcie_line_sample(l, 1000000, false, false);
    pcie_line_sample(l, 20000000, false, true);  // 19 ns of 0: still idle (merged)
    pcie_line_sample(l, 20400000, false, false); // a 1-bit run: idle ends at 20000000
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_IDLE_BEGIN), 1);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_IDLE_PERIOD), 0);
    free(l);
}

WCX_TEST(x_then_long_run_is_one_period_and_flush_ends_it) {
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_GEN1, false, record, &log);
    wire w = {0};
    put_ts1(&w);
    const uint64_t end = drive(l, &w, 0, PCIE_UI_GEN1_FS);
    pcie_line_sample(l, end + 400000u, true, false); // the 1-bit closing run, then X
    pcie_line_sample(l, end + 1000000u, false, false);
    pcie_line_sample(l, end + 12000000u, true, true); // 11 ns, then X: still one period
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_IDLE_BEGIN), 1);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_IDLE_PERIOD), 0);
    pcie_line_flush_idle(l, end + 14000000u);
    WCX_REQUIRE(count(&log, PCIE_LINE_IDLE_PERIOD) == 1u);
    const pcie_line_event *ip = nth(&log, PCIE_LINE_IDLE_PERIOD, 0);
    WCX_CHECK_EQ_U64(ip->start_fs, end + 400000u);
    WCX_CHECK_EQ_U64(ip->end_fs, end + 14000000u);
    free(l);
}

WCX_TEST(flush_ends_idle_at_the_open_run) {
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_GEN1, false, record, &log);
    wire w = {0};
    put_ts1(&w);
    const uint64_t end = drive(l, &w, 0, PCIE_UI_GEN1_FS);
    pcie_line_sample(l, end + 400000u, true, false);  // X
    pcie_line_sample(l, end + 2000000u, false, true); // known again: the open run
    pcie_line_flush_runs(l);
    pcie_line_flush_idle(l, end + 3000000u);
    WCX_REQUIRE(count(&log, PCIE_LINE_IDLE_PERIOD) == 1u);
    WCX_CHECK_EQ_U64(nth(&log, PCIE_LINE_IDLE_PERIOD, 0)->end_fs, end + 2000000u);
    free(l);
}

WCX_TEST(repeated_levels_do_not_end_a_run) {
    evlog log = {0};
    pcie_line *l = calloc(1, sizeof *l);
    WCX_REQUIRE(l != NULL);
    pcie_line_init(l, PCIE_RATE_GEN1, false, record, &log);
    wire w = {0};
    put_ts1(&w);
    // Same stream, with a duplicate sample in the middle of the 5-bit run.
    pcie_line_sample(l, 0, false, w.bit[0]);
    for (size_t i = 1; i < w.n; i++) {
        if (w.bit[i] != w.bit[i - 1u]) {
            pcie_line_sample(l, i * PCIE_UI_GEN1_FS, false, w.bit[i]);
        } else if (i == 4u) {
            pcie_line_sample(l, i * PCIE_UI_GEN1_FS + 5u, false, w.bit[i]);
        }
    }
    pcie_line_sample(l, w.n * PCIE_UI_GEN1_FS, false, !w.bit[w.n - 1u]);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_SYMBOL), 16);
    WCX_CHECK_EQ_U64(count(&log, PCIE_LINE_CODE_ERROR), 0);
    free(l);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, gen1_lock_and_symbol_times);
    WCX_RUN(&t, com_of_either_disparity_locks_with_its_disparity);
    WCX_RUN(&t, auto_rate_gen2_from_a_short_run);
    WCX_RUN(&t, auto_rate_gen1_after_forty_runs);
    WCX_RUN(&t, auto_rate_gen1_when_the_burst_ends_first);
    WCX_RUN(&t, flush_converts_held_runs_as_gen1);
    WCX_RUN(&t, bit_count_rounds_half_up_and_times_floor);
    WCX_RUN(&t, bit_times_inside_a_run_floor);
    WCX_RUN(&t, realign_on_a_com_off_the_boundary);
    WCX_RUN(&t, disparity_error_decodes_the_symbol);
    WCX_RUN(&t, invert_flips_every_bit);
    WCX_RUN(&t, long_run_is_idle_and_ends_at_the_next_bits);
    WCX_RUN(&t, idle_before_any_bits_is_not_reported);
    WCX_RUN(&t, x_then_long_run_is_one_period_and_flush_ends_it);
    WCX_RUN(&t, flush_ends_idle_at_the_open_run);
    WCX_RUN(&t, repeated_levels_do_not_end_a_run);
    return wcx_test_finish(&t);
}
