// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "wcx/emit.h"

#include <stdlib.h>
#include <string.h>

#include "wcx/size.h"

bool wcx_emit_init(wcx_emit_queue *q, size_t ceiling) {
    if (q == NULL) {
        return false;
    }
    memset(q, 0, sizeof *q);
    size_t slots = 0;
    if (ceiling == 0 || !wcx_size_add(ceiling, 1u, &slots)) {
        return false;
    }
    q->items = calloc(slots, sizeof *q->items);
    if (q->items == NULL) {
        return false;
    }
    q->ceiling = ceiling;
    return true;
}

void wcx_emit_free(wcx_emit_queue *q) {
    if (q == NULL) {
        return;
    }
    free(q->items);
    memset(q, 0, sizeof *q);
}

bool wcx_emit_begin(wcx_emit_queue *q) {
    if (q == NULL || q->pending) {
        return false;
    }
    q->count = 0;
    q->delivered = false;
    return true;
}

bool wcx_emit_pending(const wcx_emit_queue *q) {
    return q != NULL && q->pending;
}

static bool push_at(wcx_emit_queue *q, size_t limit, uint64_t start_fs, uint64_t end_fs,
                    const char *label, const char *fields_json, bool is_error) {
    if (q == NULL || q->items == NULL || label == NULL || fields_json == NULL ||
        q->count >= limit) {
        return false;
    }
    WcTransaction *t = &q->items[q->count];
    memset(t, 0, sizeof *t);
    t->start_fs = start_fs;
    t->end_fs = end_fs;
    t->label = label;
    t->fields_json = fields_json;
    t->is_error = is_error ? 1u : 0u;
    q->count++;
    return true;
}

bool wcx_emit_push(wcx_emit_queue *q, uint64_t start_fs, uint64_t end_fs, const char *label,
                   const char *fields_json, bool is_error) {
    return q != NULL && push_at(q, q->ceiling, start_fs, end_fs, label, fields_json, is_error);
}

bool wcx_emit_push_reserved(wcx_emit_queue *q, uint64_t start_fs, uint64_t end_fs,
                            const char *label, const char *fields_json, bool is_error) {
    return q != NULL && push_at(q, q->ceiling + 1u, start_fs, end_fs, label, fields_json, is_error);
}

size_t wcx_emit_count(const wcx_emit_queue *q) {
    return q != NULL ? q->count : 0;
}

int32_t wcx_emit_drain(wcx_emit_queue *q, WcTransaction *out, size_t *inout_count) {
    if (q == NULL || inout_count == NULL) {
        return WC_DECODER_ERR;
    }
    if (q->count > *inout_count || (q->count > 0 && out == NULL)) {
        *inout_count = q->count;
        q->pending = true;
        return WC_DECODER_NEED_MORE_SLOTS;
    }
    if (q->count > 0) {
        memcpy(out, q->items, q->count * sizeof *q->items);
    }
    *inout_count = q->count;
    q->pending = false;
    q->delivered = true;
    return WC_DECODER_OK;
}
