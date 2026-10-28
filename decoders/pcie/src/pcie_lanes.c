// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Section references are to decoders/pcie/SPEC.md.

#include "pcie_lanes.h"

#include <stdlib.h>
#include <string.h>

#include "pcie_symbols.h"
#include "pcie_time.h"
#include "wcx/arena.h"
#include "wcx/json_writer.h"
#include "wcx/str.h"

#define SKP_MAX 5u  // §5.3
#define FTS_LEN 3u  // COM + 3 FTS; EIOS: COM + 3 IDL
#define EIE_LEN 14u // COM + 14 EIE + D10.2

// TS1/TS2 symbols within pcie_lane_os.ts (symbol n is ts[n-1]).
#define TS_LINK  0u
#define TS_LANE  1u
#define TS_NFTS  2u
#define TS_RATE  3u
#define TS_CTRL  4u
#define TS_ID_AT 5u

// §12.4: a round can still gain an ordered set, or its reference lane's
// next item is still unknown, until max_skew + 17 symbols past its COM.
#define ROUND_SETTLE 17u

// §12.5 rule 1: the most SKP ordered sets a TS2 round looks past. At the
// cap the TS2 run ends, so a lane holds at most this many while it waits.
#define TS2_SKP_LOOKAHEAD 16u

// "0,1,2,3": 16 entries of at most 20 digits and 15 commas, with room.
#define LIST_CAP 352u

static const char *const k_os_names[] = {"TS1", "TS2", "SKP", "FTS", "EIOS", "EIEOS", "unknown"};

// ── queues ─────────────────────────────────────────────────────────────────

static pcie_lane_item *item_at(pcie_lane *l, size_t i) {
    return &l->items[(l->head + i) % PCIE_LANE_ITEMS];
}

static pcie_lane_item *head(pcie_lane *l) {
    return l->count > 0 ? item_at(l, 0) : NULL;
}

// The first item of a lane the caller knows is not empty.
static pcie_lane_item *front(pcie_lane *l) {
    return item_at(l, 0);
}

// Lane l of the link; l is always below bound, the modulo only proves it.
static pcie_lane *lane_of(pcie_lanes *ln, unsigned l) {
    return &ln->lane[l % PCIE_MAX_LANES];
}

static pcie_lane_item *tail(pcie_lane *l) {
    return l->count > 0 ? item_at(l, l->count - 1u) : NULL;
}

static void pop(pcie_lane *l) {
    if (l->count > 0) {
        l->head = (l->head + 1u) % PCIE_LANE_ITEMS;
        l->count--;
    }
}

static pcie_lane_item *push(pcie_lanes *ln, pcie_lane *l) {
    if (l->count >= PCIE_LANE_ITEMS) {
        /* defensive: §12.4 bounds what a lane holds well below the ring */
        if (!ln->failed) {
            ln->failed = true;
            wcx_decoder_fail(ln->d, "internal decoder error: a lane queue overflowed");
        }
        return NULL;
    }
    pcie_lane_item *it = item_at(l, l->count);
    l->count++;
    memset(it, 0, sizeof *it);
    return it;
}

// ── per-lane items (§12.3) ─────────────────────────────────────────────────

static void os_begin(pcie_lane *l, uint64_t index) {
    memset(&l->cur, 0, sizeof l->cur);
    l->cur_index = index;
    l->os_n = 0;
}

static void os_add(pcie_lane *l, uint8_t byte, bool k, uint64_t start_fs, uint64_t end_fs) {
    pcie_lane_os *o = &l->cur;
    if (o->n >= PCIE_OS_MAX_SYMS) {
        return; /* defensive: no ordered set is longer */
    }
    o->byte[o->n] = byte;
    if (k) {
        o->kmask = (uint16_t)(o->kmask | (1u << o->n));
    }
    o->start_fs[o->n] = start_fs;
    o->end_fs[o->n] = end_fs;
    o->n++;
}

static void os_finish(pcie_lanes *ln, pcie_lane *l, pcie_os_type type) {
    l->pst = LPS_OUT;
    pcie_lane_item *it = push(ln, l);
    if (it == NULL) {
        return;
    }
    l->cur.type = type;
    it->kind = PCIE_ITEM_OS;
    it->index = l->cur_index;
    it->start_fs = l->cur.start_fs[0];
    it->end_fs = l->cur.end_fs[l->cur.n > 0 ? l->cur.n - 1u : 0];
    it->os = l->cur;
}

static void close_gap(pcie_lane *l) {
    pcie_lane_item *t = tail(l);
    if (t != NULL && t->kind == PCIE_ITEM_GAP) {
        t->open = false;
    }
}

// One raw symbol of a lane, with its index and time (§12.2).
typedef struct lane_sym {
    uint8_t byte;
    bool k;
    uint64_t index;
    uint64_t start_fs;
    uint64_t end_fs;
} lane_sym;

static void os_take(pcie_lane *l, const lane_sym *s) {
    os_add(l, s->byte, s->k, s->start_fs, s->end_fs);
}

// The symbol after COM decides the ordered set (§5.3).
static void os_start_symbol(pcie_lanes *ln, pcie_lane *l, const lane_sym *s) {
    os_take(l, s);
    l->os_n = 1;
    if (!s->k || s->byte == PCIE_K_PAD) {
        l->cur.ts[0] = s->byte;
        l->cur.ts_kmask = s->k ? 1u : 0u;
        l->pst = LPS_TS;
        return;
    }
    switch (s->byte) {
        case PCIE_K_SKP:
            l->cur.skp = 1;
            l->pst = LPS_SKP;
            break;
        case PCIE_K_FTS:
            l->pst = LPS_FTS;
            break;
        case PCIE_K_IDL:
            l->pst = LPS_IDL;
            break;
        case PCIE_K_EIE:
            l->pst = LPS_EIE;
            break;
        default:
            os_finish(ln, l, PCIE_OS_UNKNOWN);
            break;
    }
}

// TS1/TS2 symbols 1-15; symbol 6 names it (§5.3).
static void os_ts_symbol(pcie_lanes *ln, pcie_lane *l, const lane_sym *s) {
    os_take(l, s);
    l->cur.ts[l->os_n] = s->byte;
    if (s->k) {
        l->cur.ts_kmask = (uint16_t)(l->cur.ts_kmask | (1u << l->os_n));
    }
    l->os_n++;
    if (l->os_n < PCIE_TS_BODY) {
        return;
    }
    pcie_os_type t = PCIE_OS_UNKNOWN;
    if ((l->cur.ts_kmask & (1u << TS_ID_AT)) == 0) {
        if (l->cur.ts[TS_ID_AT] == PCIE_D_TS1_ID) {
            t = PCIE_OS_TS1;
        } else if (l->cur.ts[TS_ID_AT] == PCIE_D_TS2_ID) {
            t = PCIE_OS_TS2;
        }
    }
    os_finish(ln, l, t);
}

// COM + 1..5 SKP. False when the symbol ends the SKP OS and is not part of it.
static bool os_skp_symbol(pcie_lanes *ln, pcie_lane *l, const lane_sym *s) {
    if (!(s->k && s->byte == PCIE_K_SKP)) {
        os_finish(ln, l, PCIE_OS_SKP);
        return false;
    }
    os_take(l, s);
    l->cur.skp++;
    if (l->cur.skp == SKP_MAX) {
        os_finish(ln, l, PCIE_OS_SKP);
    }
    return true;
}

// COM + 3 FTS (FTS OS), COM + 3 IDL (EIOS), COM + 14 EIE + D10.2 (EIEOS).
static void os_repeat_symbol(pcie_lanes *ln, pcie_lane *l, const lane_sym *s) {
    os_take(l, s);
    if (l->pst == LPS_EIE_END) {
        os_finish(ln, l, !s->k && s->byte == PCIE_D_TS1_ID ? PCIE_OS_EIEOS : PCIE_OS_UNKNOWN);
        return;
    }
    uint8_t want = PCIE_K_EIE;
    unsigned len = EIE_LEN;
    if (l->pst != LPS_EIE) {
        want = l->pst == LPS_FTS ? PCIE_K_FTS : PCIE_K_IDL;
        len = FTS_LEN;
    }
    if (!(s->k && s->byte == want)) {
        os_finish(ln, l, PCIE_OS_UNKNOWN);
        return;
    }
    l->os_n++;
    if (l->os_n < len) {
        return;
    }
    if (l->pst == LPS_EIE) {
        l->pst = LPS_EIE_END;
    } else {
        os_finish(ln, l, l->pst == LPS_FTS ? PCIE_OS_FTS : PCIE_OS_EIOS);
    }
}

// The ordered-set recogniser of §5.3 on raw symbols: true when the symbol
// is part of an ordered set, false when it is a row symbol.
static bool lane_os_symbol(pcie_lanes *ln, pcie_lane *l, const lane_sym *s) {
    switch (l->pst) {
        case LPS_OUT:
            break;
        case LPS_OS_START:
            os_start_symbol(ln, l, s);
            return true;
        case LPS_TS:
            os_ts_symbol(ln, l, s);
            return true;
        case LPS_SKP:
            if (os_skp_symbol(ln, l, s)) {
                return true;
            }
            break; // the SKP OS ended before this symbol
        case LPS_FTS:
        case LPS_IDL:
        case LPS_EIE:
        case LPS_EIE_END:
            os_repeat_symbol(ln, l, s);
            return true;
        default: /* defensive: every state is handled above */
            l->pst = LPS_OUT;
            break;
    }
    if (!(s->k && s->byte == PCIE_K_COM)) {
        return false;
    }
    os_begin(l, s->index);
    os_take(l, s);
    l->pst = LPS_OS_START;
    return true;
}

static void lane_symbol(pcie_lanes *ln, pcie_lane *l, uint8_t byte, bool k, uint64_t index,
                        uint64_t start_fs, uint64_t end_fs) {
    close_gap(l);
    const lane_sym sym = {byte, k, index, start_fs, end_fs};
    const bool in_os = lane_os_symbol(ln, l, &sym);
    // §12.3: the lane's own descrambler, as §5.2.
    uint8_t plain = byte;
    if (!in_os && !k) {
        plain = (uint8_t)(byte ^ pcie_scrambler_key(&l->lfsr));
    }
    if (!(k && byte == PCIE_K_SKP)) {
        (void)pcie_scrambler_advance(&l->lfsr);
    }
    if (k && byte == PCIE_K_COM) {
        pcie_scrambler_reset(&l->lfsr);
    }
    if (in_os) {
        return;
    }
    pcie_lane_item *it = push(ln, l);
    if (it == NULL) {
        return;
    }
    it->kind = PCIE_ITEM_SYM;
    it->index = index;
    it->start_fs = start_fs;
    it->end_fs = end_fs;
    it->byte = byte;
    it->plain = plain;
    it->k = k;
}

// §12.3: a gated or X/Z edge. It cuts an ordered set in progress (a SKP OS
// with a SKP is complete) and grows the lane's gap.
static void lane_gap(pcie_lanes *ln, pcie_lane *l, pcie_break_reason kind, uint64_t index,
                     uint64_t start_fs, uint64_t end_fs) {
    if (l->pst != LPS_OUT) {
        const bool skp = l->pst == LPS_SKP;
        l->cur.cut = true;
        l->cur.cut_by = kind;
        os_finish(ln, l, skp ? PCIE_OS_SKP : PCIE_OS_UNKNOWN);
    }
    pcie_lane_item *t = tail(l);
    if (t != NULL && t->kind == PCIE_ITEM_GAP && t->open && t->gap == kind) {
        t->edges++;
        t->end_fs = end_fs;
        return;
    }
    close_gap(l);
    pcie_lane_item *it = push(ln, l);
    if (it == NULL) {
        return;
    }
    it->kind = PCIE_ITEM_GAP;
    it->index = index;
    it->start_fs = start_fs;
    it->end_fs = end_fs;
    it->gap = kind;
    it->open = true;
    it->edges = 1;
}

// ── lane-level transactions (§12.12) ───────────────────────────────────────

static void list_u64(char out[LIST_CAP], const uint64_t *v, unsigned n) {
    size_t at = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < n; i++) {
        char e[WCX_FMT_U64_CAP] = {0};
        const size_t len = wcx_fmt_u64(e, v[i]);
        if (at + len + 2u > LIST_CAP) {
            return; /* defensive: 16 entries always fit */
        }
        if (i > 0) {
            out[at++] = ',';
        }
        memcpy(out + at, e, len);
        at += len;
        out[at] = '\0';
    }
}

static void list_lanes(char out[LIST_CAP], const uint8_t *lanes, unsigned n) {
    uint64_t v[PCIE_MAX_LANES] = {0};
    for (unsigned i = 0; i < n && i < PCIE_MAX_LANES; i++) {
        v[i] = lanes[i];
    }
    list_u64(out, v, n);
}

static void emit(pcie_lanes *ln, uint64_t start_fs, uint64_t end_fs, const char *label,
                 const char *fields, bool is_error) {
    pcie_detector_emit(ln->det, start_fs, end_fs, label, fields, is_error);
}

// §12.11: the run of withheld symbols in progress, if it counts any.
static void end_run(pcie_lanes *ln) {
    if (!ln->run_open) {
        return;
    }
    ln->run_open = false;
    if (ln->run_count == 0) {
        return;
    }
    wcx_json j;
    wcx_json_begin(&j, pcie_detector_arena(ln->det));
    wcx_json_add_str(&j, "type", "unaligned");
    wcx_json_add_u64(&j, "symbols", ln->run_count);
    if (ln->run_error) {
        wcx_json_add_str(&j, "error", "Packet data after lane alignment was lost is not decoded.");
    }
    emit(ln, ln->run_start_fs, ln->run_end_fs,
         ln->run_error ? "Packet data after lane alignment lost"
                       : "Packet data before lane alignment",
         wcx_json_end(&j), ln->run_error);
}

static void run_add(pcie_lanes *ln, const pcie_lane_item *it) {
    if (!ln->run_open) {
        ln->run_open = true;
        ln->run_error = ln->skew_since_lock;
        ln->run_count = 0;
        ln->run_start_fs = it->start_fs;
    }
    ln->run_count++;
    ln->run_end_fs = it->end_fs;
}

static void emit_skew(pcie_lanes *ln, unsigned lane, uint64_t start_fs, uint64_t end_fs) {
    end_run(ln);
    ln->skew_since_lock = true;
    ln->skew_reported = true;
    wcx_arena *a = pcie_detector_arena(ln->det);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "lane_skew");
    wcx_json_add_u64(&j, "lane", lane);
    wcx_json_add_str(&j, "error", "The lane did not line up with the other lanes of the link.");
    emit(ln, start_fs, end_fs, wcx_arena_printf(a, "Lanes not aligned (lane %u)", lane),
         wcx_json_end(&j), true);
}

static void emit_xz(pcie_lanes *ln, unsigned lane, const pcie_lane_item *gap) {
    wcx_json j;
    wcx_json_begin(&j, pcie_detector_arena(ln->det));
    wcx_json_add_str(&j, "type", "unknown_value");
    wcx_json_add_u64(&j, "edges", gap->edges);
    wcx_json_add_u64(&j, "lane", lane);
    wcx_json_add_str(&j, "error", "The data or datak bus carries X or Z.");
    emit(ln, gap->start_fs, gap->end_fs, "X/Z on data", wcx_json_end(&j), true);
}

// ── the link (§12.5) ───────────────────────────────────────────────────────

static bool is_pad(const pcie_lane_os *o, unsigned at) {
    return (o->ts_kmask & (1u << at)) != 0 && o->ts[at] == PCIE_K_PAD;
}

static bool is_link_width(unsigned w) {
    return w == 1u || w == 2u || w == 4u || w == 8u || w == 12u || w == 16u;
}

static unsigned width_below(unsigned w) {
    static const unsigned k_widths[] = {16u, 12u, 8u, 4u, 2u, 1u};
    for (size_t i = 0; i < sizeof k_widths / sizeof k_widths[0]; i++) {
        if (k_widths[i] < w) {
            return k_widths[i];
        }
    }
    return 1u; /* defensive: w >= 2 here */
}

// The span of a round: its present ordered sets, earliest start to latest end.
typedef struct round_span {
    uint64_t start_fs;
    uint64_t end_fs;
} round_span;

// The link's lanes in logical order: physical order, or by Lane# when the
// Lane#s are exactly 0..w-1 (lane_order=auto, rule 1). Returns w.
static unsigned link_order(pcie_lanes *ln, uint16_t set, bool from_skp,
                           uint8_t order[PCIE_MAX_LANES]) {
    unsigned w = 0;
    for (unsigned l = 0; l < ln->bound; l++) {
        if ((set & (1u << l)) != 0) {
            order[w++] = (uint8_t)l;
        }
    }
    if (from_skp || !ln->order_auto) {
        return w;
    }
    uint8_t by_lane[PCIE_MAX_LANES] = {0};
    uint16_t seen = 0;
    for (unsigned i = 0; i < w; i++) {
        const unsigned number = front(lane_of(ln, order[i]))->os.ts[TS_LANE];
        if (number >= w || (seen & (1u << number)) != 0) {
            return w;
        }
        seen = (uint16_t)(seen | (1u << number));
        by_lane[number] = order[i];
    }
    memcpy(order, by_lane, w);
    return w;
}

static void emit_link_width(pcie_lanes *ln, const char *lanes, bool from_skp,
                            const round_span *span) {
    wcx_arena *a = pcie_detector_arena(ln->det);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "link_width");
    wcx_json_add_u64(&j, "width", ln->width);
    wcx_json_add_u64(&j, "bound", ln->bound);
    wcx_json_add_str(&j, "lanes", lanes);
    wcx_json_add_str(&j, "from", from_skp ? "skp" : "ts2");
    emit(ln, span->start_fs, span->end_fs,
         from_skp ? wcx_arena_printf(a, "Link width x%u (%u bound, from SKP)", ln->width, ln->bound)
                  : wcx_arena_printf(a, "Link width x%u (%u bound)", ln->width, ln->bound),
         wcx_json_end(&j), false);
}

// §12.5: lane_order when the order is not physical and differs from the
// last link's, or is physical again after one that was not.
static void update_lane_order(pcie_lanes *ln, const char *lanes, const round_span *span) {
    bool physical = true;
    for (unsigned i = 1; i < ln->width; i++) {
        if (ln->link[i] < ln->link[i - 1u]) {
            physical = false;
        }
    }
    const bool differs =
        ln->width != ln->last_order_n || memcmp(ln->link, ln->last_order, ln->width) != 0;
    if (!ln->dll &&
        ((!physical && (ln->order_physical || differs)) || (physical && !ln->order_physical))) {
        wcx_arena *a = pcie_detector_arena(ln->det);
        wcx_json j;
        wcx_json_begin(&j, a);
        wcx_json_add_str(&j, "type", "lane_order");
        wcx_json_add_str(&j, "order", lanes);
        emit(ln, span->start_fs, span->end_fs, wcx_arena_printf(a, "Lane order %s", lanes),
             wcx_json_end(&j), false);
    }
    ln->order_physical = physical;
    memcpy(ln->last_order, ln->link, sizeof ln->last_order);
    ln->last_order_n = ln->width;
}

static void apply_link(pcie_lanes *ln, uint16_t set, bool from_skp, const round_span *span) {
    uint8_t order[PCIE_MAX_LANES] = {0};
    unsigned w = link_order(ln, set, from_skp, order);
    // Width: keep the first w' lanes in logical order.
    if (!is_link_width(w)) {
        const unsigned keep = width_below(w);
        for (unsigned i = keep; i < w; i++) {
            const pcie_lane_item *it = front(lane_of(ln, order[i]));
            emit_skew(ln, order[i], it->start_fs, it->end_fs);
        }
        w = keep;
    }
    const bool changed = !ln->decided || w != ln->width || memcmp(order, ln->link, w) != 0;
    ln->decided = true;
    ln->width = w;
    memcpy(ln->link, order, sizeof order);
    // §12.4: after a TS2 run only the link's lanes are considered; a link
    // from SKP keeps every bound lane, so a later SKP can widen it.
    if (!from_skp) {
        ln->by_ts2 = true;
        ln->considered = 0;
        for (unsigned i = 0; i < w; i++) {
            ln->considered = (uint16_t)(ln->considered | (1u << order[i]));
        }
    }
    ln->skew_reported = false;
    char lanes[LIST_CAP] = {0};
    list_lanes(lanes, order, w);
    if (changed && !ln->dll) {
        emit_link_width(ln, lanes, from_skp, span);
    }
    update_lane_order(ln, lanes, span);
}

// ── rounds (§12.4-§12.6, §12.9) ────────────────────────────────────────────

static bool lane_considered(const pcie_lanes *ln, unsigned l) {
    return (ln->considered & (1u << l)) != 0;
}

static bool in_link(const pcie_lanes *ln, unsigned l) {
    for (unsigned i = 0; ln->decided && i < ln->width; i++) {
        if (ln->link[i] == l) {
            return true;
        }
    }
    return false;
}

// §12.5 rule 1: the lane's next item after its head ordered set, skipping
// SKP ordered sets. One in progress counts once its type is known. After
// TS2_SKP_LOOKAHEAD of them the run ends, as if a row symbol followed.
typedef enum ts2_next { NEXT_NOT_OS, NEXT_OS, NEXT_PENDING } ts2_next;

static ts2_next next_after_skps(pcie_lane *l, bool flushing) {
    unsigned skps = 0;
    for (size_t i = 1; i < l->count; i++) {
        const pcie_lane_item *it = item_at(l, i);
        if (it->kind != PCIE_ITEM_OS) {
            return NEXT_NOT_OS;
        }
        if (it->os.type != PCIE_OS_SKP) {
            return NEXT_OS;
        }
        if (++skps == TS2_SKP_LOOKAHEAD) {
            return NEXT_NOT_OS;
        }
    }
    switch (l->pst) {
        case LPS_OUT:
            return flushing ? NEXT_NOT_OS : NEXT_PENDING;
        case LPS_OS_START: // COM only: it may still be a SKP OS
        case LPS_SKP:
            return NEXT_PENDING;
        case LPS_TS:
        case LPS_FTS:
        case LPS_IDL:
        case LPS_EIE:
        case LPS_EIE_END:
        default:
            return NEXT_OS;
    }
}

// §12.9: the first difference between a lane's ordered set and the round's.
static bool os_differs(const pcie_lane_os *ref, const pcie_lane_os *o,
                       char expected[PCIE_MISMATCH_CAP], char got[PCIE_MISMATCH_CAP]) {
    if (ref->type != o->type) {
        WCX_IGNORE(wcx_str_copy(expected, PCIE_MISMATCH_CAP, k_os_names[ref->type]));
        WCX_IGNORE(wcx_str_copy(got, PCIE_MISMATCH_CAP, k_os_names[o->type]));
        return true;
    }
    if (ref->type != PCIE_OS_TS1 && ref->type != PCIE_OS_TS2) {
        return false;
    }
    if (ref->ts[TS_NFTS] != o->ts[TS_NFTS]) {
        WCX_IGNORE(
            wcx_str_format(expected, PCIE_MISMATCH_CAP, "n_fts=%u", (unsigned)ref->ts[TS_NFTS]));
        WCX_IGNORE(wcx_str_format(got, PCIE_MISMATCH_CAP, "n_fts=%u", (unsigned)o->ts[TS_NFTS]));
        return true;
    }
    if (ref->ts[TS_RATE] != o->ts[TS_RATE]) {
        WCX_IGNORE(wcx_str_format(expected, PCIE_MISMATCH_CAP, "rate_id=0x%02X",
                                  (unsigned)ref->ts[TS_RATE]));
        WCX_IGNORE(
            wcx_str_format(got, PCIE_MISMATCH_CAP, "rate_id=0x%02X", (unsigned)o->ts[TS_RATE]));
        return true;
    }
    if (ref->ts[TS_CTRL] != o->ts[TS_CTRL]) {
        WCX_IGNORE(wcx_str_format(expected, PCIE_MISMATCH_CAP, "training_control=0x%02X",
                                  (unsigned)ref->ts[TS_CTRL]));
        WCX_IGNORE(wcx_str_format(got, PCIE_MISMATCH_CAP, "training_control=0x%02X",
                                  (unsigned)o->ts[TS_CTRL]));
        return true;
    }
    return false;
}

static void check_mismatch(pcie_lanes *ln, unsigned lane, unsigned ref) {
    pcie_lane *l = lane_of(ln, lane);
    const pcie_lane_item *it = front(l);
    char expected[PCIE_MISMATCH_CAP] = {0};
    char got[PCIE_MISMATCH_CAP] = {0};
    if (!os_differs(&front(lane_of(ln, ref))->os, &it->os, expected, got)) {
        l->mm_have = false;
        return;
    }
    if (l->mm_have && strcmp(l->mm_expected, expected) == 0 && strcmp(l->mm_got, got) == 0) {
        return; // the same difference as in its previous round
    }
    l->mm_have = true;
    WCX_IGNORE(wcx_str_copy(l->mm_expected, sizeof l->mm_expected, expected));
    WCX_IGNORE(wcx_str_copy(l->mm_got, sizeof l->mm_got, got));
    if (ln->dll) {
        return;
    }
    wcx_arena *a = pcie_detector_arena(ln->det);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "lane_mismatch");
    wcx_json_add_u64(&j, "lane", lane);
    wcx_json_add_str(&j, "expected", expected);
    wcx_json_add_str(&j, "got", got);
    wcx_json_add_str(&j, "error", "The lanes of one link sent different ordered sets.");
    emit(ln, it->start_fs, it->end_fs,
         wcx_arena_printf(a, "Lane %u differs: %s, lane %u %s", lane, got, ref, expected),
         wcx_json_end(&j), true);
}

static void emit_aligned(pcie_lanes *ln, const uint64_t *offsets, const round_span *span) {
    ln->last_width = ln->width;
    memcpy(ln->last_offsets, offsets, sizeof ln->last_offsets);
    if (ln->dll) {
        return;
    }
    wcx_arena *a = pcie_detector_arena(ln->det);
    char list[LIST_CAP] = {0};
    list_u64(list, offsets, ln->width);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "lanes_aligned");
    wcx_json_add_u64(&j, "width", ln->width);
    wcx_json_add_str(&j, "offsets", list);
    emit(ln, span->start_fs, span->end_fs,
         wcx_arena_printf(a, "Lanes aligned x%u (offsets %s)", ln->width, list), wcx_json_end(&j),
         false);
}

static void align_round(pcie_lanes *ln, uint16_t members, const round_span *span) {
    unsigned absent = PCIE_MAX_LANES;
    uint64_t lowest = UINT64_MAX;
    for (unsigned i = 0; i < ln->width; i++) {
        const unsigned l = ln->link[i];
        if ((members & (1u << l)) == 0) {
            if (absent == PCIE_MAX_LANES) {
                absent = l;
            }
        } else if (front(lane_of(ln, l))->index < lowest) {
            lowest = front(lane_of(ln, l))->index;
        }
    }
    if (absent != PCIE_MAX_LANES) {
        // §12.6: loss, or a round still incomplete after one.
        if (ln->aligned) {
            // §12.6: a loss is a pipeline break of kind unknown, before
            // the round's ordered set is fed.
            ln->aligned = false;
            emit_skew(ln, absent, span->start_fs, span->end_fs);
            pcie_detector_break(ln->det, PCIE_BREAK_UNKNOWN);
        } else if (!ln->skew_reported) {
            emit_skew(ln, absent, span->start_fs, span->end_fs);
        }
        return;
    }
    uint64_t offsets[PCIE_MAX_LANES] = {0};
    for (unsigned i = 0; i < ln->width; i++) {
        offsets[i] = front(lane_of(ln, ln->link[i]))->index - lowest;
    }
    if (!ln->aligned) {
        end_run(ln);
        ln->aligned = true;
        ln->skew_since_lock = false;
        ln->skew_reported = false;
        emit_aligned(ln, offsets, span);
    } else if (ln->width != ln->last_width ||
               memcmp(offsets, ln->last_offsets, sizeof offsets) != 0) {
        emit_aligned(ln, offsets, span);
    }
    // §12.6: after a SKP round the offsets to compare with are taken again,
    // silently, at the end of each lane's SKP OS, so an elastic buffer that
    // adds or removes SKP symbols changes nothing.
    uint64_t lowest_end = UINT64_MAX;
    for (unsigned i = 0; i < ln->width; i++) {
        const pcie_lane_item *it = front(lane_of(ln, ln->link[i]));
        if (it->os.type != PCIE_OS_SKP) {
            return;
        }
        const uint64_t end = it->index + it->os.n;
        lowest_end = end < lowest_end ? end : lowest_end;
    }
    for (unsigned i = 0; i < ln->width; i++) {
        const pcie_lane_item *it = front(lane_of(ln, ln->link[i]));
        ln->last_offsets[i] = it->index + it->os.n - lowest_end;
    }
}

// §12.7, §12.9: the round's ordered set to the pipeline, with every bound
// lane's Link#, Lane# and SKP count.
static void forward_os(pcie_lanes *ln, unsigned ref, uint16_t members) {
    pcie_os_lanes *ann = &ln->ann;
    memset(ann, 0, sizeof *ann);
    ann->bound = (uint8_t)ln->bound;
    for (unsigned l = 0; l < ln->bound && l < PCIE_MAX_LANES; l++) {
        if ((members & (1u << l)) == 0) {
            continue;
        }
        const pcie_lane_os *o = &front(lane_of(ln, l))->os;
        const uint16_t bit = (uint16_t)(1u << l);
        if (o->type == PCIE_OS_TS1 || o->type == PCIE_OS_TS2) {
            ann->ts = (uint16_t)(ann->ts | bit);
            ann->link[l] = o->ts[TS_LINK];
            ann->lane[l] = o->ts[TS_LANE];
            if (is_pad(o, TS_LINK)) {
                ann->link_pad = (uint16_t)(ann->link_pad | bit);
            }
            if (is_pad(o, TS_LANE)) {
                ann->lane_pad = (uint16_t)(ann->lane_pad | bit);
            }
        } else if (o->type == PCIE_OS_SKP) {
            ann->skp = (uint16_t)(ann->skp | bit);
            ann->skp_count[l] = (uint8_t)o->skp;
        }
    }
    const pcie_lane_os *o = &front(lane_of(ln, ref))->os;
    pcie_detector_set_os_lanes(ln->det, ann);
    for (unsigned i = 0; i < o->n; i++) {
        pcie_sym s;
        memset(&s, 0, sizeof s);
        s.byte = o->byte[i];
        s.k = (o->kmask & (1u << i)) != 0;
        s.start_fs = o->start_fs[i];
        s.end_fs = o->end_fs[i];
        pcie_detector_symbol(ln->det, &s);
    }
    pcie_detector_set_os_lanes(ln->det, NULL);
    if (o->cut) {
        pcie_detector_break(ln->det, o->cut_by);
        // The gap that cut it follows on this lane: it has had its break.
        pcie_lane *l = lane_of(ln, ref);
        if (l->count >= 2u && item_at(l, 1)->kind == PCIE_ITEM_GAP) {
            item_at(l, 1)->broken = true;
        }
    }
}

// §12.4: the lowest present link lane in logical order, else the lowest
// present lane; with the link as it was before this round.
static unsigned round_ref(const pcie_lanes *ln, uint16_t members) {
    if (ln->decided) {
        for (unsigned i = 0; i < ln->width; i++) {
            if ((members & (1u << ln->link[i])) != 0) {
                return ln->link[i];
            }
        }
    }
    for (unsigned l = 0; l < ln->bound; l++) {
        if ((members & (1u << l)) != 0) {
            return l;
        }
    }
    return 0; /* defensive: a round always has its opener */
}

static round_span round_span_of(pcie_lanes *ln, uint16_t members) {
    round_span span = {UINT64_MAX, 0};
    for (unsigned l = 0; l < ln->bound; l++) {
        if ((members & (1u << l)) != 0) {
            const pcie_lane_item *it = front(lane_of(ln, l));
            span.start_fs = it->start_fs < span.start_fs ? it->start_fs : span.start_fs;
            span.end_fs = it->end_fs > span.end_fs ? it->end_fs : span.end_fs;
        }
    }
    return span;
}

// The present lanes whose ordered set is of `type`; for a TS2, only those
// with Link# and Lane# both not PAD (§12.5).
static uint16_t lanes_with(pcie_lanes *ln, uint16_t members, pcie_os_type type) {
    uint16_t set = 0;
    for (unsigned l = 0; l < ln->bound; l++) {
        if ((members & (1u << l)) == 0) {
            continue;
        }
        const pcie_lane_os *o = &front(lane_of(ln, l))->os;
        if (o->type == type &&
            (type != PCIE_OS_TS2 || (!is_pad(o, TS_LINK) && !is_pad(o, TS_LANE)))) {
            set = (uint16_t)(set | (1u << l));
        }
    }
    return set;
}

// §12.5: the two kinds of round that decide the link.
static void round_link(pcie_lanes *ln, uint16_t members, unsigned ref, const round_span *span) {
    const pcie_os_type type = front(lane_of(ln, ref))->os.type;
    if (type == PCIE_OS_TS2 && next_after_skps(lane_of(ln, ref), ln->flushing) == NEXT_NOT_OS) {
        const uint16_t set = lanes_with(ln, members, PCIE_OS_TS2);
        if (set != 0) {
            apply_link(ln, set, false, span);
        }
    } else if (!ln->seen_ts && type == PCIE_OS_SKP) {
        // §12.5 rule 2, only before any TS1/TS2 round: a wider link than
        // the current one, after the width rule; an undecided link has
        // width 0.
        const uint16_t set = lanes_with(ln, members, PCIE_OS_SKP);
        unsigned w = 0;
        for (unsigned l = 0; l < ln->bound; l++) {
            w += ((unsigned)set >> l) & 1u;
        }
        const unsigned usable = is_link_width(w) ? w : width_below(w);
        if (usable > (ln->decided ? ln->width : 0u)) {
            apply_link(ln, set, true, span);
        }
    }
    if (type == PCIE_OS_TS1 || type == PCIE_OS_TS2) {
        ln->seen_ts = true;
    }
}

// §12.9: the present considered lanes, in logical (else physical) order.
static void round_mismatch(pcie_lanes *ln, uint16_t members, unsigned ref) {
    const unsigned n = ln->decided ? ln->width : ln->bound;
    for (unsigned i = 0; i < n; i++) {
        const unsigned l = ln->decided ? ln->link[i] : i;
        if (l != ref && (members & (1u << l)) != 0) {
            check_mismatch(ln, l, ref);
        }
    }
}

static void process_round(pcie_lanes *ln, uint16_t members) {
    const unsigned ref = round_ref(ln, members);
    const round_span span = round_span_of(ln, members);
    round_mismatch(ln, members, ref);
    round_link(ln, members, ref, &span);
    if (ln->decided) {
        align_round(ln, members, &span);
    }
    forward_os(ln, ref, members);
    for (unsigned l = 0; l < ln->bound; l++) {
        if ((members & (1u << l)) != 0) {
            pop(lane_of(ln, l));
        }
    }
}

// §12.4: forms and processes the next round once it is settled.
// The index of the next round (§12.4): the lowest ordered set at the head of
// a lane that opens rounds (a link lane once the link is decided, else any
// considered lane). UINT64_MAX when there is none.
static uint64_t next_round_index(pcie_lanes *ln) {
    uint64_t c = UINT64_MAX;
    for (unsigned l = 0; l < ln->bound; l++) {
        const pcie_lane_item *it = head(lane_of(ln, l));
        if (lane_considered(ln, l) && (!ln->decided || in_link(ln, l)) && it != NULL &&
            it->kind == PCIE_ITEM_OS && it->index < c) {
            c = it->index;
        }
    }
    return c;
}

// §12.4: forms and processes the next round once it is settled.
static bool try_round(pcie_lanes *ln) {
    const uint64_t c = next_round_index(ln);
    if (c == UINT64_MAX) {
        return false;
    }
    const uint64_t last = pcie_time_add(c, ln->max_skew);
    if (!ln->flushing && ln->next_index < pcie_time_add(last, ROUND_SETTLE)) {
        return false;
    }
    const uint64_t first = c > ln->max_skew ? c - ln->max_skew : 0u;
    uint16_t members = 0;
    for (unsigned l = 0; l < ln->bound; l++) {
        pcie_lane *lane = lane_of(ln, l);
        if (!lane_considered(ln, l)) {
            continue;
        }
        const bool opener = !ln->decided || in_link(ln, l);
        // A considered lane outside the link joins within W either side of
        // c; its ordered sets more than W before c are dropped.
        while (!opener && lane->count > 0 && front(lane)->kind == PCIE_ITEM_OS &&
               front(lane)->index < first) {
            pop(lane);
        }
        const pcie_lane_item *it = head(lane);
        if (it != NULL && it->kind == PCIE_ITEM_OS && it->index <= last &&
            (opener || it->index >= first)) {
            members = (uint16_t)(members | (1u << l));
        }
    }
    // §12.5 rule 1: a TS2 round waits until its reference lane shows what
    // follows the TS2 and any SKP ordered sets after it.
    pcie_lane *ref = lane_of(ln, round_ref(ln, members));
    if (front(ref)->os.type == PCIE_OS_TS2 && next_after_skps(ref, ln->flushing) == NEXT_PENDING) {
        return false;
    }
    process_round(ln, members);
    return true;
}

// ── rows (§12.6, §12.7) ────────────────────────────────────────────────────

// An item's kind for pairing (§12.6): none (the lane is at its next ordered
// set), a row symbol, or a gap of one kind.
#define PAIR_KINDS 8u

static unsigned pair_kind(const pcie_lane_item *it) {
    if (it == NULL || it->kind == PCIE_ITEM_OS) {
        return 0u;
    }
    return it->kind == PCIE_ITEM_SYM ? 1u : (2u + (unsigned)it->gap) % PAIR_KINDS;
}

// §12.6: the kind most link lanes have; on a tie, the first in logical
// order, which puts logical lane 0's first.
static unsigned norm_kind(pcie_lanes *ln) {
    unsigned count[PAIR_KINDS] = {0};
    for (unsigned i = 0; i < ln->width; i++) {
        count[pair_kind(head(lane_of(ln, ln->link[i])))]++;
    }
    unsigned norm = pair_kind(head(lane_of(ln, ln->link[0])));
    for (unsigned i = 1; i < ln->width; i++) {
        const unsigned k = pair_kind(head(lane_of(ln, ln->link[i])));
        if (count[k] > count[norm]) {
            norm = k;
        }
    }
    return norm;
}

static void lose_at_row(pcie_lanes *ln) {
    const unsigned norm = norm_kind(ln);
    unsigned named = ln->link[0];
    for (unsigned i = 0; i < ln->width; i++) {
        if (pair_kind(head(lane_of(ln, ln->link[i]))) != norm) {
            named = ln->link[i];
            break;
        }
    }
    // The span: the named lane's item, else the first one of the norm kind.
    const pcie_lane_item *at = head(lane_of(ln, named));
    for (unsigned i = 0; i < ln->width && pair_kind(at) == 0u; i++) {
        const pcie_lane_item *it = head(lane_of(ln, ln->link[i]));
        if (pair_kind(it) == norm) {
            at = it;
        }
    }
    ln->aligned = false;
    emit_skew(ln, named, at != NULL ? at->start_fs : 0u, at != NULL ? at->end_fs : 0u);
    // §12.6: a loss is a pipeline break of kind unknown.
    pcie_detector_break(ln->det, PCIE_BREAK_UNKNOWN);
}

static void feed_row(pcie_lanes *ln) {
    uint64_t start = UINT64_MAX;
    uint64_t end = 0;
    for (unsigned i = 0; i < ln->width; i++) {
        const pcie_lane_item *it = front(lane_of(ln, ln->link[i]));
        start = it->start_fs < start ? it->start_fs : start;
        end = it->end_fs > end ? it->end_fs : end;
    }
    for (unsigned i = 0; i < ln->width; i++) {
        pcie_lane *l = lane_of(ln, ln->link[i]);
        const pcie_lane_item *it = front(l);
        pcie_sym s;
        memset(&s, 0, sizeof s);
        s.byte = it->byte;
        s.k = it->k;
        s.start_fs = start;
        s.end_fs = end;
        s.has_plain = !it->k;
        s.plain = it->plain;
        s.width = (uint8_t)ln->width;
        s.pos = (uint8_t)i;
        s.lane = ln->link[i];
        pcie_detector_symbol(ln->det, &s);
        pop(l);
    }
}

// The reference lane's gap reaching the pipeline (§12.7, §12.8): a break
// once, and when it is closed an `X/Z on data` for an unknown one. False
// while it is still open.
static bool ref_gap(pcie_lanes *ln, unsigned lane, pcie_lane_item *it) {
    if (!it->broken) {
        it->broken = true;
        pcie_detector_break(ln->det, it->gap);
    }
    if (it->open) {
        return false;
    }
    if (it->gap == PCIE_BREAK_UNKNOWN) {
        emit_xz(ln, lane, it);
    }
    return true;
}

// A considered lane outside the link (a link from SKP, §12.4): its row
// symbols and gaps produce nothing, and its ordered sets wait for a round.
// One that no round can take any more is dropped: a link lane's ordered set
// within W after it would have settled its round by index + 2W + 17.
static bool outside_drop(pcie_lanes *ln, unsigned l) {
    pcie_lane *lane = lane_of(ln, l);
    const pcie_lane_item *it = head(lane);
    if (it == NULL) {
        return false;
    }
    if (it->kind == PCIE_ITEM_OS) {
        const uint64_t settled =
            pcie_time_add(pcie_time_add(it->index, 2u * ln->max_skew), ROUND_SETTLE);
        if (ln->next_index <= settled) {
            return false;
        }
    }
    pop(lane);
    return true;
}

static bool outside_link(const pcie_lanes *ln, unsigned l) {
    return lane_considered(ln, l) && ln->decided && !in_link(ln, l);
}

static void drain_outside_link(pcie_lanes *ln) {
    for (unsigned l = 0; l < ln->bound; l++) {
        while (outside_link(ln, l) && outside_drop(ln, l)) {
        }
    }
}

static bool step_aligned(pcie_lanes *ln) {
    drain_outside_link(ln);
    bool any_empty = false;
    bool all_os = true;
    bool any_os = false;
    for (unsigned i = 0; i < ln->width; i++) {
        const pcie_lane_item *it = head(lane_of(ln, ln->link[i]));
        if (it == NULL) {
            any_empty = true;
            all_os = false;
        } else if (it->kind == PCIE_ITEM_OS) {
            any_os = true;
        } else {
            all_os = false;
        }
    }
    if (any_empty) {
        if (!ln->flushing) {
            return false;
        }
        // §12.13: a row some lane cannot complete is dropped without error,
        // but a gap of the reference lane in it still reaches the pipeline.
        bool popped = false;
        for (unsigned i = 0; i < ln->width; i++) {
            pcie_lane *l = lane_of(ln, ln->link[i]);
            while (l->count > 0 && front(l)->kind != PCIE_ITEM_OS) {
                if (i == 0 && front(l)->kind == PCIE_ITEM_GAP) {
                    (void)ref_gap(ln, ln->link[0], head(l));
                }
                pop(l);
                popped = true;
            }
        }
        return try_round(ln) || popped;
    }
    if (all_os) {
        return try_round(ln);
    }
    if (any_os) {
        lose_at_row(ln);
        return true;
    }
    const pcie_lane_item *first = front(lane_of(ln, ln->link[0]));
    bool same = true;
    for (unsigned i = 1; i < ln->width; i++) {
        if (pair_kind(head(lane_of(ln, ln->link[i]))) != pair_kind(first)) {
            same = false;
        }
    }
    if (!same) {
        lose_at_row(ln);
        return true;
    }
    if (first->kind == PCIE_ITEM_SYM) {
        feed_row(ln);
        return true;
    }
    // A common break (§12.6): the unaligned path below breaks the pipeline
    // at the reference lane's gap.
    ln->aligned = false;
    return true;
}

// While not aligned: withheld row symbols, the reference lane's gaps
// (§12.7, §12.8), then the next round.
static bool step_unaligned(pcie_lanes *ln) {
    bool progress = false;
    const unsigned ref = ln->decided ? ln->link[0] : 0u;
    // Other lanes first: their withheld items produce nothing.
    for (unsigned l = 0; l < ln->bound; l++) {
        pcie_lane *lane = lane_of(ln, l);
        if (!lane_considered(ln, l) || l == ref) {
            continue;
        }
        if (outside_link(ln, l)) {
            while (outside_drop(ln, l)) {
                progress = true;
            }
            continue;
        }
        while (lane->count > 0 && front(lane)->kind != PCIE_ITEM_OS) {
            pop(lane);
            progress = true;
        }
    }
    // §12.7: the reference lane's items reach the pipeline in index order
    // with the rounds, so only those before the next round go now, and those
    // before the lane's own ordered set when that one joins the round.
    uint64_t until = next_round_index(ln);
    pcie_lane *lane = lane_of(ln, ref);
    for (size_t i = 0; until != UINT64_MAX && i < lane->count; i++) {
        const pcie_lane_item *it = item_at(lane, i);
        if (it->kind == PCIE_ITEM_OS) {
            if (it->index <= pcie_time_add(until, ln->max_skew)) {
                until = UINT64_MAX;
            }
            break;
        }
    }
    while (lane->count > 0 && front(lane)->kind != PCIE_ITEM_OS && front(lane)->index < until) {
        pcie_lane_item *it = front(lane);
        if (it->kind == PCIE_ITEM_GAP) {
            const bool was_broken = it->broken;
            const bool closed = ref_gap(ln, ref, it);
            progress = progress || !was_broken;
            if (!closed) {
                break;
            }
        } else {
            run_add(ln, it);
        }
        pop(lane);
        progress = true;
    }
    return try_round(ln) || progress;
}

// Lanes outside the link: their items produce nothing (§12.4).
static void drain_ignored(pcie_lanes *ln) {
    for (unsigned l = 0; l < ln->bound; l++) {
        if (!lane_considered(ln, l)) {
            pcie_lane *lane = lane_of(ln, l);
            lane->head = 0;
            lane->count = 0;
        }
    }
}

static void run_lanes(pcie_lanes *ln) {
    bool progress = true;
    while (progress && !ln->failed) {
        drain_ignored(ln);
        progress = ln->aligned ? step_aligned(ln) : step_unaligned(ln);
    }
}

// ── public ─────────────────────────────────────────────────────────────────

bool pcie_lanes_init(pcie_lanes *ln, wcx_decoder *d, pcie_detector *det, unsigned bound, unsigned n,
                     unsigned max_skew, bool order_auto, bool dll) {
    memset(ln, 0, sizeof *ln);
    ln->d = d;
    ln->det = det;
    ln->dll = dll;
    ln->bound = bound < PCIE_MAX_LANES ? bound : PCIE_MAX_LANES;
    ln->n = n;
    ln->max_skew = max_skew;
    ln->order_auto = order_auto;
    ln->order_physical = true;
    for (unsigned l = 0; l < ln->bound && l < PCIE_MAX_LANES; l++) {
        ln->considered = (uint16_t)(ln->considered | (1u << l));
        pcie_scrambler_reset(&lane_of(ln, l)->lfsr);
        ln->lane[l].items = calloc(PCIE_LANE_ITEMS, sizeof *ln->lane[l].items);
        if (ln->lane[l].items == NULL) {
            return false; /* defensive: allocation failure */
        }
    }
    return true;
}

void pcie_lanes_free(pcie_lanes *ln) {
    for (unsigned l = 0; l < PCIE_MAX_LANES; l++) {
        free(ln->lane[l].items);
        ln->lane[l].items = NULL;
    }
}

void pcie_lanes_edge(pcie_lanes *ln, const pcie_lane_in *in, uint64_t edge_fs, uint64_t period_fs) {
    if (ln->failed) {
        return;
    }
    const uint64_t duration = pcie_symbol_duration(period_fs, ln->n);
    const uint64_t start_fs = edge_fs;
    const uint64_t end_fs = pcie_edge_end(edge_fs, period_fs, ln->n);
    for (unsigned l = 0; l < ln->bound; l++) {
        pcie_lane *lane = lane_of(ln, l);
        if (in[l].gap) {
            lane_gap(ln, lane, in[l].gap_kind, ln->next_index, start_fs, end_fs);
            continue;
        }
        for (unsigned j = 0; j < ln->n; j++) {
            const uint64_t start = pcie_symbol_start(edge_fs, period_fs, j, ln->n);
            lane_symbol(ln, lane, (uint8_t)((in[l].data >> (8u * j)) & 0xFFu),
                        ((in[l].datak >> j) & 1u) != 0, ln->next_index + j, start,
                        pcie_symbol_end(start, duration));
        }
    }
    ln->next_index += ln->n;
    run_lanes(ln);
}

void pcie_lanes_flush(pcie_lanes *ln) {
    if (ln->failed) {
        return;
    }
    for (unsigned l = 0; l < ln->bound; l++) {
        pcie_lane *lane = lane_of(ln, l);
        if (lane->pst != LPS_OUT) {
            // §12.3: cut by the end of the trace; the pipeline's own flush
            // reports it (§10.6), so no break follows it.
            os_finish(ln, lane, lane->pst == LPS_SKP ? PCIE_OS_SKP : PCIE_OS_UNKNOWN);
        }
        close_gap(lane);
    }
    ln->flushing = true;
    run_lanes(ln);
    end_run(ln);
}
