// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Symbol times (SPEC.md §4.2). With t the k-th rising edge of pclk, P the
// period measured from the previous rising edge (0 for the first) and N
// symbols per clock, byte lane j is at
//
//   start = t + floor(j * P / N)      duration = floor(P / N)
//
// computed without the intermediate product j * P, which could exceed 64
// bits for absurd periods: floor(j * P / N) == (P / N) * j + floor((P % N) * j / N)
// exactly, because P = (P / N) * N + P % N.
//
// Every addition saturates at UINT64_MAX (wcx_u64_add): a hostile trace can
// place an edge within one period of the end of the 64-bit range, and a
// wrapped time would put the transaction at the start of the waveform. The
// host floors and ceils femtoseconds into ticks, so a clamped end still
// lands at the end of the trace.

#ifndef PCIE_TIME_H
#define PCIE_TIME_H

#include <stdint.h>

#include "wcx/size.h"

// a + b, or UINT64_MAX when the sum does not fit.
static inline uint64_t pcie_time_add(uint64_t a, uint64_t b) {
    uint64_t sum = 0;
    return wcx_u64_add(a, b, &sum) ? sum : UINT64_MAX;
}

static inline uint64_t pcie_symbol_duration(uint64_t period_fs, unsigned lanes) {
    return period_fs / lanes;
}

static inline uint64_t pcie_symbol_start(uint64_t edge_fs, uint64_t period_fs, unsigned lane,
                                         unsigned lanes) {
    // whole + part <= P (lane < lanes), so only the add to the edge can overflow.
    const uint64_t whole = (period_fs / lanes) * lane;
    const uint64_t part = ((period_fs % lanes) * lane) / lanes;
    return pcie_time_add(edge_fs, whole + part);
}

// End of a symbol that started at `start_fs`.
static inline uint64_t pcie_symbol_end(uint64_t start_fs, uint64_t duration_fs) {
    return pcie_time_add(start_fs, duration_fs);
}

// End of the last lane: where a transaction covering the whole edge ends.
static inline uint64_t pcie_edge_end(uint64_t edge_fs, uint64_t period_fs, unsigned lanes) {
    return pcie_symbol_end(pcie_symbol_start(edge_fs, period_fs, lanes - 1u, lanes),
                           pcie_symbol_duration(period_fs, lanes));
}

#endif // PCIE_TIME_H
