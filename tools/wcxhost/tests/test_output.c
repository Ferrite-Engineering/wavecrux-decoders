// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The canonical output format and expected-file parsing (cmake/README.md,
// "Expected files").

#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "wcx/test.h"

// The WaveCrux example's pretty-printed layout (examples/decoder-plugin-demo/
// fixtures/onewire_basic.expected_transactions.json): different key order,
// whitespace, and "isError" after "fields". It must read as canonical.
static const char k_pretty[] = "[\n"
                               "  {\n"
                               "    \"startTime\": 100000,\n"
                               "    \"endTime\": 580000,\n"
                               "    \"label\": \"RESET\",\n"
                               "    \"fields\": {\n"
                               "      \"kind\": \"reset\"\n"
                               "    },\n"
                               "    \"isError\": false\n"
                               "  },\n"
                               "  {\"label\": \"E\\u00e9\", \"startTime\": 1, \"endTime\": 2, "
                               "\"isError\": true}\n"
                               "]\n";

WCX_TEST(parses_and_formats_canonically) {
    wcxh_txlist l;
    wcx_error err = {{0}};
    WCX_REQUIRE(wcxh_parse_expected(k_pretty, strlen(k_pretty), &l, &err));
    WCX_REQUIRE(l.count == 2);
    char *text = wcxh_format(&l);
    WCX_CHECK_STR_EQ(
        text, "[\n"
              "{\"startTime\":100000,\"endTime\":580000,\"label\":\"RESET\",\"isError\":false,"
              "\"fields\":{\"kind\":\"reset\"}},\n"
              "{\"startTime\":1,\"endTime\":2,\"label\":\"E\xC3\xA9\",\"isError\":true,"
              "\"fields\":{}}\n"
              "]\n");
    // Round trip: the canonical text parses to the same list.
    wcxh_txlist again;
    WCX_REQUIRE(text != NULL && wcxh_parse_expected(text, strlen(text), &again, &err));
    WCX_CHECK_EQ_U64(wcxh_first_difference(&l, &again), SIZE_MAX);
    free(text);
    wcxh_txlist_free(&again);
    wcxh_txlist_free(&l);
}

WCX_TEST(empty_list) {
    wcxh_txlist l;
    WCX_REQUIRE(wcxh_parse_expected("[]", 2, &l, NULL));
    char *text = wcxh_format(&l);
    WCX_CHECK_STR_EQ(text, "[\n]\n");
    free(text);
    wcxh_txlist_free(&l);
}

WCX_TEST(first_difference) {
    wcxh_txlist a;
    wcxh_txlist b;
    WCX_REQUIRE(wcxh_parse_expected(k_pretty, strlen(k_pretty), &a, NULL));
    WCX_REQUIRE(wcxh_parse_expected(k_pretty, strlen(k_pretty), &b, NULL));
    WCX_CHECK_EQ_U64(wcxh_first_difference(&a, &b), SIZE_MAX);
    b.items[1].end = 3;
    WCX_CHECK_EQ_U64(wcxh_first_difference(&a, &b), 1);
    b.count = 1;
    WCX_CHECK_EQ_U64(wcxh_first_difference(&a, &b), 1); // a has one more
    b.count = 2;
    wcxh_txlist_free(&a);
    wcxh_txlist_free(&b);
}

// --sort: two lists holding the same transactions in different orders sort
// to the same list; the order is startTime, endTime, label, isError, fields.
WCX_TEST(sort_is_canonical_and_order_insensitive) {
    static const char k_a[] =
        "["
        "{\"startTime\":5,\"endTime\":9,\"label\":\"B\",\"isError\":false,\"fields\":{}},"
        "{\"startTime\":5,\"endTime\":6,\"label\":\"Z\",\"isError\":true,\"fields\":{}},"
        "{\"startTime\":1,\"endTime\":2,\"label\":\"A\",\"isError\":false,\"fields\":{}},"
        "{\"startTime\":5,\"endTime\":9,\"label\":\"B\",\"isError\":true,\"fields\":{}},"
        "{\"startTime\":5,\"endTime\":9,\"label\":\"A\",\"isError\":false,\"fields\":{\"k\":\"2\"}}"
        ","
        "{\"startTime\":5,\"endTime\":9,\"label\":\"A\",\"isError\":false,\"fields\":{\"k\":\"1\"}}"
        "]";
    // The same six, reversed.
    static const char k_b[] =
        "["
        "{\"startTime\":5,\"endTime\":9,\"label\":\"A\",\"isError\":false,\"fields\":{\"k\":\"1\"}}"
        ","
        "{\"startTime\":5,\"endTime\":9,\"label\":\"A\",\"isError\":false,\"fields\":{\"k\":\"2\"}}"
        ","
        "{\"startTime\":5,\"endTime\":9,\"label\":\"B\",\"isError\":true,\"fields\":{}},"
        "{\"startTime\":1,\"endTime\":2,\"label\":\"A\",\"isError\":false,\"fields\":{}},"
        "{\"startTime\":5,\"endTime\":6,\"label\":\"Z\",\"isError\":true,\"fields\":{}},"
        "{\"startTime\":5,\"endTime\":9,\"label\":\"B\",\"isError\":false,\"fields\":{}}"
        "]";
    wcxh_txlist a;
    wcxh_txlist b;
    WCX_REQUIRE(wcxh_parse_expected(k_a, strlen(k_a), &a, NULL));
    WCX_REQUIRE(wcxh_parse_expected(k_b, strlen(k_b), &b, NULL));
    WCX_CHECK(wcxh_first_difference(&a, &b) != SIZE_MAX); // differ as given
    wcxh_txlist_sort(&a);
    wcxh_txlist_sort(&b);
    WCX_CHECK_EQ_U64(wcxh_first_difference(&a, &b), SIZE_MAX);
    // Hand-derived order: (1,2,A) first; then start 5: end 6 before end 9;
    // among end 9: label A before B, fields "1" before "2"; B false before B true.
    char *text = wcxh_format(&a);
    WCX_CHECK_STR_EQ(
        text, "[\n"
              "{\"startTime\":1,\"endTime\":2,\"label\":\"A\",\"isError\":false,\"fields\":{}},\n"
              "{\"startTime\":5,\"endTime\":6,\"label\":\"Z\",\"isError\":true,\"fields\":{}},\n"
              "{\"startTime\":5,\"endTime\":9,\"label\":\"A\",\"isError\":false,"
              "\"fields\":{\"k\":\"1\"}},\n"
              "{\"startTime\":5,\"endTime\":9,\"label\":\"A\",\"isError\":false,"
              "\"fields\":{\"k\":\"2\"}},\n"
              "{\"startTime\":5,\"endTime\":9,\"label\":\"B\",\"isError\":false,\"fields\":{}},\n"
              "{\"startTime\":5,\"endTime\":9,\"label\":\"B\",\"isError\":true,\"fields\":{}}\n"
              "]\n");
    free(text);
    // A duplicate on one side only still differs.
    b.count = 5;
    WCX_CHECK_EQ_U64(wcxh_first_difference(&a, &b), 5);
    b.count = 6;
    // Sorting an empty or single-element list is a no-op.
    wcxh_txlist empty;
    WCX_REQUIRE(wcxh_parse_expected("[]", 2, &empty, NULL));
    wcxh_txlist_sort(&empty);
    WCX_CHECK_EQ_U64(empty.count, 0);
    wcxh_txlist_free(&empty);
    wcxh_txlist_free(&a);
    wcxh_txlist_free(&b);
}

WCX_TEST(rejects_malformed_expected_files) {
    static const char *const bad[] = {
        "{}",
        "[1]",
        "[{\"startTime\":1,\"endTime\":2,\"label\":\"x\"}]",                    // no isError
        "[{\"startTime\":-1,\"endTime\":2,\"label\":\"x\",\"isError\":false}]", // negative
        "[{\"startTime\":1.5,\"endTime\":2,\"label\":\"x\",\"isError\":false}]",
        "[{\"startTime\":1,\"endTime\":2,\"label\":3,\"isError\":false}]",
        "[{\"startTime\":1,\"endTime\":2,\"label\":\"x\",\"isError\":0}]",
        "[{\"startTime\":1,\"endTime\":2,\"label\":\"x\",\"isError\":false,\"fields\":{\"a\":1}}]",
        "[{\"startTime\":1,\"endTime\":2,\"label\":\"x\",\"isError\":false,\"extra\":1}]",
        "[",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        wcxh_txlist l;
        wcx_error err = {{0}};
        if (wcxh_parse_expected(bad[i], strlen(bad[i]), &l, &err)) {
            WCX_FAIL("accepted malformed expected file #%lu", (unsigned long)i);
            wcxh_txlist_free(&l);
        }
        WCX_CHECK(err.msg[0] != '\0');
    }
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, parses_and_formats_canonically);
    WCX_RUN(&t, empty_list);
    WCX_RUN(&t, first_difference);
    WCX_RUN(&t, sort_is_canonical_and_order_insensitive);
    WCX_RUN(&t, rejects_malformed_expected_files);
    return wcx_test_finish(&t);
}
