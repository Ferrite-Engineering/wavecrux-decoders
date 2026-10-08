// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The host emulator's core: everything WaveCrux's
// _PluginProtocolDecoder.decode() does (lib/services/decoders/ffi/
// ffi_decoder_loader_io.dart lines 902-1125), split into
//
//   input.c      the inputs decode() is given: the instance configuration
//                (decoder_config_dialog.dart seeding + _serializeConfig),
//                the timeline (_collectTimestamps) and the time base
//                (_fsPerTick);
//   session.c    the create / feed... / flush / destroy loop with the
//                loader's NEED_MORE_SLOTS handling and string copying
//                (_drainTransactions);
//   output.c     canonical JSON output, expected-file parsing, fuzz seeds;
//   lifecycle.c  the --lifecycle contract checks.

#ifndef WCXH_HOST_H
#define WCXH_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "plugin.h"
#include "vcd.h"
#include "wavecrux_decoder.h"
#include "wcx/error.h"
#include "wcx/export.h"
#include "wcx/manifest.h"

// ── input ───────────────────────────────────────────────────────────────────

typedef struct wcxh_param_override {
    const char *name;
    const char *value; // text from --param name=value, typed by the parameter's kind
} wcxh_param_override;

typedef struct wcxh_setup {
    const char *decoder_id; // --decoder; the bindings file's "decoder" wins
    const char *vcd_path;
    const char *bindings_path;
    const wcxh_param_override *params;
    size_t param_count;
} wcxh_setup;

typedef struct wcxh_input {
    const WcDecoderDef *def;
    size_t def_index; // position in register order
    wcx_manifest manifest;
    char *vcd_text;
    size_t vcd_len;
    wcxh_vcd vcd;
    bool bound[WCX_MAX_SIGNALS];               // per manifest signal
    size_t vcd_sig[WCX_MAX_SIGNALS];           // its signal in vcd.sigs, when bound
    char *binding_ref[WCX_MAX_SIGNALS];        // its signal_bindings value, when bound
    char *param_json[WCX_MANIFEST_MAX_PARAMS]; // compact JSON value per parameter
    uint32_t bits_per_sample;
    uint64_t fs_per_tick;
    uint64_t *timeline; // ticks, ascending, unique
    size_t timeline_count;
    char *config_json; // what WaveCrux passes to create
} wcxh_input;

WCX_NODISCARD bool wcxh_input_build(wcxh_input *in, const wcxh_plugin *p, const wcxh_setup *s,
                                    wcx_error *err);
void wcxh_input_free(wcxh_input *in);

// A variation of the configuration, for the lifecycle tests.
typedef struct wcxh_config_variant {
    size_t param;            // replace this parameter's value (SIZE_MAX: none)
    const char *param_value; // raw text written in its place
    bool drop_bindings;
    const char *decoder_id; // raw text replacing the decoder_id value (NULL: keep)
} wcxh_config_variant;

// The configuration, optionally varied; malloc'd (NULL on failure).
char *wcxh_build_config(const wcxh_input *in, const wcxh_config_variant *v);

// ── a decode run ────────────────────────────────────────────────────────────

typedef struct wcxh_tx {
    uint64_t start; // ticks (floor of start_fs)
    uint64_t end;   // ticks (ceil of end_fs)
    char *label;
    char *fields; // canonical fields object
    bool is_error;
} wcxh_tx;

typedef struct wcxh_txlist {
    wcxh_tx *items;
    size_t count;
    size_t cap;
} wcxh_txlist;

typedef struct wcxh_run_opts {
    size_t slots;        // slots offered on the first call (WaveCrux: 16)
    bool reset_slots;    // offer `slots` again on every call, not just the first
    bool fresh_bits;     // a new bits buffer for every feed, freed right after it
    bool strict;         // soft contract breaches (non-object fields_json, ...) are violations
    const char *config;  // instead of in->config_json
    bool null_config;    // pass NULL to create
    size_t max_samples;  // feed at most this many samples
    bool override_width; // claim `width` in every WcSample (buffer sized to match)
    uint32_t width;
    bool skip_flush;
} wcxh_run_opts;

void wcxh_run_opts_default(wcxh_run_opts *o);

typedef struct wcxh_run {
    wcxh_txlist tx;
    bool created;
    int32_t stop_rc;       // WC_DECODER_OK unless a call failed
    const char *stop_call; // "feed" or "flush"
    size_t calls;
    size_t need_more; // NEED_MORE_SLOTS responses
    unsigned violations;
    char violation[512]; // the first one
    unsigned warnings;
    char warning[512]; // the first one
} wcxh_run;

void wcxh_run_decode(const wcxh_input *in, const wcxh_run_opts *o, wcxh_run *r);

// Two instances fed alternately, sample by sample (no shared state allowed).
void wcxh_run_pair(const wcxh_input *in, const wcxh_run_opts *o, wcxh_run *a, wcxh_run *b);

void wcxh_run_free(wcxh_run *r);
void wcxh_txlist_free(wcxh_txlist *l);

// ── output ──────────────────────────────────────────────────────────────────

// One transaction as its canonical line (malloc'd).
char *wcxh_tx_line(const wcxh_tx *tx);

// The whole list: "[\n" + one line per transaction joined by ",\n" + "\n]\n".
char *wcxh_format(const wcxh_txlist *l);

// Index of the first difference, or SIZE_MAX when equal.
size_t wcxh_first_difference(const wcxh_txlist *a, const wcxh_txlist *b);

// Canonical order for an order-insensitive comparison (--sort): by
// startTime, then endTime, label, isError, fields. Two lists with the same
// transactions in a different order sort to identical lists.
void wcxh_txlist_sort(wcxh_txlist *l);

// Parses an expected file into canonical transactions.
WCX_NODISCARD bool wcxh_parse_expected(const char *text, size_t len, wcxh_txlist *out,
                                       wcx_error *err);

// Writes the input as a seed for the shared fuzz driver.
WCX_NODISCARD bool wcxh_write_fuzz_seed(const wcxh_input *in, const char *path, wcx_error *err);

// ── lifecycle ───────────────────────────────────────────────────────────────

// Runs the ABI contract checks; prints one line per check to `log`.
// Returns the number of failed checks.
unsigned wcxh_lifecycle(const wcxh_input *in, FILE *log);

#endif // WCXH_HOST_H
