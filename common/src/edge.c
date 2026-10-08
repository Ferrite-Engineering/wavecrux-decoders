// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "wcx/edge.h"

#include <stdlib.h>
#include <string.h>

bool wcx_edge_init(wcx_edge *e, const wcx_layout *layout, size_t clock, wcx_sample_point point) {
    if (e == NULL) {
        return false;
    }
    memset(e, 0, sizeof *e);
    if (!wcx_signal_bound(layout, clock) || layout->sig[clock].width != 1u) {
        return false;
    }
    e->layout = layout;
    e->clock = clock;
    e->point = point;
    e->nbytes = layout->encoded_bytes;
    // At least one byte each, so the buffers are always valid pointers.
    const size_t alloc = e->nbytes > 0 ? e->nbytes : 1u;
    e->buf[0] = calloc(1, alloc);
    e->buf[1] = calloc(1, alloc);
    if (e->buf[0] == NULL || e->buf[1] == NULL) {
        wcx_edge_free(e);
        return false;
    }
    return true;
}

void wcx_edge_free(wcx_edge *e) {
    if (e == NULL) {
        return;
    }
    free(e->buf[0]);
    free(e->buf[1]);
    memset(e, 0, sizeof *e);
}

wcx_edge_kind wcx_edge_step(wcx_edge *e, uint64_t timestamp_fs, const uint8_t *bits) {
    if (e == NULL || e->layout == NULL || bits == NULL) {
        return WCX_EDGE_NONE;
    }
    if (e->have_cur) {
        // The current sample becomes the previous one; the new sample goes
        // into the other buffer.
        e->cur ^= 1u;
        e->prev_ts = e->cur_ts;
        e->prev_level = e->cur_level;
        e->prev_unknown = e->cur_unknown;
        e->have_prev = true;
    }
    memcpy(e->buf[e->cur], bits, e->nbytes);
    e->cur_ts = timestamp_fs;
    WCX_IGNORE(wcx_sample_read_bit(e->layout, bits, e->clock, 0, &e->cur_level, &e->cur_unknown));
    e->have_cur = true;
    if (!e->have_prev || e->prev_unknown || e->cur_unknown || e->prev_level == e->cur_level) {
        return WCX_EDGE_NONE;
    }
    return e->cur_level ? WCX_EDGE_RISING : WCX_EDGE_FALLING;
}

const uint8_t *wcx_edge_data(const wcx_edge *e) {
    if (e == NULL || e->layout == NULL) {
        return NULL;
    }
    if (e->point == WCX_SAMPLE_AT_EDGE || !e->have_prev) {
        return e->buf[e->cur];
    }
    return e->buf[e->cur ^ 1u];
}

const uint8_t *wcx_edge_prev(const wcx_edge *e) {
    if (e == NULL || e->layout == NULL || !e->have_prev) {
        return NULL;
    }
    return e->buf[e->cur ^ 1u];
}

const uint8_t *wcx_edge_cur(const wcx_edge *e) {
    if (e == NULL || e->layout == NULL) {
        return NULL;
    }
    return e->buf[e->cur];
}

uint64_t wcx_edge_prev_ts(const wcx_edge *e) {
    return e != NULL ? e->prev_ts : 0;
}

uint64_t wcx_edge_cur_ts(const wcx_edge *e) {
    return e != NULL ? e->cur_ts : 0;
}
