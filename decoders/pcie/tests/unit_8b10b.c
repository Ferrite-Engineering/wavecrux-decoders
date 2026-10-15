// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// 8b/10b against the known answers of docs/protocol-notes.md §9 (checked
// there against pcieVHost's encoder), never against this code's own output;
// then the decode table built from it (SPEC.md §11.7).

#include <string.h>

#include "pcie_8b10b.h"
#include "wcx/test.h"

typedef struct kat {
    const char *name;
    const char *neg; // code for RD-, wire order
    const char *pos; // code for RD+
    uint8_t byte;
    bool k;
    bool neg_after_pos;
    bool pos_after_pos;
} kat;

// protocol-notes.md §9, "Known answers".
static const kat k_kats[] = {
    {"K28.5", "0011111010", "1100000101", 0xBC, true, true, false},
    {"K28.0", "0011110100", "1100001011", 0x1C, true, false, true},
    {"K28.1", "0011111001", "1100000110", 0x3C, true, true, false},
    {"K28.2", "0011110101", "1100001010", 0x5C, true, true, false},
    {"K28.3", "0011110011", "1100001100", 0x7C, true, true, false},
    {"K28.7", "0011111000", "1100000111", 0xFC, true, false, true},
    {"K23.7", "1110101000", "0001010111", 0xF7, true, false, true},
    {"K27.7", "1101101000", "0010010111", 0xFB, true, false, true},
    {"K29.7", "1011101000", "0100010111", 0xFD, true, false, true},
    {"K30.7", "0111101000", "1000010111", 0xFE, true, false, true},
    {"D0.0", "1001110100", "0110001011", 0x00, false, false, true},
    {"D10.2", "0101010101", "0101010101", 0x4A, false, false, true},
    {"D5.2", "1010010101", "1010010101", 0x45, false, false, true},
    {"D21.5", "1010101010", "1010101010", 0xB5, false, false, true},
    {"D7.7", "1110001110", "0001110001", 0xE7, false, true, false},
    {"D17.7", "1000110111", "1000110001", 0xF1, false, true, false},
    {"D11.7", "1101001110", "1101001000", 0xEB, false, true, false},
};

static uint16_t bits(const char *s) {
    uint16_t v = 0;
    for (size_t i = 0; s[i] != '\0'; i++) {
        v = (uint16_t)((unsigned)v << 1u | (s[i] == '1' ? 1u : 0u));
    }
    return v;
}

WCX_TEST(encoder_matches_every_known_answer) {
    for (size_t i = 0; i < sizeof k_kats / sizeof k_kats[0]; i++) {
        const kat *v = &k_kats[i];
        for (int rd = 0; rd < 2; rd++) {
            uint16_t code = 0;
            bool after = false;
            WCX_REQUIRE(pcie_8b10b_encode(v->byte, v->k, rd != 0, &code, &after));
            const uint16_t want = bits(rd != 0 ? v->pos : v->neg);
            const bool want_after = rd != 0 ? v->pos_after_pos : v->neg_after_pos;
            if (code != want || after != want_after) {
                WCX_FAIL("%s RD%c: got 0x%03X rd%c, want 0x%03X rd%c", v->name, rd ? '+' : '-',
                         code, after ? '+' : '-', want, want_after ? '+' : '-');
            }
            t->checks++;
        }
    }
}

WCX_TEST(com_constants_are_k28_5) {
    WCX_CHECK_EQ_U64(PCIE_CODE_COM_NEG, bits("0011111010"));
    WCX_CHECK_EQ_U64(PCIE_CODE_COM_POS, bits("1100000101"));
}

WCX_TEST(only_the_twelve_k_symbols_encode) {
    unsigned valid = 0;
    for (unsigned b = 0; b < 256u; b++) {
        uint16_t code = 0;
        bool after = false;
        if (pcie_8b10b_encode((uint8_t)b, true, false, &code, &after)) {
            valid++;
        }
    }
    WCX_CHECK_EQ_U64(valid, 12);
    uint16_t code = 0;
    bool after = false;
    WCX_CHECK(!pcie_8b10b_encode(0x00, true, false, &code, &after)); // K0.0
    WCX_CHECK(pcie_8b10b_encode(0x9C, true, false, &code, &after));  // K28.4: 8b/10b, not PCIe
}

WCX_TEST(every_symbol_round_trips_at_both_disparities) {
    pcie_8b10b t8;
    pcie_8b10b_init(&t8);
    unsigned codes = 0;
    for (unsigned rd = 0; rd < 2u; rd++) {
        for (unsigned c = 0; c < 1024u; c++) {
            if (t8.dec[rd][c] != 0u) {
                codes++;
            }
        }
        for (unsigned v = 0; v < 512u; v++) {
            uint16_t code = 0;
            bool after = false;
            if (!pcie_8b10b_encode((uint8_t)(v & 0xFFu), v >= 256u, rd != 0u, &code, &after)) {
                continue;
            }
            bool r = rd != 0u;
            uint8_t byte = 0;
            bool k = false;
            WCX_CHECK_EQ_U64(pcie_8b10b_decode(&t8, code, &r, &byte, &k), PCIE_DEC_OK);
            WCX_CHECK_EQ_U64(byte, v & 0xFFu);
            WCX_CHECK_EQ_U64(k, v >= 256u);
            WCX_CHECK_EQ_U64(r, after);
        }
    }
    // 268 symbols at each disparity; the balanced ones share a code.
    WCX_CHECK_EQ_U64(codes, 2u * 268u);
}

WCX_TEST(no_code_means_two_symbols) {
    // SPEC.md §11.7: a code valid at both disparities is the same symbol.
    pcie_8b10b t8;
    pcie_8b10b_init(&t8);
    unsigned distinct = 0;
    for (unsigned c = 0; c < 1024u; c++) {
        const uint16_t a = t8.dec[0][c];
        const uint16_t b = t8.dec[1][c];
        if (a != 0u && b != 0u) {
            WCX_CHECK_EQ_U64(a & 0x2FFu, b & 0x2FFu); // byte and K flag
        }
        if (a != 0u || b != 0u) {
            distinct++;
        }
    }
    WCX_CHECK_EQ_U64(distinct, 464); // protocol-notes.md §9 cross-check
}

WCX_TEST(wrong_disparity_and_invalid_codes) {
    pcie_8b10b t8;
    pcie_8b10b_init(&t8);
    // D0.0 for RD- arriving at RD+.
    bool rd = true;
    uint8_t byte = 0xAA;
    bool k = true;
    WCX_CHECK_EQ_U64(pcie_8b10b_decode(&t8, bits("1001110100"), &rd, &byte, &k),
                     PCIE_DEC_DISPARITY);
    WCX_CHECK_EQ_U64(byte, 0x00);
    WCX_CHECK(!k);
    WCX_CHECK(!rd); // the RD after D0.0 sent at RD-
    // Not a code group: the state is left alone.
    rd = true;
    byte = 0xAA;
    k = false;
    WCX_CHECK_EQ_U64(pcie_8b10b_decode(&t8, bits("1111111111"), &rd, &byte, &k), PCIE_DEC_CODE);
    WCX_CHECK_EQ_U64(pcie_8b10b_decode(&t8, bits("0000011111"), &rd, &byte, &k), PCIE_DEC_CODE);
    WCX_CHECK(rd);
    WCX_CHECK_EQ_U64(byte, 0xAA);
    // Bits above the 10-bit group are ignored.
    rd = false;
    WCX_CHECK_EQ_U64(
        pcie_8b10b_decode(&t8, (uint16_t)(0xFC00u | PCIE_CODE_COM_NEG), &rd, &byte, &k),
        PCIE_DEC_OK);
    WCX_CHECK_EQ_U64(byte, 0xBC);
    WCX_CHECK(k);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, encoder_matches_every_known_answer);
    WCX_RUN(&t, com_constants_are_k28_5);
    WCX_RUN(&t, only_the_twelve_k_symbols_encode);
    WCX_RUN(&t, every_symbol_round_trips_at_both_disparities);
    WCX_RUN(&t, no_code_means_two_symbols);
    WCX_RUN(&t, wrong_disparity_and_invalid_codes);
    return wcx_test_finish(&t);
}
