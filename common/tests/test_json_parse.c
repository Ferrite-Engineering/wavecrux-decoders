// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include <stdlib.h>
#include <string.h>

#include "wcx/json_parse.h"
#include "wcx/test.h"

static bool parses(const char *s) {
    wcx_jdoc d;
    const bool ok = wcx_jdoc_parse(&d, s, strlen(s), 1000, NULL);
    wcx_jdoc_free(&d);
    return ok;
}

WCX_TEST(accepts_valid_documents) {
    WCX_CHECK(parses("{}"));
    WCX_CHECK(parses("[]"));
    WCX_CHECK(parses("0"));
    WCX_CHECK(parses("-0.5e+10"));
    WCX_CHECK(parses("\"\""));
    WCX_CHECK(parses(" \t\r\n{\"a\" : [1, true, false, null, {\"b\":\"c\"}]} \n"));
    WCX_CHECK(parses("\"\\u00e9\\ud83d\\ude00\\n\\\"\\\\\\/\""));
    WCX_CHECK(parses("\"\xC3\xA9\""));
}

WCX_TEST(rejects_hostile_documents) {
    static const char *const bad[] = {
        "",
        " ",
        "{",
        "}",
        "[1,]",
        "{\"a\":1,}",
        "{\"a\" 1}",
        "{1:2}",
        "[1 2]",
        "01",
        "-",
        "1.",
        ".5",
        "1e",
        "+1",
        "tru",
        "nul",
        "truex",
        "\"abc",
        "\"\\x\"",
        "\"\\u12\"",
        "\"\\u12G4\"",
        "[1]]",
        "{}{}",
        "\"a\nb\"",
        "\"\x01\"",
        "\"\xFF\"",
        "\"\xC0\xAF\"",
        "NaN",
        "[-]",
        "{\"a\":}",
        "[,]",
        "\xEF\xBB\xBF{}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        wcx_error err = {{0}};
        wcx_jdoc d;
        const bool ok = wcx_jdoc_parse(&d, bad[i], strlen(bad[i]), 1000, &err);
        if (ok) {
            WCX_FAIL("accepted invalid JSON #%lu: %s", (unsigned long)i, bad[i]);
        }
        WCX_CHECK(strstr(err.msg, "invalid JSON at byte") != NULL);
        wcx_jdoc_free(&d);
    }
}

WCX_TEST(depth_and_token_limits) {
    char deep[200];
    memset(deep, '[', 65);
    memset(deep + 65, ']', 65);
    deep[130] = '\0';
    WCX_CHECK(!parses(deep)); // 65 levels
    memset(deep, '[', 64);
    memset(deep + 64, ']', 64);
    deep[128] = '\0';
    WCX_CHECK(parses(deep)); // 64 levels is the limit

    wcx_jdoc d;
    WCX_CHECK(!wcx_jdoc_parse(&d, "[1,2,3]", 7, 3, NULL)); // needs 4 tokens
    WCX_CHECK(wcx_jdoc_parse(&d, "[1,2,3]", 7, 4, NULL));
    WCX_CHECK_EQ_U64(d.count, 4);
    wcx_jdoc_free(&d);
}

WCX_TEST(very_deep_input_does_not_recurse) {
    // A million brackets: rejected at depth 65 without stack growth.
    const size_t n = 1000000;
    char *s = malloc(n + 1);
    WCX_REQUIRE(s != NULL);
    memset(s, '[', n);
    s[n] = '\0';
    WCX_CHECK(!parses(s));
    free(s);
}

WCX_TEST(token_structure) {
    const char *s = "{\"a\":[1,{\"b\":2}],\"c\":\"x\",\"a\":3}";
    wcx_jdoc d;
    WCX_REQUIRE(wcx_jdoc_parse(&d, s, strlen(s), 100, NULL));
    WCX_CHECK_EQ_U64(d.toks[0].type, WCX_JT_OBJECT);
    WCX_CHECK_EQ_U64(d.toks[0].size, 3); // three members
    WCX_CHECK_EQ_U64(d.toks[0].next, d.count);
    const size_t arr = 2; // "a" key is 1, its value 2
    WCX_CHECK_EQ_U64(d.toks[arr].type, WCX_JT_ARRAY);
    WCX_CHECK_EQ_U64(d.toks[arr].size, 2);
    // Duplicate key: the last value wins.
    const size_t a = wcx_jdoc_member(&d, 0, "a");
    WCX_CHECK_EQ_U64(wcx_jdoc_type(&d, a), WCX_JT_NUMBER);
    int64_t v = 0;
    WCX_CHECK(wcx_jdoc_int64(&d, a, &v));
    WCX_CHECK_EQ_I64(v, 3);
    WCX_CHECK_EQ_U64(wcx_jdoc_member(&d, 0, "zz"), WCX_JSON_NONE);
    WCX_CHECK_EQ_U64(wcx_jdoc_member(&d, arr, "a"), WCX_JSON_NONE); // not an object
    WCX_CHECK_EQ_U64(wcx_jdoc_type(&d, 999), WCX_JT_NONE);
    wcx_jdoc_free(&d);
}

WCX_TEST(string_decoding) {
    const char *s = "[\"a\\u00e9\\ud83d\\ude00\\t\", \"\\ud800x\", \"\\u0000\", \"k\\\"ey\"]";
    wcx_jdoc d;
    WCX_REQUIRE(wcx_jdoc_parse(&d, s, strlen(s), 100, NULL));
    char buf[32] = {0};
    size_t len = 0;
    WCX_REQUIRE(wcx_jdoc_string(&d, 1, buf, sizeof buf, &len));
    WCX_CHECK_STR_EQ(buf, "a\xC3\xA9\xF0\x9F\x98\x80\t");
    WCX_CHECK_EQ_U64(len, 8);
    WCX_CHECK_EQ_U64(wcx_jdoc_string_len(&d, 1), 8);
    WCX_REQUIRE(wcx_jdoc_string(&d, 2, buf, sizeof buf, &len));
    WCX_CHECK_STR_EQ(buf, "\xEF\xBF\xBDx"); // lone surrogate -> U+FFFD
    WCX_REQUIRE(wcx_jdoc_string(&d, 3, buf, sizeof buf, &len));
    WCX_CHECK_EQ_U64(len, 1); // embedded NUL is visible through len
    WCX_CHECK(wcx_jdoc_string_equals(&d, 4, "k\"ey"));
    WCX_CHECK(!wcx_jdoc_string_equals(&d, 4, "k\"e"));
    WCX_CHECK(!wcx_jdoc_string_equals(&d, 4, "k\"eyy"));
    WCX_CHECK(!wcx_jdoc_string(&d, 1, buf, 8, &len));          // does not fit
    WCX_CHECK(!wcx_jdoc_string(&d, 0, buf, sizeof buf, &len)); // not a string
    WCX_CHECK_EQ_U64(wcx_jdoc_string_len(&d, 0), SIZE_MAX);
    wcx_jdoc_free(&d);
}

WCX_TEST(integers) {
    const char *s = "[9223372036854775807,-9223372036854775808,9223372036854775808,"
                    "18446744073709551615,18446744073709551616,1.0,1e3,-0,-1]";
    wcx_jdoc d;
    WCX_REQUIRE(wcx_jdoc_parse(&d, s, strlen(s), 100, NULL));
    int64_t i = 0;
    uint64_t u = 0;
    WCX_CHECK(wcx_jdoc_int64(&d, 1, &i));
    WCX_CHECK_EQ_I64(i, INT64_MAX);
    WCX_CHECK(wcx_jdoc_int64(&d, 2, &i));
    WCX_CHECK(i == INT64_MIN);
    WCX_CHECK(!wcx_jdoc_int64(&d, 3, &i));
    WCX_CHECK(wcx_jdoc_uint64(&d, 3, &u));
    WCX_CHECK_EQ_U64(u, 9223372036854775808u);
    WCX_CHECK(wcx_jdoc_uint64(&d, 4, &u));
    WCX_CHECK_EQ_U64(u, UINT64_MAX);
    WCX_CHECK(!wcx_jdoc_uint64(&d, 5, &u));
    WCX_CHECK(!wcx_jdoc_int64(&d, 6, &i)); // 1.0 is not an integer literal
    WCX_CHECK(!wcx_jdoc_is_integer(&d, 7));
    WCX_CHECK(wcx_jdoc_uint64(&d, 8, &u)); // -0
    WCX_CHECK_EQ_U64(u, 0);
    WCX_CHECK(!wcx_jdoc_uint64(&d, 9, &u));
    WCX_CHECK(!wcx_jdoc_int64(&d, 0, &i)); // the array
    size_t len = 0;
    const char *raw = wcx_jdoc_raw(&d, 6, &len);
    WCX_CHECK(raw != NULL && len == 3 && memcmp(raw, "1.0", 3) == 0);
    WCX_CHECK(wcx_jdoc_raw(&d, 99, &len) == NULL);
    wcx_jdoc_free(&d);
}

WCX_TEST(tokenize_null_and_huge_inputs) {
    size_t count = 7;
    WCX_CHECK(!wcx_json_tokenize(NULL, 0, NULL, 10, &count, NULL));
    WCX_CHECK_EQ_U64(count, 0);
    WCX_CHECK(!wcx_jdoc_parse(NULL, "{}", 2, 10, NULL));
    // Every possible single byte: never crashes, accepts exactly the digits.
    for (unsigned b = 0; b < 256; b++) {
        const char c = (char)b;
        wcx_jdoc d;
        const bool ok = wcx_jdoc_parse(&d, &c, 1, 10, NULL);
        WCX_CHECK(ok == (b >= '0' && b <= '9'));
        wcx_jdoc_free(&d);
    }
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, accepts_valid_documents);
    WCX_RUN(&t, rejects_hostile_documents);
    WCX_RUN(&t, depth_and_token_limits);
    WCX_RUN(&t, very_deep_input_does_not_recurse);
    WCX_RUN(&t, token_structure);
    WCX_RUN(&t, string_decoding);
    WCX_RUN(&t, integers);
    WCX_RUN(&t, tokenize_null_and_huge_inputs);
    return wcx_test_finish(&t);
}
