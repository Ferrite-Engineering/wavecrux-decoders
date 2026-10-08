// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The VCD reader on handcrafted files. Expected values follow IEEE
// 1364-2005 §18 and wellen's behaviour (vcd.rs decode_vcd_bit_vec_change /
// expand_special_vector_cases, wavemem.rs time_change), which is what
// WaveCrux reads VCDs with.

#include <string.h>

#include "vcd.h"
#include "wcx/str.h"
#include "wcx/test.h"

static bool load(wcxh_vcd *v, const char *text, wcx_error *err) {
    return wcxh_vcd_parse_header(v, text, strlen(text), err);
}

static const char k_basic[] = "$date today $end\n"
                              "$version x $end\n"
                              "$timescale\n  10 ps\n$end\n"
                              "$scope module tb $end\n"
                              " $var wire 1 ! clk $end\n"
                              " $var wire 8 \" data [7:0] $end\n"
                              " $scope module dut $end\n"
                              "  $var reg 4 # nib[3:0] $end\n"
                              "  $var wire 1 ! clk_alias $end\n"
                              "  $var real 64 $ temp $end\n"
                              " $upscope $end\n"
                              "$upscope $end\n"
                              "$enddefinitions $end\n"
                              "#0\n$dumpvars\n0!\nb101 \"\nbx #\nr0.5 $\n$end\n"
                              "#7\n1!\nb1 #\nbz1 \"\n"
                              "#7\nb11110000 \"\n"
                              "#3\n0!\n"
                              "#9\n$comment changes inside a comment 1! $end\n0!\n"
                              "$dumpoff\nbx \"\n$end\n"
                              "#12\n";

WCX_TEST(header_scopes_widths_ranges) {
    wcxh_vcd v;
    wcx_error err = {{0}};
    WCX_REQUIRE(load(&v, k_basic, &err));
    WCX_CHECK(v.has_timescale);
    WCX_CHECK_EQ_U64(v.ts_factor, 10);
    WCX_CHECK_EQ_I64(v.ts_exponent, -12);
    WCX_REQUIRE(v.var_count == 5);
    WCX_CHECK_STR_EQ(v.vars[0].path, "tb.clk");
    WCX_CHECK_STR_EQ(v.vars[1].path, "tb.data");
    WCX_CHECK_STR_EQ(v.vars[1].range, "[7:0]");
    WCX_CHECK_EQ_U64(v.vars[1].width, 8);
    WCX_CHECK_STR_EQ(v.vars[2].path, "tb.dut.nib");
    WCX_CHECK_STR_EQ(v.vars[2].range, "[3:0]");
    WCX_CHECK_STR_EQ(v.vars[3].path, "tb.dut.clk_alias");
    WCX_CHECK(v.vars[4].is_real);
    // wellen signal refs: distinct id codes in first-declaration order
    // (! " # $), the alias sharing "!".
    WCX_CHECK_EQ_U64(v.vars[0].signal_ref, 0);
    WCX_CHECK_EQ_U64(v.vars[1].signal_ref, 1);
    WCX_CHECK_EQ_U64(v.vars[2].signal_ref, 2);
    WCX_CHECK_EQ_U64(v.vars[3].signal_ref, 0);
    WCX_CHECK_EQ_U64(v.vars[4].signal_ref, 3);
    WCX_CHECK_EQ_U64(wcxh_vcd_find(&v, "tb.data", &err), 1);
    WCX_CHECK_EQ_U64(wcxh_vcd_find(&v, "tb.data[7:0]", &err), 1);
    WCX_CHECK_EQ_U64(wcxh_vcd_find(&v, "tb.data[3:0]", &err), WCXH_VCD_NONE);
    WCX_CHECK_EQ_U64(wcxh_vcd_find(&v, "tb.nope", &err), WCXH_VCD_NONE);
    WCX_CHECK_STR_EQ(err.msg, "the VCD has no signal \"tb.nope\"");
    wcxh_vcd_free(&v);
}

WCX_TEST(body_values_extension_and_time_rules) {
    wcxh_vcd v;
    wcx_error err = {{0}};
    WCX_REQUIRE(load(&v, k_basic, &err));
    const size_t vars[] = {0, 1, 2, 3};
    WCX_REQUIRE(wcxh_vcd_parse_body(&v, vars, 4, &err));
    // end_time is the last time stamp; "#3" went backwards and was skipped.
    WCX_CHECK(v.has_time);
    WCX_CHECK_EQ_U64(v.end_time, 12);
    WCX_CHECK_EQ_U64(v.backwards_times, 1);
    WCX_CHECK_EQ_U64(v.real_changes_ignored, 1);
    size_t c = 0;
    // clk: 0 at 0, 1 at 7, the 0 at "#3" skipped, 0 at 9 (the "1!" inside
    // $comment is not a change).
    WCX_CHECK_EQ_U64(v.sigs[0].count, 3);
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 0, 0, &c), "0");
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 0, 7, &c), "1");
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 0, 8, &c), "1");
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 0, 100, &c), "0");
    // data: b101 zero-extends; bz1 z-extends; two changes at #7 (the repeated
    // #7 is the same step): the last wins; $dumpoff's bx x-extends.
    c = 0;
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 1, 0, &c), "00000101");
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 1, 7, &c), "11110000");
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 1, 9, &c), "xxxxxxxx");
    // Queries going backwards still answer correctly.
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 1, 0, &c), "00000101");
    // nib: bx x-extends, b1 zero-extends.
    c = 0;
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 2, 0, &c), "xxxx");
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 2, 7, &c), "0001");
    // The alias sees the same changes as clk.
    WCX_CHECK_EQ_U64(v.sigs[3].count, 3);
    WCX_CHECK(wcxh_vcd_value_at(&v, 99, 0, &c) == NULL);
    wcxh_vcd_free(&v);
}

WCX_TEST(value_before_first_change_is_null) {
    const char *text = "$timescale 1ns $end $scope module m $end $var wire 1 a x $end "
                       "$var wire 1 b y $end $upscope $end $enddefinitions $end "
                       "#0 0a #5 1b #6";
    wcxh_vcd v;
    WCX_REQUIRE(load(&v, text, NULL));
    const size_t vars[] = {1};
    WCX_REQUIRE(wcxh_vcd_parse_body(&v, vars, 1, NULL));
    size_t c = 0;
    WCX_CHECK(wcxh_vcd_value_at(&v, 0, 4, &c) == NULL);
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 0, 5, &c), "1");
    wcxh_vcd_free(&v);
}

WCX_TEST(timescale_forms) {
    static const struct {
        const char *ts;
        uint32_t factor;
        int exponent;
    } ok[] = {{"1s", 1, 0},    {"10ms", 10, -3},   {"100us", 100, -6},
              {"1 ns", 1, -9}, {"10 ps", 10, -12}, {"100fs", 100, -15}};
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        char text[128] = {0};
        WCX_IGNORE(
            wcx_str_format(text, sizeof text, "$timescale %s $end $enddefinitions $end", ok[i].ts));
        wcxh_vcd v;
        WCX_REQUIRE(load(&v, text, NULL));
        WCX_CHECK_EQ_U64(v.ts_factor, ok[i].factor);
        WCX_CHECK_EQ_I64(v.ts_exponent, ok[i].exponent);
        wcxh_vcd_free(&v);
    }
    static const char *const bad[] = {"ns", "1 xs", "0ns", "1nsx", "abc"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char text[128] = {0};
        WCX_IGNORE(
            wcx_str_format(text, sizeof text, "$timescale %s $end $enddefinitions $end", bad[i]));
        wcxh_vcd v;
        WCX_CHECK(!load(&v, text, NULL));
        wcxh_vcd_free(&v);
    }
    wcxh_vcd v;
    WCX_REQUIRE(load(&v, "$enddefinitions $end", NULL));
    WCX_CHECK(!v.has_timescale); // no timescale: WaveCrux uses 1 ns
    wcxh_vcd_free(&v);
}

WCX_TEST(malformed_files_are_errors) {
    static const char *const headers[] = {
        "",
        "$scope module a $end",                      // no $enddefinitions
        "$upscope $end $enddefinitions $end",        // upscope without scope
        "$var wire x ! a $end $enddefinitions $end", // bad width
        "$var wire 0 ! a $end $enddefinitions $end",
        "$var wire 1 ! $enddefinitions $end",
        "garbage $enddefinitions $end",
        "$comment never ends",
    };
    for (size_t i = 0; i < sizeof headers / sizeof headers[0]; i++) {
        wcxh_vcd v;
        wcx_error err = {{0}};
        if (load(&v, headers[i], &err)) {
            WCX_FAIL("accepted malformed header #%lu", (unsigned long)i);
        }
        WCX_CHECK(err.msg[0] != '\0');
        wcxh_vcd_free(&v);
    }
    static const char *const bodies[] = {
        "#x",            // time not a number
        "b1111111111 !", // longer than 8 bits: wellen refuses it
        "bh01 !",        // 'h' is a valid state but cannot extend: wellen refuses it
        "bq !",          // invalid digit
        "b !",           // empty vector
        "1",             // scalar with no id
        "?!",            // unknown token
        "r1.0 !",        // a real change on a bound (non-real) signal
        "$comment open",
    };
    const char *head = "$var wire 8 ! d $end $enddefinitions $end ";
    for (size_t i = 0; i < sizeof bodies / sizeof bodies[0]; i++) {
        char text[256] = {0};
        WCX_IGNORE(wcx_str_format(text, sizeof text, "%s%s", head, bodies[i]));
        wcxh_vcd v;
        WCX_REQUIRE(load(&v, text, NULL));
        const size_t vars[] = {0};
        wcx_error err = {{0}};
        if (wcxh_vcd_parse_body(&v, vars, 1, &err)) {
            WCX_FAIL("accepted malformed body #%lu: %s", (unsigned long)i, bodies[i]);
        }
        wcxh_vcd_free(&v);
    }
}

WCX_TEST(ambiguous_path_needs_a_range) {
    const char *text = "$scope module m $end $var wire 1 ! b [0] $end $var wire 1 \" b [1] $end "
                       "$upscope $end $enddefinitions $end";
    wcxh_vcd v;
    WCX_REQUIRE(load(&v, text, NULL));
    wcx_error err = {{0}};
    WCX_CHECK_EQ_U64(wcxh_vcd_find(&v, "m.b", &err), WCXH_VCD_NONE);
    WCX_CHECK(strstr(err.msg, "names 2 signals") != NULL);
    WCX_CHECK_EQ_U64(wcxh_vcd_find(&v, "m.b[1]", &err), 1);
    wcxh_vcd_free(&v);
}

WCX_TEST(one_bit_signal_takes_the_last_character) {
    // wellen decode_vcd_bit_vec_change, len == 1: value.last(), "b" -> 0.
    const char *text = "$var wire 1 ! a $end $enddefinitions $end #0 b1 ! #1 b ! #2 b10 ! #3";
    wcxh_vcd v;
    WCX_REQUIRE(load(&v, text, NULL));
    const size_t vars[] = {0};
    WCX_REQUIRE(wcxh_vcd_parse_body(&v, vars, 1, NULL));
    size_t c = 0;
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 0, 0, &c), "1");
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 0, 1, &c), "0");
    WCX_CHECK_STR_EQ(wcxh_vcd_value_at(&v, 0, 2, &c), "0");
    wcxh_vcd_free(&v);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, header_scopes_widths_ranges);
    WCX_RUN(&t, body_values_extension_and_time_rules);
    WCX_RUN(&t, value_before_first_change_is_null);
    WCX_RUN(&t, timescale_forms);
    WCX_RUN(&t, malformed_files_are_errors);
    WCX_RUN(&t, ambiguous_path_needs_a_range);
    WCX_RUN(&t, one_bit_signal_takes_the_last_character);
    return wcx_test_finish(&t);
}
