// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The Gen1/Gen2 data scrambler (PCIe Base 2.1 §4.2.1.3; protocol-notes.md
// §3): a 16-bit LFSR with G(x) = x^16 + x^5 + x^4 + x^3 + 1, seeded with
// 0xFFFF by every COM. One key byte per symbol: key bit 0 (the first bit on
// the wire) is the register MSB before the shift, so key = bitrev8(lfsr >> 8)
// and the register then advances eight steps.
//
// Who advances it and who is XORed with it is the pipeline's business
// (SPEC.md §5.2): every symbol except SKP advances; only D symbols outside
// TS1/TS2 bodies are XORed. This module just keeps the register.

#ifndef PCIE_SCRAMBLER_H
#define PCIE_SCRAMBLER_H

#include <stdint.h>

#define PCIE_LFSR_SEED 0xFFFFu

typedef struct pcie_scrambler {
    uint16_t lfsr;
} pcie_scrambler;

// Seeds the register (what COM does).
void pcie_scrambler_reset(pcie_scrambler *s);

// The key for the next symbol, without advancing.
uint8_t pcie_scrambler_key(const pcie_scrambler *s);

// Advances the register by one symbol (eight bit-steps) and returns the key
// that symbol used.
uint8_t pcie_scrambler_advance(pcie_scrambler *s);

#endif // PCIE_SCRAMBLER_H
