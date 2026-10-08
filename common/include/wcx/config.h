// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The host's per-instance configuration (the `config_json` argument of
// `create`), parsed and checked.
//
// Shape (wavecrux_decoder.h, WcDecoderCreateFn):
//
//   {"decoder_id": "<WcDecoderDef.id>",
//    "signal_bindings": {"<signal name>": "<waveform signal ref>", ...},
//    "parameters": {"<parameter name>": <value>, ...},
//    "options": {"<parameter name>": <value>, ...}}
//
// WaveCrux writes every parameter the manifest declares into "parameters"
// (seeded from the manifest default, then the user's value) and repeats them
// in "options". Binding values are opaque signal references (WaveCrux uses
// wellen's numeric signal ids); only whether a signal is bound matters to a
// decoder. Unknown top-level keys are ignored, for forward compatibility.
//
// Everything is treated as hostile (C coding standard §2): the text is
// bounded (WCX_CONFIG_MAX_BYTES), parsed by the bounded parser, the four
// known keys are type-checked, and every parameter read is type- and
// range-checked. Each failure fills a wcx_error with a sentence a user can
// act on.
//
// Parameter values. A parameter is read from "parameters", then from
// "options" when "parameters" lacks it. Absent or JSON null means the
// caller's default. Accepted forms follow WaveCrux's own decoders
// (decoder_value_helpers.dart), which may store a typed-in value as text:
//   int:    a JSON integer, or a string holding one ("12", "-3", "+4",
//           "0x1F"). Fractions, exponents and out-of-range values are errors.
//   bool:   true/false, or "true"/"false"/"1"/"0" in any case.
//   string: a JSON string without NUL characters.
//   enum:   a JSON string equal to one of the allowed values.

#ifndef WCX_CONFIG_H
#define WCX_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wcx/error.h"
#include "wcx/export.h"
#include "wcx/json_parse.h"

// Ceilings derived from what a host can reasonably send: a few dozen
// bindings to hierarchical paths and a few dozen parameters fit in a few
// KiB; 1 MiB and 64 Ki tokens leave two orders of magnitude of headroom.
#define WCX_CONFIG_MAX_BYTES  ((size_t)1024u * 1024u)
#define WCX_CONFIG_MAX_TOKENS 65536u

typedef struct wcx_config {
    char *text; // owned copy of config_json
    wcx_jdoc doc;
    size_t decoder_id; // token index or WCX_JSON_NONE, for each of the four keys
    size_t bindings;
    size_t parameters;
    size_t options;
} wcx_config;

// Parses `config_json` (NUL-terminated; NULL is an error). On failure the
// config is left empty and safe to free. Free with wcx_config_free.
WCX_NODISCARD bool wcx_config_parse(wcx_config *cfg, const char *config_json, wcx_error *err);
void wcx_config_free(wcx_config *cfg);

// True when "decoder_id" is present.
bool wcx_config_has_decoder_id(const wcx_config *cfg);

// True when "decoder_id" is present and equals `id`.
bool wcx_config_decoder_id_is(const wcx_config *cfg, const char *id);

// True when `signal` has a binding with a non-empty value.
bool wcx_config_is_bound(const wcx_config *cfg, const char *signal);

// Integer parameter within [min, max]; `def` when absent.
WCX_NODISCARD bool wcx_config_int(const wcx_config *cfg, const char *name, int64_t def, int64_t min,
                                  int64_t max, int64_t *out, wcx_error *err);

// Boolean parameter; `def` when absent.
WCX_NODISCARD bool wcx_config_bool(const wcx_config *cfg, const char *name, bool def, bool *out,
                                   wcx_error *err);

// String parameter copied into buf (capacity cap); `def` when absent. A
// value that does not fit is an error, not a silent truncation.
WCX_NODISCARD bool wcx_config_string(const wcx_config *cfg, const char *name, const char *def,
                                     char *buf, size_t cap, wcx_error *err);

// Enumeration parameter: *out_index is the index in `values` of the
// configured value, or `def_index` when absent.
WCX_NODISCARD bool wcx_config_enum(const wcx_config *cfg, const char *name,
                                   const char *const *values, size_t count, size_t def_index,
                                   size_t *out_index, wcx_error *err);

#endif // WCX_CONFIG_H
