// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Symbol times (SPEC.md §4.2) and their saturation at the end of the 64-bit
// range. Expected values are worked out by hand in the comments.

#include "pcie_test_util.h"
#include "pcie_time.h"

WCX_TEST(lane_times_are_floor_of_j_p_over_n) {
    // P = 100, N = 8: floor(100 j / 8) = 0, 12, 25, 37, 50, 62, 75, 87;
    // duration floor(100 / 8) = 12; edge end = 87 + 12 = 99.
    static const uint64_t starts[8] = {0, 12, 25, 37, 50, 62, 75, 87};
    for (unsigned j = 0; j < 8u; j++) {
        WCX_CHECK_EQ_U64(pcie_symbol_start(1000, 100, j, 8), 1000 + starts[j]);
    }
    WCX_CHECK_EQ_U64(pcie_symbol_duration(100, 8), 12);
    WCX_CHECK_EQ_U64(pcie_edge_end(1000, 100, 8), 1099);
    // P = 8000, N = 2: lanes at 0 and 4000, each 4000 long; N = 1: the edge.
    WCX_CHECK_EQ_U64(pcie_symbol_start(10000, 8000, 1, 2), 14000);
    WCX_CHECK_EQ_U64(pcie_edge_end(10000, 8000, 2), 18000);
    WCX_CHECK_EQ_U64(pcie_symbol_start(10000, 8000, 0, 1), 10000);
    WCX_CHECK_EQ_U64(pcie_edge_end(10000, 8000, 1), 18000);
    // The first edge: P = 0, every lane at the edge with zero duration.
    WCX_CHECK_EQ_U64(pcie_symbol_start(500, 0, 7, 8), 500);
    WCX_CHECK_EQ_U64(pcie_edge_end(500, 0, 8), 500);
    WCX_CHECK_EQ_U64(pcie_symbol_end(500, 0), 500);
}

WCX_TEST(a_huge_period_does_not_overflow_the_lane_product) {
    // P = UINT64_MAX = 8 q + 7 with q = UINT64_MAX / 8. Lane 7 starts at
    // 7 q + floor(7 * 7 / 8) = 7 q + 6 and the edge ends at 8 q + 6 =
    // UINT64_MAX - 1: no saturation, and no wrapped 7 * P.
    const uint64_t q = UINT64_MAX / 8u;
    WCX_CHECK_EQ_U64(pcie_symbol_start(0, UINT64_MAX, 7, 8), 7u * q + 6u);
    WCX_CHECK_EQ_U64(pcie_edge_end(0, UINT64_MAX, 8), UINT64_MAX - 1u);
}

WCX_TEST(times_saturate_at_the_end_of_the_range) {
    // An edge 5 fs before the end with P = 100 and 8 lanes: lane 0 is still
    // exact, lane 1 (+12) and everything after clamp to UINT64_MAX.
    const uint64_t edge = UINT64_MAX - 5u;
    WCX_CHECK_EQ_U64(pcie_symbol_start(edge, 100, 0, 8), edge);
    WCX_CHECK_EQ_U64(pcie_symbol_start(edge, 100, 1, 8), UINT64_MAX);
    WCX_CHECK_EQ_U64(pcie_symbol_start(edge, 100, 7, 8), UINT64_MAX);
    WCX_CHECK_EQ_U64(pcie_symbol_end(edge, 12), UINT64_MAX);
    WCX_CHECK_EQ_U64(pcie_symbol_end(edge, 5), UINT64_MAX); // exactly fits
    WCX_CHECK_EQ_U64(pcie_symbol_end(edge, 4), UINT64_MAX - 1u);
    WCX_CHECK_EQ_U64(pcie_edge_end(edge, 100, 8), UINT64_MAX);
    WCX_CHECK_EQ_U64(pcie_time_add(UINT64_MAX, 1), UINT64_MAX);
    WCX_CHECK_EQ_U64(pcie_time_add(UINT64_MAX, 0), UINT64_MAX);
}

WCX_TEST(plugin_clamps_symbol_times_near_the_end_of_the_range) {
    // 8-bit PIPE, rising edges at UINT64_MAX - 8000 and UINT64_MAX - 100
    // (P = 7900), each carrying an invalid K symbol. The first edge has
    // P = 0 (zero length); the second symbol would end at UINT64_MAX + 7800
    // and is clamped to UINT64_MAX.
    abi_drv d;
    const abi_cfg cfg = {1, false, false, false};
    WCX_REQUIRE(abi_open(&d, t, "ferrite.pcie_pipe_w8", &cfg, "{\"scrambling\":\"off\"}",
                         UINT64_MAX - 8000u, 7900));
    static const tsym bad[1] = {KS(0x9C)};
    const abi_word w = abi_word_from(bad, 1);
    abi_edge(&d, &w);
    abi_edge(&d, &w);
    WCX_REQUIRE(d.log.n == 2);
    WCX_CHECK_STR_EQ(d.log.items[0].label, "Invalid K symbol 0x9C");
    WCX_CHECK_EQ_U64(d.log.items[0].start, UINT64_MAX - 8000u);
    WCX_CHECK_EQ_U64(d.log.items[0].end, UINT64_MAX - 8000u);
    WCX_CHECK_EQ_U64(d.log.items[1].start, UINT64_MAX - 100u);
    WCX_CHECK_EQ_U64(d.log.items[1].end, UINT64_MAX);
    abi_close(&d);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, lane_times_are_floor_of_j_p_over_n);
    WCX_RUN(&t, a_huge_period_does_not_overflow_the_lane_product);
    WCX_RUN(&t, times_saturate_at_the_end_of_the_range);
    WCX_RUN(&t, plugin_clamps_symbol_times_near_the_end_of_the_range);
    return wcx_test_finish(&t);
}
