// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Section references are to decoders/pcie/SPEC.md unless they name the PCIe
// Base Specification.

#include "pcie_dll_fe.h"

#include <string.h>

#include "pcie_crc.h"
#include "pcie_fe_common.h"
#include "wcx/json_writer.h"
#include "wcx/str.h"

#define DLLP_BODY      PCIE_DLLP_BYTES
#define TLP_MIN_BODY   18u // 2 seq + 3-DW header + 4 LCRC (§8.2)
#define TLP_FRAME_OVER 6u  // seq + LCRC around the TLP bytes
#define SEQ_MOD        4096u
#define REPLAY_WINDOW  2048u // §8.2: (next - seq) mod 4096 in 1..2048 is a replay
#define LENGTH_MAX_DW  1024u // a Length field of 0 means 1024 DW (PCIe Base 2.1 §2.2.2)

// §10.1: the sentences of several problems are joined by one space.
#define JOIN " "

void pcie_dll_fe_init(pcie_dll_fe *fe, pcie_sink *sink, bool coalesce) {
    memset(fe, 0, sizeof *fe);
    fe->sink = sink;
    fe->coalesce = coalesce;
}

// ── DLLPs (§8.1) ───────────────────────────────────────────────────────────

typedef struct dllp_kind {
    uint8_t byte0; // exact match, or the high 5 bits for flow control
    bool fc;       // 0x40-0x47 style: low 3 bits are the VC
    const char *type;
    const char *name;
    const char *short_name; // §9.1 short_label (Ack and Nak add the sequence number)
} dllp_kind;

// PCIe Base 2.1 §3.4.1, Table 3-1 (protocol-notes.md §5).
static const dllp_kind k_dllp_kinds[] = {
    {0x00u, false, "ack", "Ack", "Ack"},
    {0x10u, false, "nak", "Nak", "Nak"},
    {0x20u, false, "pm_enter_l1", "PM_Enter_L1", "PM L1"},
    {0x21u, false, "pm_enter_l23", "PM_Enter_L23", "PM L23"},
    {0x23u, false, "pm_as_request_l1", "PM_Active_State_Request_L1", "PM ASL1"},
    {0x24u, false, "pm_request_ack", "PM_Request_Ack", "PM Ack"},
    {0x30u, false, "vendor", "Vendor", "Vendor"},
    {0x40u, true, "initfc1_p", "InitFC1-P", "IFC1-P"},
    {0x50u, true, "initfc1_np", "InitFC1-NP", "IFC1-NP"},
    {0x60u, true, "initfc1_cpl", "InitFC1-Cpl", "IFC1-Cpl"},
    {0xC0u, true, "initfc2_p", "InitFC2-P", "IFC2-P"},
    {0xD0u, true, "initfc2_np", "InitFC2-NP", "IFC2-NP"},
    {0xE0u, true, "initfc2_cpl", "InitFC2-Cpl", "IFC2-Cpl"},
    {0x80u, true, "updatefc_p", "UpdateFC-P", "UFC-P"},
    {0x90u, true, "updatefc_np", "UpdateFC-NP", "UFC-NP"},
    {0xA0u, true, "updatefc_cpl", "UpdateFC-Cpl", "UFC-Cpl"},
};

static const dllp_kind *dllp_lookup(uint8_t byte0) {
    for (size_t i = 0; i < sizeof k_dllp_kinds / sizeof k_dllp_kinds[0]; i++) {
        const dllp_kind *k = &k_dllp_kinds[i];
        if (k->fc ? (byte0 & 0xF8u) == k->byte0 : byte0 == k->byte0) {
            return k;
        }
    }
    return NULL;
}

static bool dllp_crc_ok(const uint8_t *b, uint8_t want[2]) {
    pcie_crc16_wire(pcie_crc16(b, 4), want);
    return want[0] == b[4] && want[1] == b[5];
}

static void emit_dllp_length(pcie_dll_fe *fe, const pcie_event *ev) {
    wcx_arena *a = pcie_sink_arena(fe->sink);
    const unsigned long long n = ev->u.packet.length;
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "dllp_length");
    wcx_json_add_u64(&j, "symbols", n);
    wcx_json_add_str(&j, "error", "A DLLP is 6 symbols between SDP and END.");
    pcie_sink_emit(fe->sink, ev->start_fs, ev->end_fs,
                   wcx_arena_printf(a, "DLLP length %llu, expected 6", n), wcx_json_end(&j), true);
}

// " ×N" for a run of N > 1 DLLPs or triplets (§8.3), else "".
#define TIMES_CAP 32u

static void times_suffix(uint64_t count, char out[TIMES_CAP]) {
    out[0] = '\0';
    if (count > 1u) {
        WCX_IGNORE(
            wcx_str_format(out, TIMES_CAP, " " PCIE_TIMES "%llu", (unsigned long long)count));
    }
}

// Flow-control credits: b1 = rsvd[7:6] HdrFC[7:2]; b2 = HdrFC[1:0] rsvd
// DataFC[11:8]; b3 = DataFC[7:0] (protocol-notes.md §5).
static void fc_credits(const uint8_t *b, unsigned *hdr_fc, unsigned *data_fc) {
    *hdr_fc = (((unsigned)b[1] & 0x3Fu) << 2u) | ((unsigned)b[2] >> 6u);
    *data_fc = ((b[2] & 0x0Fu) << 8u) | b[3];
}

// The label and the type-specific fields of one DLLP, or of a run of
// `count` identical ones (§8.3): `type`, `short_label`, then `count` when
// the run holds more than one (§9).
static const char *dllp_body(wcx_arena *a, wcx_json *j, const dllp_kind *k, const uint8_t *b,
                             uint64_t count) {
    if (k == NULL) {
        // A reserved DLLP never coalesces, so `count` is 1 here.
        wcx_json_add_str(j, "type", "reserved");
        wcx_json_add_str(j, "short_label", "Rsvd");
        wcx_json_add_hex(j, "byte0", b[0], 2);
        return wcx_arena_printf(a, "Reserved DLLP 0x%02X", (unsigned)b[0]);
    }
    char times[TIMES_CAP];
    times_suffix(count, times);
    // Ack / Nak: AckNak_Seq_Num = {b2[3:0], b3} (PCIe Base 2.1 §3.4.1).
    const bool acknak = k->byte0 == 0x00u || k->byte0 == 0x10u;
    const unsigned seq = ((b[2] & 0x0Fu) << 8u) | b[3];
    wcx_json_add_str(j, "type", k->type);
    if (acknak) {
        wcx_json_add_str(j, "short_label",
                         wcx_arena_printf(a, "%s %u%s", k->short_name, seq, times));
    } else if (count > 1u) {
        wcx_json_add_str(j, "short_label", wcx_arena_printf(a, "%s%s", k->short_name, times));
    } else {
        wcx_json_add_str(j, "short_label", k->short_name);
    }
    if (count > 1u) {
        wcx_json_add_u64(j, "count", count);
    }
    if (k->fc) {
        const unsigned vc = b[0] & 0x07u;
        unsigned hdr_fc = 0;
        unsigned data_fc = 0;
        fc_credits(b, &hdr_fc, &data_fc);
        wcx_json_add_u64(j, "vc", vc);
        wcx_json_add_u64(j, "hdr_fc", hdr_fc);
        wcx_json_add_u64(j, "data_fc", data_fc);
        return wcx_arena_printf(a, "%s%s VC%u H=%u D=%u", k->name, times, vc, hdr_fc, data_fc);
    }
    if (acknak) {
        wcx_json_add_u64(j, "seq", seq);
        return wcx_arena_printf(a, "%s%s seq=%u", k->name, times, seq);
    }
    if (k->byte0 == 0x30u) {
        const unsigned payload = ((unsigned)b[1] << 16u) | ((unsigned)b[2] << 8u) | b[3];
        wcx_json_add_hex(j, "payload", payload, 6);
        return wcx_arena_printf(a, "Vendor%s 0x%06X", times, payload);
    }
    // The PM DLLPs carry nothing.
    return count > 1u ? wcx_arena_printf(a, "%s%s", k->name, times) : k->name;
}

#define RESERVED_SENTENCE "This DLLP type is reserved in PCIe Gen1 and Gen2."
#define DLLP_CRC_SENTENCE "DLLP CRC-16 does not match its contents."

// One DLLP of §8.1, or a run of `count` identical good ones (§8.3).
static void emit_single(pcie_dll_fe *fe, const uint8_t *b, uint64_t count, uint64_t start_fs,
                        uint64_t end_fs) {
    wcx_arena *a = pcie_sink_arena(fe->sink);
    const dllp_kind *k = dllp_lookup(b[0]);
    uint8_t want[2];
    const bool crc_ok = dllp_crc_ok(b, want);
    wcx_json j;
    wcx_json_begin(&j, a);
    const char *label = dllp_body(a, &j, k, b, count);
    // The CRC bytes in wire order (§8.1: "0x35BC" for wire bytes 35 BC).
    wcx_json_add_hex(&j, "crc", ((unsigned)b[4] << 8u) | b[5], 4);
    if (!crc_ok) {
        wcx_json_add_hex(&j, "expected_crc", ((unsigned)want[0] << 8u) | want[1], 4);
        label = label != NULL ? wcx_arena_printf(a, "%s CRC mismatch", label) : NULL;
    }
    // §10.1: `error` last; a reserved type with a bad CRC gets both
    // sentences, in label order.
    if (k == NULL && !crc_ok) {
        wcx_json_add_str(&j, "error", RESERVED_SENTENCE JOIN DLLP_CRC_SENTENCE);
    } else if (k == NULL) {
        wcx_json_add_str(&j, "error", RESERVED_SENTENCE);
    } else if (!crc_ok) {
        wcx_json_add_str(&j, "error", DLLP_CRC_SENTENCE);
    }
    pcie_sink_emit(fe->sink, start_fs, end_fs, label, wcx_json_end(&j), !crc_ok || k == NULL);
}

// ── coalescing (§8.3) ──────────────────────────────────────────────────────

// InitFC1-P/NP/Cpl have a high nibble of 4, 5, 6 and InitFC2 of C, D, E;
// bit 3 must be clear (the VC is bits 2:0).
static bool is_initfc(uint8_t b0) {
    const unsigned low3 = ((unsigned)b0 >> 4u) & 0x07u;
    return (b0 & 0x08u) == 0 && low3 >= 4u && low3 <= 6u;
}

// 0 = P, 1 = NP, 2 = Cpl, for an InitFC byte 0.
static unsigned fc_slot(uint8_t b0) {
    return ((unsigned)b0 >> 4u) & 0x03u;
}

// The `n` held DLLPs (1 to 3) can begin (n < 3) or make (n = 3) an InitFC
// triplet: all InitFC1 or all InitFC2, one VC, and P, NP and Cpl at most
// once each.
static bool triplet_prefix(const pcie_dllp_item *h, unsigned n) {
    unsigned seen = 0;
    for (unsigned i = 0; i < n; i++) {
        const uint8_t b0 = h[i].b[0];
        // 0x87: the InitFC1/InitFC2 bit and the VC.
        if (!is_initfc(b0) || (b0 & 0x87u) != (h[0].b[0] & 0x87u)) {
            return false;
        }
        const unsigned bit = 1u << fc_slot(b0);
        if ((seen & bit) != 0) {
            return false;
        }
        seen |= bit;
    }
    return true;
}

// A triplet run: one transaction with the credits of P, NP and Cpl in that
// order, whatever their order on the wire.
static void emit_triplet(pcie_dll_fe *fe, const uint8_t *bytes, uint64_t count, uint64_t start_fs,
                         uint64_t end_fs) {
    wcx_arena *a = pcie_sink_arena(fe->sink);
    const uint8_t *by_slot[PCIE_TRIPLET_DLLP] = {bytes, bytes, bytes};
    for (unsigned i = 0; i < PCIE_TRIPLET_DLLP; i++) {
        const uint8_t *b = bytes + (size_t)i * PCIE_DLLP_BYTES;
        // A triplet's slots are 0-2 (triplet_prefix); the modulo only shows
        // static analysis that the index stays in range.
        by_slot[fc_slot(b[0]) % PCIE_TRIPLET_DLLP] = b;
    }
    unsigned hdr[PCIE_TRIPLET_DLLP] = {0};
    unsigned data[PCIE_TRIPLET_DLLP] = {0};
    for (unsigned s = 0; s < PCIE_TRIPLET_DLLP; s++) {
        fc_credits(by_slot[s], &hdr[s], &data[s]);
    }
    const char level = (bytes[0] & 0x80u) != 0 ? '2' : '1';
    const unsigned vc = bytes[0] & 0x07u;
    char times[TIMES_CAP];
    times_suffix(count, times);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", level == '2' ? "initfc2" : "initfc1");
    wcx_json_add_str(&j, "short_label", wcx_arena_printf(a, "IFC%c%s", level, times));
    if (count > 1u) {
        wcx_json_add_u64(&j, "count", count);
    }
    wcx_json_add_u64(&j, "vc", vc);
    wcx_json_add_u64(&j, "p_hdr_fc", hdr[0]);
    wcx_json_add_u64(&j, "p_data_fc", data[0]);
    wcx_json_add_u64(&j, "np_hdr_fc", hdr[1]);
    wcx_json_add_u64(&j, "np_data_fc", data[1]);
    wcx_json_add_u64(&j, "cpl_hdr_fc", hdr[2]);
    wcx_json_add_u64(&j, "cpl_data_fc", data[2]);
    wcx_json_add_str(&j, "crc",
                     wcx_arena_printf(a, "0x%02X%02X,0x%02X%02X,0x%02X%02X",
                                      (unsigned)by_slot[0][4], (unsigned)by_slot[0][5],
                                      (unsigned)by_slot[1][4], (unsigned)by_slot[1][5],
                                      (unsigned)by_slot[2][4], (unsigned)by_slot[2][5]));
    pcie_sink_emit(fe->sink, start_fs, end_fs,
                   wcx_arena_printf(a, "InitFC%c%s VC%u P H=%u D=%u, NP H=%u D=%u, Cpl H=%u D=%u",
                                    level, times, vc, hdr[0], data[0], hdr[1], data[1], hdr[2],
                                    data[2]),
                   wcx_json_end(&j), false);
}

static void emit_run(pcie_dll_fe *fe) {
    const unsigned n = fe->run_n;
    fe->run_n = 0;
    if (n == PCIE_TRIPLET_DLLP) {
        emit_triplet(fe, fe->run_bytes, fe->run_count, fe->run_start_fs, fe->run_end_fs);
    } else if (n == 1u) {
        emit_single(fe, fe->run_bytes, fe->run_count, fe->run_start_fs, fe->run_end_fs);
    }
}

// One unit (a single DLLP, n = 1, or a triplet, n = 3): it extends the run
// when it is identical to the run's unit, else ends the run and starts one.
static void add_unit(pcie_dll_fe *fe, const pcie_dllp_item *u, unsigned n) {
    uint8_t bytes[PCIE_TRIPLET_DLLP * PCIE_DLLP_BYTES] = {0};
    for (unsigned i = 0; i < n; i++) {
        memcpy(bytes + (size_t)i * PCIE_DLLP_BYTES, u[i].b, PCIE_DLLP_BYTES);
    }
    const size_t len = (size_t)n * PCIE_DLLP_BYTES;
    if (fe->run_n == n && memcmp(fe->run_bytes, bytes, len) == 0) {
        fe->run_count++;
        fe->run_end_fs = u[n - 1u].end_fs;
        return;
    }
    emit_run(fe);
    fe->run_n = n;
    memcpy(fe->run_bytes, bytes, len);
    fe->run_count = 1;
    fe->run_start_fs = u[0].start_fs;
    fe->run_end_fs = u[n - 1u].end_fs;
}

// The first held DLLP cannot begin a triplet: it is a single unit.
static void release_first(pcie_dll_fe *fe) {
    add_unit(fe, &fe->hold[0], 1u);
    fe->held--;
    for (unsigned i = 0; i < fe->held; i++) {
        fe->hold[i] = fe->hold[i + 1u];
    }
}

// A coalescing DLLP: held while it may still be part of a triplet.
static void coalesce_dllp(pcie_dll_fe *fe, const pcie_dllp_item *it) {
    fe->hold[fe->held++] = *it; // at most two were held
    while (fe->held > 0) {
        if (triplet_prefix(fe->hold, fe->held)) {
            if (fe->held == PCIE_TRIPLET_DLLP) {
                add_unit(fe, fe->hold, PCIE_TRIPLET_DLLP);
                fe->held = 0;
            }
            return;
        }
        release_first(fe);
    }
}

// The stretch ends: held DLLPs become single units, and the run is
// emitted.
static void end_stretch(pcie_dll_fe *fe) {
    while (fe->held > 0) {
        release_first(fe);
    }
    emit_run(fe);
}

static void on_dllp(pcie_dll_fe *fe, const pcie_event *ev) {
    const pcie_packet *p = &ev->u.packet;
    if (p->edb) {
        end_stretch(fe);
        pcie_fe_emit_dllp_edb(fe->sink, ev); // §10.2: not decoded
        return;
    }
    if (p->length != DLLP_BODY) {
        end_stretch(fe);
        emit_dllp_length(fe, ev);
        return;
    }
    const uint8_t *b = p->head;
    uint8_t want[2];
    if (fe->coalesce && dllp_lookup(b[0]) != NULL && dllp_crc_ok(b, want)) {
        pcie_dllp_item it;
        memcpy(it.b, b, PCIE_DLLP_BYTES);
        it.start_fs = ev->start_fs;
        it.end_fs = ev->end_fs;
        coalesce_dllp(fe, &it);
        return;
    }
    end_stretch(fe);
    emit_single(fe, b, 1u, ev->start_fs, ev->end_fs);
}

// ── TLPs (§8.2) ────────────────────────────────────────────────────────────

typedef struct tlp_name {
    uint8_t fmt;
    uint8_t type;
    const char *name;
} tlp_name;

// PCIe Base 2.1 §2.2.1, Table 2-3. Msg/MsgD (Type 10rrr) are matched on the
// high two Type bits below.
static const tlp_name k_tlp_names[] = {
    {0u, 0x00u, "MRd32"},  {1u, 0x00u, "MRd64"},  {0u, 0x01u, "MRdLk32"}, {1u, 0x01u, "MRdLk64"},
    {2u, 0x00u, "MWr32"},  {3u, 0x00u, "MWr64"},  {0u, 0x02u, "IORd"},    {2u, 0x02u, "IOWr"},
    {0u, 0x04u, "CfgRd0"}, {2u, 0x04u, "CfgWr0"}, {0u, 0x05u, "CfgRd1"},  {2u, 0x05u, "CfgWr1"},
    {1u, 0x10u, "Msg"},    {3u, 0x10u, "MsgD"},   {0u, 0x0Au, "Cpl"},     {2u, 0x0Au, "CplD"},
    {0u, 0x0Bu, "CplLk"},  {2u, 0x0Bu, "CplDLk"},
};

const char *pcie_tlp_name(uint8_t byte0) {
    const uint8_t fmt = (uint8_t)(byte0 >> 5u);
    uint8_t type = (uint8_t)(byte0 & 0x1Fu);
    if ((type & 0x18u) == 0x10u) {
        type = 0x10u; // Msg routing bits rrr do not change the name
    }
    for (size_t i = 0; i < sizeof k_tlp_names / sizeof k_tlp_names[0]; i++) {
        if (k_tlp_names[i].fmt == fmt && k_tlp_names[i].type == type) {
            return k_tlp_names[i].name;
        }
    }
    return NULL;
}

typedef struct tlp_header {
    const char *name;
    unsigned fmt;
    unsigned length_dw;
    unsigned tc;
    bool td;
    bool ep;
    uint64_t bytes;          // TLP bytes between seq and LCRC
    uint64_t expected_bytes; // what the header implies
    bool length_ok;
} tlp_header;

// Byte 0 = Fmt[2:0] Type[4:0]; byte 1 bits 6:4 = TC; byte 2 bit 7 = TD,
// bit 6 = EP, bits 1:0 = Length[9:8]; byte 3 = Length[7:0] (§8.2; PCIe Base
// 2.1 §2.2.1).
static void decode_header(wcx_arena *a, const pcie_packet *p, tlp_header *h) {
    const uint8_t *t = &p->head[2];
    memset(h, 0, sizeof *h);
    h->name = pcie_tlp_name(t[0]);
    if (h->name == NULL) {
        h->name = pcie_str(wcx_arena_printf(a, "Type 0x%02X", (unsigned)t[0]));
    }
    h->fmt = (unsigned)t[0] >> 5u;
    h->tc = ((unsigned)t[1] >> 4u) & 0x07u;
    h->td = (t[2] & 0x80u) != 0;
    h->ep = (t[2] & 0x40u) != 0;
    const unsigned len10 = (((unsigned)t[2] & 0x03u) << 8u) | (unsigned)t[3];
    h->length_dw = len10 == 0 ? LENGTH_MAX_DW : len10;
    h->bytes = p->length - TLP_FRAME_OVER;
    // Length check (§8.2): header DWs from Fmt bit 0, a data payload when
    // Fmt bit 1 is set, a 4-byte ECRC when TD is set.
    const uint64_t hdr_dw = (h->fmt & 1u) != 0 ? 4u : 3u;
    h->expected_bytes = 4u * hdr_dw + (h->td ? 4u : 0u);
    if ((h->fmt & 2u) != 0) {
        h->expected_bytes += 4u * (uint64_t)h->length_dw;
    }
    h->length_ok = h->bytes == h->expected_bytes;
}

static void add_header_fields(wcx_json *j, uint32_t seq, const tlp_header *h, const uint8_t *lcrc) {
    wcx_json_add_u64(j, "seq", seq);
    wcx_json_add_str(j, "tlp", h->name);
    wcx_json_add_u64(j, "length_dw", h->length_dw);
    wcx_json_add_u64(j, "tc", h->tc);
    wcx_json_add_bool(j, "td", h->td);
    wcx_json_add_bool(j, "ep", h->ep);
    wcx_json_add_u64(j, "bytes", h->bytes);
    wcx_json_add_hex(j, "lcrc",
                     ((uint64_t)lcrc[0] << 24u) | ((uint64_t)lcrc[1] << 16u) |
                         ((uint64_t)lcrc[2] << 8u) | lcrc[3],
                     8);
}

static void emit_tlp_length(pcie_dll_fe *fe, const pcie_event *ev) {
    wcx_arena *a = pcie_sink_arena(fe->sink);
    const unsigned long long n = ev->u.packet.length;
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "tlp_length");
    wcx_json_add_u64(&j, "symbols", n);
    wcx_json_add_str(&j, "error",
                     "A TLP body is at least 18 symbols, and a multiple of 4 between the "
                     "sequence number and the LCRC.");
    pcie_sink_emit(fe->sink, ev->start_fs, ev->end_fs,
                   wcx_arena_printf(a, "TLP length %llu symbols is not valid", n), wcx_json_end(&j),
                   true);
}

// EDB: a nullified TLP when the LCRC is the inverse of the correct one,
// otherwise an error (§8.2).
static void emit_tlp_edb(pcie_dll_fe *fe, const pcie_event *ev, const uint8_t want[4]) {
    const pcie_packet *p = &ev->u.packet;
    wcx_arena *a = pcie_sink_arena(fe->sink);
    const uint32_t seq = pcie_tlp_seq(p->head);
    bool inverse = true;
    for (unsigned i = 0; i < 4u; i++) {
        inverse = inverse && p->tail[i] == (uint8_t)~want[i];
    }
    wcx_json j;
    wcx_json_begin(&j, a);
    if (inverse) {
        tlp_header h;
        decode_header(a, p, &h);
        wcx_json_add_str(&j, "type", "tlp_nullified");
        add_header_fields(&j, seq, &h, p->tail);
        pcie_sink_emit(fe->sink, ev->start_fs, ev->end_fs,
                       wcx_arena_printf(a, "Nullified TLP seq=%u %s", (unsigned)seq, h.name),
                       wcx_json_end(&j), false);
        return;
    }
    wcx_json_add_str(&j, "type", "tlp_edb");
    wcx_json_add_u64(&j, "seq", seq);
    wcx_json_add_u64(&j, "bytes", p->length - TLP_FRAME_OVER);
    wcx_json_add_str(&j, "error",
                     "A TLP ended by EDB must carry the inverted LCRC of a nullified TLP.");
    pcie_sink_emit(fe->sink, ev->start_fs, ev->end_fs,
                   wcx_arena_printf(a, "TLP ended by EDB seq=%u", (unsigned)seq), wcx_json_end(&j),
                   true);
}

typedef enum seq_status { SEQ_NORMAL, SEQ_REPLAY, SEQ_ERROR } seq_status;

// §8.2 sequence tracking, for good-LCRC END-terminated TLPs only.
static seq_status track_seq(pcie_dll_fe *fe, uint32_t seq, uint32_t *expected) {
    if (!fe->have_next) {
        fe->have_next = true;
        fe->next_seq = (seq + 1u) % SEQ_MOD;
        return SEQ_NORMAL;
    }
    *expected = fe->next_seq;
    if (seq == fe->next_seq) {
        fe->next_seq = (seq + 1u) % SEQ_MOD;
        return SEQ_NORMAL;
    }
    const uint32_t behind = (fe->next_seq + SEQ_MOD - seq) % SEQ_MOD;
    if (behind >= 1u && behind <= REPLAY_WINDOW) {
        return SEQ_REPLAY;
    }
    fe->next_seq = (seq + 1u) % SEQ_MOD;
    return SEQ_ERROR;
}

// Appends `part` to the sentence being built (NULL = nothing yet).
static const char *join(wcx_arena *a, const char *so_far, const char *part) {
    if (so_far == NULL) {
        return pcie_str(part);
    }
    return wcx_arena_printf(a, "%s" JOIN "%s", so_far, pcie_str(part));
}

static void emit_tlp_end(pcie_dll_fe *fe, const pcie_event *ev, const uint8_t want[4]) {
    const pcie_packet *p = &ev->u.packet;
    wcx_arena *a = pcie_sink_arena(fe->sink);
    const uint32_t seq = pcie_tlp_seq(p->head);
    tlp_header h;
    decode_header(a, p, &h);
    const bool lcrc_ok = memcmp(want, p->tail, 4) == 0;
    uint32_t expected_seq = 0;
    const seq_status st = lcrc_ok ? track_seq(fe, seq, &expected_seq) : SEQ_NORMAL;

    // Suffixes in the order of §8.2: LCRC, length, replay, sequence.
    const char *label = wcx_arena_printf(
        a, "TLP seq=%u %s len=%u%s%s%s%s", (unsigned)seq, h.name, h.length_dw,
        lcrc_ok ? "" : " LCRC mismatch", h.length_ok ? "" : " length mismatch",
        st == SEQ_REPLAY ? " (replay)" : "", st == SEQ_ERROR ? " sequence error" : "");
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "tlp");
    add_header_fields(&j, seq, &h, p->tail);
    wcx_json_add_bool(&j, "replay", st == SEQ_REPLAY);
    // §10.1: gained fields expected_lcrc, expected_seq, then one `error`
    // joining the sentences in suffix order.
    const char *error = NULL;
    if (!lcrc_ok) {
        wcx_json_add_hex(&j, "expected_lcrc",
                         ((uint64_t)want[0] << 24u) | ((uint64_t)want[1] << 16u) |
                             ((uint64_t)want[2] << 8u) | want[3],
                         8);
        error = join(a, error, "TLP LCRC does not match its contents.");
    }
    if (!h.length_ok) {
        error = join(a, error, "The header Length field does not match the framed payload.");
    }
    if (st == SEQ_ERROR) {
        wcx_json_add_u64(&j, "expected_seq", expected_seq);
        error =
            join(a, error, "The sequence number is neither the next expected one nor a replay.");
    }
    if (error != NULL) {
        wcx_json_add_str(&j, "error", error);
    }
    pcie_sink_emit(fe->sink, ev->start_fs, ev->end_fs, label, wcx_json_end(&j), error != NULL);
}

static void emit_tlp(pcie_dll_fe *fe, const pcie_event *ev) {
    const pcie_packet *p = &ev->u.packet;
    if (p->length < TLP_MIN_BODY || (p->length - TLP_FRAME_OVER) % 4u != 0) {
        emit_tlp_length(fe, ev);
        return;
    }
    uint8_t want[4];
    pcie_crc32_wire(p->crc32_before_tail, want);
    if (p->edb) {
        emit_tlp_edb(fe, ev, want);
    } else {
        emit_tlp_end(fe, ev, want);
    }
}

void pcie_dll_fe_event(void *ctx, const pcie_event *ev) {
    pcie_dll_fe *fe = ctx;
    switch (ev->kind) {
        case PCIE_EV_PACKET:
            if (ev->u.packet.tlp) {
                end_stretch(fe);
                emit_tlp(fe, ev);
            } else {
                on_dllp(fe, ev);
            }
            return;
        case PCIE_EV_STRAY_RUN:
        case PCIE_EV_INVALID_K:
        case PCIE_EV_TRUNCATED:
        case PCIE_EV_UNMATCHED_END:
        case PCIE_EV_K_IN_PACKET:
        case PCIE_EV_UNEXPECTED_K:
        case PCIE_EV_UNKNOWN_OS:
        case PCIE_EV_FRAMING_LANE:
        case PCIE_EV_PAD_MISPLACED:
            end_stretch(fe);
            pcie_fe_emit_error(fe->sink, ev);
            return;
        case PCIE_EV_TS:
        case PCIE_EV_FTS:
        case PCIE_EV_EIOS:
        case PCIE_EV_EIEOS:
        case PCIE_EV_BREAK:
            // §8: nothing for ordered sets; §8.3: these, and a break, end a
            // stretch of coalescing DLLPs.
            end_stretch(fe);
            return;
        case PCIE_EV_SKP:
        case PCIE_EV_IDLE_RUN:
        default: // §8: nothing for SKP or idle, which do not end a stretch (§8.3)
            return;
    }
}
