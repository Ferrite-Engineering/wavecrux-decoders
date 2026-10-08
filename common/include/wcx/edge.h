// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Clock-edge detection with pre-edge sampling.
//
// The host feeds one sample per timestamp at which any bound signal
// changes, carrying every bound signal's value at that time. A clock edge
// is therefore "the clock's level differs from the previous sample's".
//
// Which data value belongs to an edge? In a zero-delay RTL dump the
// flip-flops that launch new data change on the same timestamp as the clock
// edge, so the sample at the edge already holds the NEXT value. What a
// flip-flop captured is the value just before the edge: the previous
// sample's. So the tracker keeps a copy of the previous sample, and
// wcx_edge_data() returns it by default (WCX_SAMPLE_BEFORE_EDGE). For a
// testbench whose data changes between edges both choices agree; choose
// WCX_SAMPLE_AT_EDGE only for a protocol that is defined on the value
// present at the edge.
//
//   // in init (sizes are known from the layout):
//   if (!wcx_edge_init(&st->clk, layout, SIG_PCLK, WCX_SAMPLE_BEFORE_EDGE)) fail
//   // once per sample, exactly once:
//   if (wcx_edge_step(&st->clk, s->timestamp_fs, s->bits) == WCX_EDGE_RISING) {
//       uint64_t data; bool x;
//       (void)wcx_sample_read(layout, wcx_edge_data(&st->clk), SIG_DATA, &data, &x);
//   }
//   // in fini:
//   wcx_edge_free(&st->clk);
//
// Edges are between known levels only: 0 -> 1 is rising, 1 -> 0 falling.
// A transition into or out of X/Z is not an edge (read the clock's unknown
// flag if the decoder should report it). The first sample has no
// predecessor, so it is never an edge.
//
// wcx_edge_step allocates nothing; the two buffers are sized in init.

#ifndef WCX_EDGE_H
#define WCX_EDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wcx/export.h"
#include "wcx/sample.h"

typedef enum wcx_edge_kind { WCX_EDGE_NONE = 0, WCX_EDGE_RISING, WCX_EDGE_FALLING } wcx_edge_kind;

typedef enum wcx_sample_point {
    WCX_SAMPLE_BEFORE_EDGE = 0, // data from the previous sample (default)
    WCX_SAMPLE_AT_EDGE          // data from the sample at the edge
} wcx_sample_point;

typedef struct wcx_edge {
    const wcx_layout *layout;
    size_t clock; // signal index of the clock (a 1-bit signal)
    wcx_sample_point point;
    uint8_t *buf[2]; // [cur] holds this sample, [cur ^ 1] the previous one
    size_t nbytes;   // bytes copied per sample (layout->encoded_bytes)
    unsigned cur;
    bool have_cur;  // at least one step
    bool have_prev; // at least two steps
    bool cur_level, cur_unknown;
    bool prev_level, prev_unknown;
    uint64_t prev_ts, cur_ts;
} wcx_edge;

// Prepares a tracker for 1-bit signal `clock` of `layout` (which must stay
// valid and unchanged while the tracker is used). False when the clock is
// unbound or not 1 bit wide, or on allocation failure.
WCX_NODISCARD bool wcx_edge_init(wcx_edge *e, const wcx_layout *layout, size_t clock,
                                 wcx_sample_point point);
void wcx_edge_free(wcx_edge *e);

// Records a sample (call exactly once per sample, in order) and reports the
// edge between the previous sample and this one. `bits` must hold
// layout->encoded_bytes bytes.
wcx_edge_kind wcx_edge_step(wcx_edge *e, uint64_t timestamp_fs, const uint8_t *bits);

// The packed bits at the configured sample point: the previous sample for
// WCX_SAMPLE_BEFORE_EDGE (the current one when there is none yet), the
// current sample for WCX_SAMPLE_AT_EDGE. Valid until the next step.
const uint8_t *wcx_edge_data(const wcx_edge *e);

// The previous and current samples' bits and timestamps. prev is NULL
// before the second step.
const uint8_t *wcx_edge_prev(const wcx_edge *e);
const uint8_t *wcx_edge_cur(const wcx_edge *e);
uint64_t wcx_edge_prev_ts(const wcx_edge *e);
uint64_t wcx_edge_cur_ts(const wcx_edge *e);

#endif // WCX_EDGE_H
