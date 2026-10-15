// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// 8b/10b (protocol-notes.md §9), for the serial line decoders (SPEC.md §11).
//
// A code group is a 10-bit value in wire order with the first bit on the
// wire (a) in bit 9 and the last (j) in bit 0, so it reads like the tables:
// COM for negative running disparity, "0011111010", is 0x0FA.
//
// The decoder is a lookup table built once per instance from the encoder:
// a code is valid for a running disparity exactly when the encoder produces
// it for some symbol at that disparity (SPEC.md §11.7). Building it from the
// encoder means there is one transcription of the standard's tables, and
// the unit tests check that transcription against the known answers.

#ifndef PCIE_8B10B_H
#define PCIE_8B10B_H

#include <stdbool.h>
#include <stdint.h>

#define PCIE_CODE_COM_NEG 0x0FAu // K28.5 for negative RD: 0011111010
#define PCIE_CODE_COM_POS 0x305u // K28.5 for positive RD: 1100000101
#define PCIE_CODE_MASK    0x3FFu

// Encodes `byte` (a K symbol when `k`) at running disparity `rd_pos`
// (true = positive). False for a K byte that is not one of the twelve valid
// K symbols. On success *code is the group and *rd_after_pos the running
// disparity after it.
bool pcie_8b10b_encode(uint8_t byte, bool k, bool rd_pos, uint16_t *code, bool *rd_after_pos);

typedef struct pcie_8b10b {
    // [rd][code]: 0 = not a valid code group at that disparity; otherwise
    // PCIE_DEC_VALID | (K ? PCIE_DEC_K : 0) | (RD after positive ? PCIE_DEC_RD_POS : 0) | byte.
    uint16_t dec[2][1024];
} pcie_8b10b;

#define PCIE_DEC_VALID  0x8000u
#define PCIE_DEC_K      0x0200u
#define PCIE_DEC_RD_POS 0x0100u

void pcie_8b10b_init(pcie_8b10b *t);

typedef enum pcie_dec_status {
    PCIE_DEC_OK,        // valid at the current running disparity
    PCIE_DEC_DISPARITY, // valid only at the other one
    PCIE_DEC_CODE       // not a code group
} pcie_dec_status;

// Decodes `code` at running disparity *rd_pos and updates it (SPEC.md
// §11.7): OK and DISPARITY set *byte, *k and the disparity after the
// symbol; CODE leaves all three alone.
pcie_dec_status pcie_8b10b_decode(const pcie_8b10b *t, uint16_t code, bool *rd_pos, uint8_t *byte,
                                  bool *k);

#endif // PCIE_8B10B_H
