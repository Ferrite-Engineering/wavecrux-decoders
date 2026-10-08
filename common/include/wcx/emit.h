// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Transaction output queue implementing the ABI's NEED_MORE_SLOTS contract.
//
// The contract (wavecrux_decoder.h, "lifecycle callback signatures"): the
// host passes a buffer of *inout_count slots. If a call has more
// transactions than that, the plugin returns WC_DECODER_NEED_MORE_SLOTS with
// *inout_count set to the number required, the host copies NOTHING, and
// calls again with the SAME sample and a buffer of that size. The retry
// must deliver the same transactions, in order, exactly once, and must not
// decode the sample a second time (decoding is stateful: a second pass would
// advance the protocol state machine twice). The strings the transactions
// point at must stay valid until the call after the one that delivered them.
//
// The state machine (one queue per handle):
//
//               push...                drain: fits -> OK
//   [EMPTY] ---------------> [FILLED] -------------------------> [DELIVERED]
//      ^                        |  ^                                  |
//      |                        |  | retry: drain again               |
//      |      drain: too big    v  |                                  |
//      |                     [PENDING]                                |
//      +--------- wcx_emit_begin() on the next call (reset arena) ----+
//
//   * wcx_emit_begin(q) at the start of every feed/flush that is NOT a
//     retry. It returns true when the previous batch was delivered: the queue
//     is now empty and the caller MUST reset its string arena (the host has
//     copied everything). It returns false when a batch is still pending
//     (the host did not retry but moved on, which a conforming host never
//     does): the pending transactions are kept, nothing is lost, the arena
//     must NOT be reset, and new transactions are appended after them.
//   * wcx_emit_push adds a transaction (strings must live in the arena or
//     in static storage).
//   * wcx_emit_drain copies everything into the host's buffer, or nothing
//     and returns NEED_MORE_SLOTS with the count required.
//   * A retry is recognised by wcx_emit_pending(q) being true when the next
//     call arrives with the same sample: then call ONLY wcx_emit_drain.
//
// The decoder base (wcx/decoder.h) runs this machine for every decoder; a
// decoder built on it never sees a retry and never touches this header.
//
// Bounded: at most `ceiling` transactions per call (derive it from the
// protocol: the most a single sample, or flush, can complete). Pushing more
// fails; one extra reserved slot lets the caller still report that as an
// error transaction (wcx_emit_push_reserved).

#ifndef WCX_EMIT_H
#define WCX_EMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wavecrux_decoder.h"
#include "wcx/export.h"

typedef struct wcx_emit_queue {
    WcTransaction *items; // ceiling + 1 slots, allocated once
    size_t count;
    size_t ceiling;
    bool pending;   // last drain returned NEED_MORE_SLOTS; batch undelivered
    bool delivered; // last drain delivered the batch; cleared by begin
} wcx_emit_queue;

// Allocates a queue for up to `ceiling` (>= 1) transactions per call.
WCX_NODISCARD bool wcx_emit_init(wcx_emit_queue *q, size_t ceiling);
void wcx_emit_free(wcx_emit_queue *q);

// Starts a call that is not a retry. See the state machine above: true
// means "queue emptied, reset your arena now".
WCX_NODISCARD bool wcx_emit_begin(wcx_emit_queue *q);

// True when a batch is waiting for the host's retry.
bool wcx_emit_pending(const wcx_emit_queue *q);

// Queues one transaction. False when the queue holds `ceiling` already or
// label / fields_json is NULL (then nothing was queued).
WCX_NODISCARD bool wcx_emit_push(wcx_emit_queue *q, uint64_t start_fs, uint64_t end_fs,
                                 const char *label, const char *fields_json, bool is_error);

// Queues one transaction into the reserved slot beyond the ceiling (once
// per call): for the error transaction that reports an overflow or failure.
WCX_NODISCARD bool wcx_emit_push_reserved(wcx_emit_queue *q, uint64_t start_fs, uint64_t end_fs,
                                          const char *label, const char *fields_json,
                                          bool is_error);

// Number of transactions queued for this call.
size_t wcx_emit_count(const wcx_emit_queue *q);

// Copies the queued transactions to `out` (capacity *inout_count) and
// returns WC_DECODER_OK with *inout_count = the number written, or, when
// they do not all fit, writes nothing, sets *inout_count to the number
// required and returns WC_DECODER_NEED_MORE_SLOTS (the batch stays pending).
// WC_DECODER_ERR when inout_count is NULL.
int32_t wcx_emit_drain(wcx_emit_queue *q, WcTransaction *out, size_t *inout_count);

#endif // WCX_EMIT_H
