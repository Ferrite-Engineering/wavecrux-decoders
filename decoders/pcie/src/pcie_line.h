// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The serial line front end (SPEC.md §11.2-§11.7): from samples of a 1-bit
// line to 8b/10b-decoded symbols.
//
//   samples ──▶ runs (constant level, §11.2)
//                 ├── longer than 8 ns, or X/Z ──▶ electrical idle (§11.3)
//                 └── held until the burst's rate is known (rate=auto, §11.4)
//                       ──▶ bits (§11.5) ──▶ COM alignment (§11.6)
//                             ──▶ 8b/10b (§11.7) ──▶ pcie_sym
//
// It knows nothing about transactions: it reports symbols and line events
// to a callback, and the plugin (pcie_serial_plugin.c) turns them into
// pipeline input and transactions. Every array is fixed-size; nothing
// allocates.

#ifndef PCIE_LINE_H
#define PCIE_LINE_H

#include <stdbool.h>
#include <stdint.h>

#include "pcie_8b10b.h"
#include "pcie_symbols.h"

#define PCIE_UI_GEN1_FS     UINT64_C(400000)  // 2.5 GT/s
#define PCIE_UI_GEN2_FS     UINT64_C(200000)  // 5.0 GT/s
#define PCIE_IDLE_RUN_FS    UINT64_C(8000000) // §11.3: a longer run is electrical idle
#define PCIE_GEN2_RUN_FS    UINT64_C(300000)  // §11.4: a shorter run decides Gen2
#define PCIE_RATE_HOLD_RUNS 40u               // §11.4: runs held before deciding Gen1
#define PCIE_SYMBOL_BITS    10u

typedef enum pcie_rate_param {
    PCIE_RATE_AUTO = 0, // manifest enum order
    PCIE_RATE_GEN1,
    PCIE_RATE_GEN2
} pcie_rate_param;

typedef enum pcie_line_event_kind {
    PCIE_LINE_LOCK,       // symbol lock found; u.gen2; spans the COM
    PCIE_LINE_REALIGN,    // u.shift; spans the COM
    PCIE_LINE_CODE_ERROR, // u.code; spans the symbol (EDB follows as its SYMBOL)
    PCIE_LINE_DISPARITY,  // u.disp; spans the symbol
    PCIE_LINE_SYMBOL,     // u.sym: feed it to the pipeline
    PCIE_LINE_IDLE_BEGIN, // electrical idle began: break the pipeline
    PCIE_LINE_IDLE_PERIOD // a reportable idle period ended (or the trace did)
} pcie_line_event_kind;

typedef struct pcie_line_event {
    pcie_line_event_kind kind;
    uint64_t start_fs;
    uint64_t end_fs;
    union {
        bool gen2;
        unsigned shift;
        uint16_t code;
        struct {
            uint16_t code;
            uint8_t byte;
            bool k;
        } disp;
        pcie_sym sym;
    } u;
} pcie_line_event;

typedef void (*pcie_line_fn)(void *ctx, const pcie_line_event *ev);

typedef struct pcie_held_run {
    uint64_t start_fs;
    uint64_t dur_fs;
    bool level;
} pcie_held_run;

typedef struct pcie_line {
    // Callback.
    pcie_line_fn fn;
    void *ctx;
    // Times first, then the smaller fields (keeps the struct unpadded).
    uint64_t run_start_fs; // the run in progress (§11.2)
    uint64_t idle_start_fs;
    uint64_t ui_fs; // the burst's rate (§11.4): 0 while rate=auto is still deciding
    uint64_t bit_start[PCIE_SYMBOL_BITS]; // start times of the last 10 bits (ring)
    pcie_held_run held[PCIE_RATE_HOLD_RUNS];
    pcie_rate_param rate;
    unsigned held_count;
    unsigned seen;     // bits in shreg (saturates at 10)
    unsigned ring_pos; // where the next bit's start goes
    unsigned into;     // bits of the current symbol received (0..9)
    uint16_t shreg;    // the last 10 bits, newest in bit 0
    pcie_8b10b codes;
    bool invert;
    bool have_level; // the last sample was known
    bool level;
    bool idle; // electrical idle (§11.3)
    bool idle_reportable;
    bool activity; // some run has been converted to bits
    bool locked;   // symbol alignment (§11.6)
    bool rd_pos;   // running disparity (§11.7)
} pcie_line;

void pcie_line_init(pcie_line *l, pcie_rate_param rate, bool invert, pcie_line_fn fn, void *ctx);

// One host sample: its time, and the line's level (ignored when unknown).
void pcie_line_sample(pcie_line *l, uint64_t t_fs, bool unknown, bool level);

// End of trace, in two steps (§11.9): pcie_line_flush_runs() converts the
// runs rate=auto still holds; pcie_line_flush_idle() reports an
// electrical-idle period still open at `now_fs`, the last sample's time.
void pcie_line_flush_runs(pcie_line *l);
void pcie_line_flush_idle(pcie_line *l, uint64_t now_fs);

#endif // PCIE_LINE_H
