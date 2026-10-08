// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// VCD reader (IEEE 1364-2005 §18) with the value semantics WaveCrux gets
// from wellen, its waveform library:
//
//   * $timescale: factor (1, 10, 100; others accepted with a warning) and
//     unit s/ms/us/ns/ps/fs, with or without a space between them.
//   * $scope/$upscope nest; a variable's path is its scopes and reference
//     joined with '.', e.g. "tb.dut.pclk". A bit range ("data [7:0]" or
//     "data[7:0]") is not part of the path; it can be given for
//     disambiguation as "tb.data[7:0]".
//   * Value changes: scalars 0 1 x z X Z (and the nine-state h l u w -),
//     b/B vectors. A vector shorter than its declared width is extended the
//     VCD way, as wellen's expand_special_vector_cases does: with '0' when
//     its first character is 0 or 1, with that character when it is x or z.
//     A value longer than the width, or with an invalid character, is an
//     error (wellen refuses the file). A 1-bit variable takes the last
//     character of its value. Real (r) and string (s) changes are ignored
//     with a warning; binding a real variable is an error.
//   * $dumpvars/$dumpall/$dumpon/$dumpoff blocks hold ordinary changes.
//   * Time: a "#t" equal to the previous one is ignored; one that goes
//     backwards is skipped together with its changes, as wellen does
//     (with a warning). Changes before the first "#t" happen at time 0.
//   * end_time is the last time stamp (wellen's time table end), which
//     WaveCrux passes as decode()'s endTime.
//
// Each variable also gets wellen's signal reference: the index of its
// identifier code among distinct codes in declaration order. WaveCrux puts
// such a reference (not the path) into config_json's signal_bindings.

#ifndef WCXH_VCD_H
#define WCXH_VCD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wcx/error.h"
#include "wcx/export.h"

#define WCXH_VCD_NONE SIZE_MAX

typedef struct wcxh_vcd_var {
    char *path;  // "tb.dut.data"
    char *range; // "[7:0]", or NULL
    char *id;    // identifier code
    uint32_t width;
    bool is_real;
    uint32_t signal_ref;
} wcxh_vcd_var;

typedef struct wcxh_vcd_change {
    uint64_t time;
    size_t off; // NUL-terminated value in the pool
} wcxh_vcd_change;

typedef struct wcxh_vcd_signal {
    size_t var;
    wcxh_vcd_change *changes;
    size_t count;
    size_t cap;
} wcxh_vcd_signal;

typedef struct wcxh_vcd {
    const char *text; // borrowed
    size_t len;
    bool has_timescale;
    uint32_t ts_factor;
    int ts_exponent; // 0, -3, -6, -9, -12, -15
    wcxh_vcd_var *vars;
    size_t var_count;
    size_t var_cap;
    size_t body; // offset just past "$enddefinitions $end"
    bool has_time;
    uint64_t end_time;
    wcxh_vcd_signal *sigs; // one per wanted variable, in request order
    size_t sig_count;
    char *pool;
    size_t pool_len;
    size_t pool_cap;
    unsigned real_changes_ignored;
    unsigned string_changes_ignored;
    unsigned backwards_times;
} wcxh_vcd;

// Parses the header of text[0..len) (borrowed; must outlive the vcd).
WCX_NODISCARD bool wcxh_vcd_parse_header(wcxh_vcd *v, const char *text, size_t len, wcx_error *err);

// The variable at `path` ("a.b.c" or "a.b.c[7:0]"); WCXH_VCD_NONE with a
// message when absent or ambiguous.
size_t wcxh_vcd_find(const wcxh_vcd *v, const char *path, wcx_error *err);

// Reads all value changes of the variables vars[0..n) (indices from
// wcxh_vcd_find; repeats allowed). Signal i of the result is vars[i].
WCX_NODISCARD bool wcxh_vcd_parse_body(wcxh_vcd *v, const size_t *vars, size_t n, wcx_error *err);

// The value of signal `sig` at time t: its last change at or before t, or
// NULL before the first change (wellen's upperBoundLE). *cursor speeds up
// non-decreasing queries; start it at 0.
const char *wcxh_vcd_value_at(const wcxh_vcd *v, size_t sig, uint64_t t, size_t *cursor);

void wcxh_vcd_free(wcxh_vcd *v);

#endif // WCXH_VCD_H
