// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Expected buffers are derived by hand from WaveCrux's _packBindingValue
// (lib/services/decoders/ffi/ffi_decoder_loader_io.dart lines 1192-1241):
// signal bit i of a binding at offset o lands at buffer bit 2*(o+i) (level)
// and 2*(o+i)+1 (unknown), LSB-first within each byte.

#include <string.h>

#include "wcx/config.h"
#include "wcx/sample.h"
#include "wcx/test.h"

// clk:1, data:8, valid:1 (optional), strobe:3 (optional)
static const wcx_signal_spec k_specs[] = {
    {"clk", 1, false}, {"data", 8, false}, {"valid", 1, true}, {"strobe", 3, true}};

// Sets signal bit `bit` (absolute signal-bit position) to level/unknown.
static void put(uint8_t *buf, unsigned bit, int level, int unknown) {
    const unsigned pos = bit * 2u;
    if (level) {
        buf[pos / 8u] = (uint8_t)(buf[pos / 8u] | (1u << (pos % 8u)));
    }
    if (unknown) {
        buf[pos / 8u] = (uint8_t)(buf[pos / 8u] | (1u << (pos % 8u + 1u)));
    }
}

WCX_TEST(layout_follows_declaration_order_and_skips_unbound_optionals) {
    wcx_layout l;
    const bool all[] = {true, true, true, true};
    WCX_REQUIRE(wcx_layout_build_bound(&l, k_specs, 4, all, NULL));
    WCX_CHECK_EQ_U64(l.sig[0].offset, 0);
    WCX_CHECK_EQ_U64(l.sig[1].offset, 1);
    WCX_CHECK_EQ_U64(l.sig[2].offset, 9);
    WCX_CHECK_EQ_U64(l.sig[3].offset, 10);
    WCX_CHECK_EQ_U64(l.total_bits, 13);
    WCX_CHECK_EQ_U64(l.encoded_bytes, 4); // (2*13+7)/8

    // valid unbound: strobe moves down to offset 9 (loader: activeBindings
    // only, bitOffset += spec.bitWidth for bound specs).
    const bool no_valid[] = {true, true, false, true};
    WCX_REQUIRE(wcx_layout_build_bound(&l, k_specs, 4, no_valid, NULL));
    WCX_CHECK(!l.sig[2].bound);
    WCX_CHECK_EQ_U64(l.sig[3].offset, 9);
    WCX_CHECK_EQ_U64(l.total_bits, 12);
    WCX_CHECK(!wcx_signal_bound(&l, 2));
    WCX_CHECK(!wcx_signal_bound(&l, 99));
}

WCX_TEST(layout_from_config) {
    wcx_config c;
    WCX_REQUIRE(wcx_config_parse(
        &c, "{\"signal_bindings\":{\"clk\":\"1\",\"data\":\"2\",\"strobe\":\"5\"}}", NULL));
    wcx_layout l;
    WCX_REQUIRE(wcx_layout_build(&l, k_specs, 4, &c, NULL));
    WCX_CHECK_EQ_U64(l.total_bits, 12);
    wcx_config_free(&c);

    WCX_REQUIRE(wcx_config_parse(&c, "{\"signal_bindings\":{\"data\":\"2\"}}", NULL));
    wcx_error err = {{0}};
    WCX_CHECK(!wcx_layout_build(&l, k_specs, 4, &c, &err));
    WCX_CHECK_STR_EQ(err.msg, "required signal \"clk\" is not bound");
    wcx_config_free(&c);
}

WCX_TEST(layout_rejects_malformed_tables) {
    wcx_layout l;
    wcx_error err = {{0}};
    const bool all[] = {true, true};
    const wcx_signal_spec zero[] = {{"a", 0, false}};
    WCX_CHECK(!wcx_layout_build_bound(&l, zero, 1, all, &err));
    const wcx_signal_spec wide[] = {{"a", WCX_MAX_SIGNAL_BITS + 1, false}};
    WCX_CHECK(!wcx_layout_build_bound(&l, wide, 1, all, &err));
    const wcx_signal_spec order[] = {{"a", 1, true}, {"b", 1, false}};
    WCX_CHECK(!wcx_layout_build_bound(&l, order, 2, all, &err));
    WCX_CHECK(strstr(err.msg, "declared after an optional") != NULL);
    WCX_CHECK(!wcx_layout_build_bound(&l, k_specs, WCX_MAX_SIGNALS + 1, all, &err));
    WCX_CHECK(!wcx_layout_build(&l, k_specs, WCX_MAX_SIGNALS + 1, NULL, &err));
    WCX_CHECK(!wcx_layout_build_bound(NULL, k_specs, 1, all, &err));
}

WCX_TEST(reads_across_byte_boundaries_with_x_and_z) {
    wcx_layout l;
    const bool all[] = {true, true, true, true};
    WCX_REQUIRE(wcx_layout_build_bound(&l, k_specs, 4, all, NULL));
    uint8_t buf[32] = {0}; // the loader's 32-byte minimum
    put(buf, 0, 1, 0);     // clk = 1
    // data = 0xA5 = 1010_0101 at signal bits 1..8 (buffer bits 2..17: spans
    // bytes 0, 1 and 2).
    for (unsigned i = 0; i < 8; i++) {
        put(buf, 1 + i, (int)((0xA5u >> i) & 1u), 0);
    }
    put(buf, 9, 0, 1);  // valid = X
    put(buf, 10, 1, 0); // strobe = 0b101 with bit 1 = Z
    put(buf, 11, 0, 1);
    put(buf, 12, 1, 0);
    // Hand check of the buffer: byte0 bits: clk L@0, d0=1 L@2, d1=0, d2=1 L@6
    // -> 0b0100_0101 = 0x45.
    WCX_CHECK_EQ_U64(buf[0], 0x45);

    uint64_t v = 0;
    bool x = true;
    WCX_CHECK(wcx_sample_read(&l, buf, 0, &v, &x));
    WCX_CHECK_EQ_U64(v, 1);
    WCX_CHECK(!x);
    WCX_CHECK(wcx_sample_read(&l, buf, 1, &v, &x));
    WCX_CHECK_EQ_U64(v, 0xA5);
    WCX_CHECK(!x);
    WCX_CHECK(wcx_sample_read(&l, buf, 2, &v, &x));
    WCX_CHECK_EQ_U64(v, 0);
    WCX_CHECK(x);
    WCX_CHECK(wcx_sample_read(&l, buf, 3, &v, &x));
    WCX_CHECK_EQ_U64(v, 0x5); // X/Z read as 0
    WCX_CHECK(x);
    WCX_CHECK(wcx_sample_read_slice(&l, buf, 1, 4, 4, &v, &x));
    WCX_CHECK_EQ_U64(v, 0xA);
    bool level = false;
    WCX_CHECK(wcx_sample_read_bit(&l, buf, 3, 1, &level, &x));
    WCX_CHECK(!level && x);
    WCX_CHECK(wcx_sample_read_bit(&l, buf, 3, 2, &level, &x));
    WCX_CHECK(level && !x);
}

WCX_TEST(reads_reject_bad_requests) {
    wcx_layout l;
    const bool some[] = {true, true, false, true};
    WCX_REQUIRE(wcx_layout_build_bound(&l, k_specs, 4, some, NULL));
    const uint8_t buf[32] = {0};
    uint64_t v = 7;
    bool x = true;
    WCX_CHECK(!wcx_sample_read(&l, buf, 2, &v, &x)); // unbound
    WCX_CHECK(v == 0 && !x);
    WCX_CHECK(!wcx_sample_read(&l, buf, 9, &v, &x));
    WCX_CHECK(!wcx_sample_read_slice(&l, buf, 1, 8, 1, &v, &x)); // past the signal
    WCX_CHECK(!wcx_sample_read_slice(&l, buf, 1, 4, 5, &v, &x));
    WCX_CHECK(!wcx_sample_read_slice(&l, buf, 1, 0, 0, &v, &x));
    WCX_CHECK(!wcx_sample_read_slice(&l, NULL, 1, 0, 1, &v, &x));
    WCX_CHECK(!wcx_sample_read(&l, buf, 1, NULL, &x));
    bool level = true;
    WCX_CHECK(!wcx_sample_read_bit(&l, buf, 2, 0, &level, &x));
    WCX_CHECK(!level);

    const wcx_signal_spec wide[] = {{"bus", 100, false}};
    const bool on[] = {true};
    WCX_REQUIRE(wcx_layout_build_bound(&l, wide, 1, on, NULL));
    WCX_CHECK(!wcx_sample_read(&l, buf, 0, &v, &x)); // > 64 bits: slice it
    WCX_CHECK(wcx_sample_read_slice(&l, buf, 0, 36, 64, &v, &x));
}

WCX_TEST(sample_width_is_checked) {
    wcx_layout l;
    const bool all[] = {true, true, true, true};
    WCX_REQUIRE(wcx_layout_build_bound(&l, k_specs, 4, all, NULL));
    uint8_t buf[32] = {0};
    WcSample s = {0, buf, 13, 0};
    wcx_error err = {{0}};
    WCX_CHECK(wcx_layout_check_sample(&l, &s, &err));
    s.bit_width = 12;
    WCX_CHECK(!wcx_layout_check_sample(&l, &s, &err));
    WCX_CHECK_STR_EQ(err.msg, "sample is 12 bits wide but the bound signals need 13 bits; "
                              "decoding stopped");
    s.bit_width = 13;
    s.bits_ptr = NULL;
    WCX_CHECK(!wcx_layout_check_sample(&l, &s, &err));
    WCX_CHECK(!wcx_layout_check_sample(&l, NULL, &err));
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, layout_follows_declaration_order_and_skips_unbound_optionals);
    WCX_RUN(&t, layout_from_config);
    WCX_RUN(&t, layout_rejects_malformed_tables);
    WCX_RUN(&t, reads_across_byte_boundaries_with_x_and_z);
    WCX_RUN(&t, reads_reject_bad_requests);
    WCX_RUN(&t, sample_width_is_checked);
    return wcx_test_finish(&t);
}
