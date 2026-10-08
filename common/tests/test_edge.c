// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include <string.h>

#include "wcx/edge.h"
#include "wcx/test.h"

static const wcx_signal_spec k_specs[] = {{"clk", 1, false}, {"d", 4, false}};

// Packs clk (with unknown flag) and a 4-bit d into buf.
static void pack(uint8_t *buf, int clk, int clk_x, unsigned d) {
    memset(buf, 0, 32);
    buf[0] = (uint8_t)((clk ? 1u : 0u) | (clk_x ? 2u : 0u));
    for (unsigned i = 0; i < 4; i++) {
        if ((d >> i) & 1u) {
            const unsigned pos = (1u + i) * 2u;
            buf[pos / 8u] = (uint8_t)(buf[pos / 8u] | (1u << (pos % 8u)));
        }
    }
}

static unsigned data_at(const wcx_layout *l, const uint8_t *bits) {
    uint64_t v = 0;
    bool x = false;
    WCX_IGNORE(wcx_sample_read(l, bits, 1, &v, &x));
    return (unsigned)v;
}

WCX_TEST(zero_delay_dump_samples_the_pre_edge_value) {
    wcx_layout l;
    const bool all[] = {true, true};
    WCX_REQUIRE(wcx_layout_build_bound(&l, k_specs, 2, all, NULL));
    wcx_edge e;
    WCX_REQUIRE(wcx_edge_init(&e, &l, 0, WCX_SAMPLE_BEFORE_EDGE));
    uint8_t buf[32];
    pack(buf, 0, 0, 0x3);
    WCX_CHECK_EQ_U64(wcx_edge_step(&e, 0, buf), WCX_EDGE_NONE); // first sample
    WCX_CHECK(wcx_edge_prev(&e) == NULL);
    WCX_CHECK_EQ_U64(data_at(&l, wcx_edge_data(&e)), 0x3);
    // Clock rises and d changes to 0x9 at the same timestamp (zero delay):
    // the flip-flop captured 0x3.
    pack(buf, 1, 0, 0x9);
    WCX_CHECK_EQ_U64(wcx_edge_step(&e, 10, buf), WCX_EDGE_RISING);
    WCX_CHECK_EQ_U64(data_at(&l, wcx_edge_data(&e)), 0x3);
    WCX_CHECK_EQ_U64(data_at(&l, wcx_edge_cur(&e)), 0x9);
    WCX_CHECK_EQ_U64(wcx_edge_prev_ts(&e), 0);
    WCX_CHECK_EQ_U64(wcx_edge_cur_ts(&e), 10);
    pack(buf, 0, 0, 0x9);
    WCX_CHECK_EQ_U64(wcx_edge_step(&e, 20, buf), WCX_EDGE_FALLING);
    wcx_edge_free(&e);
}

WCX_TEST(at_edge_sampling_and_delayed_data) {
    wcx_layout l;
    const bool all[] = {true, true};
    WCX_REQUIRE(wcx_layout_build_bound(&l, k_specs, 2, all, NULL));
    wcx_edge e;
    WCX_REQUIRE(wcx_edge_init(&e, &l, 0, WCX_SAMPLE_AT_EDGE));
    uint8_t buf[32];
    pack(buf, 0, 0, 0x1);
    (void)wcx_edge_step(&e, 0, buf);
    pack(buf, 0, 0, 0x6); // data changes between edges (testbench delay)
    WCX_CHECK_EQ_U64(wcx_edge_step(&e, 5, buf), WCX_EDGE_NONE);
    pack(buf, 1, 0, 0x6);
    WCX_CHECK_EQ_U64(wcx_edge_step(&e, 10, buf), WCX_EDGE_RISING);
    WCX_CHECK_EQ_U64(data_at(&l, wcx_edge_data(&e)), 0x6);
    // Before-edge sampling agrees when data changed between edges.
    WCX_CHECK_EQ_U64(data_at(&l, wcx_edge_prev(&e)), 0x6);
    wcx_edge_free(&e);
}

WCX_TEST(transitions_through_x_are_not_edges) {
    wcx_layout l;
    const bool all[] = {true, true};
    WCX_REQUIRE(wcx_layout_build_bound(&l, k_specs, 2, all, NULL));
    wcx_edge e;
    WCX_REQUIRE(wcx_edge_init(&e, &l, 0, WCX_SAMPLE_BEFORE_EDGE));
    uint8_t buf[32];
    pack(buf, 0, 0, 0);
    (void)wcx_edge_step(&e, 0, buf);
    pack(buf, 0, 1, 0); // X
    WCX_CHECK_EQ_U64(wcx_edge_step(&e, 1, buf), WCX_EDGE_NONE);
    pack(buf, 1, 0, 0); // X -> 1
    WCX_CHECK_EQ_U64(wcx_edge_step(&e, 2, buf), WCX_EDGE_NONE);
    pack(buf, 1, 0, 0); // 1 -> 1
    WCX_CHECK_EQ_U64(wcx_edge_step(&e, 3, buf), WCX_EDGE_NONE);
    wcx_edge_free(&e);
}

WCX_TEST(init_rejects_bad_clocks) {
    wcx_layout l;
    const bool all[] = {true, true};
    WCX_REQUIRE(wcx_layout_build_bound(&l, k_specs, 2, all, NULL));
    wcx_edge e;
    WCX_CHECK(!wcx_edge_init(&e, &l, 1, WCX_SAMPLE_BEFORE_EDGE)); // 4 bits wide
    WCX_CHECK(!wcx_edge_init(&e, &l, 7, WCX_SAMPLE_BEFORE_EDGE));
    WCX_CHECK(!wcx_edge_init(&e, NULL, 0, WCX_SAMPLE_BEFORE_EDGE));
    WCX_CHECK(!wcx_edge_init(NULL, &l, 0, WCX_SAMPLE_BEFORE_EDGE));
    wcx_edge_free(&e);
    wcx_edge_free(NULL);
    WCX_CHECK_EQ_U64(wcx_edge_step(&e, 0, NULL), WCX_EDGE_NONE);
    WCX_CHECK(wcx_edge_data(&e) == NULL);
    WCX_CHECK(wcx_edge_cur(&e) == NULL);
    WCX_CHECK(wcx_edge_prev(NULL) == NULL);
    WCX_CHECK_EQ_U64(wcx_edge_prev_ts(NULL), 0);
    WCX_CHECK_EQ_U64(wcx_edge_cur_ts(NULL), 0);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, zero_delay_dump_samples_the_pre_edge_value);
    WCX_RUN(&t, at_edge_sampling_and_delayed_data);
    WCX_RUN(&t, transitions_through_x_are_not_edges);
    WCX_RUN(&t, init_rejects_bad_clocks);
    return wcx_test_finish(&t);
}
