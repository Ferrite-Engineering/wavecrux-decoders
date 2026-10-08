// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Builds one flat JSON object (a transaction's fields_json) with every string
// escaped (C coding standard §3: never hand-build JSON with snprintf).
//
//   wcx_json j;
//   wcx_json_begin(&j, arena);
//   wcx_json_add_hex(&j, "addr", addr, 8);   // "addr":"0x00001000"
//   wcx_json_add_u64(&j, "len", len);        // "len":4
//   wcx_json_add_str(&j, "kind", "MWr32");
//   const char *fields = wcx_json_end(&j);   // NULL if anything failed
//
// Escaping: '"' and '\\' are backslash-escaped, every byte below 0x20 is
// written as \u00XX (upper-case hex), well-formed UTF-8 passes through
// unchanged, and each byte of an ill-formed sequence becomes U+FFFD, so the
// output is always valid UTF-8 and valid JSON. Numbers are formatted without
// the locale. The same calls produce the same bytes on every platform.
//
// Failure is sticky: once a call fails (the arena ceiling or a fixed buffer
// is exhausted), later calls do nothing and wcx_json_end returns NULL. Check
// once, at the end. The decoder base treats a NULL fields_json passed to
// wcx_emit as an error, so `wcx_emit(d, s, e, label, wcx_json_end(&j), 0)`
// is safe.
//
// Keys are written as given; the writer does not detect duplicates. WaveCrux
// shows fields as strings (it calls toString() on each value), so an integer
// added here appears as its decimal text and a bool as "true"/"false".

#ifndef WCX_JSON_WRITER_H
#define WCX_JSON_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wcx/arena.h"
#include "wcx/export.h"

typedef struct wcx_json {
    wcx_arena *arena; // NULL when writing into a caller's fixed buffer
    char *buf;
    size_t len; // bytes written, excluding the NUL
    size_t cap; // bytes available, including the NUL
    bool failed;
    bool need_comma;
    bool ended;
} wcx_json;

// Starts an object in `arena`; the text grows inside the arena as needed.
void wcx_json_begin(wcx_json *j, wcx_arena *arena);

// Starts an object in a caller-provided buffer of `cap` bytes (no
// allocation; used for error messages that must outlive the arena).
void wcx_json_begin_fixed(wcx_json *j, char *buf, size_t cap);

void wcx_json_add_str(wcx_json *j, const char *key, const char *value);
void wcx_json_add_strn(wcx_json *j, const char *key, const char *value, size_t n);
void wcx_json_add_i64(wcx_json *j, const char *key, int64_t value);
void wcx_json_add_u64(wcx_json *j, const char *key, uint64_t value);
void wcx_json_add_bool(wcx_json *j, const char *key, bool value);

// A string value "0x" + upper-case hex, zero-padded to `digits` (1..16).
void wcx_json_add_hex(wcx_json *j, const char *key, uint64_t value, unsigned digits);

// Closes the object and returns it (NUL-terminated, owned by the arena or
// the fixed buffer), or NULL when any step failed. Calls after this one
// fail.
WCX_NODISCARD const char *wcx_json_end(wcx_json *j);

// Writes `s[0..n)` as a quoted, escaped JSON string into the writer. Exposed
// for tools that build other JSON shapes with the same escaping.
void wcx_json_write_string(wcx_json *j, const char *s, size_t n);

// Appends raw bytes (already valid JSON) to the writer.
void wcx_json_write_raw(wcx_json *j, const char *s, size_t n);

#endif // WCX_JSON_WRITER_H
