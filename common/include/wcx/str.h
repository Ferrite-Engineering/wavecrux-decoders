// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Bounded strings, locale-free number formatting and UTF-8 checks.
//
// The replacements for the banned functions (C coding standard §6). Every
// writer takes the destination capacity, always NUL-terminates when the
// capacity is non-zero, never writes past it, and reports truncation by
// returning false. A truncated result is cut at a UTF-8 code point boundary,
// so it is still valid UTF-8 (the host rejects invalid UTF-8 outright).
//
// Number formatting never calls into the locale (§9): the output for a value
// is the same bytes on every platform and in every process.

#ifndef WCX_STR_H
#define WCX_STR_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wcx/export.h"

// Buffer sizes that always fit: 20 digits + NUL; sign + 19 digits + NUL;
// "0x" + 16 digits + NUL.
#define WCX_FMT_U64_CAP 21
#define WCX_FMT_I64_CAP 21
#define WCX_FMT_HEX_CAP 19

// strlen that reads at most `max` bytes: the index of the first NUL, or
// `max` when there is none in range.
size_t wcx_str_len(const char *s, size_t max);

// Copies `src` into `dst` (capacity `cap`). Returns false when it did not fit
// (dst then holds the longest UTF-8-clean prefix that did) or cap is 0.
WCX_NODISCARD bool wcx_str_copy(char *dst, size_t cap, const char *src);

// snprintf into `dst`. Returns false on truncation (dst then holds a
// UTF-8-clean prefix) or on an encoding error (dst is then ""). Integer and
// string conversions only: %f and friends depend on the locale (§9).
WCX_NODISCARD bool wcx_str_format(char *dst, size_t cap, const char *fmt, ...) WCX_PRINTF(3, 4);
WCX_NODISCARD bool wcx_str_vformat(char *dst, size_t cap, const char *fmt, va_list ap)
    WCX_PRINTF(3, 0);

// Decimal and hexadecimal formatting. Each writes a NUL-terminated string
// and returns its length (excluding the NUL).
size_t wcx_fmt_u64(char out[WCX_FMT_U64_CAP], uint64_t value);
size_t wcx_fmt_i64(char out[WCX_FMT_I64_CAP], int64_t value);

// "0x" followed by upper-case hex digits, zero-padded to at least
// `min_digits` (clamped to 1..16) digits, like printf("0x%0*llX"). A value
// that needs more digits prints them all.
size_t wcx_fmt_hex(char out[WCX_FMT_HEX_CAP], uint64_t value, unsigned min_digits);

// Length (1..4) of the well-formed UTF-8 sequence that starts at s[0], or 0
// if the bytes there are not one (overlong forms, surrogates, code points
// above U+10FFFF and truncated sequences are all rejected). Reads at most
// `n` bytes.
size_t wcx_utf8_seq_len(const unsigned char *s, size_t n);

// True when s[0..n) is entirely well-formed UTF-8.
bool wcx_utf8_valid(const char *s, size_t n);

// The largest k <= n such that s[0..k) does not end inside a multi-byte
// sequence. Used to cut a string without splitting a code point.
size_t wcx_utf8_boundary(const char *s, size_t n);

#endif // WCX_STR_H
