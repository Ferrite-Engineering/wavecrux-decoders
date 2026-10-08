// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Scrambling detection (SPEC.md §5.5), and the single place the plugin
// feeds symbols into.
//
// With `scrambling` = on or off there is one pipeline whose front end emits
// straight through the decoder base. With `auto` there are two, one
// descrambling and one not, each with its own front end writing into its
// own bounded buffer (strings in the detector's own arena, since the base
// resets its arena every call). The first pipeline to close a packet with a
// correct CRC wins (`off` on a tie, which §10.5 shows cannot happen for
// valid packets); a TS1/TS2 with Disable Scrambling set
// locks to `off`; and if neither has happened when a buffer holds
// PCIE_AUTO_LOCK_TRANSACTIONS transactions, or at flush, `off` wins as
// "undetermined". On lock the winner's buffer is emitted, then an info
// transaction, and from then on the winner emits directly.
//
// Edge-level transactions (X/Z runs, RxStatus) are not pipeline output but
// must keep their place in time, so before the lock they go into both
// buffers; pcie_detector_emit() routes them.

#ifndef PCIE_DETECTOR_H
#define PCIE_DETECTOR_H

#include <stdbool.h>
#include <stdint.h>

#include "pcie_dll_fe.h"
#include "pcie_pipe_fe.h"
#include "pcie_pipeline.h"
#include "pcie_sink.h"
#include "wcx/arena.h"
#include "wcx/decoder.h"

// §5.5: lock to `off` when this many transactions are buffered.
#define PCIE_AUTO_LOCK_TRANSACTIONS 4096u
// A symbol produces at most 2 transactions per pipeline (a run it ends and a
// unit it completes); the lock check runs after every symbol, so 8 spare
// slots mean nothing is ever dropped at the boundary.
#define PCIE_AUTO_BUFFER_CAP (PCIE_AUTO_LOCK_TRANSACTIONS + 8u)
// Strings of the buffered transactions. The longest label plus fields_json
// is under 600 bytes (a TLP with every suffix and error sentence), so two
// full buffers need under 5 MiB; 8 MiB cannot be reached before the
// transaction ceiling locks.
#define PCIE_AUTO_ARENA_BLOCK ((size_t)64u * 1024u)
#define PCIE_AUTO_ARENA_MAX   ((size_t)8u * 1024u * 1024u)

typedef enum pcie_scrambling_param {
    PCIE_SCRAMBLING_AUTO = 0, // manifest enum order
    PCIE_SCRAMBLING_ON,
    PCIE_SCRAMBLING_OFF
} pcie_scrambling_param;

typedef struct pcie_branch {
    pcie_pipeline pipe;
    pcie_sink sink;
    pcie_txbuf buf;
    union {
        pcie_pipe_fe pipe_fe;
        pcie_dll_fe dll_fe;
    } fe;
} pcie_branch;

typedef struct pcie_detector {
    wcx_decoder *d;
    bool dll;
    pcie_scrambling_param mode;
    bool locked;
    unsigned winner;   // index into br
    unsigned branches; // 1 (on/off) or 2 (auto: [0] off, [1] on)
    pcie_branch br[2];
    wcx_arena arena; // buffered strings (auto only)
    bool arena_live;
    bool arena_lent_to_host; // the lock batch points into `arena`; free it once the
                             // base reports that batch delivered
    bool have_sym;
    uint64_t last_sym_start_fs;
    uint64_t last_sym_end_fs;
} pcie_detector;

// Sets up the pipelines for the configured mode. False on allocation
// failure (the detector is then safe to free).
bool pcie_detector_init(pcie_detector *det, wcx_decoder *d, pcie_scrambling_param mode, bool dll,
                        bool coalesce, bool show_idle);
void pcie_detector_free(pcie_detector *det);

// At the start of every on_sample / on_flush: releases the lock batch's
// strings once the base reports that the host has copied them
// (wcx_decoder_batch_delivered), which may be later than the next call.
void pcie_detector_begin_call(pcie_detector *det);

void pcie_detector_symbol(pcie_detector *det, const pcie_sym *s);
void pcie_detector_break(pcie_detector *det, pcie_break_reason reason);

// Where edge-level strings are built, and how they are emitted.
wcx_arena *pcie_detector_arena(pcie_detector *det);
void pcie_detector_emit(pcie_detector *det, uint64_t start_fs, uint64_t end_fs, const char *label,
                        const char *fields, bool is_error);

// End of stream, in two steps so the plugin can place its own X/Z run
// between them (SPEC.md §10.6): pcie_detector_end_of_trace() lets the
// pipelines report what they still hold (steps 1-3 and the stray run);
// pcie_detector_finish() then locks an undecided `auto` to `off`
// (step 5). `now_fs` spans the info transaction when nothing was decoded.
void pcie_detector_end_of_trace(pcie_detector *det);
void pcie_detector_finish(pcie_detector *det, uint64_t now_fs);

#endif // PCIE_DETECTOR_H
