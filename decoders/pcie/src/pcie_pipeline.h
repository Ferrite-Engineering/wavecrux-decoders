// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// One symbol pipeline (SPEC.md §5): descrambler, ordered-set recogniser,
// packet framer and idle/stray-data run tracking, for a stream of symbols
// in wire order. It knows nothing about transactions: it reports protocol
// units and errors as events, and a front end (pcie_pipe_fe.c for the PIPE
// decoder, pcie_dll_fe.c for the Data Link Layer decoder) turns them into
// transactions. The scrambling detector (pcie_detector.c) runs two of these
// side by side, one descrambling and one not, until it knows which is right.
//
// The state machine (one switch over pcie_pstate in pcie_pipeline.c):
//
//   OUT ──COM──▶ OS_START ──D/PAD──▶ OS_TS (15 symbols) ──▶ TS1/TS2 or unknown_os
//    │              ├──SKP──▶ OS_SKP (1..5, stops at the first non-SKP) ──▶ SKP
//    │              ├──FTS──▶ OS_FTS (×3) ──▶ FTS      ├──IDL──▶ OS_IDL (×3) ──▶ EIOS
//    │              ├──EIE──▶ OS_EIE (×14) ──▶ OS_EIE_END ──D10.2──▶ EIEOS
//    │              └──other──▶ unknown_os
//    ├──STP/SDP──▶ PKT ──END/EDB──▶ packet closed
//    │              ├──COM/STP/SDP──▶ truncated, then as from OUT
//    │              └──other K──▶ k_in_packet or invalid_k; the packet is dropped
//    ├──END/EDB──▶ unmatched_end     ├──unknown K──▶ invalid_k
//    ├──PAD/SKP/FTS/IDL/EIE──▶ unexpected_k
//    └──D──▶ idle run (0x00) or stray-data run (anything else)
//
// Packet bodies are never stored: the framer keeps the first PCIE_HEAD_KEEP
// bytes (sequence number, TLP header, or the whole 6-byte DLLP), the last 4
// bytes, and a running CRC-32 over everything before those last 4, which is
// all the front ends and the lock rule of SPEC.md §5.5 need. So there is no
// ceiling on packet length and no allocation per packet.

#ifndef PCIE_PIPELINE_H
#define PCIE_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "pcie_scrambler.h"
#include "pcie_symbols.h"

// Body bytes kept verbatim: a DLLP is exactly 6 (SPEC.md §8.1); a TLP's
// sequence number (2) and the header bytes the light decode reads (4) are
// the first 6 (§8.2). 8 keeps the arithmetic byte-aligned.
#define PCIE_HEAD_KEEP 8u

// TS1/TS2 body: symbols 1-15 after COM (protocol-notes.md §2).
#define PCIE_TS_BODY 15u

typedef enum pcie_event_kind {
    PCIE_EV_TS,            // u.ts
    PCIE_EV_SKP,           // u.count = SKP symbols (1..5)
    PCIE_EV_FTS,           // u.count = 1
    PCIE_EV_EIOS,          // no payload
    PCIE_EV_EIEOS,         // u.count = 1
    PCIE_EV_IDLE_RUN,      // u.count = idle symbols
    PCIE_EV_STRAY_RUN,     // u.stray
    PCIE_EV_PACKET,        // u.packet
    PCIE_EV_INVALID_K,     // u.symbol: the K byte (outside a packet, or inside: §10.2)
    PCIE_EV_TRUNCATED,     // u.truncated
    PCIE_EV_UNMATCHED_END, // u.symbol: END or EDB
    PCIE_EV_K_IN_PACKET,   // u.symbol: the K byte; the packet was dropped (§10.2)
    PCIE_EV_UNEXPECTED_K,  // u.symbol: PAD/SKP/FTS/IDL/EIE outside a packet and OS (§10.2)
    PCIE_EV_UNKNOWN_OS,    // u.symbol: the symbol that did not fit
    PCIE_EV_BREAK          // gating, X/Z or end of stream: nothing is pending any more
} pcie_event_kind;

typedef enum pcie_trunc_by {
    PCIE_TRUNC_STP,
    PCIE_TRUNC_SDP,
    PCIE_TRUNC_COM,
    PCIE_TRUNC_RXVALID,
    PCIE_TRUNC_TXELECIDLE,
    PCIE_TRUNC_END_OF_TRACE // §10.6
} pcie_trunc_by;

typedef enum pcie_break_reason {
    PCIE_BREAK_RXVALID,    // rxvalid sampled 0 (SPEC.md §4.3)
    PCIE_BREAK_TXELECIDLE, // txelecidle sampled 1 (§4.3)
    PCIE_BREAK_UNKNOWN,    // X or Z on data/datak (§4.4): an open packet is dropped silently
    PCIE_BREAK_FLUSH       // end of stream
} pcie_break_reason;

typedef struct pcie_ts {
    bool ts2;                   // symbol 6 was 0x45 (else 0x4A)
    uint8_t syms[PCIE_TS_BODY]; // symbols 1..15 (syms[0] = Link#, ... syms[5] = identifier)
    uint16_t kmask;             // bit i set: syms[i] was a K symbol (PAD)
} pcie_ts;

typedef struct pcie_packet {
    bool tlp;                     // opened by STP (else SDP)
    bool edb;                     // closed by EDB (else END)
    uint64_t length;              // body symbols between start and end
    uint8_t head[PCIE_HEAD_KEEP]; // body[0..min(length, 8))
    uint8_t tail[4];              // body[length-4..length), valid when length >= 4
    uint32_t crc32_before_tail;   // CRC-32 register over body[0..length-4), valid when length >= 4
    bool crc_valid;               // SPEC.md §5.5: END-terminated, a DLLP with good CRC-16 or a
                                  // TLP with good LCRC (and a valid length)
} pcie_packet;

typedef struct pcie_event {
    pcie_event_kind kind;
    uint64_t start_fs; // first symbol of the unit
    uint64_t end_fs;   // end of its last symbol
    union {
        pcie_ts ts;
        uint64_t count;
        struct {
            uint64_t count;
            uint8_t first; // the first stray byte (after descrambling)
        } stray;
        pcie_packet packet;
        uint8_t symbol;
        struct {
            bool tlp;
            uint64_t symbols; // body length when truncated
            pcie_trunc_by by;
        } truncated;
    } u;
} pcie_event;

typedef void (*pcie_event_fn)(void *ctx, const pcie_event *ev);

typedef enum pcie_pstate {
    PST_OUT,
    PST_PKT,
    PST_OS_START,
    PST_OS_TS,
    PST_OS_SKP,
    PST_OS_FTS,
    PST_OS_IDL,
    PST_OS_EIE,
    PST_OS_EIE_END
} pcie_pstate;

typedef enum pcie_run_kind { PCIE_RUN_NONE, PCIE_RUN_IDLE, PCIE_RUN_STRAY } pcie_run_kind;

typedef struct pcie_pipeline {
    bool descramble; // SPEC.md §5.5 `on` (true) or `off` (false)
    pcie_scrambler lfsr;
    bool synced; // a COM has been seen (§5.2)
    pcie_event_fn fn;
    void *ctx;
    pcie_pstate state;
    uint64_t unit_start_fs; // the open packet's or ordered set's first symbol
    uint64_t unit_end_fs;   // end of the last symbol taken into it
    pcie_packet pkt;
    unsigned tail_fill; // bytes held in pkt.tail (0..4)
    unsigned os_n;      // symbols collected after COM
    uint8_t os_last;    // the last symbol taken into the ordered set (for §10.6)
    pcie_ts ts;
    pcie_run_kind run;
    uint64_t run_count;
    uint8_t run_first;
    uint64_t run_start_fs;
    uint64_t run_end_fs;
    // What the last pcie_pipeline_symbol() call did, for the scrambling
    // detector's lock rule (§5.5).
    bool step_valid_packet;       // closed a packet with a correct CRC
    bool step_disable_scrambling; // completed a TS with Training Control bit 3 set
    uint64_t step_start_fs;       // span of that packet or TS
    uint64_t step_end_fs;
} pcie_pipeline;

// Prepares a pipeline that descrambles (or not) and reports events to fn(ctx, ev).
void pcie_pipeline_init(pcie_pipeline *p, bool descramble_on, pcie_event_fn fn, void *ctx);

// Feeds one symbol in wire order.
void pcie_pipeline_symbol(pcie_pipeline *p, const pcie_sym *s);

// The symbol stream was interrupted (SPEC.md §4.3, §4.4) or has ended. For
// gating and X/Z: pending runs are reported, a complete SKP ordered set
// still waiting for its terminator is emitted, an incomplete ordered set is
// unknown_os (gating) or dropped (X/Z), an open packet is reported as
// truncated (gating) or dropped (X/Z), and a PCIE_EV_BREAK event follows.
// At end of stream the order of
// §10.6 applies: a SKP ordered set still open is completed (anything else
// in progress is unknown_os), an idle run is reported, PCIE_EV_BREAK lets
// the front end emit its coalesced run, an open packet is truncated "by end
// of trace", and a stray-data run is reported last. The descrambler keeps
// its state either way.
void pcie_pipeline_break(pcie_pipeline *p, pcie_break_reason reason);

#endif // PCIE_PIPELINE_H
