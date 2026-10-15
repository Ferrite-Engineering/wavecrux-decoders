// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The tables are protocol-notes.md §9 (IEEE 802.3 Clause 36, Tables 36-1a
// to 36-2), written with the first wire bit as the most significant bit of
// each sub-block: abcdei for 5b/6b, fghj for 3b/4b.

#include "pcie_8b10b.h"

#include <string.h>

// 5b/6b, indexed by x = EDCBA: the form for negative and for positive
// running disparity (equal for the balanced codes).
static const uint8_t k_6b_neg[32] = {
    0x27u, 0x1Du, 0x2Du, 0x31u, 0x35u, 0x29u, 0x19u, 0x38u, // 100111 011101 101101 110001 110101 101001 011001 111000
    0x39u, 0x25u, 0x15u, 0x34u, 0x0Du, 0x2Cu, 0x1Cu, 0x17u, // 111001 100101 010101 110100 001101 101100 011100 010111
    0x1Bu, 0x23u, 0x13u, 0x32u, 0x0Bu, 0x2Au, 0x1Au, 0x3Au, // 011011 100011 010011 110010 001011 101010 011010 111010
    0x33u, 0x26u, 0x16u, 0x36u, 0x0Eu, 0x2Eu, 0x1Eu, 0x2Bu, // 110011 100110 010110 110110 001110 101110 011110 101011
};
static const uint8_t k_6b_pos[32] = {
    0x18u, 0x22u, 0x12u, 0x31u, 0x0Au, 0x29u, 0x19u, 0x07u, // 011000 100010 010010 110001 001010 101001 011001 000111
    0x06u, 0x25u, 0x15u, 0x34u, 0x0Du, 0x2Cu, 0x1Cu, 0x28u, // 000110 100101 010101 110100 001101 101100 011100 101000
    0x24u, 0x23u, 0x13u, 0x32u, 0x0Bu, 0x2Au, 0x1Au, 0x05u, // 100100 100011 010011 110010 001011 101010 011010 000101
    0x0Cu, 0x26u, 0x16u, 0x09u, 0x0Eu, 0x11u, 0x21u, 0x14u, // 001100 100110 010110 001001 001110 010001 100001 010100
};
#define K28_6B_NEG 0x0Fu // 001111
#define K28_6B_POS 0x30u // 110000

// 3b/4b, indexed by y = HGF, chosen by the running disparity after the
// 6-bit sub-block. D.x.7 has a primary and an alternate form.
static const uint8_t k_4b_d_neg[8] = {0xBu, 0x9u, 0x5u, 0xCu, 0xDu, 0xAu, 0x6u, 0xEu};
static const uint8_t k_4b_d_pos[8] = {0x4u, 0x9u, 0x5u, 0x3u, 0x2u, 0xAu, 0x6u, 0x1u};
static const uint8_t k_4b_k_neg[8] = {0xBu, 0x6u, 0xAu, 0xCu, 0xDu, 0x5u, 0x9u, 0x7u};
static const uint8_t k_4b_k_pos[8] = {0x4u, 0x9u, 0x5u, 0x3u, 0x2u, 0xAu, 0x6u, 0x8u};
#define A7_NEG 0x7u // 0111
#define A7_POS 0x8u // 1000

static unsigned ones(unsigned v) {
    unsigned n = 0;
    while (v != 0u) {
        n += v & 1u;
        v >>= 1u;
    }
    return n;
}

// A sub-block of `width` bits flips the running disparity unless balanced.
static bool rd_after(bool rd_pos, unsigned bits, unsigned width) {
    return 2u * ones(bits) == width ? rd_pos : !rd_pos;
}

// The twelve K symbols 8b/10b defines: K28.0-K28.7, K23.7, K27.7, K29.7, K30.7.
static bool k_valid(uint8_t byte) {
    const unsigned x = byte & 0x1Fu;
    const unsigned y = (unsigned)byte >> 5u;
    return x == 28u || (y == 7u && (x == 23u || x == 27u || x == 29u || x == 30u));
}

bool pcie_8b10b_encode(uint8_t byte, bool k, bool rd_pos, uint16_t *code, bool *rd_after_pos) {
    if (k && !k_valid(byte)) {
        return false;
    }
    const unsigned x = byte & 0x1Fu;
    const unsigned y = (unsigned)byte >> 5u;
    unsigned six = 0;
    if (k && x == 28u) {
        six = rd_pos ? K28_6B_POS : K28_6B_NEG;
    } else {
        six = rd_pos ? k_6b_pos[x] : k_6b_neg[x];
    }
    const bool rd_mid = rd_after(rd_pos, six, 6u);
    unsigned four = 0;
    if (k) {
        four = rd_mid ? k_4b_k_pos[y] : k_4b_k_neg[y];
    } else if (y == 7u && ((!rd_mid && (x == 17u || x == 18u || x == 20u)) ||
                           (rd_mid && (x == 11u || x == 13u || x == 14u)))) {
        four = rd_mid ? A7_POS : A7_NEG; // keeps five equal bits out of data
    } else {
        four = rd_mid ? k_4b_d_pos[y] : k_4b_d_neg[y];
    }
    *code = (uint16_t)((six << 4u) | four);
    *rd_after_pos = rd_after(rd_mid, four, 4u);
    return true;
}

void pcie_8b10b_init(pcie_8b10b *t) {
    memset(t, 0, sizeof *t);
    for (unsigned rd = 0; rd < 2u; rd++) {
        for (unsigned v = 0; v < 512u; v++) {
            const bool k = v >= 256u;
            const uint8_t byte = (uint8_t)(v & 0xFFu);
            uint16_t code = 0;
            bool after = false;
            if (pcie_8b10b_encode(byte, k, rd != 0u, &code, &after)) {
                t->dec[rd][code] = (uint16_t)(PCIE_DEC_VALID | (k ? PCIE_DEC_K : 0u) |
                                              (after ? PCIE_DEC_RD_POS : 0u) | byte);
            }
        }
    }
}

pcie_dec_status pcie_8b10b_decode(const pcie_8b10b *t, uint16_t code, bool *rd_pos, uint8_t *byte,
                                  bool *k) {
    const unsigned c = code & PCIE_CODE_MASK;
    pcie_dec_status status = PCIE_DEC_OK;
    uint16_t e = t->dec[*rd_pos ? 1 : 0][c];
    if (e == 0u) {
        e = t->dec[*rd_pos ? 0 : 1][c];
        if (e == 0u) {
            return PCIE_DEC_CODE;
        }
        status = PCIE_DEC_DISPARITY;
    }
    *byte = (uint8_t)(e & 0xFFu);
    *k = (e & PCIE_DEC_K) != 0u;
    *rd_pos = (e & PCIE_DEC_RD_POS) != 0u;
    return status;
}
