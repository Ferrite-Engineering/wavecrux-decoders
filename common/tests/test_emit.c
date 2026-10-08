// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The NEED_MORE_SLOTS contract, driven exactly as WaveCrux's loader does
// (loader decode(): on NEED_MORE_SLOTS it frees the buffer, allocates
// *inout_count slots and calls again with the same sample).

#include <string.h>

#include "wcx/arena.h"
#include "wcx/emit.h"
#include "wcx/str.h"
#include "wcx/test.h"

static void push_n(wcx_test *t, wcx_emit_queue *q, wcx_arena *a, unsigned n, unsigned base) {
    for (unsigned i = 0; i < n; i++) {
        const char *label = wcx_arena_printf(a, "tx%u", base + i);
        WCX_CHECK(wcx_emit_push(q, base + i, base + i + 1u, label, "{}", false));
    }
}

WCX_TEST(fits_in_one_call) {
    wcx_emit_queue q;
    WCX_REQUIRE(wcx_emit_init(&q, 8));
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 128, 4096));
    WCX_CHECK(wcx_emit_begin(&q));
    push_n(t, &q, &a, 3, 0);
    WcTransaction out[4];
    size_t n = 4;
    WCX_CHECK_EQ_I64(wcx_emit_drain(&q, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 3);
    WCX_CHECK_STR_EQ(out[2].label, "tx2");
    WCX_CHECK_EQ_U64(out[2].start_fs, 2);
    WCX_CHECK_EQ_U64(out[2]._reserved0, 0);
    WCX_CHECK(!wcx_emit_pending(&q));
    wcx_arena_free(&a);
    wcx_emit_free(&q);
}

WCX_TEST(need_more_slots_then_retry_delivers_exactly_once) {
    wcx_emit_queue q;
    WCX_REQUIRE(wcx_emit_init(&q, 8));
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 128, 4096));
    WCX_CHECK(wcx_emit_begin(&q));
    push_n(t, &q, &a, 5, 10);
    WcTransaction small[2];
    memset(small, 0xAB, sizeof small);
    size_t n = 2;
    WCX_CHECK_EQ_I64(wcx_emit_drain(&q, small, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_U64(n, 5);
    WCX_CHECK_EQ_U64(small[0].start_fs, 0xABABABABABABABABu); // buffer untouched
    WCX_CHECK(wcx_emit_pending(&q));
    // The retry: same queue, bigger buffer, no new pushes.
    WcTransaction big[5];
    n = 5;
    WCX_CHECK_EQ_I64(wcx_emit_drain(&q, big, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 5);
    for (unsigned i = 0; i < 5; i++) {
        char want[8] = {0};
        WCX_IGNORE(wcx_str_format(want, sizeof want, "tx%u", 10u + i));
        WCX_CHECK_STR_EQ(big[i].label, want); // arena strings still valid
    }
    // Next call: the batch was delivered, the queue empties.
    WCX_CHECK(wcx_emit_begin(&q));
    WCX_CHECK_EQ_U64(wcx_emit_count(&q), 0);
    n = 5;
    WCX_CHECK_EQ_I64(wcx_emit_drain(&q, big, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 0);
    wcx_arena_free(&a);
    wcx_emit_free(&q);
}

WCX_TEST(zero_slots_and_null_buffer) {
    wcx_emit_queue q;
    WCX_REQUIRE(wcx_emit_init(&q, 4));
    WCX_CHECK(wcx_emit_begin(&q));
    size_t n = 0;
    WCX_CHECK_EQ_I64(wcx_emit_drain(&q, NULL, &n), WC_DECODER_OK); // nothing queued
    WCX_CHECK(wcx_emit_begin(&q));
    WCX_CHECK(wcx_emit_push(&q, 1, 2, "x", "{}", true));
    n = 0;
    WCX_CHECK_EQ_I64(wcx_emit_drain(&q, NULL, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_U64(n, 1);
    n = 5;
    WCX_CHECK_EQ_I64(wcx_emit_drain(&q, NULL, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK_EQ_I64(wcx_emit_drain(&q, NULL, NULL), WC_DECODER_ERR);
    WcTransaction out[1];
    n = 1;
    WCX_CHECK_EQ_I64(wcx_emit_drain(&q, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(out[0].is_error, 1);
    wcx_emit_free(&q);
}

WCX_TEST(pending_batch_survives_a_host_that_moves_on) {
    // A host that does not retry but calls with the next sample: nothing is
    // lost or duplicated, and begin() tells the caller not to reset its arena.
    wcx_emit_queue q;
    WCX_REQUIRE(wcx_emit_init(&q, 8));
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 128, 4096));
    WCX_CHECK(wcx_emit_begin(&q));
    push_n(t, &q, &a, 2, 0);
    size_t n = 1;
    WcTransaction out[8];
    WCX_CHECK_EQ_I64(wcx_emit_drain(&q, out, &n), WC_DECODER_NEED_MORE_SLOTS);
    WCX_CHECK(!wcx_emit_begin(&q)); // pending: do not reset
    push_n(t, &q, &a, 2, 2);
    n = 8;
    WCX_CHECK_EQ_I64(wcx_emit_drain(&q, out, &n), WC_DECODER_OK);
    WCX_CHECK_EQ_U64(n, 4);
    WCX_CHECK_STR_EQ(out[0].label, "tx0");
    WCX_CHECK_STR_EQ(out[3].label, "tx3");
    wcx_arena_free(&a);
    wcx_emit_free(&q);
}

WCX_TEST(ceiling_and_reserved_slot) {
    wcx_emit_queue q;
    WCX_REQUIRE(wcx_emit_init(&q, 2));
    WCX_CHECK(wcx_emit_begin(&q));
    WCX_CHECK(wcx_emit_push(&q, 0, 0, "a", "{}", false));
    WCX_CHECK(wcx_emit_push(&q, 0, 0, "b", "{}", false));
    WCX_CHECK(!wcx_emit_push(&q, 0, 0, "c", "{}", false));
    WCX_CHECK(wcx_emit_push_reserved(&q, 0, 0, "overflow", "{}", true));
    WCX_CHECK(!wcx_emit_push_reserved(&q, 0, 0, "again", "{}", true));
    WCX_CHECK_EQ_U64(wcx_emit_count(&q), 3);
    WCX_CHECK(!wcx_emit_push(&q, 0, 0, NULL, "{}", false));
    WCX_CHECK(!wcx_emit_push(&q, 0, 0, "x", NULL, false));
    wcx_emit_free(&q);
    WCX_CHECK(!wcx_emit_init(&q, 0));
    WCX_CHECK(!wcx_emit_init(NULL, 1));
    WCX_CHECK(!wcx_emit_push(NULL, 0, 0, "x", "{}", false));
    WCX_CHECK(!wcx_emit_begin(NULL));
    WCX_CHECK_EQ_U64(wcx_emit_count(NULL), 0);
    wcx_emit_free(NULL);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, fits_in_one_call);
    WCX_RUN(&t, need_more_slots_then_retry_delivers_exactly_once);
    WCX_RUN(&t, zero_slots_and_null_buffer);
    WCX_RUN(&t, pending_batch_survives_a_host_that_moves_on);
    WCX_RUN(&t, ceiling_and_reserved_slot);
    return wcx_test_finish(&t);
}
