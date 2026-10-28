// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "pcie_sink.h"

#include <stdlib.h>
#include <string.h>

#include "wcx/json_writer.h"

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

static void emit_now(pcie_sink *s, uint64_t start_fs, uint64_t end_fs, const char *label,
                     const char *fields, bool is_error) {
    if (s->buf != NULL) {
        pcie_txbuf_push(s->buf, start_fs, end_fs, label, fields, is_error);
        return;
    }
    (void)wcx_emit(s->d, start_fs, end_fs, label, fields, is_error);
}

void pcie_sink_emit(pcie_sink *s, uint64_t start_fs, uint64_t end_fs, const char *label,
                    const char *fields, bool is_error) {
    pcie_sink_end_run(s);
    emit_now(s, start_fs, end_fs, label, fields, is_error);
}

void pcie_sink_emit_idle(pcie_sink *s, uint64_t start_fs, uint64_t end_fs, const char *label,
                         const char *fields) {
    emit_now(s, start_fs, end_fs, label, fields, false);
}

// SPEC.md §12.12: the run's error as its first occurrence reports it, with
// ` (N times)` and `occurrences` when it occurred more than once.
void pcie_sink_end_run(pcie_sink *s) {
    const pcie_err_run r = s->run;
    s->run.kind = PCIE_ERR_NONE;
    if (r.kind == PCIE_ERR_NONE) {
        return;
    }
    wcx_arena *a = pcie_sink_arena(s);
    wcx_json j;
    wcx_json_begin(&j, a);
    const char *label = NULL;
    if (r.kind == PCIE_ERR_PAD) {
        wcx_json_add_str(&j, "type", "pad_misplaced");
        wcx_json_add_u64(&j, "lane", r.key);
        wcx_json_add_str(&j, "error",
                         "PAD outside a packet is valid only as fill after END or EDB.");
        label = wcx_arena_printf(a, "PAD outside a packet (lane %u)", r.key);
    } else {
        wcx_json_add_str(&j, "type", "unknown_os");
        wcx_json_add_hex(&j, "symbol", r.key, 2);
        wcx_json_add_str(&j, "error", "COM was not followed by a recognised ordered set.");
        label = "Unknown ordered set";
    }
    if (r.count > 1u) {
        wcx_json_add_u64(&j, "occurrences", r.count);
        label = wcx_arena_printf(a, "%s (%llu times)", label != NULL ? label : "",
                                 (unsigned long long)r.count);
    }
    emit_now(s, r.start_fs, r.end_fs, label, wcx_json_end(&j), true);
}

void pcie_sink_run(pcie_sink *s, pcie_err_kind kind, unsigned key, uint64_t start_fs,
                   uint64_t end_fs) {
    pcie_err_run *r = &s->run;
    if (r->kind == kind && r->key == key) {
        r->end_fs = end_fs;
        r->count++;
        return;
    }
    pcie_sink_end_run(s);
    r->kind = kind;
    r->key = key;
    r->start_fs = start_fs;
    r->end_fs = end_fs;
    r->count = 1u;
}
