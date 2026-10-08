// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// A decoder's manifest_json, parsed with WaveCrux's rules.
//
// Used by the decoder base (to check the C signal table against the
// manifest the host will read), by tools/wcxhost (to load a plugin the way
// WaveCrux does) and by the fuzz driver. The rules mirror the loader's
// _adaptDefinition, _decodeBindings, _decodeParameters, _decodeEnumLabels
// and _parameterTypeOf (lib/services/decoders/ffi/ffi_decoder_loader_io.dart
// in the open core):
//
//   * top level: a JSON object.
//   * "signals", "optional_signals": absent/null = none, else an array of
//     objects with a non-empty string "name", an optional integer
//     "bit_width" (absent/null = 1) and an optional string "description".
//   * "parameters": absent/null = none, else an array of objects with a
//     non-empty string "name", a string "kind" (bool|boolean, int|integer,
//     enum|enumeration, string), an optional "default" (any JSON value;
//     absent/null = false / 0 / ""), optional string "description" and
//     "display_name", optional "enum_values" (array of strings) and optional
//     "enum_labels" (object of strings, or an array of strings the same
//     length as enum_values).
//   * "description", "category", "required_tier": optional strings.
//
// Two checks are stricter than the loader, which accepts and then
// mis-packs these: a negative bit_width, and a bit_width above
// WCX_MAX_SIGNAL_BITS, are rejected.

#ifndef WCX_MANIFEST_H
#define WCX_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wcx/error.h"
#include "wcx/export.h"
#include "wcx/json_parse.h"
#include "wcx/sample.h"

#define WCX_MANIFEST_MAX_PARAMS 64u
#define WCX_MANIFEST_MAX_BYTES  ((size_t)256u * 1024u)
#define WCX_MANIFEST_MAX_TOKENS 16384u

typedef enum wcx_param_kind {
    WCX_PARAM_NONE = 0, // not a known kind
    WCX_PARAM_BOOL,
    WCX_PARAM_INT,
    WCX_PARAM_ENUM,
    WCX_PARAM_STRING
} wcx_param_kind;

typedef struct wcx_manifest_signal {
    size_t name;        // string token
    uint32_t bit_width; // 1 when the manifest omits it
    bool optional;      // declared in "optional_signals"
} wcx_manifest_signal;

typedef struct wcx_manifest_param {
    size_t name; // string token
    wcx_param_kind kind;
    size_t default_value; // token, or WCX_JSON_NONE when absent or null
    size_t enum_values;   // array token, or WCX_JSON_NONE
    size_t enum_labels;   // object/array token, or WCX_JSON_NONE
} wcx_manifest_param;

typedef struct wcx_manifest {
    char *text; // owned copy of the manifest
    wcx_jdoc doc;
    size_t signal_count; // required signals first, then optional ones
    wcx_manifest_signal signals[WCX_MAX_SIGNALS];
    size_t param_count;
    wcx_manifest_param params[WCX_MANIFEST_MAX_PARAMS];
} wcx_manifest;

// Parses and validates `json`. Free with wcx_manifest_free (also after a
// failure).
WCX_NODISCARD bool wcx_manifest_parse(wcx_manifest *m, const char *json, wcx_error *err);
void wcx_manifest_free(wcx_manifest *m);

// Decodes a string token of the manifest into buf. False if it does not fit.
WCX_NODISCARD bool wcx_manifest_text(const wcx_manifest *m, size_t tok, char *buf, size_t cap);

// Index of the signal named `name`, or WCX_JSON_NONE.
size_t wcx_manifest_find_signal(const wcx_manifest *m, const char *name);

// Index of the parameter named `name`, or WCX_JSON_NONE.
size_t wcx_manifest_find_param(const wcx_manifest *m, const char *name);

// Checks that a decoder's C signal table says exactly what its manifest
// says: same names, widths and optional flags, in the same order.
WCX_NODISCARD bool wcx_manifest_check_signals(const wcx_manifest *m, const wcx_signal_spec *specs,
                                              size_t n, wcx_error *err);

#endif // WCX_MANIFEST_H
