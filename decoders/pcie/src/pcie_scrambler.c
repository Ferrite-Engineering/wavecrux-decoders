// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "pcie_scrambler.h"

// Galois taps of x^16 + x^5 + x^4 + x^3 + 1 on a left-shifting register:
// bits 5, 4, 3 and 0.
#define LFSR_TAPS 0x0039u

void pcie_scrambler_reset(pcie_scrambler *s) {
    s->lfsr = PCIE_LFSR_SEED;
}

uint8_t pcie_scrambler_key(const pcie_scrambler *s) {
    // bitrev8 of the top byte: register bit 15 is key bit 0.
    unsigned key = 0;
    unsigned top = (unsigned)s->lfsr >> 8u;
    for (unsigned i = 0; i < 8u; i++) {
        key = (key << 1u) | (top & 1u);
        top >>= 1u;
    }
    return (uint8_t)key;
}

uint8_t pcie_scrambler_advance(pcie_scrambler *s) {
    const uint8_t key = pcie_scrambler_key(s);
    unsigned lfsr = s->lfsr;
    for (unsigned i = 0; i < 8u; i++) {
        const unsigned fb = (lfsr >> 15u) & 1u;
        lfsr = (lfsr << 1u) & 0xFFFFu;
        if (fb != 0) {
            lfsr ^= LFSR_TAPS;
        }
    }
    s->lfsr = (uint16_t)lfsr;
    return key;
}
