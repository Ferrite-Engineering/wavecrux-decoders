// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include <stdlib.h>
#include <string.h>

#include "wcx/config.h"
#include "wcx/str.h"
#include "wcx/test.h"

// The shape WaveCrux's _serializeConfig writes (loader lines 1134-1141).
static const char k_good[] =
    "{\"decoder_id\":\"ferrite.x\",\"signal_bindings\":{\"clk\":\"3\",\"data\":\"7\",\"en\":\"\"},"
    "\"parameters\":{\"lanes\":4,\"text\":\"12\",\"hex\":\"0x1F\",\"flag\":true,\"sflag\":\"TRUE\","
    "\"mode\":\"fast\",\"name\":\"x\\u00e9\",\"nul\":\"a\\u0000b\",\"frac\":1.5,\"big\":"
    "99999999999999999999,\"nothing\":null},"
    "\"options\":{\"lanes\":4,\"only_opt\":9}}";

WCX_TEST(parses_the_host_shape) {
    wcx_config c;
    wcx_error err = {{0}};
    WCX_REQUIRE(wcx_config_parse(&c, k_good, &err));
    WCX_CHECK(wcx_config_has_decoder_id(&c));
    WCX_CHECK(wcx_config_decoder_id_is(&c, "ferrite.x"));
    WCX_CHECK(!wcx_config_decoder_id_is(&c, "ferrite.y"));
    WCX_CHECK(wcx_config_is_bound(&c, "clk"));
    WCX_CHECK(wcx_config_is_bound(&c, "data"));
    WCX_CHECK(!wcx_config_is_bound(&c, "en")); // empty binding
    WCX_CHECK(!wcx_config_is_bound(&c, "missing"));
    wcx_config_free(&c);
    wcx_config_free(&c); // idempotent
}

WCX_TEST(int_parameters) {
    wcx_config c;
    wcx_error err = {{0}};
    WCX_REQUIRE(wcx_config_parse(&c, k_good, &err));
    int64_t v = 0;
    WCX_CHECK(wcx_config_int(&c, "lanes", 1, 1, 32, &v, &err));
    WCX_CHECK_EQ_I64(v, 4);
    WCX_CHECK(wcx_config_int(&c, "text", 0, 0, 100, &v, &err));
    WCX_CHECK_EQ_I64(v, 12);
    WCX_CHECK(wcx_config_int(&c, "hex", 0, 0, 100, &v, &err));
    WCX_CHECK_EQ_I64(v, 31);
    WCX_CHECK(wcx_config_int(&c, "absent", 5, 0, 10, &v, &err));
    WCX_CHECK_EQ_I64(v, 5);
    WCX_CHECK(wcx_config_int(&c, "nothing", 6, 0, 10, &v, &err)); // null -> default
    WCX_CHECK_EQ_I64(v, 6);
    WCX_CHECK(wcx_config_int(&c, "only_opt", 0, 0, 10, &v, &err)); // from "options"
    WCX_CHECK_EQ_I64(v, 9);

    WCX_CHECK(!wcx_config_int(&c, "lanes", 1, 1, 2, &v, &err));
    WCX_CHECK_STR_EQ(err.msg, "parameter \"lanes\" must be an integer from 1 to 2, got 4");
    WCX_CHECK(!wcx_config_int(&c, "frac", 0, 0, 10, &v, &err));
    WCX_CHECK_STR_EQ(err.msg, "parameter \"frac\" must be an integer from 0 to 10, got 1.5");
    WCX_CHECK(!wcx_config_int(&c, "big", 0, INT64_MIN, INT64_MAX, &v, &err));
    WCX_CHECK(!wcx_config_int(&c, "mode", 0, 0, 10, &v, &err));
    WCX_CHECK_STR_EQ(err.msg, "parameter \"mode\" must be an integer from 0 to 10, got \"fast\"");
    WCX_CHECK(!wcx_config_int(&c, "flag", 0, 0, 10, &v, &err));
    WCX_CHECK(!wcx_config_int(&c, "lanes", 50, 0, 10, &v, &err)); // bad default
    wcx_config_free(&c);
}

WCX_TEST(int_text_forms) {
    static const struct {
        const char *json;
        bool ok;
        int64_t value;
    } cases[] = {
        {"\"+4\"", true, 4},
        {"\"-4\"", true, -4},
        {"\"0X10\"", true, 16},
        {"\"\"", false, 0},
        {"\"0x\"", false, 0},
        {"\" 4\"", false, 0},
        {"\"4a\"", false, 0},
        {"\"9223372036854775807\"", true, INT64_MAX},
        {"\"-9223372036854775808\"", true, INT64_MIN},
        {"\"9223372036854775808\"", false, 0},
        {"\"-9223372036854775809\"", false, 0},
        {"\"99999999999999999999999999\"", false, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char json[128] = {0};
        WCX_IGNORE(wcx_str_format(json, sizeof json, "{\"parameters\":{\"p\":%s}}", cases[i].json));
        wcx_config c;
        WCX_REQUIRE(wcx_config_parse(&c, json, NULL));
        int64_t v = 0;
        const bool ok = wcx_config_int(&c, "p", 0, INT64_MIN, INT64_MAX, &v, NULL);
        if (ok != cases[i].ok || (ok && v != cases[i].value)) {
            WCX_FAIL("int text case %s: ok=%d v=%lld", cases[i].json, ok, (long long)v);
        }
        wcx_config_free(&c);
    }
}

WCX_TEST(bool_string_enum_parameters) {
    wcx_config c;
    wcx_error err = {{0}};
    WCX_REQUIRE(wcx_config_parse(&c, k_good, &err));
    bool b = false;
    WCX_CHECK(wcx_config_bool(&c, "flag", false, &b, &err) && b);
    WCX_CHECK(wcx_config_bool(&c, "sflag", false, &b, &err) && b);
    WCX_CHECK(wcx_config_bool(&c, "absent", true, &b, &err) && b);
    WCX_CHECK(!wcx_config_bool(&c, "lanes", false, &b, &err));
    WCX_CHECK_STR_EQ(err.msg, "parameter \"lanes\" must be true or false, got 4");

    char s[8] = {0};
    WCX_CHECK(wcx_config_string(&c, "name", "", s, sizeof s, &err));
    WCX_CHECK_STR_EQ(s, "x\xC3\xA9");
    WCX_CHECK(wcx_config_string(&c, "absent", "dflt", s, sizeof s, &err));
    WCX_CHECK_STR_EQ(s, "dflt");
    WCX_CHECK(!wcx_config_string(&c, "nul", "", s, sizeof s, &err));
    WCX_CHECK_STR_EQ(err.msg, "parameter \"nul\" must not contain NUL characters");
    WCX_CHECK(!wcx_config_string(&c, "lanes", "", s, sizeof s, &err));
    WCX_CHECK(!wcx_config_string(&c, "mode", "", s, 3, &err));
    WCX_CHECK_STR_EQ(err.msg, "parameter \"mode\" must be at most 2 bytes long");

    static const char *const modes[] = {"slow", "fast"};
    size_t idx = 9;
    WCX_CHECK(wcx_config_enum(&c, "mode", modes, 2, 0, &idx, &err));
    WCX_CHECK_EQ_U64(idx, 1);
    WCX_CHECK(wcx_config_enum(&c, "absent", modes, 2, 0, &idx, &err));
    WCX_CHECK_EQ_U64(idx, 0);
    WCX_CHECK(!wcx_config_enum(&c, "name", modes, 2, 0, &idx, &err));
    WCX_CHECK_STR_EQ(err.msg, "parameter \"name\" must be one of \"slow\", \"fast\", got "
                              "\"x\\u00e9\"");
    WCX_CHECK(!wcx_config_enum(&c, "mode", modes, 2, 5, &idx, &err)); // bad default
    wcx_config_free(&c);
}

WCX_TEST(rejects_hostile_configs) {
    static const char *const bad[] = {
        "",
        "{",
        "null",
        "[]",
        "42",
        "\"str\"",
        "{\"signal_bindings\":5}",
        "{\"signal_bindings\":[]}",
        "{\"signal_bindings\":{\"clk\":1}}",
        "{\"parameters\":\"x\"}",
        "{\"options\":[1]}",
        "{\"decoder_id\":7}",
        "\xFF\xFE{}",
        "{\"parameters\":{\"p\":\"\xC0\xAF\"}}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        wcx_config c;
        wcx_error err = {{0}};
        if (wcx_config_parse(&c, bad[i], &err)) {
            WCX_FAIL("accepted hostile config #%lu", (unsigned long)i);
        }
        WCX_CHECK(err.msg[0] != '\0');
        // A failed config answers every query safely.
        int64_t v = 0;
        WCX_CHECK(!wcx_config_is_bound(&c, "clk"));
        WCX_CHECK(wcx_config_int(&c, "p", 1, 0, 2, &v, NULL) && v == 1);
        wcx_config_free(&c);
    }
    wcx_config c;
    WCX_CHECK(!wcx_config_parse(&c, NULL, NULL));
    WCX_CHECK(!wcx_config_parse(NULL, "{}", NULL));
}

WCX_TEST(null_members_are_absent) {
    wcx_config c;
    WCX_REQUIRE(wcx_config_parse(
        &c, "{\"signal_bindings\":null,\"parameters\":null,\"unknown\":[1,2]}", NULL));
    WCX_CHECK(!wcx_config_has_decoder_id(&c));
    WCX_CHECK(!wcx_config_is_bound(&c, "clk"));
    wcx_config_free(&c);
}

WCX_TEST(size_ceiling) {
    const size_t n = WCX_CONFIG_MAX_BYTES + 16u;
    char *big = malloc(n + 1);
    WCX_REQUIRE(big != NULL);
    memset(big, ' ', n);
    big[0] = '{';
    big[n - 1] = '}';
    big[n] = '\0';
    wcx_config c;
    wcx_error err = {{0}};
    WCX_CHECK(!wcx_config_parse(&c, big, &err));
    WCX_CHECK(strstr(err.msg, "larger than") != NULL);
    free(big);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, parses_the_host_shape);
    WCX_RUN(&t, int_parameters);
    WCX_RUN(&t, int_text_forms);
    WCX_RUN(&t, bool_string_enum_parameters);
    WCX_RUN(&t, rejects_hostile_configs);
    WCX_RUN(&t, null_members_are_absent);
    WCX_RUN(&t, size_ceiling);
    return wcx_test_finish(&t);
}
