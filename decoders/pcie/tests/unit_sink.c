// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The transaction buffer behind `auto` scrambling detection and the K-code
// table (SPEC.md §5.1, §5.5).

#include <string.h>

#include "pcie_sink.h"
#include "pcie_symbols.h"
#include "pcie_test_util.h"

WCX_TEST(txbuf_holds_its_capacity_and_flags_overflow) {
    pcie_txbuf b;
    WCX_REQUIRE(pcie_txbuf_init(&b, 2));
    WCX_CHECK_EQ_U64(b.cap, 2);
    pcie_txbuf_push(&b, 1, 2, "a", "{}", false);
    pcie_txbuf_push(&b, 3, 4, "b", "{}", true);
    WCX_CHECK_EQ_U64(b.count, 2);
    WCX_CHECK(!b.overflow);
    WCX_CHECK_STR_EQ(b.items[1].label, "b");
    WCX_CHECK(b.items[1].is_error);
    WCX_CHECK_EQ_U64(b.items[1].start_fs, 3);
    WCX_CHECK_EQ_U64(b.items[1].end_fs, 4);
    // A third push does not fit: dropped, and the buffer says so.
    pcie_txbuf_push(&b, 5, 6, "c", "{}", false);
    WCX_CHECK_EQ_U64(b.count, 2);
    WCX_CHECK(b.overflow);
    pcie_txbuf_free(&b);
    WCX_CHECK(b.items == NULL && b.count == 0 && b.cap == 0);
    pcie_txbuf_free(&b); // idempotent
}

WCX_TEST(txbuf_treats_a_null_string_as_overflow) {
    // A NULL label or fields_json means the arena ran out (the base would
    // put the instance into its error state); the detector reads `overflow`
    // and locks instead of buffering a broken transaction.
    pcie_txbuf b;
    WCX_REQUIRE(pcie_txbuf_init(&b, 4));
    pcie_txbuf_push(&b, 1, 2, NULL, "{}", false);
    WCX_CHECK_EQ_U64(b.count, 0);
    WCX_CHECK(b.overflow);
    b.overflow = false;
    pcie_txbuf_push(&b, 1, 2, "x", NULL, false);
    WCX_CHECK_EQ_U64(b.count, 0);
    WCX_CHECK(b.overflow);
    pcie_txbuf_free(&b);
    // A zero capacity still allocates (so items is never NULL) and overflows
    // on the first push.
    WCX_REQUIRE(pcie_txbuf_init(&b, 0));
    WCX_CHECK(b.items != NULL);
    pcie_txbuf_push(&b, 1, 2, "x", "{}", false);
    WCX_CHECK(b.overflow);
    pcie_txbuf_free(&b);
}

WCX_TEST(buffered_sink_uses_its_own_arena) {
    test_sink s;
    WCX_REQUIRE(test_sink_init(&s, 4));
    WCX_CHECK(pcie_sink_arena(&s.sink) == &s.arena);
    const char *label = wcx_arena_strdup(&s.arena, "hello");
    pcie_sink_emit(&s.sink, 10, 20, label, "{\"type\":\"x\"}", false);
    WCX_REQUIRE(s.buf.count == 1);
    WCX_CHECK_STR_EQ(s.buf.items[0].label, "hello");
    WCX_CHECK_EQ_U64(s.buf.items[0].start_fs, 10);
    test_sink_free(&s);
}

WCX_TEST(k_code_table_is_exactly_the_ten_of_spec_5_1) {
    static const uint8_t known[10] = {0xBC, 0xFB, 0x5C, 0xFD, 0xFE, 0xF7, 0x1C, 0x3C, 0x7C, 0xFC};
    for (unsigned b = 0; b < 256u; b++) {
        bool expected = false;
        for (unsigned i = 0; i < 10u; i++) {
            expected = expected || known[i] == b;
        }
        if (pcie_k_is_known((uint8_t)b) != expected) {
            WCX_FAIL("K code 0x%02X: got %d, want %d", b, pcie_k_is_known((uint8_t)b) ? 1 : 0,
                     expected ? 1 : 0);
        }
        t->checks++;
    }
    // The two reserved K28.4 / K28.6 codes named in protocol-notes.md §1.
    WCX_CHECK(!pcie_k_is_known(0x9C));
    WCX_CHECK(!pcie_k_is_known(0xDC));
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, txbuf_holds_its_capacity_and_flags_overflow);
    WCX_RUN(&t, txbuf_treats_a_null_string_as_overflow);
    WCX_RUN(&t, buffered_sink_uses_its_own_arena);
    WCX_RUN(&t, k_code_table_is_exactly_the_ten_of_spec_5_1);
    return wcx_test_finish(&t);
}
