// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The serial line decoders (SPEC.md §11): `ferrite.pcie_serial` reports
// what the PIPE decoder does, `ferrite.pcie_serial_dll` what the DLL
// decoder does, both from one 1-bit line. pcie_line.c recovers the symbols;
// this file reads the samples, turns line events into the transactions of
// §11.8 and feeds the symbols to the scrambling detector and pipelines the
// PIPE decoders use.

#include "pcie_serial_plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pcie_detector.h"
#include "pcie_fe_common.h"
#include "pcie_line.h"
#include "pcie_manifest.h"
#include "wcx/arena.h"
#include "wcx/json_writer.h"
#include "wcx/sample.h"

enum { SIG_LINE, SIG_COUNT };

static const wcx_signal_spec k_signals[SIG_COUNT] = {{"line", 1, false}};

// ── manifests (SPEC.md §11.1) ──────────────────────────────────────────────

#define MANIFEST_SERIAL_SIGNALS                                                               \
    "\"signals\":[{\"name\":\"line\",\"bit_width\":1,\"description\":\"The serial line: TXP " \
    "or RXP of one lane, from a SerDes model. X or Z is read as electrical idle\"}],"

#define MANIFEST_PARAM_RATE                                                                     \
    "{\"name\":\"rate\",\"kind\":\"enum\",\"default\":\"auto\",\"display_name\":\"Line rate\"," \
    "\"description\":\"Detect: decided per burst from the shortest bit times. Or fixed at "     \
    "2.5 GT/s (400 ps per bit) or 5.0 GT/s (200 ps per bit)\","                                 \
    "\"enum_values\":[\"auto\",\"gen1\",\"gen2\"],"                                             \
    "\"enum_labels\":{\"auto\":\"Detect\",\"gen1\":\"2.5 GT/s (Gen1)\",\"gen2\":\"5.0 GT/s "    \
    "(Gen2)\"}}"

#define MANIFEST_PARAM_INVERT                                                               \
    "{\"name\":\"invert\",\"kind\":\"bool\",\"default\":false,"                             \
    "\"display_name\":\"Invert the line\","                                                 \
    "\"description\":\"Invert every bit: for an inverted differential pair, or when the N " \
    "side is bound\"}"

static const char k_manifest_serial[] =
    "{\"description\":\"PCI Express Gen1/Gen2 from the serial line (x1, 8b/10b): bit "
    "recovery, symbol lock, 8b/10b and disparity errors, electrical idle, and the PIPE "
    "decoder's view: ordered sets, logical idle, TLP and DLLP framing\","
    "\"category\":\"highSpeed\"," MANIFEST_SERIAL_SIGNALS "\"parameters\":[" MANIFEST_PARAM_RATE
    "," MANIFEST_PARAM_INVERT "," MANIFEST_PARAM_SCRAMBLING "," MANIFEST_PARAM_COALESCE
    "," MANIFEST_PARAM_SHOW_IDLE "]}";

static const char k_manifest_serial_dll[] =
    "{\"description\":\"PCI Express Gen1/Gen2 Data Link Layer from the serial line (x1, "
    "8b/10b): DLLPs with CRC-16 checks, TLPs with LCRC checks, header names and sequence "
    "tracking, with 8b/10b, disparity and symbol alignment errors\","
    "\"category\":\"highSpeed\"," MANIFEST_SERIAL_SIGNALS "\"parameters\":[" MANIFEST_PARAM_RATE
    "," MANIFEST_PARAM_INVERT "," MANIFEST_PARAM_SCRAMBLING "]}";

// Manifest enum order matches pcie_rate_param and pcie_scrambling_param.
static const char *const k_rates[] = {"auto", "gen1", "gen2"};
static const char *const k_scrambling_values[] = {"auto", "on", "off"};

// ── per-instance state ─────────────────────────────────────────────────────

typedef struct serial_state {
    wcx_decoder *d;
    bool dll;
    pcie_line line;
    pcie_detector det;
} serial_state;

// "0011111010": a code group in wire order (§11.8 `code`).
static const char *code_bits(wcx_arena *a, uint16_t code) {
    char bits[PCIE_SYMBOL_BITS + 1u];
    for (unsigned i = 0; i < PCIE_SYMBOL_BITS; i++) {
        bits[i] = (((unsigned)code >> (PCIE_SYMBOL_BITS - 1u - i)) & 1u) != 0u ? '1' : '0';
    }
    bits[PCIE_SYMBOL_BITS] = '\0';
    return wcx_arena_printf(a, "%s", bits);
}

static void emit_lock(serial_state *st, const pcie_line_event *ev) {
    if (st->dll) {
        return; // §11.8: pcie_serial only
    }
    wcx_json j;
    wcx_json_begin(&j, pcie_detector_arena(&st->det));
    wcx_json_add_str(&j, "type", "symbol_lock");
    wcx_json_add_str(&j, "rate", ev->u.gen2 ? "gen2" : "gen1");
    pcie_detector_emit(&st->det, ev->start_fs, ev->end_fs,
                       ev->u.gen2 ? "Symbol lock (Gen2, 5.0 GT/s)" : "Symbol lock (Gen1, 2.5 GT/s)",
                       wcx_json_end(&j), false);
}

static void emit_realign(serial_state *st, const pcie_line_event *ev) {
    wcx_arena *a = pcie_detector_arena(&st->det);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "realign");
    wcx_json_add_u64(&j, "shift", ev->u.shift);
    wcx_json_add_str(&j, "error",
                     "A COM arrived off the symbol boundary; symbol alignment moved to it.");
    pcie_detector_emit(&st->det, ev->start_fs, ev->end_fs,
                       wcx_arena_printf(a, "Symbol lock moved by %u %s", ev->u.shift,
                                        ev->u.shift == 1u ? "bit" : "bits"),
                       wcx_json_end(&j), true);
}

static void emit_code_error(serial_state *st, const pcie_line_event *ev) {
    wcx_arena *a = pcie_detector_arena(&st->det);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "code_error");
    wcx_json_add_str(&j, "code", pcie_str(code_bits(a, ev->u.code)));
    wcx_json_add_str(&j, "error", "Not a valid 8b/10b code group.");
    pcie_detector_emit(&st->det, ev->start_fs, ev->end_fs, "8b/10b code error", wcx_json_end(&j),
                       true);
}

static void emit_disparity(serial_state *st, const pcie_line_event *ev) {
    wcx_arena *a = pcie_detector_arena(&st->det);
    const unsigned byte = ev->u.disp.byte;
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "disparity_error");
    wcx_json_add_str(&j, "code", pcie_str(code_bits(a, ev->u.disp.code)));
    wcx_json_add_hex(&j, "symbol", byte, 2);
    wcx_json_add_str(&j, "error", "The code group is valid only for the other running disparity.");
    // Dx.y / Kx.y: x = bits 4:0, y = bits 7:5.
    pcie_detector_emit(&st->det, ev->start_fs, ev->end_fs,
                       wcx_arena_printf(a, "Disparity error %c%u.%u", ev->u.disp.k ? 'K' : 'D',
                                        byte & 0x1Fu, byte >> 5u),
                       wcx_json_end(&j), true);
}

static void emit_idle(serial_state *st, const pcie_line_event *ev) {
    if (st->dll) {
        return; // §11.8: pcie_serial only
    }
    wcx_json j;
    wcx_json_begin(&j, pcie_detector_arena(&st->det));
    wcx_json_add_str(&j, "type", "electrical_idle");
    pcie_detector_emit(&st->det, ev->start_fs, ev->end_fs, "Electrical idle", wcx_json_end(&j),
                       false);
}

static void on_line_event(void *ctx, const pcie_line_event *ev) {
    serial_state *st = ctx;
    switch (ev->kind) {
        case PCIE_LINE_LOCK:
            emit_lock(st, ev);
            break;
        case PCIE_LINE_REALIGN:
            emit_realign(st, ev);
            break;
        case PCIE_LINE_CODE_ERROR:
            emit_code_error(st, ev);
            break;
        case PCIE_LINE_DISPARITY:
            emit_disparity(st, ev);
            break;
        case PCIE_LINE_SYMBOL:
            pcie_detector_symbol(&st->det, &ev->u.sym);
            break;
        case PCIE_LINE_IDLE_BEGIN:
            pcie_detector_break(&st->det, PCIE_BREAK_ELECIDLE);
            break;
        case PCIE_LINE_IDLE_PERIOD:
            emit_idle(st, ev);
            break;
        default: /* defensive: every pcie_line_event_kind is handled above */
            break;
    }
}

// ── callbacks ──────────────────────────────────────────────────────────────

static bool serial_init(wcx_decoder *d, void *state) {
    serial_state *st = state;
    st->d = d;
    st->dll = wcx_decoder_class_of(d) == &pcie_serial_dll_class;
    size_t rate = 0;
    size_t scrambling = 0;
    bool invert = false;
    bool coalesce = true;
    bool show_idle = false;
    if (!wcx_param_enum(d, "rate", k_rates, 3, PCIE_RATE_AUTO, &rate) ||
        !wcx_param_bool(d, "invert", false, &invert) ||
        !wcx_param_enum(d, "scrambling", k_scrambling_values, 3, PCIE_SCRAMBLING_AUTO,
                        &scrambling)) {
        return false;
    }
    if (!st->dll && (!wcx_param_bool(d, "coalesce", true, &coalesce) ||
                     !wcx_param_bool(d, "show_idle", false, &show_idle))) {
        return false;
    }
    pcie_line_init(&st->line, (pcie_rate_param)rate, invert, on_line_event, st);
    if (!pcie_detector_init(&st->det, d, (pcie_scrambling_param)scrambling, st->dll, coalesce,
                            show_idle)) {
        wcx_decoder_fail(d, "out of memory starting the decoder"); /* defensive: allocation */
        return false;
    }
    return true;
}

static void serial_fini(wcx_decoder *d, void *state) {
    (void)d;
    serial_state *st = state;
    pcie_detector_free(&st->det);
}

// cppcheck-suppress constParameterCallback ; wcx_decoder_class fixes the callback signature
static void serial_sample(wcx_decoder *d, void *state, const wcx_sample *s) {
    serial_state *st = state;
    pcie_detector_begin_call(&st->det);
    bool level = false;
    bool unknown = false;
    WCX_IGNORE(wcx_sample_read_bit(wcx_decoder_layout(d), s->bits, SIG_LINE, 0, &level, &unknown));
    pcie_line_sample(&st->line, s->timestamp_fs, unknown, level);
}

// §11.9: held runs, then the pipelines (§10.6 steps 1-4), then an open
// electrical-idle period, then an undecided `auto` locks.
// cppcheck-suppress constParameterCallback ; wcx_decoder_class fixes the callback signature
static void serial_flush(wcx_decoder *d, void *state) {
    serial_state *st = state;
    pcie_detector_begin_call(&st->det);
    pcie_line_flush_runs(&st->line);
    pcie_detector_end_of_trace(&st->det);
    pcie_line_flush_idle(&st->line, wcx_decoder_now(d));
    pcie_detector_finish(&st->det, wcx_decoder_now(d));
}

// Transactions one call can produce: a scrambling lock emits a whole buffer
// (PCIE_AUTO_LOCK_TRANSACTIONS) plus its notice; and one sample can release
// the 40 runs rate=auto holds, at most 40 bits each (a run of more than
// 8 ns is idle, and 8 ns is 40 Gen2 bits), so 1600 bits or 160 symbols,
// each with a line transaction and up to two pipeline transactions, and an
// idle period. 4096 + 1024 covers that with room to spare.
#define SERIAL_MAX_TX_PER_CALL (PCIE_AUTO_LOCK_TRANSACTIONS + 1024u)

static WcDecoderHandle serial_create(const char *config_json);
static WcDecoderHandle serial_dll_create(const char *config_json);

const wcx_decoder_class pcie_serial_class = {
    .id = "ferrite.pcie_serial",
    .display_name = "PCIe serial line (symbols)",
    .manifest_json = k_manifest_serial,
    .signals = k_signals,
    .signal_count = SIG_COUNT,
    .state_size = sizeof(serial_state),
    .max_transactions_per_call = SERIAL_MAX_TX_PER_CALL,
    .init = serial_init,
    .on_sample = serial_sample,
    .on_flush = serial_flush,
    .fini = serial_fini,
    .create = serial_create,
};

const wcx_decoder_class pcie_serial_dll_class = {
    .id = "ferrite.pcie_serial_dll",
    .display_name = "PCIe serial line (Data Link Layer)",
    .manifest_json = k_manifest_serial_dll,
    .signals = k_signals,
    .signal_count = SIG_COUNT,
    .state_size = sizeof(serial_state),
    .max_transactions_per_call = SERIAL_MAX_TX_PER_CALL,
    .init = serial_init,
    .on_sample = serial_sample,
    .on_flush = serial_flush,
    .fini = serial_fini,
    .create = serial_dll_create,
};

static WcDecoderHandle serial_create(const char *config_json) {
    return wcx_decoder_create(&pcie_serial_class, config_json);
}

static WcDecoderHandle serial_dll_create(const char *config_json) {
    return wcx_decoder_create(&pcie_serial_dll_class, config_json);
}
