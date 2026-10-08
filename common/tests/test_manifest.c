// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Each rejected manifest corresponds to a FormatException or failed cast in
// WaveCrux's loader (lib/services/decoders/ffi/ffi_decoder_loader_io.dart,
// _adaptDefinition lines 578-622, _decodeBindings 624-659,
// _decodeParameters 661-698, _decodeEnumLabels 705-744,
// _parameterTypeOf 746-762, _decodeCategory/_decodeTier 776-796).

#include <string.h>

#include "wcx/manifest.h"
#include "wcx/str.h"
#include "wcx/test.h"

static bool parses(const char *json) {
    wcx_manifest m;
    const bool ok = wcx_manifest_parse(&m, json, NULL);
    wcx_manifest_free(&m);
    return ok;
}

WCX_TEST(full_manifest) {
    const char *json =
        "{\"description\":\"d\",\"category\":\"bus\",\"required_tier\":\"pro\","
        "\"signals\":[{\"name\":\"clk\"},{\"name\":\"data\",\"bit_width\":8,\"description\":\"x\"}]"
        ","
        "\"optional_signals\":[{\"name\":\"valid\",\"bit_width\":null}],"
        "\"parameters\":[{\"name\":\"mode\",\"kind\":\"enumeration\",\"default\":\"a\","
        "\"enum_values\":[\"a\",\"b\"],\"enum_labels\":[\"A\",\"B\"]},"
        "{\"name\":\"n\",\"kind\":\"integer\"},{\"name\":\"f\",\"kind\":\"boolean\",\"default\":"
        "null},"
        "{\"name\":\"s\",\"kind\":\"string\",\"display_name\":\"S\"}]}";
    wcx_manifest m;
    wcx_error err = {{0}};
    WCX_REQUIRE(wcx_manifest_parse(&m, json, &err));
    WCX_CHECK_EQ_U64(m.signal_count, 3);
    WCX_CHECK_EQ_U64(m.signals[0].bit_width, 1); // absent -> 1
    WCX_CHECK_EQ_U64(m.signals[1].bit_width, 8);
    WCX_CHECK(m.signals[2].optional);
    WCX_CHECK_EQ_U64(m.signals[2].bit_width, 1); // null -> 1
    WCX_CHECK_EQ_U64(m.param_count, 4);
    WCX_CHECK_EQ_U64(m.params[0].kind, WCX_PARAM_ENUM);
    WCX_CHECK_EQ_U64(m.params[1].kind, WCX_PARAM_INT);
    WCX_CHECK_EQ_U64(m.params[2].kind, WCX_PARAM_BOOL);
    WCX_CHECK_EQ_U64(m.params[2].default_value, WCX_JSON_NONE);
    WCX_CHECK_EQ_U64(m.params[3].kind, WCX_PARAM_STRING);
    WCX_CHECK_EQ_U64(wcx_manifest_find_signal(&m, "data"), 1);
    WCX_CHECK_EQ_U64(wcx_manifest_find_signal(&m, "nope"), WCX_JSON_NONE);
    WCX_CHECK_EQ_U64(wcx_manifest_find_param(&m, "s"), 3);
    WCX_CHECK_EQ_U64(wcx_manifest_find_param(&m, "zz"), WCX_JSON_NONE);
    char name[16] = {0};
    WCX_CHECK(wcx_manifest_text(&m, m.signals[2].name, name, sizeof name));
    WCX_CHECK_STR_EQ(name, "valid");

    const wcx_signal_spec good[] = {{"clk", 1, false}, {"data", 8, false}, {"valid", 1, true}};
    WCX_CHECK(wcx_manifest_check_signals(&m, good, 3, &err));
    const wcx_signal_spec bad_width[] = {{"clk", 1, false}, {"data", 7, false}, {"valid", 1, true}};
    WCX_CHECK(!wcx_manifest_check_signals(&m, bad_width, 3, &err));
    WCX_CHECK_STR_EQ(err.msg, "decoder signal table entry 1 does not match manifest signal "
                              "\"data\" (8 bits)");
    const wcx_signal_spec bad_opt[] = {{"clk", 1, false}, {"data", 8, false}, {"valid", 1, false}};
    WCX_CHECK(!wcx_manifest_check_signals(&m, bad_opt, 3, &err));
    WCX_CHECK(!wcx_manifest_check_signals(&m, good, 2, &err));
    wcx_manifest_free(&m);
}

WCX_TEST(minimal_manifests) {
    WCX_CHECK(parses("{}"));
    WCX_CHECK(parses("{\"signals\":null,\"parameters\":null}"));
    WCX_CHECK(parses("{\"signals\":[{\"name\":\"a\",\"bit_width\":0}]}")); // loader accepts 0
    WCX_CHECK(parses("{\"parameters\":[{\"name\":\"e\",\"kind\":\"enum\","
                     "\"enum_labels\":{\"x\":\"X\"}}]}"));
}

static const char k_label_count_mismatch[] =
    "{\"parameters\":[{\"name\":\"p\",\"kind\":\"enum\",\"enum_values\":[\"a\"],"
    "\"enum_labels\":[\"A\",\"B\"]}]}";

WCX_TEST(rejects_what_the_loader_rejects) {
    static const char *const bad[] = {
        "[]",
        "{\"signals\":{}}",
        "{\"signals\":[1]}",
        "{\"signals\":[{}]}",
        "{\"signals\":[{\"name\":\"\"}]}",
        "{\"signals\":[{\"name\":5}]}",
        "{\"signals\":[{\"name\":\"a\",\"bit_width\":\"8\"}]}",
        "{\"signals\":[{\"name\":\"a\",\"bit_width\":1.0}]}",
        "{\"signals\":[{\"name\":\"a\",\"description\":3}]}",
        "{\"optional_signals\":\"x\"}",
        "{\"parameters\":{}}",
        "{\"parameters\":[{\"name\":\"p\"}]}",
        "{\"parameters\":[{\"name\":\"p\",\"kind\":\"float\"}]}",
        "{\"parameters\":[{\"name\":\"p\",\"kind\":7}]}",
        "{\"parameters\":[{\"kind\":\"int\"}]}",
        "{\"parameters\":[{\"name\":\"p\",\"kind\":\"enum\",\"enum_values\":\"a\"}]}",
        "{\"parameters\":[{\"name\":\"p\",\"kind\":\"enum\",\"enum_values\":[1]}]}",
        k_label_count_mismatch,
        "{\"parameters\":[{\"name\":\"p\",\"kind\":\"enum\",\"enum_labels\":[\"A\"]}]}",
        "{\"parameters\":[{\"name\":\"p\",\"kind\":\"enum\",\"enum_labels\":{\"a\":1}}]}",
        "{\"parameters\":[{\"name\":\"p\",\"kind\":\"enum\",\"enum_labels\":5}]}",
        "{\"parameters\":[{\"name\":\"p\",\"kind\":\"int\",\"display_name\":[]}]}",
        "{\"category\":1}",
        "{\"required_tier\":true}",
        "{\"description\":{}}",
        "not json",
        "{\"signals\":[{\"name\":\"a\",\"bit_width\":-1}]}",
        "{\"signals\":[{\"name\":\"a\",\"bit_width\":5000}]}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        if (parses(bad[i])) {
            WCX_FAIL("accepted invalid manifest #%lu: %s", (unsigned long)i, bad[i]);
        }
    }
    wcx_manifest m;
    WCX_CHECK(!wcx_manifest_parse(&m, NULL, NULL));
    wcx_manifest_free(&m);
    WCX_CHECK(!wcx_manifest_parse(NULL, "{}", NULL));
}

WCX_TEST(too_many_signals) {
    char json[8192] = "{\"signals\":[";
    size_t used = strlen(json);
    for (unsigned i = 0; i <= WCX_MAX_SIGNALS; i++) {
        char entry[32] = {0};
        WCX_IGNORE(
            wcx_str_format(entry, sizeof entry, "%s{\"name\":\"s%u\"}", i == 0 ? "" : ",", i));
        memcpy(json + used, entry, strlen(entry) + 1u); /* keeps json terminated */
        used += strlen(entry);
    }
    memcpy(json + used, "]}", 3);
    WCX_CHECK(!parses(json));
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, full_manifest);
    WCX_RUN(&t, minimal_manifests);
    WCX_RUN(&t, rejects_what_the_loader_rejects);
    WCX_RUN(&t, too_many_signals);
    return wcx_test_finish(&t);
}
