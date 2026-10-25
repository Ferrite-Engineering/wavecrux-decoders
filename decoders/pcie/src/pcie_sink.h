// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Where a front end's transactions go: straight to the decoder base
// (wcx_emit), or into a bounded buffer while the scrambling detector
// (SPEC.md §5.5) has not decided which pipeline is right. A front end builds
// its strings in pcie_sink_arena() and calls pcie_sink_emit(); it never
// knows which of the two it is talking to.
//
// Buffered strings live in the detector's own arena, not the base's, which
// the base resets at every call: they must survive until the lock.

#ifndef PCIE_SINK_H
#define PCIE_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wcx/arena.h"
#include "wcx/decoder.h"

typedef struct pcie_tx {
    uint64_t start_fs;
    uint64_t end_fs;
    const char *label;
    const char *fields;
    bool is_error;
} pcie_tx;

typedef struct pcie_txbuf {
    pcie_tx *items;
    size_t count;
    size_t cap;
    bool overflow; // a push failed: the buffer is full or a string was NULL
} pcie_txbuf;

// SPEC.md §12.12: a run of one repeated error on a multi-lane link.
typedef enum pcie_err_kind {
    PCIE_ERR_NONE = 0,
    PCIE_ERR_PAD,        // pad_misplaced; key: the lane
    PCIE_ERR_UNKNOWN_OS, // unknown_os; key: the symbol
} pcie_err_kind;

typedef struct pcie_err_run {
    pcie_err_kind kind;
    unsigned key;
    uint64_t start_fs;
    uint64_t end_fs;
    uint64_t count;
} pcie_err_run;

typedef struct pcie_sink {
    wcx_decoder *d;
    pcie_txbuf *buf;      // NULL: emit directly through the base
    wcx_arena *buf_arena; // strings for buffered transactions
    bool runs;            // more than one lane bound: repeated errors form runs
    pcie_err_run run;     // the run in progress
} pcie_sink;

// Allocates room for `cap` transactions. False on allocation failure.
bool pcie_txbuf_init(pcie_txbuf *b, size_t cap);
void pcie_txbuf_free(pcie_txbuf *b);

// Appends one transaction; sets `overflow` instead when it cannot.
void pcie_txbuf_push(pcie_txbuf *b, uint64_t start_fs, uint64_t end_fs, const char *label,
                     const char *fields, bool is_error);

// The arena a front end must build this sink's strings in.
wcx_arena *pcie_sink_arena(const pcie_sink *s);

// Emits or buffers one transaction. It ends the run in progress first.
void pcie_sink_emit(pcie_sink *s, uint64_t start_fs, uint64_t end_fs, const char *label,
                    const char *fields, bool is_error);

// The same, for Idle, which does not end a run (SPEC.md §12.12).
void pcie_sink_emit_idle(pcie_sink *s, uint64_t start_fs, uint64_t end_fs, const char *label,
                         const char *fields);

// One occurrence of a run's error: it extends the run in progress when that
// is of the same kind and key, else ends it and starts a new one.
void pcie_sink_run(pcie_sink *s, pcie_err_kind kind, unsigned key, uint64_t start_fs,
                   uint64_t end_fs);

// Emits the run in progress, if any.
void pcie_sink_end_run(pcie_sink *s);

#endif // PCIE_SINK_H
