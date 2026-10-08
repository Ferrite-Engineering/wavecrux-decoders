// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The two PCIe link CRCs (docs/protocol-notes.md §6-§7), in the reflected
// form a byte-oriented decoder wants, bitwise (no tables to get wrong).
//
//   DLLP CRC-16   polynomial 0x100B fed bit 0 first == reflected 0xD008,
//                 init 0xFFFF, xorout 0xFFFF; the LOW byte goes first on the
//                 wire. Over the 4 DLLP bytes plus the 2 received CRC bytes
//                 the result is the constant PCIE_CRC16_RESIDUE.
//   TLP LCRC-32   IEEE 802.3 CRC-32 (reflected 0xEDB88320, init and xorout
//                 0xFFFFFFFF) over the 2 sequence bytes and the TLP bytes,
//                 sent least-significant byte first. Over everything
//                 including the 4 received LCRC bytes the result is
//                 PCIE_CRC32_RESIDUE.
//
// The CRC-32 is exposed as a running register so the framer can fold bytes
// in as they arrive (it never stores a packet body): pcie_crc32_update()
// per byte from PCIE_CRC32_INIT, then pcie_crc32_wire() to get the 4 bytes
// the transmitter would have sent after exactly those bytes.

#ifndef PCIE_CRC_H
#define PCIE_CRC_H

#include <stddef.h>
#include <stdint.h>

#define PCIE_CRC16_INIT    0xFFFFu
#define PCIE_CRC16_RESIDUE 0xAA90u

#define PCIE_CRC32_INIT    0xFFFFFFFFu
#define PCIE_CRC32_RESIDUE 0x2144DF1Cu

// Reflected CRC-16 register after `byte` (no xorout applied).
uint16_t pcie_crc16_update(uint16_t reg, uint8_t byte);

// CRC-16 of data[0..n) with init and xorout applied: the value whose low
// byte is the first CRC byte on the wire.
uint16_t pcie_crc16(const uint8_t *data, size_t n);

// The two wire bytes of a CRC-16 value: out[0] = low byte, out[1] = high.
void pcie_crc16_wire(uint16_t crc, uint8_t out[2]);

// Reflected CRC-32 register after `byte` (no xorout applied).
uint32_t pcie_crc32_update(uint32_t reg, uint8_t byte);

// CRC-32 of data[0..n) with init and xorout applied.
uint32_t pcie_crc32(const uint8_t *data, size_t n);

// The four LCRC wire bytes for a register state (applies xorout; LSB first).
void pcie_crc32_wire(uint32_t reg, uint8_t out[4]);

#endif // PCIE_CRC_H
