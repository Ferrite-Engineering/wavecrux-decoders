// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The Gen1/Gen2 scrambler against the known key sequence of
// docs/protocol-notes.md §3 (what 32 scrambled idles look like after COM).

#include "pcie_scrambler.h"
#include "wcx/test.h"

// protocol-notes.md §3, "Known answer": keys for the first 32 symbols after
// a COM.
static const uint8_t k_keys[32] = {0xFF, 0x17, 0xC0, 0x14, 0xB2, 0xE7, 0x02, 0x82, 0x72, 0x6E, 0x28,
                                   0xA6, 0xBE, 0x6D, 0xBF, 0x8D, 0xBE, 0x40, 0xA7, 0xE6, 0x2C, 0xD3,
                                   0xE2, 0xB2, 0x07, 0x02, 0x77, 0x2A, 0xCD, 0x34, 0xBE, 0xE0};

WCX_TEST(thirty_two_keys_after_com) {
    pcie_scrambler s;
    pcie_scrambler_reset(&s);
    WCX_CHECK_EQ_U64(s.lfsr, 0xFFFFu);
    for (unsigned i = 0; i < 32u; i++) {
        WCX_CHECK_EQ_U64(pcie_scrambler_key(&s), k_keys[i]);     // peek
        WCX_CHECK_EQ_U64(pcie_scrambler_advance(&s), k_keys[i]); // then step
    }
}

WCX_TEST(reset_restarts_the_sequence) {
    pcie_scrambler s;
    pcie_scrambler_reset(&s);
    for (unsigned i = 0; i < 5u; i++) {
        (void)pcie_scrambler_advance(&s);
    }
    WCX_CHECK_EQ_U64(pcie_scrambler_key(&s), k_keys[5]);
    pcie_scrambler_reset(&s);
    WCX_CHECK_EQ_U64(pcie_scrambler_key(&s), 0xFFu);
}

WCX_TEST(period_is_65535) {
    // A maximal-length 16-bit LFSR returns to its seed after 2^16 - 1 bit
    // steps; eight bit steps per symbol means 65535 symbols cycle through
    // every state exactly once (gcd(8, 65535) = 1).
    pcie_scrambler s;
    pcie_scrambler_reset(&s);
    unsigned back_at = 0;
    for (unsigned i = 1; i <= 65535u; i++) {
        (void)pcie_scrambler_advance(&s);
        if (s.lfsr == 0xFFFFu && back_at == 0) {
            back_at = i;
        }
    }
    WCX_CHECK_EQ_U64(back_at, 65535u);
    WCX_CHECK(s.lfsr != 0); // the all-zero state is unreachable
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, thirty_two_keys_after_com);
    WCX_RUN(&t, reset_restarts_the_sequence);
    WCX_RUN(&t, period_is_65535);
    return wcx_test_finish(&t);
}
