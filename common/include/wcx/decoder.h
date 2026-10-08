// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Decoder base: the ABI plumbing every decoder needs, done once and right.
//
// A decoder is a wcx_decoder_class (its id, manifest, signal table and four
// callbacks). The base implements create/feed/flush/destroy and register on
// top of it, so a decoder never handles:
//
//   * config_json: parsed, shape-checked; the decoder reads its parameters
//     in `init` through wcx_param_*, which validate type and range;
//   * the manifest: the C signal table is checked against manifest_json at
//     every create, so the two cannot drift apart unnoticed;
//   * the sample layout and WcSample.bit_width: checked before every
//     on_sample; a mismatch never reaches the decoder;
//   * WC_DECODER_NEED_MORE_SLOTS: a retry is recognised and served from the
//     queue; on_sample runs exactly once per sample (wcx/emit.h);
//   * string lifetime: wcx_decoder_arena() is reset exactly when the host has
//     copied the previous batch, never sooner;
//   * the error state: wcx_decoder_fail() (and every failure above) emits ONE
//     error transaction and then ignores all further input (C coding
//     standard §2: never abort the host);
//   * timestamps going backwards (a broken host or a fuzzer): an error.
//
// Writing a decoder:
//
//   enum { SIG_CLK, SIG_DATA, SIG_VALID };                // manifest order
//   static const wcx_signal_spec k_signals[] = {
//       {"clk", 1, false}, {"data", 8, false}, {"valid", 1, true}};
//   typedef struct my_state { wcx_edge clk; int64_t lanes; } my_state;
//
//   static bool my_init(wcx_decoder *d, void *state) {
//       my_state *st = state;
//       if (!wcx_param_int(d, "lanes", 1, 1, 32, &st->lanes)) return false;
//       return wcx_edge_init(&st->clk, wcx_decoder_layout(d), SIG_CLK,
//                            WCX_SAMPLE_BEFORE_EDGE);
//   }
//   static void my_sample(wcx_decoder *d, void *state, const wcx_sample *s) { ... wcx_emit ... }
//   static void my_flush(wcx_decoder *d, void *state) { ... }
//   static void my_fini(wcx_decoder *d, void *state) { wcx_edge_free(&((my_state *)state)->clk); }
//
//   static WcDecoderHandle my_create(const char *config_json);
//   static const wcx_decoder_class k_my_class = {
//       .id = "ferrite.my", .display_name = "My protocol", .manifest_json = k_manifest,
//       .signals = k_signals, .signal_count = 3, .state_size = sizeof(my_state),
//       .max_transactions_per_call = 8,          // derive from the protocol
//       .init = my_init, .on_sample = my_sample, .on_flush = my_flush, .fini = my_fini,
//       .create = my_create};
//   static WcDecoderHandle my_create(const char *config_json) {
//       return wcx_decoder_create(&k_my_class, config_json);
//   }
//
//   static const wcx_decoder_class *const k_classes[] = {&k_my_class};
//   WCX_PLUGIN("Ferrite My Decoders", "Apache-2.0, Ferrite Engineering", k_classes)
//
// Callback rules:
//   * init runs once, in create. Parameters are readable only there. It
//     returns false after a failure (a wcx_param_* failure has already set
//     the error; otherwise the base reports a generic one). fini runs in
//     destroy whenever init ran, even if it returned false, and must free
//     what init allocated (the state starts zeroed, so free(NULL) paths work).
//   * on_sample runs once per sample, in timestamp order, never in the error
//     state. on_flush runs once at end of stream.
//   * Emit with wcx_emit; build strings with wcx_decoder_arena() (or point at
//     static strings). Do not keep pointers to the sample bits or to arena
//     strings beyond the callback.

#ifndef WCX_DECODER_H
#define WCX_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wavecrux_decoder.h"
#include "wcx/arena.h"
#include "wcx/export.h"
#include "wcx/sample.h"

typedef struct wcx_decoder wcx_decoder;

// One sample as the decoder sees it, already validated against the layout.
typedef struct wcx_sample {
    uint64_t timestamp_fs;
    const uint8_t *bits;      // layout->encoded_bytes readable bytes; valid in the callback only
    const wcx_layout *layout; // == wcx_decoder_layout(d)
} wcx_sample;

// Defaults when a class leaves a limit at 0.
#define WCX_DEFAULT_MAX_TRANSACTIONS 64u
#define WCX_DEFAULT_ARENA_BLOCK      4096u
#define WCX_DEFAULT_ARENA_MAX        ((size_t)256u * 1024u)

typedef struct wcx_decoder_class {
    const char *id;                 // WcDecoderDef.id, e.g. "ferrite.pcie.pipe"
    const char *display_name;       // WcDecoderDef.display_name
    const char *manifest_json;      // WcDecoderDef.manifest_json (static storage)
    const wcx_signal_spec *signals; // manifest order: required, then optional
    size_t signal_count;
    size_t state_size; // bytes of zeroed per-instance state (0 for none)
    // Most transactions one sample (or flush) can produce: the queue
    // ceiling. Derive it from the protocol; 0 means WCX_DEFAULT_MAX_TRANSACTIONS.
    size_t max_transactions_per_call;
    size_t arena_block_bytes;                  // 0: WCX_DEFAULT_ARENA_BLOCK
    size_t arena_max_bytes;                    // string bytes per call; 0: WCX_DEFAULT_ARENA_MAX
    bool (*init)(wcx_decoder *d, void *state); // may be NULL
    void (*on_sample)(wcx_decoder *d, void *state, const wcx_sample *s); // required
    void (*on_flush)(wcx_decoder *d, void *state);                       // may be NULL
    void (*fini)(wcx_decoder *d, void *state);                           // may be NULL
    WcDecoderCreateFn create; // a one-line trampoline to wcx_decoder_create
} wcx_decoder_class;

// ── the ABI callbacks ──────────────────────────────────────────────────────

// Builds an instance. Returns NULL only when memory runs out; every other
// problem (bad config, unbound signal, invalid parameter, manifest/table
// mismatch) yields an instance in the error state, which reports the
// problem as one error transaction on the first feed or flush. A user sees
// that in WaveCrux; a NULL create is only a log line.
WcDecoderHandle wcx_decoder_create(const wcx_decoder_class *cls, const char *config_json);
int32_t wcx_decoder_feed(WcDecoderHandle handle, const WcSample *sample,
                         WcTransaction *out_transactions, size_t *inout_count);
int32_t wcx_decoder_flush(WcDecoderHandle handle, WcTransaction *out_transactions,
                          size_t *inout_count);
void wcx_decoder_destroy(WcDecoderHandle handle);

// wavecrux_decoder_register for an array of classes (two-call pattern:
// NEED_MORE_SLOTS with the count when out_defs is NULL or too small).
WCX_NODISCARD int32_t wcx_register(const wcx_decoder_class *const *classes, size_t count,
                                   WcDecoderDef *out_defs, size_t *inout_count);

// Defines the four exported entry points of wavecrux_decoder.h for a static
// array of class pointers. Use once per plugin, at file scope, with no
// trailing semicolon.
#define WCX_PLUGIN(plugin_name, plugin_description, class_array)                                   \
    WCX_EXPORT uint32_t wavecrux_decoder_abi_version(void) {                                       \
        return WAVECRUX_DECODER_ABI_VERSION;                                                       \
    }                                                                                              \
    WCX_EXPORT int32_t wavecrux_decoder_register(WcDecoderDef *out_defs, size_t *inout_count) {    \
        return wcx_register(class_array, sizeof(class_array) / sizeof((class_array)[0]), out_defs, \
                            inout_count);                                                          \
    }                                                                                              \
    WCX_EXPORT const char *wavecrux_decoder_plugin_name(void) {                                    \
        return plugin_name;                                                                        \
    }                                                                                              \
    WCX_EXPORT const char *wavecrux_decoder_plugin_description(void) {                             \
        return plugin_description;                                                                 \
    }

// ── for the callbacks ──────────────────────────────────────────────────────

// Queues a transaction (times in femtoseconds; label and fields_json from
// the arena or static storage). Returns false, and drops it, in the error
// state. A NULL label or fields_json (an arena allocation that failed) or a
// full queue puts the instance into the error state. Ill-formed UTF-8 in
// either string is replaced with U+FFFD (the host rejects ill-formed UTF-8).
bool wcx_emit(wcx_decoder *d, uint64_t start_fs, uint64_t end_fs, const char *label,
              const char *fields_json, bool is_error);

// Puts the instance into the error state with a message (one sentence that
// names the field and the expected value). The first call wins; the error
// transaction is emitted at the current sample's time.
void wcx_decoder_fail(wcx_decoder *d, const char *fmt, ...) WCX_PRINTF(2, 3);
bool wcx_decoder_failed(const wcx_decoder *d);

// Parameters (init only). Each returns false after putting the instance
// into the error state with a message naming the parameter.
WCX_NODISCARD bool wcx_param_int(wcx_decoder *d, const char *name, int64_t def, int64_t min,
                                 int64_t max, int64_t *out);
WCX_NODISCARD bool wcx_param_bool(wcx_decoder *d, const char *name, bool def, bool *out);
WCX_NODISCARD bool wcx_param_string(wcx_decoder *d, const char *name, const char *def, char *buf,
                                    size_t cap);
WCX_NODISCARD bool wcx_param_enum(wcx_decoder *d, const char *name, const char *const *values,
                                  size_t count, size_t def_index, size_t *out_index);

const wcx_layout *wcx_decoder_layout(const wcx_decoder *d);
wcx_arena *wcx_decoder_arena(wcx_decoder *d);
bool wcx_decoder_is_bound(const wcx_decoder *d, size_t sig);
// Timestamp (fs) of the sample being decoded; in flush, of the last sample.
uint64_t wcx_decoder_now(const wcx_decoder *d);

// True during on_sample / on_flush when every transaction handed out by the
// previous call has been copied by the host (the base reset its arena at the
// start of this call). False when that batch is still pending: the host
// answered NEED_MORE_SLOTS by moving on to another sample instead of
// retrying (wcx/emit.h), so the base kept the batch and appends this call's
// output to it. A decoder that passed wcx_emit pointers into storage of its
// own (not the base arena) must keep that storage until this returns true.
bool wcx_decoder_batch_delivered(const wcx_decoder *d);
const wcx_decoder_class *wcx_decoder_class_of(const wcx_decoder *d);

#endif // WCX_DECODER_H
