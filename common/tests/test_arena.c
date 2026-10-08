// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include <stdint.h>
#include <string.h>

#include "wcx/arena.h"
#include "wcx/test.h"

WCX_TEST(init_rejects_bad_sizes) {
    wcx_arena a;
    WCX_CHECK(!wcx_arena_init(&a, 0, 100));
    WCX_CHECK(!wcx_arena_init(&a, 200, 100));
    wcx_arena_free(&a);
    WCX_CHECK(!wcx_arena_init(NULL, 1, 1));
}

WCX_TEST(alloc_is_aligned_and_distinct) {
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 256, 4096));
    const char *s = wcx_arena_strdup(&a, "x");
    void *p = wcx_arena_alloc(&a, 24);
    void *q = wcx_arena_alloc(&a, 8);
    WCX_REQUIRE(s != NULL && p != NULL && q != NULL);
    WCX_CHECK_EQ_U64((uintptr_t)p % WCX_ARENA_ALIGN, 0);
    WCX_CHECK_EQ_U64((uintptr_t)q % WCX_ARENA_ALIGN, 0);
    WCX_CHECK((char *)q >= (char *)p + 24);
    WCX_CHECK(wcx_arena_alloc(&a, 0) == NULL);
    wcx_arena_free(&a);
}

WCX_TEST(strings_and_printf) {
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 64, 4096));
    const char *s = wcx_arena_strndup(&a, "hello world", 5);
    WCX_CHECK_STR_EQ(s, "hello");
    const char *f = wcx_arena_printf(&a, "%s=%u", "lanes", 4u);
    WCX_CHECK_STR_EQ(f, "lanes=4");
    // Longer than a block: lands in a block of its own.
    char big[300];
    memset(big, 'z', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    const char *g = wcx_arena_printf(&a, "%s!", big);
    WCX_REQUIRE(g != NULL);
    WCX_CHECK_EQ_U64(strlen(g), 300);
    // Earlier strings are untouched.
    WCX_CHECK_STR_EQ(s, "hello");
    WCX_CHECK_STR_EQ(f, "lanes=4");
    WCX_CHECK(wcx_arena_strdup(&a, NULL) == NULL);
    WCX_CHECK(wcx_arena_strndup(&a, NULL, 3) == NULL);
    wcx_arena_free(&a);
}

WCX_TEST(ceiling_is_enforced) {
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 64, 128));
    WCX_CHECK(wcx_arena_alloc(&a, 60) != NULL);
    WCX_CHECK(wcx_arena_alloc(&a, 60) != NULL); // second block: 128 bytes held
    WCX_CHECK(wcx_arena_alloc(&a, 60) == NULL); // a third would exceed 128
    WCX_CHECK(wcx_arena_alloc(&a, 1000) == NULL);
    WCX_CHECK(wcx_arena_printf(&a, "%0200d", 1) == NULL);
    wcx_arena_free(&a);
}

WCX_TEST(reset_reuses_blocks_without_growth) {
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 64, 256));
    for (int round = 0; round < 50; round++) {
        wcx_arena_reset(&a);
        WCX_CHECK_EQ_U64(wcx_arena_used(&a), 0);
        for (int i = 0; i < 3; i++) {
            WCX_REQUIRE(wcx_arena_alloc(&a, 48) != NULL);
        }
    }
    // Three 48-byte allocations need three 64-byte blocks, every round.
    WCX_CHECK_EQ_U64(a.reserved, 192);
    wcx_arena_free(&a);
    wcx_arena_reset(NULL);
    WCX_CHECK_EQ_U64(wcx_arena_used(NULL), 0);
}

WCX_TEST(grow_in_place_and_by_moving) {
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 64, 1024));
    char *p = wcx_arena_alloc(&a, 8);
    WCX_REQUIRE(p != NULL);
    memcpy(p, "abcdefg", 8);
    char *q = wcx_arena_grow(&a, p, 8, 32);
    WCX_CHECK(q == p); // last allocation, room in the block
    const char *other = wcx_arena_strdup(&a, "o");
    WCX_REQUIRE(other != NULL);
    char *r = wcx_arena_grow(&a, q, 32, 40); // no longer the last: moves
    WCX_REQUIRE(r != NULL);
    WCX_CHECK(r != q);
    WCX_CHECK_STR_EQ(r, "abcdefg");
    WCX_CHECK_STR_EQ(other, "o");
    char *s = wcx_arena_grow(&a, r, 40, 500); // beyond the block: new block
    WCX_REQUIRE(s != NULL);
    WCX_CHECK_STR_EQ(s, "abcdefg");
    WCX_CHECK(wcx_arena_grow(&a, s, 500, 4000) == NULL); // ceiling
    WCX_CHECK_STR_EQ(s, "abcdefg");                      // unchanged on failure
    WCX_CHECK(wcx_arena_grow(&a, s, 500, 10) == NULL);   // shrinking is not growing
    wcx_arena_free(&a);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, init_rejects_bad_sizes);
    WCX_RUN(&t, alloc_is_aligned_and_distinct);
    WCX_RUN(&t, strings_and_printf);
    WCX_RUN(&t, ceiling_is_enforced);
    WCX_RUN(&t, reset_reuses_blocks_without_growth);
    WCX_RUN(&t, grow_in_place_and_by_moving);
    return wcx_test_finish(&t);
}
