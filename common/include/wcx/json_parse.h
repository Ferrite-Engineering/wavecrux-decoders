// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Bounded, non-recursive JSON parser (RFC 8259, strict).
//
// Why not jsmn: see common/README.md ("Why a parser of our own").
//
// The parser turns the whole document into a flat array of tokens in
// document order (pre-order: a container comes before its children), in one
// pass with an explicit stack, and validates everything: number grammar,
// string escapes, control characters, UTF-8, nesting depth
// (WCX_JSON_MAX_DEPTH), the token budget, and trailing garbage. Any byte
// string is safe input; anything that is not a single valid JSON value is
// rejected with a message naming the byte offset.
//
// Walking the tokens: a scalar occupies one token. A container's children
// follow it directly; `next` is the index just past its whole subtree.
//
//   size_t child = arr + 1;                        // first element
//   for (uint32_t i = 0; i < doc.toks[arr].size; i++) {
//       ...use child...
//       child = doc.toks[child].next;              // skip its subtree
//   }
//
//   size_t key = obj + 1;                          // object: key, value, ...
//   for (uint32_t i = 0; i < doc.toks[obj].size; i++) {
//       size_t value = key + 1;
//       ...
//       key = doc.toks[value].next;
//   }
//
// Token indices are size_t; WCX_JSON_NONE means "absent".

#ifndef WCX_JSON_PARSE_H
#define WCX_JSON_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wcx/error.h"
#include "wcx/export.h"

#define WCX_JSON_MAX_DEPTH 64
#define WCX_JSON_NONE      SIZE_MAX

typedef enum wcx_jtype {
    WCX_JT_NONE = 0, // not a token (out-of-range index)
    WCX_JT_OBJECT,
    WCX_JT_ARRAY,
    WCX_JT_STRING,
    WCX_JT_NUMBER,
    WCX_JT_TRUE,
    WCX_JT_FALSE,
    WCX_JT_NULL
} wcx_jtype;

typedef struct wcx_jtok {
    wcx_jtype type;
    uint32_t start; // first byte; for a string, the byte after the opening quote
    uint32_t end;   // one past the last byte; for a string, the closing quote
    uint32_t size;  // children: array elements, or object members (pairs)
    uint32_t next;  // index of the first token after this subtree
} wcx_jtok;

typedef struct wcx_jdoc {
    const char *src; // borrowed: must outlive the document
    size_t len;
    wcx_jtok *toks; // owned (wcx_jdoc_parse) or caller-provided
    size_t count;
} wcx_jdoc;

// Low level: tokenises src[0..len) into `toks` (capacity `cap`), or only
// counts the tokens when `toks` is NULL (then `cap` is the budget). On
// success stores the token count in *count. len must be < UINT32_MAX.
WCX_NODISCARD bool wcx_json_tokenize(const char *src, size_t len, wcx_jtok *toks, size_t cap,
                                     size_t *count, wcx_error *err);

// Parses src[0..len) into `doc`, allocating exactly as many tokens as the
// document has (a counting pass first), at most `max_tokens`. `src` is
// borrowed. Free with wcx_jdoc_free (also after a failure).
WCX_NODISCARD bool wcx_jdoc_parse(wcx_jdoc *doc, const char *src, size_t len, size_t max_tokens,
                                  wcx_error *err);
void wcx_jdoc_free(wcx_jdoc *doc);

// The type of token `tok`, or WCX_JT_NONE when tok is out of range.
wcx_jtype wcx_jdoc_type(const wcx_jdoc *doc, size_t tok);

// The value of member `key` of object `obj`. When a key repeats, the last
// occurrence wins (as in WaveCrux's Dart jsonDecode). WCX_JSON_NONE when
// absent or when `obj` is not an object.
size_t wcx_jdoc_member(const wcx_jdoc *doc, size_t obj, const char *key);

// True when string token `tok` decodes to exactly the C string `s`.
bool wcx_jdoc_string_equals(const wcx_jdoc *doc, size_t tok, const char *s);

// Decodes string token `tok` (escapes resolved, \u surrogate pairs joined, a
// lone surrogate becomes U+FFFD) into buf as NUL-terminated UTF-8; the length
// excluding the NUL goes to *out_len (may be NULL). The decoded text can
// contain NUL bytes (from \u0000); *out_len is then the way to see them.
// Returns false when `tok` is not a string or the text does not fit.
WCX_NODISCARD bool wcx_jdoc_string(const wcx_jdoc *doc, size_t tok, char *buf, size_t cap,
                                   size_t *out_len);

// Decoded length of string token `tok` in bytes (excluding NUL), or
// SIZE_MAX when it is not a string.
size_t wcx_jdoc_string_len(const wcx_jdoc *doc, size_t tok);

// True when number token `tok` is an integer literal: no fraction and no
// exponent.
bool wcx_jdoc_is_integer(const wcx_jdoc *doc, size_t tok);

// Integer literal `tok` as int64. False when it is not an integer literal or
// is out of range.
WCX_NODISCARD bool wcx_jdoc_int64(const wcx_jdoc *doc, size_t tok, int64_t *out);

// Integer literal `tok` as uint64. False when negative, not an integer
// literal, or out of range.
WCX_NODISCARD bool wcx_jdoc_uint64(const wcx_jdoc *doc, size_t tok, uint64_t *out);

// The raw source text of token `tok` (for a string: the content between
// the quotes, still escaped).
const char *wcx_jdoc_raw(const wcx_jdoc *doc, size_t tok, size_t *len);

#endif // WCX_JSON_PARSE_H
