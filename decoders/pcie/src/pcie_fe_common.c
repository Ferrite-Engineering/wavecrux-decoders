// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Section references are to decoders/pcie/SPEC.md. Every `error` sentence
// is the fixed text of §10.1.

#include "pcie_fe_common.h"

#include "wcx/json_writer.h"

uint32_t pcie_tlp_seq(const uint8_t head[PCIE_HEAD_KEEP]) {
    // Wire: {0000b, seq[11:8]}, seq[7:0] (protocol-notes.md §4).
    return ((uint32_t)(head[0] & 0x0Fu) << 8u) | head[1];
}

static const char *trunc_by_name(pcie_trunc_by by) {
    switch (by) {
        case PCIE_TRUNC_STP:
            return "STP";
        case PCIE_TRUNC_SDP:
            return "SDP";
        case PCIE_TRUNC_COM:
            return "COM";
        case PCIE_TRUNC_RXVALID:
            return "rxvalid";
        case PCIE_TRUNC_TXELECIDLE:
            return "txelecidle";
        case PCIE_TRUNC_END_OF_TRACE:
            return "end of trace";
        default: /* defensive: every pcie_trunc_by is handled above */
            return "?";
    }
}

// {"type":<type>,"symbol":"0x..","error":<error>} with the given label.
static void emit_symbol_error(pcie_sink *sink, const pcie_event *ev, const char *type,
                              const char *label, const char *error) {
    wcx_json j;
    wcx_json_begin(&j, pcie_sink_arena(sink));
    wcx_json_add_str(&j, "type", type);
    wcx_json_add_hex(&j, "symbol", ev->u.symbol, 2);
    wcx_json_add_str(&j, "error", error);
    pcie_sink_emit(sink, ev->start_fs, ev->end_fs, label, wcx_json_end(&j), true);
}

static void emit_truncated(pcie_sink *sink, const pcie_event *ev) {
    wcx_arena *a = pcie_sink_arena(sink);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "truncated");
    wcx_json_add_str(&j, "packet", ev->u.truncated.tlp ? "tlp" : "dllp");
    wcx_json_add_u64(&j, "symbols", ev->u.truncated.symbols);
    wcx_json_add_str(&j, "error", "The packet ended without END or EDB.");
    pcie_sink_emit(sink, ev->start_fs, ev->end_fs,
                   wcx_arena_printf(a, "Packet truncated by %s", trunc_by_name(ev->u.truncated.by)),
                   wcx_json_end(&j), true);
}

static void emit_unmatched_end(pcie_sink *sink, const pcie_event *ev) {
    wcx_arena *a = pcie_sink_arena(sink);
    const char *name = ev->u.symbol == PCIE_K_EDB ? "EDB" : "END";
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "unmatched_end");
    wcx_json_add_str(&j, "error", "END or EDB arrived with no packet open.");
    pcie_sink_emit(sink, ev->start_fs, ev->end_fs, wcx_arena_printf(a, "%s without start", name),
                   wcx_json_end(&j), true);
}

static void emit_stray(pcie_sink *sink, const pcie_event *ev) {
    wcx_json j;
    wcx_json_begin(&j, pcie_sink_arena(sink));
    wcx_json_add_str(&j, "type", "stray_data");
    wcx_json_add_u64(&j, "symbols", ev->u.stray.count);
    wcx_json_add_hex(&j, "first", ev->u.stray.first, 2);
    wcx_json_add_str(&j, "error", "Data outside a packet or ordered set; logical idle is 0x00.");
    pcie_sink_emit(sink, ev->start_fs, ev->end_fs, "Data outside a packet", wcx_json_end(&j), true);
}

void pcie_fe_emit_dllp_edb(pcie_sink *sink, const pcie_event *ev) {
    wcx_json j;
    wcx_json_begin(&j, pcie_sink_arena(sink));
    wcx_json_add_str(&j, "type", "dllp_edb");
    wcx_json_add_u64(&j, "symbols", ev->u.packet.length);
    wcx_json_add_str(&j, "error", "A DLLP cannot be ended by EDB.");
    pcie_sink_emit(sink, ev->start_fs, ev->end_fs, "DLLP ended by EDB", wcx_json_end(&j), true);
}

void pcie_fe_emit_error(pcie_sink *sink, const pcie_event *ev) {
    wcx_arena *a = pcie_sink_arena(sink);
    const unsigned sym = ev->u.symbol;
    switch (ev->kind) {
        case PCIE_EV_INVALID_K:
            emit_symbol_error(sink, ev, "invalid_k",
                              wcx_arena_printf(a, "Invalid K symbol 0x%02X", sym),
                              "Not a PCIe control symbol.");
            break;
        case PCIE_EV_K_IN_PACKET:
            emit_symbol_error(sink, ev, "k_in_packet",
                              wcx_arena_printf(a, "Invalid K symbol in packet 0x%02X", sym),
                              "A control symbol other than END or EDB appeared inside a packet.");
            break;
        case PCIE_EV_UNEXPECTED_K:
            emit_symbol_error(sink, ev, "unexpected_k",
                              wcx_arena_printf(a, "Unexpected K symbol 0x%02X", sym),
                              "A control symbol appeared outside an ordered set or packet.");
            break;
        case PCIE_EV_UNKNOWN_OS:
            emit_symbol_error(sink, ev, "unknown_os", "Unknown ordered set",
                              "COM was not followed by a recognised ordered set.");
            break;
        case PCIE_EV_TRUNCATED:
            emit_truncated(sink, ev);
            break;
        case PCIE_EV_UNMATCHED_END:
            emit_unmatched_end(sink, ev);
            break;
        case PCIE_EV_STRAY_RUN:
            emit_stray(sink, ev);
            break;
        case PCIE_EV_TS:
        case PCIE_EV_SKP:
        case PCIE_EV_FTS:
        case PCIE_EV_EIOS:
        case PCIE_EV_EIEOS:
        case PCIE_EV_IDLE_RUN:
        case PCIE_EV_PACKET:
        case PCIE_EV_BREAK:
        default: /* defensive: not an error event; the front ends never pass one */
            break;
    }
}
