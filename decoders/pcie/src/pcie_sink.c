// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "pcie_sink.h"

#include <stdlib.h>
#include <string.h>

bool pcie_txbuf_init(pcie_txbuf *b, size_t cap) {
    memset(b, 0, sizeof *b);
    b->items = calloc(cap > 0 ? cap : 1u, sizeof *b->items);
    if (b->items == NULL) {
        return false; /* defensive: allocation failure */
    }
    b->cap = cap;
    return true;
}

void pcie_txbuf_free(pcie_txbuf *b) {
    free(b->items);
    memset(b, 0, sizeof *b);
}

void pcie_txbuf_push(pcie_txbuf *b, uint64_t start_fs, uint64_t end_fs, const char *label,
                     const char *fields, bool is_error) {
    if (label == NULL || fields == NULL || b->count >= b->cap) {
        b->overflow = true;
        return;
    }
    pcie_tx *t = &b->items[b->count++];
    t->start_fs = start_fs;
    t->end_fs = end_fs;
    t->label = label;
    t->fields = fields;
    t->is_error = is_error;
}

wcx_arena *pcie_sink_arena(const pcie_sink *s) {
    return s->buf != NULL ? s->buf_arena : wcx_decoder_arena(s->d);
}

void pcie_sink_emit(pcie_sink *s, uint64_t start_fs, uint64_t end_fs, const char *label,
                    const char *fields, bool is_error) {
    if (s->buf != NULL) {
        pcie_txbuf_push(s->buf, start_fs, end_fs, label, fields, is_error);
        return;
    }
    (void)wcx_emit(s->d, start_fs, end_fs, label, fields, is_error);
}
