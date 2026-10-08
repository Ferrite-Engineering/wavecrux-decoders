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

typedef struct pcie_sink {
    wcx_decoder *d;
    pcie_txbuf *buf;      // NULL: emit directly through the base
    wcx_arena *buf_arena; // strings for buffered transactions
} pcie_sink;

// Allocates room for `cap` transactions. False on allocation failure.
bool pcie_txbuf_init(pcie_txbuf *b, size_t cap);
void pcie_txbuf_free(pcie_txbuf *b);

// Appends one transaction; sets `overflow` instead when it cannot.
void pcie_txbuf_push(pcie_txbuf *b, uint64_t start_fs, uint64_t end_fs, const char *label,
                     const char *fields, bool is_error);

// The arena a front end must build this sink's strings in.
wcx_arena *pcie_sink_arena(const pcie_sink *s);

// Emits or buffers one transaction.
void pcie_sink_emit(pcie_sink *s, uint64_t start_fs, uint64_t end_fs, const char *label,
                    const char *fields, bool is_error);

#endif // PCIE_SINK_H
