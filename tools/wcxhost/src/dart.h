// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Pure functions that mirror WaveCrux's Dart loader byte for byte. Line
// numbers are in lib/services/decoders/ffi/ffi_decoder_loader_io.dart of the
// WaveCrux open core; tools/wcxhost/tests/test_dart_parity.c pins each one.
// When the loader changes, these change in the same week (testing standard
// §5).

#ifndef WCXH_DART_H
#define WCXH_DART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sb.h"
#include "wcx/json_parse.h"

// _fsPerTick (lines 1148-1162): femtoseconds per tick for a timescale of
// `factor` x 10^`exponent` seconds; 1 ns (10^6 fs) when there is no
// timescale or the exponent is below -15.
uint64_t wcxh_fs_per_tick(bool has_timescale, uint64_t factor, int exponent);

// decode() line 1027: `ts < 0 ? 0 : ts * fsPerTick`, in Dart's wrapping
// 64-bit int arithmetic. Ticks above INT64_MAX are negative in Dart.
uint64_t wcxh_ticks_to_fs(uint64_t ticks, uint64_t fs_per_tick);

// _fsToTicksFloor / _fsToTicksCeil (lines 1166-1180): unsigned division,
// rounded down for start times and up for end times.
uint64_t wcxh_fs_to_ticks_floor(uint64_t fs, uint64_t fs_per_tick);
uint64_t wcxh_fs_to_ticks_ceil(uint64_t fs, uint64_t fs_per_tick);

// decode() lines 974-975: the scratch buffer is (2*bits+7)>>3 bytes, at
// least 32.
size_t wcxh_scratch_bytes(uint32_t bits_per_sample);

// _packBindingValue (lines 1192-1241): packs an MSB-first value string
// (NULL = no value: zeros) into `buffer` at signal-bit offset `bit_offset`,
// `bit_width` bits, two buffer bits per signal bit. Only '1' sets the level
// bit; 'x', 'X', 'z', 'Z' set the unknown bit; every other character packs
// 0. A string shorter than the width is zero-extended at the MSB end.
void wcxh_pack_value(uint8_t *buffer, size_t buffer_len, uint32_t bit_offset, uint32_t bit_width,
                     const char *value);

// _collectTimestamps (lines 1243-1266) after the merge: sorts and removes
// duplicates in place, returns the new count. An empty input becomes
// {start_time} (the caller provides room for one element).
size_t wcxh_sort_unique(uint64_t *stamps, size_t count, uint64_t start_time);

// dart:convert jsonEncode's string encoding: '"' and '\\' escaped,
// \b \t \n \f \r as short escapes, other control characters as \u00xx
// (lower-case hex), everything else verbatim.
void wcxh_dart_string(wcxh_sb *sb, const char *s, size_t n);

// Compact re-encoding of the JSON value at token `tok` the way jsonEncode
// re-emits a jsonDecode'd value: no whitespace, strings re-escaped, integer
// literals normalised (-0 -> 0). Returns false for a non-integer or
// out-of-range number (Dart would print its double form, which this does not
// reproduce; the raw text is written instead).
bool wcxh_dart_reencode(wcxh_sb *sb, const wcx_jdoc *doc, size_t tok);

// Flags from wcxh_dart_fields.
#define WCXH_FIELDS_NOT_JSON  0x1u // not JSON or not an object: shown as {}
#define WCXH_FIELDS_INEXACT   0x2u // a value Dart would render differently
#define WCXH_FIELDS_DUPLICATE 0x4u // a repeated key (the last value wins)

// _parseFieldsJson (lines 1299-1314) followed by jsonEncode of the
// resulting Map<String, String>: writes the canonical fields object
// {"key":"value",...} (keys in first-occurrence order, every value a string:
// strings as is, null as "", booleans and integers as their text). Returns
// WCXH_FIELDS_* flags describing anything that is not reproduced exactly.
unsigned wcxh_dart_fields(wcxh_sb *sb, const char *fields_json);

#endif // WCXH_DART_H
