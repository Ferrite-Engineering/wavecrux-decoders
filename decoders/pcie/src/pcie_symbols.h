// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The 8b/10b control symbols PCIe Gen1/Gen2 uses on the PIPE interface
// (PCIe Base 2.1 §4.2.1.1, Table 4-1; SPEC.md §5.1), and the one struct the
// whole decoder passes around: a symbol with its K flag and its time.
//
// A PIPE word carries N symbols per clock; the plugin splits it into
// pcie_sym values (byte lane j = the j-th symbol on the wire) and hands them
// one at a time to the pipeline. Times follow SPEC.md §4.2.

#ifndef PCIE_SYMBOLS_H
#define PCIE_SYMBOLS_H

#include <stdbool.h>
#include <stdint.h>

// K-code byte values (Kx.y = (y << 5) | x). The byte alone means nothing
// without the K flag: a D symbol 0xBC is ordinary data.
#define PCIE_K_COM 0xBCu // K28.5  starts every ordered set, resets the scrambler
#define PCIE_K_STP 0xFBu // K27.7  start of TLP
#define PCIE_K_SDP 0x5Cu // K28.2  start of DLLP
#define PCIE_K_END 0xFDu // K29.7  end of a good TLP or DLLP
#define PCIE_K_EDB 0xFEu // K30.7  end of a nullified TLP (or a PHY decode error)
#define PCIE_K_PAD 0xF7u // K23.7  "not assigned" in TS1/TS2
#define PCIE_K_SKP 0x1Cu // K28.0  SKP ordered set; never advances the scrambler
#define PCIE_K_FTS 0x3Cu // K28.1  FTS ordered set
#define PCIE_K_IDL 0x7Cu // K28.3  electrical idle ordered set
#define PCIE_K_EIE 0xFCu // K28.7  electrical idle exit ordered set (5 GT/s)

// Data symbols with a fixed meaning.
#define PCIE_D_TS1_ID 0x4Au // D10.2: TS1 identifier, and the last EIEOS symbol
#define PCIE_D_TS2_ID 0x45u // D5.2:  TS2 identifier
#define PCIE_D_IDLE   0x00u // D0.0:  logical idle (after descrambling)

typedef struct pcie_sym {
    uint8_t byte;
    bool k;            // datak bit for this lane: 1 = control symbol
    uint64_t start_fs; // SPEC.md §4.2: t_k + floor(j * P_k / N)
    uint64_t end_fs;   // start_fs + floor(P_k / N)
} pcie_sym;

// True when `byte` with K = 1 is one of the ten control symbols above
// (SPEC.md §5.1: anything else with K set is an invalid K symbol).
static inline bool pcie_k_is_known(uint8_t byte) {
    switch (byte) {
        case PCIE_K_COM:
        case PCIE_K_STP:
        case PCIE_K_SDP:
        case PCIE_K_END:
        case PCIE_K_EDB:
        case PCIE_K_PAD:
        case PCIE_K_SKP:
        case PCIE_K_FTS:
        case PCIE_K_IDL:
        case PCIE_K_EIE:
            return true;
        default:
            return false;
    }
}

#endif // PCIE_SYMBOLS_H
