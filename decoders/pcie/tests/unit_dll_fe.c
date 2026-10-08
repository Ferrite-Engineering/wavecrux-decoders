// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The Data Link Layer front end (SPEC.md §8, §10) through a real pipeline
// into a buffered sink. Every CRC and LCRC below comes from
// docs/protocol-notes.md §6-§7 or from the reflected-form rules stated
// there (CRC-16: poly 0xD008, init/xorout 0xFFFF, low byte first; LCRC:
// IEEE CRC-32 over the two sequence bytes and the TLP, LSB first), worked
// out independently of pcie_crc.c and quoted in the comments.

#include <string.h>

#include "pcie_dll_fe.h"
#include "pcie_test_util.h"

#define T(i)   (1000u + 10u * (i))
#define END(i) (1010u + 10u * (i))

typedef struct rig {
    test_sink s;
    pcie_dll_fe fe;
    pcie_pipeline p;
} rig;

static bool rig_init(rig *r) {
    if (!test_sink_init(&r->s, 64)) {
        return false;
    }
    pcie_dll_fe_init(&r->fe, &r->s.sink);
    pcie_pipeline_init(&r->p, false, pcie_dll_fe_event, &r->fe);
    return true;
}

static void rig_free(rig *r) {
    test_sink_free(&r->s);
}

#define TX(r, i) ((r).s.buf.items[(i)])

// SDP b0 b1 b2 b3 c0 c1 END
static void feed_dllp(rig *r, const uint8_t b[6], uint64_t t0) {
    tsym syms[8] = {KS(0x5C)};
    for (unsigned i = 0; i < 6u; i++) {
        syms[1 + i] = (tsym)DS(b[i]);
    }
    syms[7] = (tsym)KS(0xFD);
    feed_syms(&r->p, syms, 8, t0, 10);
}

// STP seq TLP LCRC END/EDB
static void feed_tlp(rig *r, uint32_t seq, const uint8_t *tlp, size_t n, const uint8_t lcrc[4],
                     bool edb, uint64_t t0) {
    tsym syms[64];
    size_t k = 0;
    syms[k++] = (tsym)KS(0xFB);
    syms[k++] = (tsym)DS((seq >> 8u) & 0x0Fu);
    syms[k++] = (tsym)DS(seq & 0xFFu);
    for (size_t i = 0; i < n; i++) {
        syms[k++] = (tsym)DS(tlp[i]);
    }
    for (unsigned i = 0; i < 4u; i++) {
        syms[k++] = (tsym)DS(lcrc[i]);
    }
    syms[k++] = edb ? (tsym)KS(0xFE) : (tsym)KS(0xFD);
    feed_syms(&r->p, syms, k, t0, 10);
}

typedef struct dllp_case {
    uint8_t bytes[6];
    const char *label;
    const char *fields;
} dllp_case;

// protocol-notes.md §6 vectors (first ten) and four more from the same
// CRC-16 rule: PM_Enter_L1 20 00 00 00 -> 65 AD, PM_Enter_L23 21 -> 10 55,
// PM_Active_State_Request_L1 23 -> EB 05, PM_Request_Ack 24 -> 93 0C,
// Vendor 30 12 34 56 -> 60 21, UpdateFC-NP 91 FF FF FF -> 0B 48,
// UpdateFC-Cpl A7 00 41 00 -> 3B 4A, InitFC1-P 43 00 00 00 -> 80 F5.
static const dllp_case k_cases[] = {
    {{0x00, 0x00, 0x00, 0x00, 0xB3, 0x62},
     "Ack seq=0",
     "{\"type\":\"ack\",\"seq\":0,\"crc\":\"0xB362\"}"},
    {{0x00, 0x00, 0x0F, 0xFF, 0x25, 0xA8},
     "Ack seq=4095",
     "{\"type\":\"ack\",\"seq\":4095,\"crc\":\"0x25A8\"}"},
    {{0x10, 0x00, 0x00, 0x05, 0x7D, 0x70},
     "Nak seq=5",
     "{\"type\":\"nak\",\"seq\":5,\"crc\":\"0x7D70\"}"},
    {{0x20, 0x00, 0x00, 0x00, 0x65, 0xAD},
     "PM_Enter_L1",
     "{\"type\":\"pm_enter_l1\",\"crc\":\"0x65AD\"}"},
    {{0x21, 0x00, 0x00, 0x00, 0x10, 0x55},
     "PM_Enter_L23",
     "{\"type\":\"pm_enter_l23\",\"crc\":\"0x1055\"}"},
    {{0x23, 0x00, 0x00, 0x00, 0xEB, 0x05},
     "PM_Active_State_Request_L1",
     "{\"type\":\"pm_as_request_l1\",\"crc\":\"0xEB05\"}"},
    {{0x24, 0x00, 0x00, 0x00, 0x93, 0x0C},
     "PM_Request_Ack",
     "{\"type\":\"pm_request_ack\",\"crc\":\"0x930C\"}"},
    {{0x30, 0x12, 0x34, 0x56, 0x60, 0x21},
     "Vendor 0x123456",
     "{\"type\":\"vendor\",\"payload\":\"0x123456\",\"crc\":\"0x6021\"}"},
    {{0x40, 0x08, 0x03, 0xF0, 0x35, 0xBC},
     "InitFC1-P VC0 H=32 D=1008",
     "{\"type\":\"initfc1_p\",\"vc\":0,\"hdr_fc\":32,\"data_fc\":1008,\"crc\":\"0x35BC\"}"},
    {{0x50, 0x08, 0x00, 0x01, 0xB1, 0xF6},
     "InitFC1-NP VC0 H=32 D=1",
     "{\"type\":\"initfc1_np\",\"vc\":0,\"hdr_fc\":32,\"data_fc\":1,\"crc\":\"0xB1F6\"}"},
    {{0x60, 0x00, 0x00, 0x00, 0xD8, 0x92},
     "InitFC1-Cpl VC0 H=0 D=0",
     "{\"type\":\"initfc1_cpl\",\"vc\":0,\"hdr_fc\":0,\"data_fc\":0,\"crc\":\"0xD892\"}"},
    {{0xC0, 0x08, 0x03, 0xF0, 0x4F, 0xC3},
     "InitFC2-P VC0 H=32 D=1008",
     "{\"type\":\"initfc2_p\",\"vc\":0,\"hdr_fc\":32,\"data_fc\":1008,\"crc\":\"0x4FC3\"}"},
    {{0xD0, 0x08, 0x00, 0x01, 0xCB, 0x89},
     "InitFC2-NP VC0 H=32 D=1",
     "{\"type\":\"initfc2_np\",\"vc\":0,\"hdr_fc\":32,\"data_fc\":1,\"crc\":\"0xCB89\"}"},
    {{0xE0, 0x00, 0x00, 0x00, 0xA2, 0xED},
     "InitFC2-Cpl VC0 H=0 D=0",
     "{\"type\":\"initfc2_cpl\",\"vc\":0,\"hdr_fc\":0,\"data_fc\":0,\"crc\":\"0xA2ED\"}"},
    {{0x80, 0x02, 0x80, 0xC8, 0xEC, 0xF8},
     "UpdateFC-P VC0 H=10 D=200",
     "{\"type\":\"updatefc_p\",\"vc\":0,\"hdr_fc\":10,\"data_fc\":200,\"crc\":\"0xECF8\"}"},
    // b1 = FF: HdrFC[7:2] = 111111; b2 = FF: HdrFC[1:0] = 11 -> 255, DataFC[11:8] = F; b3 = FF.
    {{0x91, 0xFF, 0xFF, 0xFF, 0x0B, 0x48},
     "UpdateFC-NP VC1 H=255 D=4095",
     "{\"type\":\"updatefc_np\",\"vc\":1,\"hdr_fc\":255,\"data_fc\":4095,\"crc\":\"0x0B48\"}"},
    // b2 = 0x41: HdrFC[1:0] = 01 -> 1, DataFC[11:8] = 1 -> 256.
    {{0xA7, 0x00, 0x41, 0x00, 0x3B, 0x4A},
     "UpdateFC-Cpl VC7 H=1 D=256",
     "{\"type\":\"updatefc_cpl\",\"vc\":7,\"hdr_fc\":1,\"data_fc\":256,\"crc\":\"0x3B4A\"}"},
    {{0x43, 0x00, 0x00, 0x00, 0x80, 0xF5},
     "InitFC1-P VC3 H=0 D=0",
     "{\"type\":\"initfc1_p\",\"vc\":3,\"hdr_fc\":0,\"data_fc\":0,\"crc\":\"0x80F5\"}"},
};

WCX_TEST(every_dllp_type) {
    rig r;
    WCX_REQUIRE(rig_init(&r));
    for (size_t i = 0; i < sizeof k_cases / sizeof k_cases[0]; i++) {
        feed_dllp(&r, k_cases[i].bytes, 1000 + 100 * i);
        WCX_REQUIRE(r.s.buf.count == i + 1);
        WCX_CHECK_STR_EQ(TX(r, i).label, k_cases[i].label);
        WCX_CHECK_STR_EQ(TX(r, i).fields, k_cases[i].fields);
        WCX_CHECK(!TX(r, i).is_error);
        WCX_CHECK_EQ_U64(TX(r, i).start_fs, 1000 + 100 * i);
        WCX_CHECK_EQ_U64(TX(r, i).end_fs, 1000 + 100 * i + 80); // SDP..END, 8 symbols
    }
    rig_free(&r);
}

WCX_TEST(dllp_errors) {
    rig r;
    WCX_REQUIRE(rig_init(&r));
    // Reserved type 0x01 with a correct CRC (01 00 00 00 -> C6 9A).
    static const uint8_t reserved[6] = {0x01, 0x00, 0x00, 0x00, 0xC6, 0x9A};
    feed_dllp(&r, reserved, 1000);
    // Reserved 0x48 (not an FC code: the low three bits are the VC only
    // for 0x40-0x47) with CRC F3 BE.
    static const uint8_t reserved48[6] = {0x48, 0x00, 0x00, 0x00, 0xF3, 0xBE};
    feed_dllp(&r, reserved48, 1100);
    // InitFC1-P with its last CRC byte off by one.
    static const uint8_t bad_crc[6] = {0x40, 0x08, 0x03, 0xF0, 0x35, 0xBD};
    feed_dllp(&r, bad_crc, 1200);
    // Reserved type and a bad CRC: both sentences, reserved first (§10.7).
    static const uint8_t both[6] = {0x01, 0x00, 0x00, 0x00, 0xC6, 0x9B};
    feed_dllp(&r, both, 1300);
    // Five body bytes.
    static const tsym five[] = {KS(0x5C), DS(0x40), DS(0x08), DS(0x03),
                                DS(0xF0), DS(0x35), KS(0xFD)};
    feed_syms(&r.p, five, 7, 1400, 10);
    // Ended by EDB (§10.2): not decoded, even with a good CRC.
    static const tsym edb[] = {KS(0x5C), DS(0x40), DS(0x08), DS(0x03),
                               DS(0xF0), DS(0x35), DS(0xBC), KS(0xFE)};
    feed_syms(&r.p, edb, 8, 1500, 10);
    WCX_REQUIRE(r.s.buf.count == 6);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "Reserved DLLP 0x01");
    WCX_CHECK_STR_EQ(
        TX(r, 0).fields,
        "{\"type\":\"reserved\",\"byte0\":\"0x01\",\"crc\":\"0xC69A\",\"error\":\"This "
        "DLLP type is reserved in PCIe Gen1 and Gen2.\"}");
    WCX_CHECK(TX(r, 0).is_error);
    WCX_CHECK_STR_EQ(TX(r, 1).label, "Reserved DLLP 0x48");
    WCX_CHECK_STR_EQ(TX(r, 2).label, "InitFC1-P VC0 H=32 D=1008 CRC mismatch");
    WCX_CHECK_STR_EQ(TX(r, 2).fields,
                     "{\"type\":\"initfc1_p\",\"vc\":0,\"hdr_fc\":32,\"data_fc\":1008,"
                     "\"crc\":\"0x35BD\",\"expected_crc\":\"0x35BC\",\"error\":\"DLLP CRC-16 does "
                     "not match its contents.\"}");
    WCX_CHECK(TX(r, 2).is_error);
    WCX_CHECK_STR_EQ(TX(r, 3).label, "Reserved DLLP 0x01 CRC mismatch");
    WCX_CHECK_STR_EQ(TX(r, 3).fields,
                     "{\"type\":\"reserved\",\"byte0\":\"0x01\",\"crc\":\"0xC69B\","
                     "\"expected_crc\":\"0xC69A\",\"error\":\"This DLLP type is reserved in PCIe "
                     "Gen1 and Gen2. DLLP CRC-16 does not match its contents.\"}");
    WCX_CHECK_STR_EQ(TX(r, 4).label, "DLLP length 5, expected 6");
    WCX_CHECK_STR_EQ(TX(r, 4).fields, "{\"type\":\"dllp_length\",\"symbols\":5,\"error\":\"A DLLP "
                                      "is 6 symbols between SDP and END.\"}");
    WCX_CHECK_EQ_U64(TX(r, 4).start_fs, 1400);
    WCX_CHECK_EQ_U64(TX(r, 4).end_fs, 1470);
    WCX_CHECK_STR_EQ(TX(r, 5).label, "DLLP ended by EDB");
    WCX_CHECK_STR_EQ(TX(r, 5).fields, "{\"type\":\"dllp_edb\",\"symbols\":6,\"error\":\"A DLLP "
                                      "cannot be ended by EDB.\"}");
    WCX_CHECK(TX(r, 5).is_error);
    rig_free(&r);
}

// protocol-notes.md §7: MRd32, 1 DW, ReqID 0100, tag 0, addr 0x1000.
static const uint8_t k_mrd32[12] = {0x00, 0x00, 0x00, 0x01, 0x01, 0x00,
                                    0x00, 0x0F, 0x00, 0x00, 0x10, 0x00};
// LCRCs of seq + k_mrd32 (seq 0 from the notes; the others from the same
// CRC-32 rule): 0 -> 9A E8 F8 C2, 1 -> 1F 31 6E 1F, 2 -> D1 5D A4 A2,
// 3 -> 54 84 32 7F, 4 -> 0C 82 41 02, 5 -> 89 5B D7 DF, 952 -> 14 D6 89 43,
// 953 -> 91 0F 1F 9E, 3000 -> 22 C8 D0 70, 4095 -> CA D0 82 A9.
static const uint8_t k_lcrc0[4] = {0x9A, 0xE8, 0xF8, 0xC2};
static const uint8_t k_lcrc1[4] = {0x1F, 0x31, 0x6E, 0x1F};
static const uint8_t k_lcrc2[4] = {0xD1, 0x5D, 0xA4, 0xA2};
static const uint8_t k_lcrc3[4] = {0x54, 0x84, 0x32, 0x7F};
static const uint8_t k_lcrc4[4] = {0x0C, 0x82, 0x41, 0x02};
static const uint8_t k_lcrc952[4] = {0x14, 0xD6, 0x89, 0x43};
static const uint8_t k_lcrc953[4] = {0x91, 0x0F, 0x1F, 0x9E};
static const uint8_t k_lcrc3000[4] = {0x22, 0xC8, 0xD0, 0x70};
static const uint8_t k_lcrc4095[4] = {0xCA, 0xD0, 0x82, 0xA9};

#define MRD32_FIELDS(seq, lcrc)                                                                  \
    "{\"type\":\"tlp\",\"seq\":" seq ",\"tlp\":\"MRd32\",\"length_dw\":1,\"tc\":0,\"td\":false," \
    "\"ep\":false,\"bytes\":12,\"lcrc\":\"" lcrc "\",\"replay\":false}"

WCX_TEST(mrd32_good_and_its_edb_forms) {
    rig r;
    WCX_REQUIRE(rig_init(&r));
    feed_tlp(&r, 0, k_mrd32, 12, k_lcrc0, false, 1000);
    // Nullified: LCRC inverted (65 17 07 3D) + EDB.
    static const uint8_t inv[4] = {0x65, 0x17, 0x07, 0x3D};
    feed_tlp(&r, 0, k_mrd32, 12, inv, true, 1200);
    // EDB with the correct (not inverted) LCRC.
    feed_tlp(&r, 0, k_mrd32, 12, k_lcrc0, true, 1400);
    // END with a wrong LCRC.
    static const uint8_t wrong[4] = {0x9A, 0xE8, 0xF8, 0xC3};
    feed_tlp(&r, 0, k_mrd32, 12, wrong, false, 1600);
    WCX_REQUIRE(r.s.buf.count == 4);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "TLP seq=0 MRd32 len=1");
    WCX_CHECK_STR_EQ(TX(r, 0).fields, MRD32_FIELDS("0", "0x9AE8F8C2"));
    WCX_CHECK(!TX(r, 0).is_error);
    WCX_CHECK_EQ_U64(TX(r, 0).start_fs, 1000);
    WCX_CHECK_EQ_U64(TX(r, 0).end_fs, 1000 + 200); // 20 symbols
    WCX_CHECK_STR_EQ(TX(r, 1).label, "Nullified TLP seq=0 MRd32");
    WCX_CHECK_STR_EQ(TX(r, 1).fields,
                     "{\"type\":\"tlp_nullified\",\"seq\":0,\"tlp\":\"MRd32\",\"length_dw\":1,"
                     "\"tc\":0,\"td\":false,\"ep\":false,\"bytes\":12,\"lcrc\":\"0x6517073D\"}");
    WCX_CHECK(!TX(r, 1).is_error);
    WCX_CHECK_STR_EQ(TX(r, 2).label, "TLP ended by EDB seq=0");
    WCX_CHECK_STR_EQ(TX(r, 2).fields,
                     "{\"type\":\"tlp_edb\",\"seq\":0,\"bytes\":12,\"error\":\"A TLP ended by EDB "
                     "must carry the inverted LCRC of a nullified TLP.\"}");
    WCX_CHECK(TX(r, 2).is_error);
    WCX_CHECK_STR_EQ(TX(r, 3).label, "TLP seq=0 MRd32 len=1 LCRC mismatch");
    WCX_CHECK_STR_EQ(TX(r, 3).fields,
                     "{\"type\":\"tlp\",\"seq\":0,\"tlp\":\"MRd32\",\"length_dw\":1,\"tc\":0,"
                     "\"td\":false,\"ep\":false,\"bytes\":12,\"lcrc\":\"0x9AE8F8C3\","
                     "\"replay\":false,\"expected_lcrc\":\"0x9AE8F8C2\",\"error\":\"TLP LCRC does "
                     "not match its contents.\"}");
    WCX_CHECK(TX(r, 3).is_error);
    rig_free(&r);
}

WCX_TEST(tlp_length_errors) {
    rig r;
    WCX_REQUIRE(rig_init(&r));
    // 17 body symbols: below the minimum.
    tsym body17[19] = {KS(0xFB)};
    for (unsigned i = 1; i <= 17u; i++) {
        body17[i] = (tsym)DS(0x00);
    }
    body17[18] = (tsym)KS(0xFD);
    feed_syms(&r.p, body17, 19, 1000, 10);
    // 19 body symbols: 19 - 6 = 13 is not a multiple of 4.
    tsym body19[21] = {KS(0xFB)};
    for (unsigned i = 1; i <= 19u; i++) {
        body19[i] = (tsym)DS(0x00);
    }
    body19[20] = (tsym)KS(0xFD);
    feed_syms(&r.p, body19, 21, 1200, 10);
    WCX_REQUIRE(r.s.buf.count == 2);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "TLP length 17 symbols is not valid");
    WCX_CHECK_STR_EQ(TX(r, 0).fields,
                     "{\"type\":\"tlp_length\",\"symbols\":17,\"error\":\"A TLP body is at least "
                     "18 symbols, and a multiple of 4 between the sequence number and the "
                     "LCRC.\"}");
    WCX_CHECK(TX(r, 0).is_error);
    WCX_CHECK_EQ_U64(TX(r, 0).end_fs, 1000 + 190);
    WCX_CHECK_STR_EQ(TX(r, 1).label, "TLP length 19 symbols is not valid");
    rig_free(&r);
}

WCX_TEST(header_decode_and_length_check) {
    rig r;
    WCX_REQUIRE(rig_init(&r));
    // MWr32 seq 5, 1 DW of data: 40 00 00 01 | 01 00 00 0F | 00 00 10 00 | DE AD BE EF,
    // LCRC DD F1 4F 7A. 3-DW header + 4 = 16 bytes.
    static const uint8_t mwr[16] = {0x40, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x0F,
                                    0x00, 0x00, 0x10, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
    static const uint8_t mwr_lcrc[4] = {0xDD, 0xF1, 0x4F, 0x7A};
    feed_tlp(&r, 5, mwr, 16, mwr_lcrc, false, 1000);
    // MRd64 seq 6: 4-DW header, no data (E2 62 38 39).
    static const uint8_t mrd64[16] = {0x20, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x0F,
                                      0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x10, 0x00};
    static const uint8_t mrd64_lcrc[4] = {0xE2, 0x62, 0x38, 0x39};
    feed_tlp(&r, 6, mrd64, 16, mrd64_lcrc, false, 1300);
    // CplD seq 7: byte1 0x30 -> TC 3; byte2 0x40 -> EP; len 1 (92 BC C8 60).
    static const uint8_t cpld[16] = {0x4A, 0x30, 0x40, 0x01, 0x01, 0x00, 0x00, 0x04,
                                     0x00, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78};
    static const uint8_t cpld_lcrc[4] = {0x92, 0xBC, 0xC8, 0x60};
    feed_tlp(&r, 7, cpld, 16, cpld_lcrc, false, 1600);
    // MRd32 seq 8 with TD set but no ECRC present: 12 bytes, 16 expected
    // (F6 32 FE 7E).
    static const uint8_t td[12] = {0x00, 0x00, 0x80, 0x01, 0x01, 0x00,
                                   0x00, 0x0F, 0x00, 0x00, 0x10, 0x00};
    static const uint8_t td_lcrc[4] = {0xF6, 0x32, 0xFE, 0x7E};
    feed_tlp(&r, 8, td, 12, td_lcrc, false, 1900);
    // Msg seq 9 (byte0 0x34: Fmt 001, Type 10100), Length field 0 -> 1024,
    // 4-DW header, no data (9B 0E 21 B8).
    static const uint8_t msg[16] = {0x34, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t msg_lcrc[4] = {0x9B, 0x0E, 0x21, 0xB8};
    feed_tlp(&r, 9, msg, 16, msg_lcrc, false, 2200);
    // Unlisted type: byte0 0xE0 (Fmt 111, Type 0): 4-DW header + 1 DW data
    // = 20 bytes (22 1C BC DE).
    static const uint8_t unk[20] = {0xE0, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD};
    static const uint8_t unk_lcrc[4] = {0x22, 0x1C, 0xBC, 0xDE};
    feed_tlp(&r, 10, unk, 20, unk_lcrc, false, 2500);
    // MWr32 seq 11 with Length 0 (= 1024 DW) but 1 DW of data (CE D0 FD E6).
    static const uint8_t len0[16] = {0x40, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x0F,
                                     0x00, 0x00, 0x10, 0x00, 0xDE, 0xAD, 0xBE, 0xEF};
    static const uint8_t len0_lcrc[4] = {0xCE, 0xD0, 0xFD, 0xE6};
    feed_tlp(&r, 11, len0, 16, len0_lcrc, false, 2800);
    WCX_REQUIRE(r.s.buf.count == 7);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "TLP seq=5 MWr32 len=1");
    WCX_CHECK_STR_EQ(TX(r, 0).fields,
                     "{\"type\":\"tlp\",\"seq\":5,\"tlp\":\"MWr32\",\"length_dw\":1,\"tc\":0,"
                     "\"td\":false,\"ep\":false,\"bytes\":16,\"lcrc\":\"0xDDF14F7A\","
                     "\"replay\":false}");
    WCX_CHECK_STR_EQ(TX(r, 1).label, "TLP seq=6 MRd64 len=1");
    WCX_CHECK(strstr(TX(r, 1).fields, "\"tlp\":\"MRd64\",\"length_dw\":1,\"tc\":0,\"td\":false,"
                                      "\"ep\":false,\"bytes\":16,") != NULL);
    WCX_CHECK(!TX(r, 1).is_error);
    WCX_CHECK_STR_EQ(TX(r, 2).label, "TLP seq=7 CplD len=1");
    WCX_CHECK(strstr(TX(r, 2).fields, "\"tlp\":\"CplD\",\"length_dw\":1,\"tc\":3,\"td\":false,"
                                      "\"ep\":true,\"bytes\":16,") != NULL);
    WCX_CHECK(!TX(r, 2).is_error);
    WCX_CHECK_STR_EQ(TX(r, 3).label, "TLP seq=8 MRd32 len=1 length mismatch");
    WCX_CHECK_STR_EQ(TX(r, 3).fields,
                     "{\"type\":\"tlp\",\"seq\":8,\"tlp\":\"MRd32\",\"length_dw\":1,\"tc\":0,"
                     "\"td\":true,\"ep\":false,\"bytes\":12,\"lcrc\":\"0xF632FE7E\","
                     "\"replay\":false,\"error\":\"The header Length field does not match the "
                     "framed payload.\"}");
    WCX_CHECK(TX(r, 3).is_error);
    WCX_CHECK_STR_EQ(TX(r, 4).label, "TLP seq=9 Msg len=1024");
    WCX_CHECK(!TX(r, 4).is_error);
    WCX_CHECK_STR_EQ(TX(r, 5).label, "TLP seq=10 Type 0xE0 len=1");
    WCX_CHECK(strstr(TX(r, 5).fields, "\"tlp\":\"Type 0xE0\",\"length_dw\":1,") != NULL);
    WCX_CHECK(!TX(r, 5).is_error);
    WCX_CHECK_STR_EQ(TX(r, 6).label, "TLP seq=11 MWr32 len=1024 length mismatch");
    rig_free(&r);
}

WCX_TEST(tlp_name_table) {
    // SPEC.md §8.2 table, byte 0 = Fmt[2:0] Type[4:0].
    static const struct {
        uint8_t byte0;
        const char *name;
    } names[] = {
        {0x00, "MRd32"},  {0x20, "MRd64"},  {0x01, "MRdLk32"}, {0x21, "MRdLk64"}, {0x40, "MWr32"},
        {0x60, "MWr64"},  {0x02, "IORd"},   {0x42, "IOWr"},    {0x04, "CfgRd0"},  {0x44, "CfgWr0"},
        {0x05, "CfgRd1"}, {0x45, "CfgWr1"}, {0x30, "Msg"},     {0x35, "Msg"},     {0x70, "MsgD"},
        {0x74, "MsgD"},   {0x0A, "Cpl"},    {0x4A, "CplD"},    {0x0B, "CplLk"},   {0x4B, "CplDLk"},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        WCX_CHECK_STR_EQ(pcie_tlp_name(names[i].byte0), names[i].name);
    }
    // Not in the table: MWr with Type 1, Msg without Fmt bit 0, Fmt 1 IORd, ...
    WCX_CHECK(pcie_tlp_name(0x41) == NULL);
    WCX_CHECK(pcie_tlp_name(0x10) == NULL);
    WCX_CHECK(pcie_tlp_name(0x22) == NULL);
    WCX_CHECK(pcie_tlp_name(0x1F) == NULL);
    WCX_CHECK(pcie_tlp_name(0xE0) == NULL);
}

WCX_TEST(sequence_tracking) {
    rig r;
    WCX_REQUIRE(rig_init(&r));
    uint64_t t0 = 1000;
    feed_tlp(&r, 0, k_mrd32, 12, k_lcrc0, false, t0); // first: sets next = 1
    t0 += 300;
    feed_tlp(&r, 1, k_mrd32, 12, k_lcrc1, false, t0); // normal
    t0 += 300;
    feed_tlp(&r, 1, k_mrd32, 12, k_lcrc1, false, t0); // 1 behind: replay, next stays 2
    t0 += 300;
    feed_tlp(&r, 3, k_mrd32, 12, k_lcrc3, false, t0); // expected 2: error, next = 4
    t0 += 300;
    feed_tlp(&r, 4, k_mrd32, 12, k_lcrc4, false, t0); // normal
    t0 += 300;
    static const uint8_t wrong[4] = {0, 0, 0, 0};
    feed_tlp(&r, 2, k_mrd32, 12, wrong, false, t0); // bad LCRC: no tracking
    t0 += 300;
    static const uint8_t inv2[4] = {0x2E, 0xA2, 0x5B, 0x5D}; // ~D1 5D A4 A2
    feed_tlp(&r, 2, k_mrd32, 12, inv2, true, t0);            // nullified: no tracking
    t0 += 300;
    feed_tlp(&r, 2, k_mrd32, 12, k_lcrc2, false, t0); // 3 behind 5: replay
    WCX_REQUIRE(r.s.buf.count == 8);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "TLP seq=0 MRd32 len=1");
    WCX_CHECK_STR_EQ(TX(r, 1).label, "TLP seq=1 MRd32 len=1");
    WCX_CHECK_STR_EQ(TX(r, 2).label, "TLP seq=1 MRd32 len=1 (replay)");
    WCX_CHECK_STR_EQ(TX(r, 2).fields,
                     "{\"type\":\"tlp\",\"seq\":1,\"tlp\":\"MRd32\",\"length_dw\":1,"
                     "\"tc\":0,\"td\":false,\"ep\":false,\"bytes\":12,"
                     "\"lcrc\":\"0x1F316E1F\",\"replay\":true}");
    WCX_CHECK(!TX(r, 2).is_error);
    WCX_CHECK_STR_EQ(TX(r, 3).label, "TLP seq=3 MRd32 len=1 sequence error");
    WCX_CHECK_STR_EQ(TX(r, 3).fields,
                     "{\"type\":\"tlp\",\"seq\":3,\"tlp\":\"MRd32\",\"length_dw\":1,\"tc\":0,"
                     "\"td\":false,\"ep\":false,\"bytes\":12,\"lcrc\":\"0x5484327F\","
                     "\"replay\":false,\"expected_seq\":2,\"error\":\"The sequence number is "
                     "neither the next expected one nor a replay.\"}");
    WCX_CHECK(TX(r, 3).is_error);
    WCX_CHECK_STR_EQ(TX(r, 4).label, "TLP seq=4 MRd32 len=1");
    WCX_CHECK_STR_EQ(TX(r, 5).label, "TLP seq=2 MRd32 len=1 LCRC mismatch");
    WCX_CHECK_STR_EQ(TX(r, 6).label, "Nullified TLP seq=2 MRd32");
    WCX_CHECK_STR_EQ(TX(r, 7).label, "TLP seq=2 MRd32 len=1 (replay)");
    rig_free(&r);
}

WCX_TEST(sequence_wrap_and_replay_window) {
    rig r;
    WCX_REQUIRE(rig_init(&r));
    // 4095 then 0: the counter wraps.
    feed_tlp(&r, 4095, k_mrd32, 12, k_lcrc4095, false, 1000);
    feed_tlp(&r, 0, k_mrd32, 12, k_lcrc0, false, 1300);
    WCX_REQUIRE(r.s.buf.count == 2);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "TLP seq=4095 MRd32 len=1");
    WCX_CHECK_STR_EQ(TX(r, 1).label, "TLP seq=0 MRd32 len=1");
    rig_free(&r);
    // After 3000 (next = 3001): 953 is 2048 behind -> replay; 952 is 2049
    // behind -> error, and next becomes 953.
    WCX_REQUIRE(rig_init(&r));
    feed_tlp(&r, 3000, k_mrd32, 12, k_lcrc3000, false, 1000);
    feed_tlp(&r, 953, k_mrd32, 12, k_lcrc953, false, 1300);
    feed_tlp(&r, 952, k_mrd32, 12, k_lcrc952, false, 1600);
    feed_tlp(&r, 953, k_mrd32, 12, k_lcrc953, false, 1900);
    WCX_REQUIRE(r.s.buf.count == 4);
    WCX_CHECK_STR_EQ(TX(r, 1).label, "TLP seq=953 MRd32 len=1 (replay)");
    WCX_CHECK_STR_EQ(TX(r, 2).label, "TLP seq=952 MRd32 len=1 sequence error");
    WCX_CHECK(strstr(TX(r, 2).fields, "\"expected_seq\":3001,") != NULL);
    WCX_CHECK_STR_EQ(TX(r, 3).label, "TLP seq=953 MRd32 len=1");
    rig_free(&r);
    // A sequence error ahead of the window: next = 1, seq 2 -> (1 - 2) mod
    // 4096 = 4095 -> error.
    WCX_REQUIRE(rig_init(&r));
    feed_tlp(&r, 0, k_mrd32, 12, k_lcrc0, false, 1000);
    feed_tlp(&r, 2, k_mrd32, 12, k_lcrc2, false, 1300);
    WCX_REQUIRE(r.s.buf.count == 2);
    WCX_CHECK_STR_EQ(TX(r, 1).label, "TLP seq=2 MRd32 len=1 sequence error");
    rig_free(&r);
}

WCX_TEST(several_problems_join_their_sentences) {
    rig r;
    WCX_REQUIRE(rig_init(&r));
    // seq 8 TD-mismatch TLP with a corrupted LCRC: LCRC + length.
    static const uint8_t td[12] = {0x00, 0x00, 0x80, 0x01, 0x01, 0x00,
                                   0x00, 0x0F, 0x00, 0x00, 0x10, 0x00};
    static const uint8_t bad[4] = {0x00, 0x00, 0x00, 0x00};
    feed_tlp(&r, 8, td, 12, bad, false, 1000);
    // Then the same TLP with its right LCRC (F6 32 FE 7E) after a seq 0
    // TLP: length + sequence (expected 1).
    feed_tlp(&r, 0, k_mrd32, 12, k_lcrc0, false, 1300);
    static const uint8_t td_lcrc[4] = {0xF6, 0x32, 0xFE, 0x7E};
    feed_tlp(&r, 8, td, 12, td_lcrc, false, 1600);
    WCX_REQUIRE(r.s.buf.count == 3);
    WCX_CHECK_STR_EQ(TX(r, 0).label, "TLP seq=8 MRd32 len=1 LCRC mismatch length mismatch");
    WCX_CHECK_STR_EQ(TX(r, 0).fields,
                     "{\"type\":\"tlp\",\"seq\":8,\"tlp\":\"MRd32\",\"length_dw\":1,\"tc\":0,"
                     "\"td\":true,\"ep\":false,\"bytes\":12,\"lcrc\":\"0x00000000\","
                     "\"replay\":false,\"expected_lcrc\":\"0xF632FE7E\",\"error\":\"TLP LCRC does "
                     "not match its contents. The header Length field does not match the framed "
                     "payload.\"}");
    WCX_CHECK_STR_EQ(TX(r, 2).label, "TLP seq=8 MRd32 len=1 length mismatch sequence error");
    WCX_CHECK_STR_EQ(TX(r, 2).fields,
                     "{\"type\":\"tlp\",\"seq\":8,\"tlp\":\"MRd32\",\"length_dw\":1,\"tc\":0,"
                     "\"td\":true,\"ep\":false,\"bytes\":12,\"lcrc\":\"0xF632FE7E\","
                     "\"replay\":false,\"expected_seq\":1,\"error\":\"The header Length field does "
                     "not match the framed payload. The sequence number is neither the next "
                     "expected one nor a replay.\"}");
    rig_free(&r);
}

WCX_TEST(ordered_sets_and_idle_produce_nothing) {
    static const tsym syms[] = {KS(0xBC), KS(0x1C), KS(0x1C), KS(0x1C), IDLE,
                                IDLE,     KS(0xBC), KS(0x3C), KS(0x3C), KS(0x3C),
                                KS(0xBC), KS(0x7C), KS(0x7C), KS(0x7C), DS(0x77)};
    rig r;
    WCX_REQUIRE(rig_init(&r));
    FEED(&r.p, syms, 1000, 10);
    pcie_pipeline_break(&r.p, PCIE_BREAK_FLUSH);
    WCX_REQUIRE(r.s.buf.count == 1); // only the stray 0x77, at flush
    WCX_CHECK_STR_EQ(TX(r, 0).label, "Data outside a packet");
    WCX_CHECK_EQ_U64(TX(r, 0).start_fs, T(14));
    rig_free(&r);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, every_dllp_type);
    WCX_RUN(&t, dllp_errors);
    WCX_RUN(&t, mrd32_good_and_its_edb_forms);
    WCX_RUN(&t, tlp_length_errors);
    WCX_RUN(&t, header_decode_and_length_check);
    WCX_RUN(&t, tlp_name_table);
    WCX_RUN(&t, sequence_tracking);
    WCX_RUN(&t, sequence_wrap_and_replay_window);
    WCX_RUN(&t, several_problems_join_their_sentences);
    WCX_RUN(&t, ordered_sets_and_idle_produce_nothing);
    return wcx_test_finish(&t);
}
