// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Section references are to decoders/pcie/SPEC.md.

#include "pcie_pipe_fe.h"

#include <string.h>

#include "pcie_fe_common.h"
#include "wcx/json_writer.h"
#include "wcx/str.h"

// TS1/TS2 symbol positions within pcie_ts.syms (symbol n is syms[n-1];
// protocol-notes.md §2).
#define TS_LINK     0
#define TS_LANE     1
#define TS_NFTS     2
#define TS_RATE     3
#define TS_CTRL     4
#define SKP_NOMINAL 3u // a SKP OS as sent carries 3 SKP symbols (§7)

// Data Rate Identifier bit 2: 5.0 GT/s supported (protocol-notes.md §2).
#define RATE_GEN2 0x04u

void pcie_pipe_fe_init(pcie_pipe_fe *fe, pcie_sink *sink, bool coalesce, bool show_idle) {
    memset(fe, 0, sizeof *fe);
    fe->sink = sink;
    fe->coalesce = coalesce;
    fe->show_idle = show_idle;
}

// Link# and Lane# are "PAD" when the symbol is the K symbol PAD, else
// decimal (§7).
static bool ts_is_pad(const pcie_ts *ts, unsigned at) {
    return (ts->kmask & (1u << at)) != 0 && ts->syms[at] == PCIE_K_PAD;
}

static void ts_number(const pcie_ts *ts, unsigned at, char out[WCX_FMT_U64_CAP]) {
    if (ts_is_pad(ts, at)) {
        WCX_IGNORE(wcx_str_copy(out, WCX_FMT_U64_CAP, "PAD"));
    } else {
        (void)wcx_fmt_u64(out, ts->syms[at]);
    }
}

static void ts_add_number(wcx_json *j, const char *key, const pcie_ts *ts, unsigned at) {
    if (ts_is_pad(ts, at)) {
        wcx_json_add_str(j, key, "PAD");
    } else {
        wcx_json_add_u64(j, key, ts->syms[at]);
    }
}

// ── multi-lane lists (§12.9) ───────────────────────────────────────────────

// One entry per bound lane, comma-separated: 16 entries of at most 3
// characters and 15 commas.
#define LANE_LIST_CAP 72u

typedef enum lane_list_kind { LIST_LINK, LIST_LANE, LIST_SKP } lane_list_kind;

static void list_entry(const pcie_os_lanes *l, unsigned lane, lane_list_kind kind,
                       char out[WCX_FMT_U64_CAP]) {
    const uint16_t bit = (uint16_t)(1u << lane);
    if (kind == LIST_SKP) {
        if ((l->skp & bit) == 0) {
            WCX_IGNORE(wcx_str_copy(out, WCX_FMT_U64_CAP, "-"));
        } else {
            (void)wcx_fmt_u64(out, l->skp_count[lane]);
        }
        return;
    }
    const uint16_t pad = kind == LIST_LINK ? l->link_pad : l->lane_pad;
    if ((l->ts & bit) == 0) {
        WCX_IGNORE(wcx_str_copy(out, WCX_FMT_U64_CAP, "-"));
    } else if ((pad & bit) != 0) {
        WCX_IGNORE(wcx_str_copy(out, WCX_FMT_U64_CAP, "PAD"));
    } else {
        (void)wcx_fmt_u64(out, kind == LIST_LINK ? l->link[lane] : l->lane[lane]);
    }
}

static void lane_list(const pcie_os_lanes *l, lane_list_kind kind, char out[LANE_LIST_CAP]) {
    size_t at = 0;
    out[0] = '\0';
    for (unsigned lane = 0; lane < l->bound && lane < PCIE_MAX_LANES; lane++) {
        char e[WCX_FMT_U64_CAP] = {0};
        list_entry(l, lane, kind, e);
        const size_t n = wcx_str_len(e, sizeof e);
        if (at + n + 2u > LANE_LIST_CAP) {
            break; /* defensive: 16 entries always fit */
        }
        if (lane > 0) {
            out[at++] = ',';
        }
        memcpy(out + at, e, n);
        at += n;
        out[at] = '\0';
    }
}

// True when every entry other than "-" equals `value` (set to the first
// such entry).
static bool list_agrees(const pcie_os_lanes *l, lane_list_kind kind, char value[WCX_FMT_U64_CAP]) {
    bool have = false;
    value[0] = '\0';
    for (unsigned lane = 0; lane < l->bound && lane < PCIE_MAX_LANES; lane++) {
        char e[WCX_FMT_U64_CAP] = {0};
        list_entry(l, lane, kind, e);
        if (e[0] == '-') {
            continue;
        }
        if (!have) {
            WCX_IGNORE(wcx_str_copy(value, WCX_FMT_U64_CAP, e));
            have = true;
        } else if (strcmp(e, value) != 0) {
            return false;
        }
    }
    return true;
}

// §12.9: two TS rounds coalesce when their `links` and `lanes` lists match;
// what other lanes sent instead of a TS is not compared.
static bool same_ts_lists(const pcie_os_lanes *a, const pcie_os_lanes *b) {
    if (a->bound != b->bound || a->ts != b->ts || a->link_pad != b->link_pad ||
        a->lane_pad != b->lane_pad) {
        return false;
    }
    for (unsigned lane = 0; lane < a->bound && lane < PCIE_MAX_LANES; lane++) {
        if ((a->ts & (1u << lane)) != 0 &&
            (a->link[lane] != b->link[lane] || a->lane[lane] != b->lane[lane])) {
            return false;
        }
    }
    return true;
}

static void emit_ts(pcie_pipe_fe *fe, const pcie_ts *ts, uint64_t count, uint64_t start_fs,
                    uint64_t end_fs) {
    wcx_arena *a = pcie_sink_arena(fe->sink);
    const unsigned ctrl = ts->syms[TS_CTRL];
    const char *name = ts->ts2 ? "TS2" : "TS1";
    const char *gen = (ts->syms[TS_RATE] & RATE_GEN2) != 0 ? "gen2" : "gen1";
    const bool multi = ts->lanes.bound > 1u;
    char link[LANE_LIST_CAP] = {0};
    char lane[LANE_LIST_CAP] = {0};
    char links[LANE_LIST_CAP] = {0};
    char lanes[LANE_LIST_CAP] = {0};
    const char *link_key = "link";
    if (multi) {
        // §12.9: `link=X` when the lanes agree, else `links=`; always `lanes=`.
        lane_list(&ts->lanes, LIST_LINK, links);
        lane_list(&ts->lanes, LIST_LANE, lanes);
        char agreed[WCX_FMT_U64_CAP] = {0};
        if (list_agrees(&ts->lanes, LIST_LINK, agreed)) {
            WCX_IGNORE(wcx_str_copy(link, sizeof link, agreed));
        } else {
            WCX_IGNORE(wcx_str_copy(link, sizeof link, links));
            link_key = "links";
        }
        WCX_IGNORE(wcx_str_copy(lane, sizeof lane, lanes));
    } else {
        ts_number(ts, TS_LINK, link);
        ts_number(ts, TS_LANE, lane);
    }
    const char *lane_key = multi ? "lanes" : "lane";
    const char *label =
        count > 1 ? wcx_arena_printf(a, "%s " PCIE_TIMES "%llu %s=%s %s=%s n_fts=%u %s", name,
                                     (unsigned long long)count, link_key, link, lane_key, lane,
                                     (unsigned)ts->syms[TS_NFTS], gen)
                  : wcx_arena_printf(a, "%s %s=%s %s=%s n_fts=%u %s", name, link_key, link,
                                     lane_key, lane, (unsigned)ts->syms[TS_NFTS], gen);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", ts->ts2 ? "ts2" : "ts1");
    wcx_json_add_u64(&j, "count", count);
    if (multi) {
        wcx_json_add_str(&j, "links", links);
        wcx_json_add_str(&j, "lanes", lanes);
    } else {
        ts_add_number(&j, "link", ts, TS_LINK);
        ts_add_number(&j, "lane", ts, TS_LANE);
    }
    wcx_json_add_u64(&j, "n_fts", ts->syms[TS_NFTS]);
    wcx_json_add_hex(&j, "rate_id", ts->syms[TS_RATE], 2);
    wcx_json_add_hex(&j, "training_control", ctrl, 2);
    // Training Control bits (protocol-notes.md §2).
    wcx_json_add_bool(&j, "hot_reset", (ctrl & 0x01u) != 0);
    wcx_json_add_bool(&j, "disable_link", (ctrl & 0x02u) != 0);
    wcx_json_add_bool(&j, "loopback", (ctrl & 0x04u) != 0);
    wcx_json_add_bool(&j, "disable_scrambling", (ctrl & 0x08u) != 0);
    wcx_json_add_bool(&j, "compliance_receive", (ctrl & 0x10u) != 0);
    pcie_sink_emit(fe->sink, start_fs, end_fs, label, wcx_json_end(&j), false);
}

// FTS OS and EIEOS: "FTS" / "EIEOS", with " ×N" when N > 1 (§7).
static void emit_counted(pcie_pipe_fe *fe, const char *name, const char *type, uint64_t count,
                         uint64_t start_fs, uint64_t end_fs) {
    wcx_arena *a = pcie_sink_arena(fe->sink);
    const char *label =
        count > 1 ? wcx_arena_printf(a, "%s " PCIE_TIMES "%llu", name, (unsigned long long)count)
                  : name;
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", type);
    wcx_json_add_u64(&j, "count", count);
    pcie_sink_emit(fe->sink, start_fs, end_fs, label, wcx_json_end(&j), false);
}

static void emit_run(pcie_pipe_fe *fe, pcie_event_kind kind, const pcie_ts *ts, uint64_t count,
                     uint64_t start_fs, uint64_t end_fs) {
    switch (kind) {
        case PCIE_EV_TS:
            emit_ts(fe, ts, count, start_fs, end_fs);
            break;
        case PCIE_EV_FTS:
            emit_counted(fe, "FTS", "fts", count, start_fs, end_fs);
            break;
        case PCIE_EV_EIEOS:
            emit_counted(fe, "EIEOS", "eieos", count, start_fs, end_fs);
            break;
        case PCIE_EV_SKP:
        case PCIE_EV_EIOS:
        case PCIE_EV_IDLE_RUN:
        case PCIE_EV_STRAY_RUN:
        case PCIE_EV_PACKET:
        case PCIE_EV_INVALID_K:
        case PCIE_EV_TRUNCATED:
        case PCIE_EV_UNMATCHED_END:
        case PCIE_EV_K_IN_PACKET:
        case PCIE_EV_UNEXPECTED_K:
        case PCIE_EV_UNKNOWN_OS:
        case PCIE_EV_FRAMING_LANE:
        case PCIE_EV_PAD_MISPLACED:
        case PCIE_EV_BREAK:
        default: /* defensive: only the three coalescable kinds reach here */
            break;
    }
}

static void flush_pending(pcie_pipe_fe *fe) {
    if (!fe->pending) {
        return;
    }
    fe->pending = false;
    emit_run(fe, fe->pending_kind, &fe->pending_ts, fe->pending_count, fe->pending_start_fs,
             fe->pending_end_fs);
}

// A TS joins the pending run only when all 15 symbols match (§7), and on a
// multi-lane link both per-lane lists too (§12.9).
static bool same_ts(const pcie_ts *a, const pcie_ts *b) {
    return a->kmask == b->kmask && memcmp(a->syms, b->syms, sizeof a->syms) == 0 &&
           same_ts_lists(&a->lanes, &b->lanes);
}

static void coalescable(pcie_pipe_fe *fe, const pcie_event *ev) {
    if (!fe->coalesce) {
        emit_run(fe, ev->kind, &ev->u.ts, 1, ev->start_fs, ev->end_fs);
        return;
    }
    if (fe->pending && fe->pending_kind == ev->kind &&
        (ev->kind != PCIE_EV_TS || same_ts(&fe->pending_ts, &ev->u.ts))) {
        fe->pending_count++;
        fe->pending_end_fs = ev->end_fs;
        return;
    }
    flush_pending(fe);
    fe->pending = true;
    fe->pending_kind = ev->kind;
    if (ev->kind == PCIE_EV_TS) {
        fe->pending_ts = ev->u.ts;
    }
    fe->pending_count = 1;
    fe->pending_start_fs = ev->start_fs;
    fe->pending_end_fs = ev->end_fs;
}

// §12.9: the SKP count of every bound lane; the label shows them unless
// every lane that has one has 3.
static void emit_skp_lanes(pcie_pipe_fe *fe, const pcie_event *ev) {
    wcx_arena *a = pcie_sink_arena(fe->sink);
    const pcie_os_lanes *l = &ev->lanes;
    char counts[LANE_LIST_CAP] = {0};
    lane_list(l, LIST_SKP, counts);
    bool nominal = true;
    for (unsigned lane = 0; lane < l->bound && lane < PCIE_MAX_LANES; lane++) {
        if ((l->skp & (1u << lane)) != 0 && l->skp_count[lane] != SKP_NOMINAL) {
            nominal = false;
        }
    }
    const char *label = nominal ? "SKP" : wcx_arena_printf(a, "SKP " PCIE_TIMES "%s", counts);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "skp");
    wcx_json_add_str(&j, "skp_symbols", counts);
    pcie_sink_emit(fe->sink, ev->start_fs, ev->end_fs, label, wcx_json_end(&j), false);
}

static void emit_skp(pcie_pipe_fe *fe, const pcie_event *ev) {
    if (ev->lanes.bound > 1u) {
        emit_skp_lanes(fe, ev);
        return;
    }
    wcx_arena *a = pcie_sink_arena(fe->sink);
    const char *label =
        ev->u.count == SKP_NOMINAL
            ? "SKP"
            : wcx_arena_printf(a, "SKP " PCIE_TIMES "%llu", (unsigned long long)ev->u.count);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "skp");
    wcx_json_add_u64(&j, "skp_symbols", ev->u.count);
    pcie_sink_emit(fe->sink, ev->start_fs, ev->end_fs, label, wcx_json_end(&j), false);
}

static void emit_eios(pcie_pipe_fe *fe, const pcie_event *ev) {
    wcx_json j;
    wcx_json_begin(&j, pcie_sink_arena(fe->sink));
    wcx_json_add_str(&j, "type", "eios");
    pcie_sink_emit(fe->sink, ev->start_fs, ev->end_fs, "EIOS", wcx_json_end(&j), false);
}

// "Idle" / "Idle ×N" (§7, §10.3: ×N only when N > 1).
static void emit_idle(pcie_pipe_fe *fe, const pcie_event *ev) {
    if (!fe->show_idle) {
        return;
    }
    wcx_arena *a = pcie_sink_arena(fe->sink);
    const char *label = ev->u.count > 1 ? wcx_arena_printf(a, "Idle " PCIE_TIMES "%llu",
                                                           (unsigned long long)ev->u.count)
                                        : "Idle";
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "idle");
    wcx_json_add_u64(&j, "symbols", ev->u.count);
    pcie_sink_emit_idle(fe->sink, ev->start_fs, ev->end_fs, label, wcx_json_end(&j));
}

// "TLP frame seq=5 (24 symbols)" / "DLLP frame (6 symbols)", ", EDB" when
// a TLP ended with EDB (§7). A DLLP ended by EDB is the dllp_edb error in
// both decoders (§10.7).
static void emit_frame(pcie_pipe_fe *fe, const pcie_event *ev) {
    const pcie_packet *k = &ev->u.packet;
    if (!k->tlp && k->edb) {
        pcie_fe_emit_dllp_edb(fe->sink, ev);
        return;
    }
    wcx_arena *a = pcie_sink_arena(fe->sink);
    const char *edb = k->edb ? ", EDB" : "";
    const bool has_seq = k->tlp && k->length >= 2u;
    const char *label = NULL;
    if (has_seq) {
        label =
            wcx_arena_printf(a, "TLP frame seq=%u (%llu symbols%s)",
                             (unsigned)pcie_tlp_seq(k->head), (unsigned long long)k->length, edb);
    } else {
        label = wcx_arena_printf(a, "%s frame (%llu symbols%s)", k->tlp ? "TLP" : "DLLP",
                                 (unsigned long long)k->length, edb);
    }
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", k->tlp ? "tlp_frame" : "dllp_frame");
    wcx_json_add_str(&j, "short_label", k->tlp ? "TLP" : "DLLP"); // §9.1
    if (has_seq) {
        wcx_json_add_u64(&j, "seq", pcie_tlp_seq(k->head));
    }
    wcx_json_add_u64(&j, "symbols", k->length);
    wcx_json_add_str(&j, "end", k->edb ? "EDB" : "END");
    pcie_sink_emit(fe->sink, ev->start_fs, ev->end_fs, label, wcx_json_end(&j), false);
}

void pcie_pipe_fe_event(void *ctx, const pcie_event *ev) {
    pcie_pipe_fe *fe = ctx;
    switch (ev->kind) {
        case PCIE_EV_TS:
        case PCIE_EV_FTS:
        case PCIE_EV_EIEOS:
            coalescable(fe, ev);
            return;
        case PCIE_EV_SKP:
            flush_pending(fe);
            emit_skp(fe, ev);
            return;
        case PCIE_EV_EIOS:
            flush_pending(fe);
            emit_eios(fe, ev);
            return;
        case PCIE_EV_IDLE_RUN:
            flush_pending(fe);
            emit_idle(fe, ev);
            return;
        case PCIE_EV_PACKET:
            flush_pending(fe);
            emit_frame(fe, ev);
            return;
        case PCIE_EV_STRAY_RUN:
        case PCIE_EV_INVALID_K:
        case PCIE_EV_TRUNCATED:
        case PCIE_EV_UNMATCHED_END:
        case PCIE_EV_K_IN_PACKET:
        case PCIE_EV_UNEXPECTED_K:
        case PCIE_EV_UNKNOWN_OS:
        case PCIE_EV_FRAMING_LANE:
        case PCIE_EV_PAD_MISPLACED:
            flush_pending(fe);
            pcie_fe_emit_error(fe->sink, ev);
            return;
        case PCIE_EV_BREAK:
            flush_pending(fe);
            return;
        default: /* defensive: every pcie_event_kind is handled above */
            return;
    }
}
