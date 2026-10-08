// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Pins wcxhost to WaveCrux's loader. Every expected value is worked out by
// hand from the cited Dart lines of
// lib/services/decoders/ffi/ffi_decoder_loader_io.dart (WaveCrux open core);
// if the loader changes, these tests say exactly which behaviour moved.

#include <stdlib.h>
#include <string.h>

#include "dart.h"
#include "sb.h"
#include "wcx/json_parse.h"
#include "wcx/test.h"

// _fsPerTick, lines 1148-1162.
WCX_TEST(fs_per_tick) {
    WCX_CHECK_EQ_U64(wcxh_fs_per_tick(false, 0, 0), 1000000u);         // line 1149: no timescale
    WCX_CHECK_EQ_U64(wcxh_fs_per_tick(true, 1, -9), 1000000u);         // 1 ns: 10^(−9+15)
    WCX_CHECK_EQ_U64(wcxh_fs_per_tick(true, 1, -12), 1000u);           // 1 ps
    WCX_CHECK_EQ_U64(wcxh_fs_per_tick(true, 10, -12), 10000u);         // 10 ps: factor × 10^3
    WCX_CHECK_EQ_U64(wcxh_fs_per_tick(true, 100, -15), 100u);          // 100 fs
    WCX_CHECK_EQ_U64(wcxh_fs_per_tick(true, 1, -15), 1u);              // 1 fs
    WCX_CHECK_EQ_U64(wcxh_fs_per_tick(true, 1, 0), 1000000000000000u); // 1 s
    WCX_CHECK_EQ_U64(wcxh_fs_per_tick(true, 100, -3), 100000000000000u);
    WCX_CHECK_EQ_U64(wcxh_fs_per_tick(true, 1, -18), 1000000u); // line 1156: shift < 0
}

// decode() line 1027: `ts < 0 ? 0 : ts * fsPerTick` (Dart's int wraps).
WCX_TEST(ticks_to_fs) {
    WCX_CHECK_EQ_U64(wcxh_ticks_to_fs(0, 1000000u), 0);
    WCX_CHECK_EQ_U64(wcxh_ticks_to_fs(5, 10000u), 50000u);
    WCX_CHECK_EQ_U64(wcxh_ticks_to_fs((uint64_t)INT64_MAX + 1u, 1u), 0); // negative in Dart
    // 2^62 × 4 wraps to 0 in 64-bit arithmetic.
    WCX_CHECK_EQ_U64(wcxh_ticks_to_fs((uint64_t)1u << 62u, 4u), 0);
}

// _fsToTicksFloor / _fsToTicksCeil, lines 1166-1180.
WCX_TEST(fs_to_ticks) {
    WCX_CHECK_EQ_U64(wcxh_fs_to_ticks_floor(5000001u, 1000000u), 5);
    WCX_CHECK_EQ_U64(wcxh_fs_to_ticks_ceil(5000001u, 1000000u), 6);
    WCX_CHECK_EQ_U64(wcxh_fs_to_ticks_floor(5000000u, 1000000u), 5);
    WCX_CHECK_EQ_U64(wcxh_fs_to_ticks_ceil(5000000u, 1000000u), 5); // exact: no round-up
    WCX_CHECK_EQ_U64(wcxh_fs_to_ticks_ceil(0, 1000u), 0);
    // Above 2^63 the loader divides as unsigned (BigInt.toUnsigned(64)).
    WCX_CHECK_EQ_U64(wcxh_fs_to_ticks_floor(UINT64_MAX, 1000u), 18446744073709551u);
    WCX_CHECK_EQ_U64(wcxh_fs_to_ticks_ceil(UINT64_MAX, 1000u), 18446744073709552u);
}

// decode() lines 974-975: `encodedBytes < 32 ? 32 : encodedBytes`.
WCX_TEST(scratch_minimum) {
    WCX_CHECK_EQ_U64(wcxh_scratch_bytes(0), 32);
    WCX_CHECK_EQ_U64(wcxh_scratch_bytes(9), 32);   // (18+7)>>3 = 3
    WCX_CHECK_EQ_U64(wcxh_scratch_bytes(128), 32); // exactly 32
    WCX_CHECK_EQ_U64(wcxh_scratch_bytes(129), 33); // (258+7)>>3
}

// _packBindingValue, lines 1192-1241.
WCX_TEST(pack_msb_first_string) {
    uint8_t buf[32] = {0};
    // "01101" = 13, width 5, offset 0: signal bits 0,2,3 set -> buffer bits
    // 0, 4, 6 -> 0b0101_0001 = 0x51.
    wcxh_pack_value(buf, sizeof buf, 0, 5, "01101");
    WCX_CHECK_EQ_U64(buf[0], 0x51);
    WCX_CHECK_EQ_U64(buf[1], 0x00);
}

WCX_TEST(pack_zero_extends_short_strings_and_truncates_long_ones) {
    uint8_t buf[32] = {0};
    // Width 4, "1": only signal bit 0 -> buffer bit 0 (line 1209: idx < 0
    // reads as zero).
    wcxh_pack_value(buf, sizeof buf, 0, 4, "1");
    WCX_CHECK_EQ_U64(buf[0], 0x01);
    memset(buf, 0, sizeof buf);
    // Width 2, "1110": the two LSB characters "10" -> signal bit 1 ->
    // buffer bit 2.
    wcxh_pack_value(buf, sizeof buf, 0, 2, "1110");
    WCX_CHECK_EQ_U64(buf[0], 0x04);
}

WCX_TEST(pack_x_and_z_set_the_unknown_bit) {
    uint8_t buf[32] = {0};
    // "zx10" width 4 at offset 3: signal bits 3..6. bit3='0', bit4='1'
    // (buffer bit 8 -> byte1 bit0), bit5='x' (buffer bit 11 -> byte1 bit3),
    // bit6='z' (buffer bit 13 -> byte1 bit5).
    wcxh_pack_value(buf, sizeof buf, 3, 4, "zx10");
    WCX_CHECK_EQ_U64(buf[0], 0x00);
    WCX_CHECK_EQ_U64(buf[1], 0x29); // 0b0010_1001
    memset(buf, 0, sizeof buf);
    wcxh_pack_value(buf, sizeof buf, 0, 4, "XZhl"); // h/l/u/w/- pack as 0
    WCX_CHECK_EQ_U64(buf[0], 0xA0);                 // bits 3 and 2 unknown: 0b1010_0000
}

WCX_TEST(pack_null_and_short_buffer) {
    uint8_t buf[2] = {0xFF, 0xFF};
    memset(buf, 0, sizeof buf);
    wcxh_pack_value(buf, sizeof buf, 0, 8, NULL); // line 1207: null packs zeros
    WCX_CHECK_EQ_U64(buf[0], 0);
    // Line 1225: stops at the end of a short buffer rather than overrun.
    wcxh_pack_value(buf, sizeof buf, 0, 16, "1111111111111111");
    WCX_CHECK_EQ_U64(buf[0], 0x55);
    WCX_CHECK_EQ_U64(buf[1], 0x55);
    wcxh_pack_value(buf, sizeof buf, 0, 0, "1"); // line 1199: width 0 is a no-op
}

// _collectTimestamps, lines 1261-1265.
WCX_TEST(timestamp_union) {
    uint64_t stamps[6] = {30, 10, 20, 10, 0, 30};
    WCX_CHECK_EQ_U64(wcxh_sort_unique(stamps, 6, 0), 4);
    WCX_CHECK_EQ_U64(stamps[0], 0);
    WCX_CHECK_EQ_U64(stamps[3], 30);
    uint64_t empty[1] = {99};
    WCX_CHECK_EQ_U64(wcxh_sort_unique(empty, 0, 0), 1); // `stamps.add(startTime)`
    WCX_CHECK_EQ_U64(empty[0], 0);
}

static char *dart_string(const char *s, size_t n) {
    wcxh_sb sb = {0};
    wcxh_dart_string(&sb, s, n);
    return wcxh_sb_take(&sb);
}

// dart:convert's JSON encoder (_JsonStringifier.writeStringContent).
WCX_TEST(dart_string_escaping) {
    char *s = dart_string("a\"b\\c\b\f\n\r\t\x01\x1f\x7f\xC3\xA9", 15);
    WCX_CHECK_STR_EQ(s, "\"a\\\"b\\\\c\\b\\f\\n\\r\\t\\u0001\\u001f\x7f\xC3\xA9\"");
    free(s);
}

static char *fields(const char *json, unsigned *flags) {
    wcxh_sb sb = {0};
    *flags = wcxh_dart_fields(&sb, json);
    return wcxh_sb_take(&sb);
}

// _parseFieldsJson, lines 1299-1314: Map<String, String> via toString().
WCX_TEST(fields_become_strings) {
    unsigned f = 0;
    char *s = fields("{\"a\":1,\"b\":true,\"c\":null,\"d\":\"x\",\"e\":-0,\"a\":2}", &f);
    // Dart LinkedHashMap: "a" keeps its first position, takes the last value.
    WCX_CHECK_STR_EQ(s, "{\"a\":\"2\",\"b\":\"true\",\"c\":\"\",\"d\":\"x\",\"e\":\"0\"}");
    WCX_CHECK_EQ_U64(f, WCXH_FIELDS_DUPLICATE);
    free(s);
    s = fields("", &f); // line 1300
    WCX_CHECK_STR_EQ(s, "{}");
    WCX_CHECK_EQ_U64(f, 0);
    free(s);
    s = fields("[1]", &f); // line 1303: not a map
    WCX_CHECK_STR_EQ(s, "{}");
    WCX_CHECK_EQ_U64(f, WCXH_FIELDS_NOT_JSON);
    free(s);
    s = fields("{bad", &f); // lines 1311-1312: FormatException
    WCX_CHECK_STR_EQ(s, "{}");
    WCX_CHECK_EQ_U64(f, WCXH_FIELDS_NOT_JSON);
    free(s);
    s = fields("{\"n\":{\"k\":[1, 2]},\"f\":1.5}", &f); // not reproduced exactly
    WCX_CHECK_STR_EQ(s, "{\"n\":\"{\\\"k\\\":[1,2]}\",\"f\":\"1.5\"}");
    WCX_CHECK_EQ_U64(f, WCXH_FIELDS_INEXACT);
    free(s);
}

static char *reencode(const char *json, bool *exact) {
    wcx_jdoc d;
    wcxh_sb sb = {0};
    if (!wcx_jdoc_parse(&d, json, strlen(json), 1000, NULL)) {
        return NULL;
    }
    *exact = wcxh_dart_reencode(&sb, &d, 0);
    wcx_jdoc_free(&d);
    return wcxh_sb_take(&sb);
}

// jsonEncode(jsonDecode(x)) for the values a manifest default or a bindings
// parameter can hold.
WCX_TEST(reencode_compacts_like_dart) {
    bool exact = false;
    char *s =
        reencode(" { \"a\" : [ 1 , -0 , \"\\u00e9\\n\" , { } , [ ] ] , \"b\" : null } ", &exact);
    WCX_CHECK_STR_EQ(s, "{\"a\":[1,0,\"\xC3\xA9\\n\",{},[]],\"b\":null}");
    WCX_CHECK(exact);
    free(s);
    s = reencode("2.50", &exact);
    WCX_CHECK_STR_EQ(s, "2.50"); // Dart would print 2.5
    WCX_CHECK(!exact);
    free(s);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, fs_per_tick);
    WCX_RUN(&t, ticks_to_fs);
    WCX_RUN(&t, fs_to_ticks);
    WCX_RUN(&t, scratch_minimum);
    WCX_RUN(&t, pack_msb_first_string);
    WCX_RUN(&t, pack_zero_extends_short_strings_and_truncates_long_ones);
    WCX_RUN(&t, pack_x_and_z_set_the_unknown_bit);
    WCX_RUN(&t, pack_null_and_short_buffer);
    WCX_RUN(&t, timestamp_union);
    WCX_RUN(&t, dart_string_escaping);
    WCX_RUN(&t, fields_become_strings);
    WCX_RUN(&t, reencode_compacts_like_dart);
    return wcx_test_finish(&t);
}
