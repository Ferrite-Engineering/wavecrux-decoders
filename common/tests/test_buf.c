// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include <stdint.h>
#include <stdlib.h>

#include "wcx/buf.h"
#include "wcx/test.h"

WCX_TEST(reserve_grows_and_keeps_contents) {
    uint32_t *p = NULL;
    size_t cap = 0;
    WCX_REQUIRE(wcx_buf_reserve((void **)&p, &cap, sizeof *p, 3, 100));
    WCX_CHECK(cap >= 3);
    for (uint32_t i = 0; i < 3; i++) {
        p[i] = i * 7u;
    }
    WCX_REQUIRE(wcx_buf_reserve((void **)&p, &cap, sizeof *p, 50, 100));
    WCX_CHECK(cap >= 50 && cap <= 100);
    WCX_CHECK_EQ_U64(p[2], 14);
    free(p);
}

WCX_TEST(reserve_respects_ceiling) {
    uint8_t *p = NULL;
    size_t cap = 0;
    WCX_CHECK(!wcx_buf_reserve((void **)&p, &cap, 1, 101, 100));
    WCX_CHECK(p == NULL && cap == 0);
    WCX_REQUIRE(wcx_buf_reserve((void **)&p, &cap, 1, 100, 100));
    WCX_CHECK_EQ_U64(cap, 100);
    WCX_CHECK(!wcx_buf_reserve((void **)&p, &cap, 1, 101, 100));
    WCX_CHECK_EQ_U64(cap, 100);
    free(p);
}

WCX_TEST(reserve_rejects_overflow_and_bad_args) {
    uint8_t *p = NULL;
    size_t cap = 0;
    WCX_CHECK(!wcx_buf_reserve((void **)&p, &cap, SIZE_MAX / 2, 4, SIZE_MAX));
    WCX_CHECK(!wcx_buf_reserve(NULL, &cap, 1, 1, 1));
    WCX_CHECK(!wcx_buf_reserve((void **)&p, &cap, 0, 1, 1));
    WCX_CHECK(wcx_buf_reserve((void **)&p, &cap, 1, 0, 1)); // nothing needed
    WCX_CHECK(p == NULL);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, reserve_grows_and_keeps_contents);
    WCX_RUN(&t, reserve_respects_ceiling);
    WCX_RUN(&t, reserve_rejects_overflow_and_bad_args);
    return wcx_test_finish(&t);
}
