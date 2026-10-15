// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Section references are to decoders/pcie/SPEC.md §11.

#include "pcie_line.h"

#include <string.h>

#include "pcie_time.h"

void pcie_line_init(pcie_line *l, pcie_rate_param rate, bool invert, pcie_line_fn fn, void *ctx) {
    memset(l, 0, sizeof *l);
    pcie_8b10b_init(&l->codes);
    l->rate = rate;
    l->invert = invert;
    l->fn = fn;
    l->ctx = ctx;
}

static uint64_t fixed_ui(pcie_rate_param rate) {
    switch (rate) {
        case PCIE_RATE_GEN1:
            return PCIE_UI_GEN1_FS;
        case PCIE_RATE_GEN2:
            return PCIE_UI_GEN2_FS;
        case PCIE_RATE_AUTO:
        default:
            return 0u; // decided per burst
    }
}

static void report(const pcie_line *l, const pcie_line_event *ev) {
    l->fn(l->ctx, ev);
}

static void report_span(const pcie_line *l, pcie_line_event_kind kind, uint64_t start_fs,
                        uint64_t end_fs) {
    pcie_line_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.start_fs = start_fs;
    ev.end_fs = end_fs;
    report(l, &ev);
}

// ── symbols (§11.7) ────────────────────────────────────────────────────────

static void feed(const pcie_line *l, uint8_t byte, bool k, uint64_t start_fs, uint64_t end_fs) {
    pcie_line_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = PCIE_LINE_SYMBOL;
    ev.start_fs = start_fs;
    ev.end_fs = end_fs;
    ev.u.sym.byte = byte;
    ev.u.sym.k = k;
    ev.u.sym.start_fs = start_fs;
    ev.u.sym.end_fs = end_fs;
    report(l, &ev);
}

static void decode_symbol(pcie_line *l, uint16_t code, uint64_t start_fs, uint64_t end_fs) {
    uint8_t byte = 0;
    bool k = false;
    pcie_line_event ev;
    memset(&ev, 0, sizeof ev);
    ev.start_fs = start_fs;
    ev.end_fs = end_fs;
    switch (pcie_8b10b_decode(&l->codes, code, &l->rd_pos, &byte, &k)) {
        case PCIE_DEC_OK:
            break;
        case PCIE_DEC_DISPARITY:
            ev.kind = PCIE_LINE_DISPARITY;
            ev.u.disp.code = code;
            ev.u.disp.byte = byte;
            ev.u.disp.k = k;
            report(l, &ev);
            break;
        case PCIE_DEC_CODE:
        default:
            // A PIPE PHY substitutes EDB for a decode error; so does this.
            ev.kind = PCIE_LINE_CODE_ERROR;
            ev.u.code = code;
            report(l, &ev);
            byte = PCIE_K_EDB;
            k = true;
            break;
    }
    feed(l, byte, k, start_fs, end_fs);
}

// ── bits and alignment (§11.5, §11.6) ──────────────────────────────────────

static bool is_com(uint16_t code) {
    return code == PCIE_CODE_COM_NEG || code == PCIE_CODE_COM_POS;
}

static void push_bit(pcie_line *l, bool bit, uint64_t start_fs, uint64_t end_fs) {
    l->shreg = (uint16_t)(((unsigned)l->shreg << 1u | (bit ? 1u : 0u)) & PCIE_CODE_MASK);
    l->bit_start[l->ring_pos] = start_fs;
    l->ring_pos = (l->ring_pos + 1u) % PCIE_SYMBOL_BITS;
    if (l->seen < PCIE_SYMBOL_BITS) {
        l->seen++;
    }
    // With 10 bits seen, the oldest of them is where the next one goes.
    const uint64_t sym_start = l->bit_start[l->ring_pos];
    const bool com = l->seen == PCIE_SYMBOL_BITS && is_com(l->shreg);
    if (!l->locked) {
        if (com) {
            l->locked = true;
            l->into = 0;
            l->rd_pos = l->shreg == PCIE_CODE_COM_POS;
            pcie_line_event ev;
            memset(&ev, 0, sizeof ev);
            ev.kind = PCIE_LINE_LOCK;
            ev.start_fs = sym_start;
            ev.end_fs = end_fs;
            ev.u.gen2 = l->ui_fs == PCIE_UI_GEN2_FS;
            report(l, &ev);
            decode_symbol(l, l->shreg, sym_start, end_fs);
        }
        return;
    }
    l->into++;
    if (l->into == PCIE_SYMBOL_BITS) {
        l->into = 0;
        decode_symbol(l, l->shreg, sym_start, end_fs);
        return;
    }
    if (com) {
        pcie_line_event ev;
        memset(&ev, 0, sizeof ev);
        ev.kind = PCIE_LINE_REALIGN;
        ev.start_fs = sym_start;
        ev.end_fs = end_fs;
        ev.u.shift = l->into;
        report(l, &ev);
        l->into = 0;
        l->rd_pos = l->shreg == PCIE_CODE_COM_POS;
        decode_symbol(l, l->shreg, sym_start, end_fs);
    }
}

// §11.5: n = max(1, floor((D + UI/2) / UI)) bits, bit i spanning
// [t + floor(i*D/n), t + floor((i+1)*D/n)). D <= 8 ns and n <= 40 here, so
// nothing overflows before the final add to t.
static void convert_run(pcie_line *l, const pcie_held_run *r) {
    uint64_t n = (r->dur_fs + l->ui_fs / 2u) / l->ui_fs;
    if (n == 0u) {
        n = 1u;
    }
    l->activity = true;
    for (uint64_t i = 0; i < n; i++) {
        const uint64_t s = pcie_time_add(r->start_fs, (i * r->dur_fs) / n);
        const uint64_t e = pcie_time_add(r->start_fs, ((i + 1u) * r->dur_fs) / n);
        push_bit(l, r->level, s, e);
    }
}

// §11.4: the held runs, in order, at the decided rate.
static void release_held(pcie_line *l, uint64_t ui_fs) {
    l->ui_fs = ui_fs;
    for (unsigned i = 0; i < l->held_count; i++) {
        convert_run(l, &l->held[i]);
    }
    l->held_count = 0;
}

static void take_run(pcie_line *l, uint64_t start_fs, uint64_t dur_fs, bool level) {
    const pcie_held_run r = {start_fs, dur_fs, level};
    if (l->ui_fs != 0u) {
        convert_run(l, &r);
        return;
    }
    l->held[l->held_count++] = r;
    if (dur_fs < PCIE_GEN2_RUN_FS) {
        release_held(l, PCIE_UI_GEN2_FS);
    } else if (l->held_count == PCIE_RATE_HOLD_RUNS) {
        release_held(l, PCIE_UI_GEN1_FS);
    }
}

// ── electrical idle (§11.3) ────────────────────────────────────────────────

static void begin_idle(pcie_line *l, uint64_t start_fs) {
    if (l->idle) {
        return; // §11.3: back-to-back idle stretches are one period
    }
    if (l->held_count > 0u) {
        release_held(l, PCIE_UI_GEN1_FS); // §11.4 rule 3: the burst ended first
    }
    l->idle = true;
    l->idle_start_fs = start_fs;
    l->idle_reportable = l->activity;
    l->locked = false;
    l->seen = 0;
    l->into = 0;
    l->shreg = 0;
    l->rd_pos = false;
    l->ui_fs = fixed_ui(l->rate);
    report_span(l, PCIE_LINE_IDLE_BEGIN, start_fs, start_fs);
}

static void end_idle(pcie_line *l, uint64_t end_fs) {
    l->idle = false;
    if (l->idle_reportable) {
        report_span(l, PCIE_LINE_IDLE_PERIOD, l->idle_start_fs, end_fs);
    }
}

// A run that ended (§11.2): idle when longer than 8 ns, bits otherwise. A
// run of bits ends an idle period at its own start.
static void run_ended(pcie_line *l, uint64_t start_fs, uint64_t end_fs, bool level) {
    const uint64_t dur = end_fs - start_fs;
    if (dur > PCIE_IDLE_RUN_FS) {
        begin_idle(l, start_fs);
        return;
    }
    if (l->idle) {
        end_idle(l, start_fs);
    }
    take_run(l, start_fs, dur, level);
}

void pcie_line_sample(pcie_line *l, uint64_t t_fs, bool unknown, bool level) {
    if (l->ui_fs == 0u && l->rate != PCIE_RATE_AUTO) {
        l->ui_fs = fixed_ui(l->rate);
    }
    if (unknown) {
        if (l->have_level) {
            l->have_level = false;
            run_ended(l, l->run_start_fs, t_fs, l->level);
        }
        begin_idle(l, t_fs);
        return;
    }
    const bool v = level != l->invert;
    if (!l->have_level) {
        l->have_level = true;
        l->level = v;
        l->run_start_fs = t_fs;
        return;
    }
    if (v == l->level) {
        return;
    }
    run_ended(l, l->run_start_fs, t_fs, l->level);
    l->level = v;
    l->run_start_fs = t_fs;
}

void pcie_line_flush_runs(pcie_line *l) {
    if (l->held_count > 0u) {
        release_held(l, PCIE_UI_GEN1_FS);
    }
}

void pcie_line_flush_idle(pcie_line *l, uint64_t now_fs) {
    if (l->idle) {
        // §11.8: to the start of the next run, or to the last sample.
        end_idle(l, l->have_level ? l->run_start_fs : now_fs);
    }
}
