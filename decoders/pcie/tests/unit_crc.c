// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// CRC-16 and LCRC-32 against the known-answer vectors of
// docs/protocol-notes.md §6-§7 (RTL constants of openCologne-PCIE's DLL and
// the CRC catalogue check value), never against this code's own output.

#include <string.h>

#include "pcie_crc.h"
#include "wcx/test.h"

typedef struct dllp_vector {
    const char *name;
    uint8_t body[4];
    uint8_t wire[2];
} dllp_vector;

// protocol-notes.md §6, table "DLLP (wire bytes) | CRC on wire".
static const dllp_vector k_dllps[] = {
    {"InitFC1-P VC0 Hdr 32 Data 1008", {0x40, 0x08, 0x03, 0xF0}, {0x35, 0xBC}},
    {"InitFC1-NP Hdr 32 Data 1", {0x50, 0x08, 0x00, 0x01}, {0xB1, 0xF6}},
    {"InitFC1-Cpl 0/0", {0x60, 0x00, 0x00, 0x00}, {0xD8, 0x92}},
    {"InitFC2-P", {0xC0, 0x08, 0x03, 0xF0}, {0x4F, 0xC3}},
    {"InitFC2-NP", {0xD0, 0x08, 0x00, 0x01}, {0xCB, 0x89}},
    {"InitFC2-Cpl", {0xE0, 0x00, 0x00, 0x00}, {0xA2, 0xED}},
    {"Ack seq 0", {0x00, 0x00, 0x00, 0x00}, {0xB3, 0x62}},
    {"Ack seq 4095", {0x00, 0x00, 0x0F, 0xFF}, {0x25, 0xA8}},
    {"Nak seq 5", {0x10, 0x00, 0x00, 0x05}, {0x7D, 0x70}},
    {"UpdateFC-P Hdr 10 Data 200", {0x80, 0x02, 0x80, 0xC8}, {0xEC, 0xF8}},
};

WCX_TEST(crc16_matches_every_known_answer) {
    for (size_t i = 0; i < sizeof k_dllps / sizeof k_dllps[0]; i++) {
        const dllp_vector *v = &k_dllps[i];
        uint8_t wire[2] = {0, 0};
        pcie_crc16_wire(pcie_crc16(v->body, 4), wire);
        if (memcmp(wire, v->wire, 2) != 0) {
            WCX_FAIL("%s: got %02X %02X, want %02X %02X", v->name, wire[0], wire[1], v->wire[0],
                     v->wire[1]);
        }
        t->checks++;
    }
}

WCX_TEST(crc16_residue_over_body_and_crc_is_aa90) {
    // protocol-notes.md §6: "run the reflected CRC over all 6 bytes and
    // expect the constant residue 0xAA90".
    for (size_t i = 0; i < sizeof k_dllps / sizeof k_dllps[0]; i++) {
        uint8_t all[6];
        memcpy(all, k_dllps[i].body, 4);
        memcpy(all + 4, k_dllps[i].wire, 2);
        WCX_CHECK_EQ_U64(pcie_crc16(all, 6), PCIE_CRC16_RESIDUE);
        // One flipped bit anywhere breaks it.
        all[i % 6u] ^= 0x01u;
        WCX_CHECK(pcie_crc16(all, 6) != PCIE_CRC16_RESIDUE);
    }
}

WCX_TEST(crc16_is_byte_incremental) {
    // The one-shot value equals folding the bytes in one at a time from the
    // seed and applying xorout once.
    uint16_t reg = PCIE_CRC16_INIT;
    for (size_t i = 0; i < 4; i++) {
        reg = pcie_crc16_update(reg, k_dllps[0].body[i]);
    }
    WCX_CHECK_EQ_U64((uint16_t)(reg ^ 0xFFFFu), pcie_crc16(k_dllps[0].body, 4));
    // Wire order: low byte first (35 BC for value 0xBC35).
    WCX_CHECK_EQ_U64(pcie_crc16(k_dllps[0].body, 4), 0xBC35u);
    WCX_CHECK_EQ_U64(pcie_crc16(NULL, 0), 0x0000u); // init ^ xorout
}

WCX_TEST(crc32_check_value_and_wire_order) {
    // CRC catalogue check: CRC-32/ISO-HDLC("123456789") = 0xCBF43926, sent
    // least-significant byte first: 26 39 F4 CB (protocol-notes.md §7).
    const uint8_t check[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    WCX_CHECK_EQ_U64(pcie_crc32(check, 9), 0xCBF43926u);
    uint32_t reg = PCIE_CRC32_INIT;
    for (size_t i = 0; i < 9; i++) {
        reg = pcie_crc32_update(reg, check[i]);
    }
    uint8_t wire[4];
    pcie_crc32_wire(reg, wire);
    const uint8_t want[4] = {0x26, 0x39, 0xF4, 0xCB};
    WCX_CHECK_MEM_EQ(wire, want, 4);
}

// protocol-notes.md §7: seq 0 + MRd32 (1 DW, ReqID 0100, tag 0, addr
// 0x1000) -> LCRC 9A E8 F8 C2; nullified 65 17 07 3D.
static const uint8_t k_mrd32_body[14] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
                                         0x00, 0x00, 0x0F, 0x00, 0x00, 0x10, 0x00};
static const uint8_t k_mrd32_lcrc[4] = {0x9A, 0xE8, 0xF8, 0xC2};
static const uint8_t k_mrd32_nullified[4] = {0x65, 0x17, 0x07, 0x3D};

WCX_TEST(lcrc_of_mrd32_and_its_nullified_form) {
    uint32_t reg = PCIE_CRC32_INIT;
    for (size_t i = 0; i < sizeof k_mrd32_body; i++) {
        reg = pcie_crc32_update(reg, k_mrd32_body[i]);
    }
    uint8_t wire[4];
    pcie_crc32_wire(reg, wire);
    WCX_CHECK_MEM_EQ(wire, k_mrd32_lcrc, 4);
    for (unsigned i = 0; i < 4; i++) {
        WCX_CHECK_EQ_U64((uint8_t)~wire[i], k_mrd32_nullified[i]);
    }
}

WCX_TEST(lcrc_residue_over_body_and_lcrc_is_2144df1c) {
    uint8_t all[18];
    memcpy(all, k_mrd32_body, 14);
    memcpy(all + 14, k_mrd32_lcrc, 4);
    WCX_CHECK_EQ_U64(pcie_crc32(all, 18), PCIE_CRC32_RESIDUE);
    // The nullified form is not a good TLP's residue.
    memcpy(all + 14, k_mrd32_nullified, 4);
    WCX_CHECK(pcie_crc32(all, 18) != PCIE_CRC32_RESIDUE);
    // Nor is a single corrupted payload byte.
    memcpy(all + 14, k_mrd32_lcrc, 4);
    all[12] ^= 0x80u;
    WCX_CHECK(pcie_crc32(all, 18) != PCIE_CRC32_RESIDUE);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, crc16_matches_every_known_answer);
    WCX_RUN(&t, crc16_residue_over_body_and_crc_is_aa90);
    WCX_RUN(&t, crc16_is_byte_incremental);
    WCX_RUN(&t, crc32_check_value_and_wire_order);
    WCX_RUN(&t, lcrc_of_mrd32_and_its_nullified_form);
    WCX_RUN(&t, lcrc_residue_over_body_and_lcrc_is_2144df1c);
    return wcx_test_finish(&t);
}
