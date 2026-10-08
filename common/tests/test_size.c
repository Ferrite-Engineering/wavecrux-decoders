// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "wcx/size.h"
#include "wcx/test.h"

WCX_TEST(size_add) {
    size_t out = 99;
    WCX_CHECK(wcx_size_add(1, 2, &out));
    WCX_CHECK_EQ_U64(out, 3);
    WCX_CHECK(wcx_size_add(SIZE_MAX - 1, 1, &out));
    WCX_CHECK_EQ_U64(out, SIZE_MAX);
    WCX_CHECK(!wcx_size_add(SIZE_MAX, 1, &out));
    WCX_CHECK_EQ_U64(out, 0);
}

WCX_TEST(size_mul) {
    size_t out = 99;
    WCX_CHECK(wcx_size_mul(0, SIZE_MAX, &out));
    WCX_CHECK_EQ_U64(out, 0);
    WCX_CHECK(wcx_size_mul(SIZE_MAX / 2, 2, &out));
    WCX_CHECK(!wcx_size_mul(SIZE_MAX / 2 + 1, 2, &out));
    WCX_CHECK_EQ_U64(out, 0);
    WCX_CHECK(!wcx_size_mul(SIZE_MAX, SIZE_MAX, &out));
}

WCX_TEST(u64_ops) {
    uint64_t out = 1;
    WCX_CHECK(wcx_u64_add(UINT64_MAX - 5, 5, &out));
    WCX_CHECK_EQ_U64(out, UINT64_MAX);
    WCX_CHECK(!wcx_u64_add(UINT64_MAX, 1, &out));
    WCX_CHECK(wcx_u64_mul(1000000u, 1000000u, &out));
    WCX_CHECK_EQ_U64(out, 1000000000000u);
    WCX_CHECK(!wcx_u64_mul(UINT64_MAX / 3 + 1, 3, &out));
    WCX_CHECK_EQ_U64(out, 0);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, size_add);
    WCX_RUN(&t, size_mul);
    WCX_RUN(&t, u64_ops);
    return wcx_test_finish(&t);
}
