// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Section references are to decoders/pcie/SPEC.md.

#include "pcie_detector.h"

#include <string.h>

#include "wcx/json_writer.h"

#define BR_OFF 0u
#define BR_ON  1u

static void branch_init(pcie_detector *det, unsigned i, bool descramble, bool buffered,
                        bool coalesce, bool show_idle) {
    pcie_branch *b = &det->br[i];
    b->sink.d = det->d;
    b->sink.buf = buffered ? &b->buf : NULL;
    b->sink.buf_arena = &det->arena;
    if (det->dll) {
        pcie_dll_fe_init(&b->fe.dll_fe, &b->sink);
        pcie_pipeline_init(&b->pipe, descramble, pcie_dll_fe_event, &b->fe.dll_fe);
    } else {
        pcie_pipe_fe_init(&b->fe.pipe_fe, &b->sink, coalesce, show_idle);
        pcie_pipeline_init(&b->pipe, descramble, pcie_pipe_fe_event, &b->fe.pipe_fe);
    }
}

bool pcie_detector_init(pcie_detector *det, wcx_decoder *d, pcie_scrambling_param mode, bool dll,
                        bool coalesce, bool show_idle) {
    memset(det, 0, sizeof *det);
    det->d = d;
    det->dll = dll;
    det->mode = mode;
    if (mode != PCIE_SCRAMBLING_AUTO) {
        det->branches = 1;
        det->locked = true;
        det->winner = 0;
        branch_init(det, 0, mode == PCIE_SCRAMBLING_ON, false, coalesce, show_idle);
        return true;
    }
    det->branches = 2;
    if (!wcx_arena_init(&det->arena, PCIE_AUTO_ARENA_BLOCK, PCIE_AUTO_ARENA_MAX)) {
        return false; /* defensive: allocation failure */
    }
    det->arena_live = true;
    for (unsigned i = 0; i < 2u; i++) {
        if (!pcie_txbuf_init(&det->br[i].buf, PCIE_AUTO_BUFFER_CAP)) {
            return false; /* defensive: allocation failure */
        }
        branch_init(det, i, i == BR_ON, true, coalesce, show_idle);
    }
    return true;
}

void pcie_detector_free(pcie_detector *det) {
    for (unsigned i = 0; i < 2u; i++) {
        pcie_txbuf_free(&det->br[i].buf);
    }
    if (det->arena_live) {
        wcx_arena_free(&det->arena);
        det->arena_live = false;
    }
}

void pcie_detector_begin_call(pcie_detector *det) {
    // The lock handed the base WcTransactions whose strings live in
    // det->arena. They must outlive the batch they are part of, and that is
    // not necessarily until this call: a host that gets NEED_MORE_SLOTS may
    // move on to another sample instead of retrying (wcx/emit.h), and the
    // base then keeps the batch pending and appends to it. So the arena is
    // freed only once the base reports the previous batch delivered, which
    // is also when it resets its own arena.
    if (det->arena_lent_to_host && wcx_decoder_batch_delivered(det->d)) {
        wcx_arena_free(&det->arena);
        det->arena_live = false;
        det->arena_lent_to_host = false;
    }
}

// ── the lock (§5.5) ────────────────────────────────────────────────────────

static void lock(pcie_detector *det, unsigned winner, const char *label, const char *mode,
                 uint64_t start_fs, uint64_t end_fs) {
    pcie_branch *w = &det->br[winner];
    for (size_t i = 0; i < w->buf.count; i++) {
        const pcie_tx *t = &w->buf.items[i];
        // Strings point into det->arena, which lives until the next call.
        (void)wcx_emit(det->d, t->start_fs, t->end_fs, t->label, t->fields, t->is_error);
    }
    wcx_json j;
    wcx_json_begin(&j, wcx_decoder_arena(det->d));
    wcx_json_add_str(&j, "type", "scrambling_detected");
    wcx_json_add_str(&j, "mode", mode);
    (void)wcx_emit(det->d, start_fs, end_fs, label, wcx_json_end(&j), false);
    for (unsigned i = 0; i < 2u; i++) {
        pcie_txbuf_free(&det->br[i].buf); // the WcTransactions were copied into the base queue
    }
    w->sink.buf = NULL;
    det->locked = true;
    det->winner = winner;
    det->arena_lent_to_host = true; // freed by pcie_detector_begin_call once delivered
}

static void lock_undetermined(pcie_detector *det, uint64_t now_fs) {
    const pcie_txbuf *b = &det->br[BR_OFF].buf;
    uint64_t start = now_fs;
    uint64_t end = now_fs;
    if (b->count > 0) {
        start = b->items[b->count - 1u].start_fs;
        end = b->items[b->count - 1u].end_fs;
    } else if (det->have_sym) {
        start = det->last_sym_start_fs;
        end = det->last_sym_end_fs;
    }
    lock(det, BR_OFF, "Scrambling: off (undetermined)", "off", start, end);
}

static bool buffers_full(const pcie_detector *det) {
    for (unsigned i = 0; i < 2u; i++) {
        const pcie_txbuf *b = &det->br[i].buf;
        if (b->count >= PCIE_AUTO_LOCK_TRANSACTIONS || b->overflow) {
            return true;
        }
    }
    return false;
}

// After both pipelines took a symbol: the rules of §5.5, in priority order.
static void decide(pcie_detector *det) {
    const pcie_pipeline *off = &det->br[BR_OFF].pipe;
    const pcie_pipeline *on = &det->br[BR_ON].pipe;
    if (off->step_disable_scrambling) {
        // §10.3: the Disable Scrambling bit in a TS1/TS2.
        lock(det, BR_OFF, "Scrambling: off (training sets)", "off", off->step_start_fs,
             off->step_end_fs);
    } else if (off->step_valid_packet) {
        lock(det, BR_OFF, "Scrambling: off (detected)", "off", off->step_start_fs,
             off->step_end_fs);
    } else if (on->step_valid_packet) {
        lock(det, BR_ON, "Scrambling: on (detected)", "on", on->step_start_fs, on->step_end_fs);
    } else if (buffers_full(det)) {
        lock_undetermined(det, det->last_sym_end_fs);
    }
}

// ── feeding ────────────────────────────────────────────────────────────────

void pcie_detector_symbol(pcie_detector *det, const pcie_sym *s) {
    det->have_sym = true;
    det->last_sym_start_fs = s->start_fs;
    det->last_sym_end_fs = s->end_fs;
    if (det->locked) {
        pcie_pipeline_symbol(&det->br[det->winner].pipe, s);
        return;
    }
    for (unsigned i = 0; i < det->branches; i++) {
        pcie_pipeline_symbol(&det->br[i].pipe, s);
    }
    decide(det);
}

void pcie_detector_break(pcie_detector *det, pcie_break_reason reason) {
    if (det->locked) {
        pcie_pipeline_break(&det->br[det->winner].pipe, reason);
        return;
    }
    for (unsigned i = 0; i < det->branches; i++) {
        pcie_pipeline_break(&det->br[i].pipe, reason);
    }
    if (buffers_full(det)) {
        lock_undetermined(det, det->last_sym_end_fs);
    }
}

wcx_arena *pcie_detector_arena(pcie_detector *det) {
    return det->locked ? wcx_decoder_arena(det->d) : &det->arena;
}

void pcie_detector_emit(pcie_detector *det, uint64_t start_fs, uint64_t end_fs, const char *label,
                        const char *fields, bool is_error) {
    if (det->locked) {
        (void)wcx_emit(det->d, start_fs, end_fs, label, fields, is_error);
        return;
    }
    for (unsigned i = 0; i < det->branches; i++) {
        pcie_txbuf_push(&det->br[i].buf, start_fs, end_fs, label, fields, is_error);
    }
    if (buffers_full(det)) {
        lock_undetermined(det, end_fs);
    }
}

void pcie_detector_end_of_trace(pcie_detector *det) {
    pcie_detector_break(det, PCIE_BREAK_FLUSH);
}

void pcie_detector_finish(pcie_detector *det, uint64_t now_fs) {
    if (!det->locked) {
        lock_undetermined(det, now_fs);
    }
}
