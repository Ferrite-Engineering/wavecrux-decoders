// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "pcie_crc.h"

// PCIe Base 2.1 §3.5.3.3 (DLLP CRC): G(x) = x^16 + x^12 + x^3 + x + 1,
// bits fed LSB first, which in reflected form is 0xD008.
#define CRC16_POLY_REFLECTED 0xD008u

// PCIe Base 2.1 §3.5.3.1 (LCRC): G(x) = 0x04C11DB7 fed LSB first, which
// in reflected form is 0xEDB88320 (IEEE 802.3).
#define CRC32_POLY_REFLECTED 0xEDB88320u

uint16_t pcie_crc16_update(uint16_t reg, uint8_t byte) {
    unsigned r = (unsigned)reg ^ byte;
    for (unsigned i = 0; i < 8u; i++) {
        r = (r & 1u) != 0 ? (r >> 1u) ^ CRC16_POLY_REFLECTED : r >> 1u;
    }
    return (uint16_t)(r & 0xFFFFu);
}

uint16_t pcie_crc16(const uint8_t *data, size_t n) {
    uint16_t reg = PCIE_CRC16_INIT;
    for (size_t i = 0; i < n; i++) {
        reg = pcie_crc16_update(reg, data[i]);
    }
    return (uint16_t)(reg ^ PCIE_CRC16_INIT); // xorout 0xFFFF
}

void pcie_crc16_wire(uint16_t crc, uint8_t out[2]) {
    out[0] = (uint8_t)(crc & 0xFFu);
    out[1] = (uint8_t)(crc >> 8u);
}

uint32_t pcie_crc32_update(uint32_t reg, uint8_t byte) {
    uint32_t r = reg ^ byte;
    for (unsigned i = 0; i < 8u; i++) {
        r = (r & 1u) != 0 ? (r >> 1u) ^ CRC32_POLY_REFLECTED : r >> 1u;
    }
    return r;
}

uint32_t pcie_crc32(const uint8_t *data, size_t n) {
    uint32_t reg = PCIE_CRC32_INIT;
    for (size_t i = 0; i < n; i++) {
        reg = pcie_crc32_update(reg, data[i]);
    }
    return reg ^ PCIE_CRC32_INIT; // xorout 0xFFFFFFFF
}

void pcie_crc32_wire(uint32_t reg, uint8_t out[4]) {
    const uint32_t crc = reg ^ PCIE_CRC32_INIT;
    out[0] = (uint8_t)(crc & 0xFFu);
    out[1] = (uint8_t)((crc >> 8u) & 0xFFu);
    out[2] = (uint8_t)((crc >> 16u) & 0xFFu);
    out[3] = (uint8_t)((crc >> 24u) & 0xFFu);
}
