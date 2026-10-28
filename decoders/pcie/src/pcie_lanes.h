// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The lane layer of a multi-lane link (SPEC.md §12), between the per-edge
// sampling in pcie_plugin.c and the one symbol stream of pcie_detector.c.
// With one lane bound it is not used at all.
//
// Each bound lane is cut into items on its own (§12.3): ordered sets,
// recognised on the raw symbols; row symbols, descrambled by the lane's own
// LFSR; and gaps (runs of gated or X/Z edges). The items wait in a small
// per-lane queue until the lanes can be paired:
//
//   * rounds (§12.4): one ordered set per lane, their COMs within
//     max_skew symbols of each other. A round decides the link (§12.5),
//     locks or loses alignment (§12.6) and hands the pipeline its reference
//     lane's ordered set, with the other lanes' Link#, Lane# and SKP counts
//     (§12.9);
//   * rows (§12.6): while aligned, the k-th item after a round on every
//     link lane. A row of symbols goes to the pipeline in logical lane
//     order (§12.7); gaps on every lane are a break; anything else loses
//     alignment.
//
// A round is processed once every lane has been read max_skew + 17 symbols
// past its first COM: by then no lane can still add an ordered set to it,
// and its reference lane's next item is known (the TS2 run rule of §12.5).
// That bounds how long an item waits, and so the queues.

#ifndef PCIE_LANES_H
#define PCIE_LANES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pcie_detector.h"
#include "pcie_pipeline.h"
#include "wcx/decoder.h"

// The most symbols one ordered set can take: a TS1/TS2 or an EIEOS is 16,
// and a symbol that does not fit is part of the unknown one it ends (§12.3).
#define PCIE_OS_MAX_SYMS 16u

// Items a lane holds at most: a round waits max_skew + 17 symbol times (at
// most 81), a lane may lead the others by max_skew (64), and an edge adds 8;
// a lane outside a link from SKP holds an ordered set for at most
// 2 max_skew + 17 (145) before it is dropped. 256 is ample.
#define PCIE_LANE_ITEMS 256u

// max_skew range (SPEC.md §12.1).
#define PCIE_MAX_SKEW_MAX 64

typedef enum pcie_item_kind { PCIE_ITEM_SYM, PCIE_ITEM_OS, PCIE_ITEM_GAP } pcie_item_kind;

typedef enum pcie_os_type {
    PCIE_OS_TS1,
    PCIE_OS_TS2,
    PCIE_OS_SKP,
    PCIE_OS_FTS,
    PCIE_OS_EIOS,
    PCIE_OS_EIEOS,
    PCIE_OS_UNKNOWN
} pcie_os_type;

// One lane's ordered set: its type, what a round compares, and its symbols
// as the pipeline will see them.
typedef struct pcie_lane_os {
    pcie_os_type type;
    bool cut;                 // cut short by a gap: a break follows it in the pipeline
    pcie_break_reason cut_by; // the gap's kind
    unsigned n;               // symbols in byte[], start_fs[], end_fs[]
    unsigned skp;             // SKP symbols of a SKP OS
    uint8_t ts[PCIE_TS_BODY]; // TS1/TS2 symbols 1-15
    uint16_t ts_kmask;        // bit i: ts[i] was a K symbol
    uint8_t byte[PCIE_OS_MAX_SYMS];
    uint16_t kmask; // bit i: byte[i] is a K symbol
    uint64_t start_fs[PCIE_OS_MAX_SYMS];
    uint64_t end_fs[PCIE_OS_MAX_SYMS];
} pcie_lane_os;

typedef struct pcie_lane_item {
    pcie_item_kind kind;
    uint64_t index; // §12.2: the symbol's, the COM's, or the gap's first
    uint64_t start_fs;
    uint64_t end_fs;
    // PCIE_ITEM_SYM
    uint8_t byte;
    uint8_t plain; // descrambled by the lane's own LFSR (D symbols)
    bool k;
    // PCIE_ITEM_GAP
    pcie_break_reason gap; // PCIE_BREAK_RXVALID, _TXELECIDLE or _UNKNOWN
    bool open;             // still growing
    bool broken;           // the pipeline has had this gap's break
    uint64_t edges;
    // PCIE_ITEM_OS
    pcie_lane_os os;
} pcie_lane_item;

typedef enum pcie_lane_pstate {
    LPS_OUT,
    LPS_OS_START,
    LPS_TS,
    LPS_SKP,
    LPS_FTS,
    LPS_IDL,
    LPS_EIE,
    LPS_EIE_END
} pcie_lane_pstate;

// §12.9: what a lane's last reported lane_mismatch was.
#define PCIE_MISMATCH_CAP 24u

typedef struct pcie_lane {
    pcie_lane_item *items; // ring of PCIE_LANE_ITEMS
    size_t head;
    size_t count;
    pcie_lane_pstate pst; // the ordered set being assembled
    unsigned os_n;
    pcie_lane_os cur;
    uint64_t cur_index;
    pcie_scrambler lfsr;
    bool mm_have;
    char mm_expected[PCIE_MISMATCH_CAP];
    char mm_got[PCIE_MISMATCH_CAP];
} pcie_lane;

// One lane at one rising edge, as the plugin samples it.
typedef struct pcie_lane_in {
    bool gap;                   // gated or X/Z (§12.3)
    pcie_break_reason gap_kind; // when gap
    uint64_t data;
    uint64_t datak;
} pcie_lane_in;

typedef struct pcie_lanes {
    wcx_decoder *d;
    pcie_detector *det;
    bool dll; // the DLL decoder: no link_width, lane_order, lanes_aligned or lane_mismatch
    unsigned bound;
    unsigned n; // symbols per clock and lane
    uint64_t max_skew;
    bool order_auto;
    bool failed;
    pcie_lane lane[PCIE_MAX_LANES];
    uint64_t next_index; // §12.2: symbols read so far on every lane
    bool flushing;
    // The link (§12.5).
    bool decided;
    bool by_ts2;  // decided by §12.5 rule 1: only link lanes are considered
    bool seen_ts; // a round's ordered set was a TS1/TS2: rule 2 is off
    unsigned width;
    uint8_t link[PCIE_MAX_LANES]; // physical lanes in logical order
    uint16_t considered;          // §12.4
    bool order_physical;          // the last link's order was physical
    uint8_t last_order[PCIE_MAX_LANES];
    unsigned last_order_n;
    // Alignment (§12.6).
    bool aligned;
    unsigned last_width;
    uint64_t last_offsets[PCIE_MAX_LANES];
    bool skew_since_lock; // a lane_skew since the last lock (§12.11)
    bool skew_reported;   // a lane_skew since the link was decided or locked
    // The unaligned run in progress (§12.11).
    bool run_open;
    bool run_error;
    uint64_t run_count;
    uint64_t run_start_fs;
    uint64_t run_end_fs;
    pcie_os_lanes ann; // the per-lane view handed to the pipeline (§12.9)
} pcie_lanes;

// Allocates the lane queues. False on allocation failure (then safe to free).
bool pcie_lanes_init(pcie_lanes *ln, wcx_decoder *d, pcie_detector *det, unsigned bound, unsigned n,
                     unsigned max_skew, bool order_auto, bool dll);
void pcie_lanes_free(pcie_lanes *ln);

// One rising edge: in[0..bound). `edge_fs` and `period_fs` as in SPEC.md §4.2.
void pcie_lanes_edge(pcie_lanes *ln, const pcie_lane_in *in, uint64_t edge_fs, uint64_t period_fs);

// End of trace (§12.13): what the lanes still hold is processed, then the
// unaligned run is emitted. The pipeline's own flush comes after.
void pcie_lanes_flush(pcie_lanes *ln);

#endif // PCIE_LANES_H
