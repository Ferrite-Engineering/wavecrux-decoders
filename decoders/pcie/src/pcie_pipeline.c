// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Section references are to decoders/pcie/SPEC.md unless they name the PCIe
// Base Specification.

#include "pcie_pipeline.h"

#include <string.h>

#include "pcie_crc.h"

// Minimum TLP body: 2 sequence bytes + 3-DW header + 4 LCRC bytes (§8.2;
// PCIe Base 2.1 §3.5.2.1). DLLP body: 4 bytes + CRC-16 (§8.1).
#define TLP_MIN_BODY   18u
#define TLP_FRAME_OVER 6u // sequence bytes + LCRC around the TLP bytes
#define DLLP_BODY      6u

#define SKP_MAX  5u  // §5.3: 1 to 5 SKP symbols after COM
#define FTS_LEN  3u  // §5.3: COM + 3 FTS; EIOS: COM + 3 IDL
#define EIE_LEN  14u // §5.3: COM + 14 EIE + D10.2
#define TS_ID_AT 5u  // syms[5] is symbol 6, the identifier (§5.3)

// Training Control bits (protocol-notes.md §2).
#define TC_DISABLE_SCRAMBLING 0x08u

static void report(pcie_pipeline *p, pcie_event *ev) {
    p->fn(p->ctx, ev);
}

static void report_symbol_error(pcie_pipeline *p, pcie_event_kind kind, const pcie_sym *s,
                                uint8_t symbol) {
    pcie_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.start_fs = s->start_fs;
    ev.end_fs = s->end_fs;
    ev.u.symbol = symbol;
    report(p, &ev);
}

// ── runs of idle and stray data (§5.4, §6) ─────────────────────────────────

static void run_end(pcie_pipeline *p) {
    if (p->run == PCIE_RUN_NONE) {
        return;
    }
    pcie_event ev;
    memset(&ev, 0, sizeof ev);
    ev.start_fs = p->run_start_fs;
    ev.end_fs = p->run_end_fs;
    if (p->run == PCIE_RUN_IDLE) {
        ev.kind = PCIE_EV_IDLE_RUN;
        ev.u.count = p->run_count;
    } else {
        ev.kind = PCIE_EV_STRAY_RUN;
        ev.u.stray.count = p->run_count;
        ev.u.stray.first = p->run_first;
    }
    p->run = PCIE_RUN_NONE;
    report(p, &ev);
}

static void run_extend(pcie_pipeline *p, pcie_run_kind kind, const pcie_sym *s, uint8_t value) {
    if (p->run != kind) {
        run_end(p);
        p->run = kind;
        p->run_count = 0;
        p->run_first = value;
        p->run_start_fs = s->start_fs;
    }
    p->run_count++;
    p->run_end_fs = s->end_fs;
}

// ── packets (§5.4) ─────────────────────────────────────────────────────────

static void open_packet(pcie_pipeline *p, const pcie_sym *s, bool tlp) {
    memset(&p->pkt, 0, sizeof p->pkt);
    p->pkt.tlp = tlp;
    p->pkt.crc32_before_tail = PCIE_CRC32_INIT;
    p->tail_fill = 0;
    p->unit_start_fs = s->start_fs;
    p->unit_end_fs = s->end_fs;
    p->state = PST_PKT;
}

static void append_body(pcie_pipeline *p, uint8_t value) {
    pcie_packet *k = &p->pkt;
    if (k->length < PCIE_HEAD_KEEP) {
        k->head[k->length] = value;
    }
    // The CRC runs four bytes behind the data, so that at close it covers
    // everything but the LCRC still sitting in `tail`.
    if (p->tail_fill == 4u) {
        k->crc32_before_tail = pcie_crc32_update(k->crc32_before_tail, k->tail[0]);
        k->tail[0] = k->tail[1];
        k->tail[1] = k->tail[2];
        k->tail[2] = k->tail[3];
        k->tail[3] = value;
    } else {
        k->tail[p->tail_fill++] = value;
    }
    k->length++;
}

// §5.5: the lock criterion. An END-terminated DLLP of exactly 6 bytes whose
// CRC-16 residue is right, or an END-terminated TLP with a valid length
// whose LCRC matches. (A DLLP ended by EDB is an error, §10.2.)
static bool packet_crc_valid(const pcie_packet *k) {
    if (k->edb) {
        return false;
    }
    if (!k->tlp) {
        return k->length == DLLP_BODY && pcie_crc16(k->head, DLLP_BODY) == PCIE_CRC16_RESIDUE;
    }
    if (k->length < TLP_MIN_BODY || (k->length - TLP_FRAME_OVER) % 4u != 0) {
        return false;
    }
    uint8_t want[4];
    pcie_crc32_wire(k->crc32_before_tail, want);
    return memcmp(want, k->tail, sizeof want) == 0;
}

static void close_packet(pcie_pipeline *p, const pcie_sym *s, bool edb) {
    p->pkt.edb = edb;
    p->pkt.crc_valid = packet_crc_valid(&p->pkt);
    pcie_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = PCIE_EV_PACKET;
    ev.start_fs = p->unit_start_fs;
    ev.end_fs = s->end_fs;
    ev.u.packet = p->pkt;
    if (p->pkt.crc_valid) {
        p->step_valid_packet = true;
        p->step_start_fs = ev.start_fs;
        p->step_end_fs = ev.end_fs;
    }
    p->state = PST_OUT;
    report(p, &ev);
}

static void truncate_packet(pcie_pipeline *p, pcie_trunc_by by) {
    pcie_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = PCIE_EV_TRUNCATED;
    ev.start_fs = p->unit_start_fs;
    ev.end_fs = p->unit_end_fs;
    ev.u.truncated.tlp = p->pkt.tlp;
    ev.u.truncated.symbols = p->pkt.length;
    ev.u.truncated.by = by;
    p->state = PST_OUT;
    report(p, &ev);
}

// ── ordered sets (§5.3) ────────────────────────────────────────────────────

static void start_os(pcie_pipeline *p, const pcie_sym *s) {
    p->state = PST_OS_START;
    p->os_n = 0;
    p->os_last = s->byte;
    memset(&p->ts, 0, sizeof p->ts);
    p->unit_start_fs = s->start_fs;
    p->unit_end_fs = s->end_fs;
}

// The symbol did not fit any ordered set: one error spanning COM..end_fs,
// and decoding resynchronises at the next symbol.
static void unknown_os(pcie_pipeline *p, uint64_t end_fs, uint8_t symbol) {
    pcie_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = PCIE_EV_UNKNOWN_OS;
    ev.start_fs = p->unit_start_fs;
    ev.end_fs = end_fs;
    ev.u.symbol = symbol;
    p->state = PST_OUT;
    report(p, &ev);
}

static void complete_os(pcie_pipeline *p, uint64_t end_fs, pcie_event_kind kind, uint64_t count) {
    pcie_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = kind;
    ev.start_fs = p->unit_start_fs;
    ev.end_fs = end_fs;
    ev.u.count = count;
    p->state = PST_OUT;
    report(p, &ev);
}

static void complete_ts(pcie_pipeline *p, const pcie_sym *s) {
    const bool id_is_k = (p->ts.kmask & (1u << TS_ID_AT)) != 0;
    const uint8_t id = p->ts.syms[TS_ID_AT];
    if (id_is_k || (id != PCIE_D_TS1_ID && id != PCIE_D_TS2_ID)) {
        unknown_os(p, s->end_fs, id);
        return;
    }
    p->ts.ts2 = id == PCIE_D_TS2_ID;
    pcie_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = PCIE_EV_TS;
    ev.start_fs = p->unit_start_fs;
    ev.end_fs = s->end_fs;
    ev.u.ts = p->ts;
    // Training Control is symbol 5 = syms[4] (protocol-notes.md §2).
    if ((p->ts.syms[4] & TC_DISABLE_SCRAMBLING) != 0) {
        p->step_disable_scrambling = true;
        p->step_start_fs = ev.start_fs;
        p->step_end_fs = ev.end_fs;
    }
    p->state = PST_OUT;
    report(p, &ev);
}

static void ts_collect(pcie_pipeline *p, const pcie_sym *s, uint8_t value) {
    p->ts.syms[p->os_n] = value;
    if (s->k) {
        p->ts.kmask = (uint16_t)(p->ts.kmask | (1u << p->os_n));
    }
    p->os_n++;
    p->os_last = value;
    p->unit_end_fs = s->end_fs;
    p->state = PST_OS_TS;
    if (p->os_n == PCIE_TS_BODY) {
        complete_ts(p, s);
    }
}

// ── per-state symbol handlers ──────────────────────────────────────────────

static void out_symbol(pcie_pipeline *p, const pcie_sym *s, uint8_t value) {
    if (!s->k) {
        run_extend(p, value == PCIE_D_IDLE ? PCIE_RUN_IDLE : PCIE_RUN_STRAY, s, value);
        return;
    }
    run_end(p);
    switch (s->byte) {
        case PCIE_K_COM:
            start_os(p, s);
            break;
        case PCIE_K_STP:
        case PCIE_K_SDP:
            open_packet(p, s, s->byte == PCIE_K_STP);
            break;
        case PCIE_K_END:
        case PCIE_K_EDB:
            report_symbol_error(p, PCIE_EV_UNMATCHED_END, s, s->byte);
            break;
        case PCIE_K_PAD:
        case PCIE_K_SKP:
        case PCIE_K_FTS:
        case PCIE_K_IDL:
        case PCIE_K_EIE:
            // §10.2: a control symbol outside any packet or ordered set.
            report_symbol_error(p, PCIE_EV_UNEXPECTED_K, s, s->byte);
            break;
        default:
            report_symbol_error(p, PCIE_EV_INVALID_K, s, s->byte);
            break;
    }
}

static void pkt_symbol(pcie_pipeline *p, const pcie_sym *s, uint8_t value) {
    if (!s->k) {
        append_body(p, value);
        p->unit_end_fs = s->end_fs;
        return;
    }
    switch (s->byte) {
        case PCIE_K_END:
        case PCIE_K_EDB:
            close_packet(p, s, s->byte == PCIE_K_EDB);
            break;
        case PCIE_K_COM:
            truncate_packet(p, PCIE_TRUNC_COM);
            start_os(p, s);
            break;
        case PCIE_K_STP:
        case PCIE_K_SDP:
            truncate_packet(p, s->byte == PCIE_K_STP ? PCIE_TRUNC_STP : PCIE_TRUNC_SDP);
            open_packet(p, s, s->byte == PCIE_K_STP);
            break;
        default:
            // §10.2: any other K inside a packet drops the packet (no
            // truncation error); a non-K-code byte is invalid_k.
            p->state = PST_OUT;
            report_symbol_error(
                p, pcie_k_is_known(s->byte) ? PCIE_EV_K_IN_PACKET : PCIE_EV_INVALID_K, s, s->byte);
            break;
    }
}

// The symbol after COM decides which ordered set this is (§5.3).
static void os_start_symbol(pcie_pipeline *p, const pcie_sym *s, uint8_t value) {
    if (!s->k || s->byte == PCIE_K_PAD) {
        ts_collect(p, s, value);
        return;
    }
    p->unit_end_fs = s->end_fs;
    p->os_n = 1;
    p->os_last = s->byte;
    switch (s->byte) {
        case PCIE_K_SKP:
            p->state = PST_OS_SKP;
            break;
        case PCIE_K_FTS:
            p->state = PST_OS_FTS;
            break;
        case PCIE_K_IDL:
            p->state = PST_OS_IDL;
            break;
        case PCIE_K_EIE:
            p->state = PST_OS_EIE;
            break;
        default:
            unknown_os(p, s->end_fs, s->byte);
            break;
    }
}

// COM + 1..5 SKP, ending at the first non-SKP, which is then an ordinary
// symbol again (§5.3).
static void os_skp_symbol(pcie_pipeline *p, const pcie_sym *s, uint8_t value) {
    if (s->k && s->byte == PCIE_K_SKP) {
        p->os_n++;
        p->unit_end_fs = s->end_fs;
        if (p->os_n == SKP_MAX) {
            complete_os(p, s->end_fs, PCIE_EV_SKP, SKP_MAX);
        }
        return;
    }
    complete_os(p, p->unit_end_fs, PCIE_EV_SKP, p->os_n);
    out_symbol(p, s, value);
}

// COM + 3 FTS (FTS OS) or COM + 3 IDL (EIOS).
static void os_repeat_symbol(pcie_pipeline *p, const pcie_sym *s, uint8_t value) {
    const bool fts = p->state == PST_OS_FTS;
    const uint8_t want = fts ? PCIE_K_FTS : PCIE_K_IDL;
    if (!s->k || s->byte != want) {
        unknown_os(p, s->end_fs, value);
        return;
    }
    p->os_n++;
    p->os_last = s->byte;
    p->unit_end_fs = s->end_fs;
    if (p->os_n == FTS_LEN) {
        complete_os(p, s->end_fs, fts ? PCIE_EV_FTS : PCIE_EV_EIOS, 1);
    }
}

// COM + 14 EIE, then D10.2.
static void os_eie_symbol(pcie_pipeline *p, const pcie_sym *s, uint8_t value) {
    if (p->state == PST_OS_EIE_END) {
        if (!s->k && value == PCIE_D_TS1_ID) {
            complete_os(p, s->end_fs, PCIE_EV_EIEOS, 1);
        } else {
            unknown_os(p, s->end_fs, value);
        }
        return;
    }
    if (!s->k || s->byte != PCIE_K_EIE) {
        unknown_os(p, s->end_fs, value);
        return;
    }
    p->os_n++;
    p->os_last = s->byte;
    p->unit_end_fs = s->end_fs;
    if (p->os_n == EIE_LEN) {
        p->state = PST_OS_EIE_END;
    }
}

// ── descrambling (§5.2, §10.3) ─────────────────────────────────────────────

// True while the pipeline is assembling an ordered set whose D symbols are
// sent unscrambled: TS1/TS2 symbols 1-15 (symbol 1 is the D right after
// COM) and the EIEOS's final D10.2. The symbol that ends a SKP ordered set
// is not part of it (§5.3) and is descrambled like any other.
static bool in_unscrambled_os(const pcie_pipeline *p, const pcie_sym *s) {
    switch (p->state) {
        case PST_OS_START:
            return !s->k;
        case PST_OS_TS:
        case PST_OS_FTS:
        case PST_OS_IDL:
        case PST_OS_EIE:
        case PST_OS_EIE_END:
            return true;
        case PST_OUT:
        case PST_PKT:
        case PST_OS_SKP:
        default:
            return false;
    }
}

// Returns the symbol's data value after descrambling and sets *discard for
// a D symbol seen before the first COM in `on` mode.
static uint8_t descramble(pcie_pipeline *p, const pcie_sym *s, bool *discard) {
    *discard = false;
    if (!p->descramble) {
        return s->byte;
    }
    uint8_t value = s->byte;
    if (!s->k) {
        if (!p->synced) {
            *discard = true;
        } else if (!in_unscrambled_os(p, s)) {
            value = (uint8_t)(value ^ pcie_scrambler_key(&p->lfsr));
        }
    }
    if (!(s->k && s->byte == PCIE_K_SKP)) {
        (void)pcie_scrambler_advance(&p->lfsr); // every symbol but SKP
    }
    if (s->k && s->byte == PCIE_K_COM) {
        pcie_scrambler_reset(&p->lfsr);
        p->synced = true;
    }
    return value;
}

// ── public ─────────────────────────────────────────────────────────────────

void pcie_pipeline_init(pcie_pipeline *p, bool descramble_on, pcie_event_fn fn, void *ctx) {
    memset(p, 0, sizeof *p);
    p->descramble = descramble_on;
    pcie_scrambler_reset(&p->lfsr);
    p->fn = fn;
    p->ctx = ctx;
    p->state = PST_OUT;
}

void pcie_pipeline_symbol(pcie_pipeline *p, const pcie_sym *s) {
    p->step_valid_packet = false;
    p->step_disable_scrambling = false;
    bool discard = false;
    const uint8_t value = descramble(p, s, &discard);
    if (discard) {
        return;
    }
    switch (p->state) {
        case PST_OUT:
            out_symbol(p, s, value);
            break;
        case PST_PKT:
            pkt_symbol(p, s, value);
            break;
        case PST_OS_START:
            os_start_symbol(p, s, value);
            break;
        case PST_OS_TS:
            ts_collect(p, s, value);
            break;
        case PST_OS_SKP:
            os_skp_symbol(p, s, value);
            break;
        case PST_OS_FTS:
        case PST_OS_IDL:
            os_repeat_symbol(p, s, value);
            break;
        case PST_OS_EIE:
        case PST_OS_EIE_END:
            os_eie_symbol(p, s, value);
            break;
        default: /* defensive: every pcie_pstate is handled above */
            p->state = PST_OUT;
            break;
    }
}

static void report_break(pcie_pipeline *p) {
    pcie_event ev;
    memset(&ev, 0, sizeof ev);
    ev.kind = PCIE_EV_BREAK;
    report(p, &ev);
}

// An ordered set still being assembled when the symbol stream stops. Only a
// SKP OS can be complete and still open (it waits for a non-SKP, §5.3):
// it is emitted. Every other state is incomplete: unknown_os spanning its
// symbols at end of trace (§10.6) and on gating (reported, like a truncated
// packet), dropped silently on X/Z (like an open packet, §4.4).
static void break_os(pcie_pipeline *p, bool report_incomplete) {
    switch (p->state) {
        case PST_OS_SKP:
            complete_os(p, p->unit_end_fs, PCIE_EV_SKP, p->os_n);
            break;
        case PST_OS_START:
        case PST_OS_TS:
        case PST_OS_FTS:
        case PST_OS_IDL:
        case PST_OS_EIE:
        case PST_OS_EIE_END:
            if (report_incomplete) {
                unknown_os(p, p->unit_end_fs, p->os_last);
            }
            break;
        case PST_OUT:
        case PST_PKT:
        default:
            break;
    }
}

// End of trace, in the order of §10.6: ordered set, idle run, the front
// end's coalesced run (BREAK), open packet, stray run.
static void flush_stream(pcie_pipeline *p) {
    break_os(p, true);
    if (p->run == PCIE_RUN_IDLE) {
        run_end(p);
    }
    report_break(p);
    if (p->state == PST_PKT) {
        truncate_packet(p, PCIE_TRUNC_END_OF_TRACE);
    }
    run_end(p); // a stray run, if any
    p->state = PST_OUT;
}

void pcie_pipeline_break(pcie_pipeline *p, pcie_break_reason reason) {
    if (reason == PCIE_BREAK_FLUSH) {
        flush_stream(p);
        return;
    }
    const bool gating = reason == PCIE_BREAK_RXVALID || reason == PCIE_BREAK_TXELECIDLE;
    run_end(p);
    break_os(p, gating);
    if (p->state == PST_PKT && gating) {
        truncate_packet(p,
                        reason == PCIE_BREAK_RXVALID ? PCIE_TRUNC_RXVALID : PCIE_TRUNC_TXELECIDLE);
    }
    // §4.4: an open packet is dropped silently on X/Z.
    p->state = PST_OUT;
    report_break(p);
}
