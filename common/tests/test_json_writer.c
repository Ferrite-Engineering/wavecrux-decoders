// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include <string.h>

#include "wcx/arena.h"
#include "wcx/json_parse.h"
#include "wcx/json_writer.h"
#include "wcx/str.h"
#include "wcx/test.h"

WCX_TEST(builds_typed_fields_byte_exact) {
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 256, 4096));
    wcx_json j;
    wcx_json_begin(&j, &a);
    wcx_json_add_str(&j, "kind", "MWr32");
    wcx_json_add_u64(&j, "len", 4);
    wcx_json_add_i64(&j, "delta", -12);
    wcx_json_add_bool(&j, "ok", true);
    wcx_json_add_bool(&j, "bad", false);
    wcx_json_add_hex(&j, "addr", 0x1000, 8);
    const char *out = wcx_json_end(&j);
    WCX_CHECK_STR_EQ(out, "{\"kind\":\"MWr32\",\"len\":4,\"delta\":-12,\"ok\":true,\"bad\":false,"
                          "\"addr\":\"0x00001000\"}");
    WCX_CHECK(wcx_json_end(&j) == NULL); // already ended
    wcx_arena_free(&a);
}

WCX_TEST(empty_object) {
    char buf[8] = {0};
    wcx_json j;
    wcx_json_begin_fixed(&j, buf, sizeof buf);
    WCX_CHECK_STR_EQ(wcx_json_end(&j), "{}");
}

WCX_TEST(escapes_everything_that_needs_it) {
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 256, 4096));
    wcx_json j;
    wcx_json_begin(&j, &a);
    // quote, backslash, every kind of control char, DEL, valid UTF-8, and a
    // lone 0xFF byte.
    wcx_json_add_str(&j, "k\"\\", "a\"b\\c\n\t\x01\x1F\x7F\xC3\xA9\xFF.");
    const char *out = wcx_json_end(&j);
    WCX_CHECK_STR_EQ(out, "{\"k\\\"\\\\\":\"a\\\"b\\\\c\\u000A\\u0009\\u0001\\u001F\x7F\xC3\xA9"
                          "\xEF\xBF\xBD.\"}");
    // The output is always valid JSON and valid UTF-8.
    WCX_REQUIRE(out != NULL);
    size_t count = 0;
    WCX_CHECK(wcx_json_tokenize(out, strlen(out), NULL, 100, &count, NULL));
    WCX_CHECK(wcx_utf8_valid(out, strlen(out)));
    wcx_arena_free(&a);
}

WCX_TEST(strn_with_embedded_nul) {
    char buf[64] = {0};
    wcx_json j;
    wcx_json_begin_fixed(&j, buf, sizeof buf);
    wcx_json_add_strn(&j, "b", "x\0y", 3);
    WCX_CHECK_STR_EQ(wcx_json_end(&j), "{\"b\":\"x\\u0000y\"}");
}

WCX_TEST(grows_inside_the_arena) {
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 64, 1u << 20));
    wcx_json j;
    wcx_json_begin(&j, &a);
    char key[8] = {0};
    for (int i = 0; i < 200; i++) {
        WCX_IGNORE(wcx_str_format(key, sizeof key, "k%d", i));
        wcx_json_add_u64(&j, key, (uint64_t)i * 1000u);
    }
    const char *out = wcx_json_end(&j);
    WCX_REQUIRE(out != NULL);
    wcx_jdoc d;
    WCX_REQUIRE(wcx_jdoc_parse(&d, out, strlen(out), 1000, NULL));
    WCX_CHECK_EQ_U64(d.toks[0].size, 200);
    uint64_t v = 0;
    WCX_CHECK(wcx_jdoc_uint64(&d, wcx_jdoc_member(&d, 0, "k199"), &v));
    WCX_CHECK_EQ_U64(v, 199000);
    wcx_jdoc_free(&d);
    wcx_arena_free(&a);
}

WCX_TEST(failure_is_sticky) {
    char buf[12] = {0};
    wcx_json j;
    wcx_json_begin_fixed(&j, buf, sizeof buf);
    wcx_json_add_str(&j, "key", "too long for twelve bytes");
    wcx_json_add_u64(&j, "x", 1);
    WCX_CHECK(wcx_json_end(&j) == NULL);

    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 64, 64));
    wcx_json k;
    wcx_json_begin(&k, &a);
    char big[100];
    memset(big, 'q', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    wcx_json_add_str(&k, "v", big);
    WCX_CHECK(wcx_json_end(&k) == NULL);
    wcx_arena_free(&a);

    wcx_json n;
    wcx_json_begin(&n, NULL);
    WCX_CHECK(wcx_json_end(&n) == NULL);
    wcx_json_begin_fixed(&n, NULL, 0);
    WCX_CHECK(wcx_json_end(&n) == NULL);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, builds_typed_fields_byte_exact);
    WCX_RUN(&t, empty_object);
    WCX_RUN(&t, escapes_everything_that_needs_it);
    WCX_RUN(&t, strn_with_embedded_nul);
    WCX_RUN(&t, grows_inside_the_arena);
    WCX_RUN(&t, failure_is_sticky);
    return wcx_test_finish(&t);
}
