// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include <string.h>

#include "wcx/error.h"
#include "wcx/str.h"
#include "wcx/test.h"

WCX_TEST(len_is_bounded) {
    WCX_CHECK_EQ_U64(wcx_str_len("abc", 10), 3);
    WCX_CHECK_EQ_U64(wcx_str_len("abc", 2), 2);
    WCX_CHECK_EQ_U64(wcx_str_len(NULL, 5), 0);
}

WCX_TEST(copy_fits_and_truncates) {
    char buf[4] = {0};
    WCX_CHECK(wcx_str_copy(buf, sizeof buf, "abc"));
    WCX_CHECK_STR_EQ(buf, "abc");
    WCX_CHECK(!wcx_str_copy(buf, sizeof buf, "abcd"));
    WCX_CHECK_STR_EQ(buf, "abc");
    WCX_CHECK(!wcx_str_copy(buf, 0, "x"));
    WCX_CHECK(!wcx_str_copy(buf, sizeof buf, NULL));
    WCX_CHECK_STR_EQ(buf, "");
}

WCX_TEST(copy_truncates_at_utf8_boundary) {
    // "a€" is 61 E2 82 AC: a 3-byte buffer holds "a" + 2 bytes of the euro
    // sign, which must be dropped rather than split.
    char buf[3] = {0};
    WCX_CHECK(!wcx_str_copy(buf, sizeof buf, "a\xE2\x82\xAC"));
    WCX_CHECK_STR_EQ(buf, "a");
}

WCX_TEST(format_reports_truncation) {
    char buf[8] = {0};
    WCX_CHECK(wcx_str_format(buf, sizeof buf, "%d-%s", 42, "ab"));
    WCX_CHECK_STR_EQ(buf, "42-ab");
    WCX_CHECK(!wcx_str_format(buf, sizeof buf, "%s", "0123456789"));
    WCX_CHECK_STR_EQ(buf, "0123456");
}

WCX_TEST(fmt_u64_and_i64_extremes) {
    char buf[WCX_FMT_U64_CAP] = {0};
    WCX_CHECK_EQ_U64(wcx_fmt_u64(buf, 0), 1);
    WCX_CHECK_STR_EQ(buf, "0");
    WCX_CHECK_EQ_U64(wcx_fmt_u64(buf, UINT64_MAX), 20);
    WCX_CHECK_STR_EQ(buf, "18446744073709551615");
    char sbuf[WCX_FMT_I64_CAP] = {0};
    WCX_CHECK_EQ_U64(wcx_fmt_i64(sbuf, INT64_MIN), 20);
    WCX_CHECK_STR_EQ(sbuf, "-9223372036854775808");
    (void)wcx_fmt_i64(sbuf, -7);
    WCX_CHECK_STR_EQ(sbuf, "-7");
    (void)wcx_fmt_i64(sbuf, INT64_MAX);
    WCX_CHECK_STR_EQ(sbuf, "9223372036854775807");
}

WCX_TEST(fmt_hex_pads_like_printf) {
    char buf[WCX_FMT_HEX_CAP] = {0};
    // printf("0x%0*llX", 4, 0xABull) == "0x00AB"
    WCX_CHECK_EQ_U64(wcx_fmt_hex(buf, 0xAB, 4), 6);
    WCX_CHECK_STR_EQ(buf, "0x00AB");
    (void)wcx_fmt_hex(buf, 0, 0);
    WCX_CHECK_STR_EQ(buf, "0x0");
    (void)wcx_fmt_hex(buf, 0x12345, 2);
    WCX_CHECK_STR_EQ(buf, "0x12345");
    (void)wcx_fmt_hex(buf, UINT64_MAX, 99);
    WCX_CHECK_STR_EQ(buf, "0xFFFFFFFFFFFFFFFF");
    (void)wcx_fmt_hex(buf, 1, 16);
    WCX_CHECK_STR_EQ(buf, "0x0000000000000001");
}

WCX_TEST(utf8_accepts_well_formed) {
    WCX_CHECK(wcx_utf8_valid("", 0));
    WCX_CHECK(wcx_utf8_valid("plain", 5));
    WCX_CHECK(wcx_utf8_valid("\xC3\xA9", 2));         // U+00E9
    WCX_CHECK(wcx_utf8_valid("\xE2\x82\xAC", 3));     // U+20AC
    WCX_CHECK(wcx_utf8_valid("\xF0\x9F\x98\x80", 4)); // U+1F600
    WCX_CHECK(wcx_utf8_valid("\xF4\x8F\xBF\xBF", 4)); // U+10FFFF
    WCX_CHECK(wcx_utf8_valid("\xED\x9F\xBF", 3));     // U+D7FF
}

WCX_TEST(utf8_rejects_ill_formed) {
    WCX_CHECK(!wcx_utf8_valid("\x80", 1));             // lone continuation
    WCX_CHECK(!wcx_utf8_valid("\xC0\xAF", 2));         // overlong '/'
    WCX_CHECK(!wcx_utf8_valid("\xE0\x80\xAF", 3));     // overlong 3-byte
    WCX_CHECK(!wcx_utf8_valid("\xED\xA0\x80", 3));     // surrogate U+D800
    WCX_CHECK(!wcx_utf8_valid("\xF4\x90\x80\x80", 4)); // above U+10FFFF
    WCX_CHECK(!wcx_utf8_valid("\xF5\x80\x80\x80", 4));
    WCX_CHECK(!wcx_utf8_valid("\xE2\x82", 2)); // truncated
    WCX_CHECK(!wcx_utf8_valid("\xFF", 1));
    WCX_CHECK(!wcx_utf8_valid(NULL, 1));
}

WCX_TEST(utf8_boundary_never_splits) {
    const char *s = "ab\xE2\x82\xAC";
    WCX_CHECK_EQ_U64(wcx_utf8_boundary(s, 5), 5);
    WCX_CHECK_EQ_U64(wcx_utf8_boundary(s, 4), 2);
    WCX_CHECK_EQ_U64(wcx_utf8_boundary(s, 3), 2);
    WCX_CHECK_EQ_U64(wcx_utf8_boundary(s, 2), 2);
    WCX_CHECK_EQ_U64(wcx_utf8_boundary(s, 0), 0);
}

WCX_TEST(error_set_and_clear) {
    wcx_error err = {{0}};
    wcx_error_set(&err, "parameter \"%s\" must be %d", "x", 3);
    WCX_CHECK_STR_EQ(err.msg, "parameter \"x\" must be 3");
    wcx_error_clear(&err);
    WCX_CHECK_STR_EQ(err.msg, "");
    wcx_error_set(NULL, "ignored");
    char longer[600];
    memset(longer, 'a', sizeof longer - 1);
    longer[sizeof longer - 1] = '\0';
    wcx_error_set(&err, "%s", longer);
    WCX_CHECK_EQ_U64(strlen(err.msg), WCX_ERROR_MAX - 1);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, len_is_bounded);
    WCX_RUN(&t, copy_fits_and_truncates);
    WCX_RUN(&t, copy_truncates_at_utf8_boundary);
    WCX_RUN(&t, format_reports_truncation);
    WCX_RUN(&t, fmt_u64_and_i64_extremes);
    WCX_RUN(&t, fmt_hex_pads_like_printf);
    WCX_RUN(&t, utf8_accepts_well_formed);
    WCX_RUN(&t, utf8_rejects_ill_formed);
    WCX_RUN(&t, utf8_boundary_never_splits);
    WCX_RUN(&t, error_set_and_clear);
    return wcx_test_finish(&t);
}
