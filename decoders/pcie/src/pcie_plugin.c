// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The wcx_pcie plugin: eight decoders (SPEC.md §1), four PIPE data widths
// times the PIPE decoder and the Data Link Layer decoder, all sharing one
// set of callbacks. The class a handle was created from tells on_sample how
// many byte lanes a `data` word holds and which front end to run.
//
// This file owns everything that is per clock edge (SPEC.md §4): edge
// detection and the sample point, symbol times, gating by rxvalid and
// txelecidle, X/Z runs and RxStatus. Everything per symbol is in
// pcie_detector.c and below.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pcie_detector.h"
#include "pcie_symbols.h"
#include "pcie_time.h"
#include "wavecrux_decoder.h"
#include "wcx/decoder.h"
#include "wcx/edge.h"
#include "wcx/json_writer.h"
#include "wcx/sample.h"

// ── signals (SPEC.md §2; manifest order) ───────────────────────────────────

enum { SIG_PCLK, SIG_DATA, SIG_DATAK, SIG_RXVALID, SIG_TXELECIDLE, SIG_RXSTATUS, SIG_COUNT };

#define PCIE_SIGNALS(lanes)                                                             \
    {                                                                                   \
        {"pclk", 1, false},   {"data", 8u * (lanes), false}, {"datak", (lanes), false}, \
        {"rxvalid", 1, true}, {"txelecidle", 1, true},       {"rxstatus", 3, true},     \
    }

static const wcx_signal_spec k_signals_w8[SIG_COUNT] = PCIE_SIGNALS(1u);
static const wcx_signal_spec k_signals_w16[SIG_COUNT] = PCIE_SIGNALS(2u);
static const wcx_signal_spec k_signals_w32[SIG_COUNT] = PCIE_SIGNALS(4u);
static const wcx_signal_spec k_signals_w64[SIG_COUNT] = PCIE_SIGNALS(8u);

// ── manifests (SPEC.md §2-§3) ──────────────────────────────────────────────

#define MANIFEST_SIGNALS(width, kwidth)                                                         \
    "\"signals\":["                                                                             \
    "{\"name\":\"pclk\",\"bit_width\":1,\"description\":\"PIPE parallel clock (PCLK); symbols " \
    "are captured on its rising edge\"},"                                                       \
    "{\"name\":\"data\",\"bit_width\":" width                                                   \
    ",\"description\":\"PIPE TxData or RxData; byte lane 0 (bits 7:0) is the first symbol on "  \
    "the wire. One decoder instance decodes one direction\"},"                                  \
    "{\"name\":\"datak\",\"bit_width\":" kwidth                                                 \
    ",\"description\":\"PIPE TxDataK or RxDataK; bit j set marks byte lane j as a K (control) " \
    "symbol\"}],"                                                                               \
    "\"optional_signals\":["                                                                    \
    "{\"name\":\"rxvalid\",\"bit_width\":1,\"description\":\"PIPE RxValid. When bound, clock "  \
    "edges where it is 0 carry no symbols. Unbound means always valid\"},"                      \
    "{\"name\":\"txelecidle\",\"bit_width\":1,\"description\":\"PIPE TxElecIdle. When bound, "  \
    "clock edges where it is 1 carry no symbols. Unbound means never idle\"},"                  \
    "{\"name\":\"rxstatus\",\"bit_width\":3,\"description\":\"PIPE RxStatus[2:0]. The PIPE "    \
    "decoder annotates SKP add/remove, receiver detection and PHY errors; the Data Link Layer " \
    "decoder ignores it\"}],"

#define MANIFEST_PARAM_SCRAMBLING                                                               \
    "{\"name\":\"scrambling\",\"kind\":\"enum\",\"default\":\"auto\","                          \
    "\"display_name\":\"Scrambling\","                                                          \
    "\"description\":\"Detect: run descrambled and plain decoding side by side until a packet " \
    "with a correct CRC settles it. On: descramble (Gen1/Gen2 LFSR, reset by COM). Off: the "   \
    "symbols are already plain, as in RTL simulation\","                                        \
    "\"enum_values\":[\"auto\",\"on\",\"off\"],"                                                \
    "\"enum_labels\":{\"auto\":\"Detect\",\"on\":\"On (descramble)\",\"off\":\"Off\"}}"

#define MANIFEST_PARAM_SAMPLE_POINT                                                          \
    "{\"name\":\"sample_point\",\"kind\":\"enum\",\"default\":\"before_edge\","              \
    "\"display_name\":\"Sample point\","                                                     \
    "\"description\":\"Before the clock edge reads the values a flip-flop captures in a "    \
    "zero-delay RTL dump; at the clock edge reads the values present at the edge itself\","  \
    "\"enum_values\":[\"before_edge\",\"at_edge\"],"                                         \
    "\"enum_labels\":{\"before_edge\":\"Before the clock edge\",\"at_edge\":\"At the clock " \
    "edge\"}}"

#define MANIFEST_PARAM_COALESCE                                                              \
    "{\"name\":\"coalesce\",\"kind\":\"bool\",\"default\":true,"                             \
    "\"display_name\":\"Coalesce repeated ordered sets\","                                   \
    "\"description\":\"One transaction per run of identical TS1, TS2, FTS or EIEOS ordered " \
    "sets, with a count\"}"

#define MANIFEST_PARAM_SHOW_IDLE                                   \
    "{\"name\":\"show_idle\",\"kind\":\"bool\",\"default\":false," \
    "\"display_name\":\"Show logical idle\","                      \
    "\"description\":\"One transaction per run of logical idle symbols between packets\"}"

#define MANIFEST_PIPE(width, kwidth)                                                          \
    "{\"description\":\"PCI Express Gen1/Gen2 at the PIPE interface (x1, " width              \
    "-bit data): TS1/TS2, SKP, FTS, EIOS and EIEOS ordered sets, logical idle, TLP and DLLP " \
    "framing, symbol errors and RxStatus\","                                                  \
    "\"category\":\"highSpeed\"," MANIFEST_SIGNALS(                                           \
        width, kwidth) "\"parameters\":[" MANIFEST_PARAM_SCRAMBLING                           \
                       "," MANIFEST_PARAM_SAMPLE_POINT "," MANIFEST_PARAM_COALESCE            \
                       "," MANIFEST_PARAM_SHOW_IDLE "]}"

#define MANIFEST_DLL(width, kwidth)                                                           \
    "{\"description\":\"PCI Express Gen1/Gen2 Data Link Layer from PIPE symbols (x1, " width  \
    "-bit data): DLLPs with CRC-16 checks, TLPs with LCRC checks, header names and sequence " \
    "tracking\","                                                                             \
    "\"category\":\"highSpeed\"," MANIFEST_SIGNALS(                                           \
        width, kwidth) "\"parameters\":[" MANIFEST_PARAM_SCRAMBLING                           \
                       "," MANIFEST_PARAM_SAMPLE_POINT "]}"

static const char k_manifest_pipe_w8[] = MANIFEST_PIPE("8", "1");
static const char k_manifest_pipe_w16[] = MANIFEST_PIPE("16", "2");
static const char k_manifest_pipe_w32[] = MANIFEST_PIPE("32", "4");
static const char k_manifest_pipe_w64[] = MANIFEST_PIPE("64", "8");
static const char k_manifest_dll_w8[] = MANIFEST_DLL("8", "1");
static const char k_manifest_dll_w16[] = MANIFEST_DLL("16", "2");
static const char k_manifest_dll_w32[] = MANIFEST_DLL("32", "4");
static const char k_manifest_dll_w64[] = MANIFEST_DLL("64", "8");

// Manifest enum order of `scrambling` matches pcie_scrambling_param.
static const char *const k_scrambling_values[] = {"auto", "on", "off"};
static const char *const k_sample_points[] = {"before_edge", "at_edge"};

// ── per-instance state ─────────────────────────────────────────────────────

typedef struct pcie_state {
    unsigned lanes; // symbols per clock: 1, 2, 4 or 8
    bool dll;
    wcx_edge clk;
    bool have_edge;
    uint64_t prev_edge_fs;
    // A run of clock edges with X or Z on data/datak (SPEC.md §4.4).
    bool xz_open;
    uint64_t xz_start_fs;
    uint64_t xz_end_fs;
    uint64_t xz_edges;
    uint64_t prev_rxstatus; // at the previous rising edge (§4.6)
    pcie_detector det;
} pcie_state;

// Which class a handle belongs to, and what that means.
typedef struct pcie_variant {
    const wcx_decoder_class *cls;
    unsigned lanes;
    bool dll;
} pcie_variant;

static const pcie_variant *find_variant(const wcx_decoder *d);

static bool pcie_init(wcx_decoder *d, void *state) {
    pcie_state *st = state;
    const pcie_variant *v = find_variant(d);
    if (v == NULL) { /* defensive: every class is in the table */
        wcx_decoder_fail(d, "internal decoder error: unknown decoder class");
        return false;
    }
    st->lanes = v->lanes;
    st->dll = v->dll;
    size_t scrambling = 0;
    size_t point = 0;
    bool coalesce = true;
    bool show_idle = false;
    if (!wcx_param_enum(d, "scrambling", k_scrambling_values, 3, PCIE_SCRAMBLING_AUTO,
                        &scrambling) ||
        !wcx_param_enum(d, "sample_point", k_sample_points, 2, 0, &point)) {
        return false;
    }
    if (!st->dll && (!wcx_param_bool(d, "coalesce", true, &coalesce) ||
                     !wcx_param_bool(d, "show_idle", false, &show_idle))) {
        return false;
    }
    const wcx_sample_point sp = point == 1 ? WCX_SAMPLE_AT_EDGE : WCX_SAMPLE_BEFORE_EDGE;
    if (!wcx_edge_init(&st->clk, wcx_decoder_layout(d), SIG_PCLK, sp) ||
        !pcie_detector_init(&st->det, d, (pcie_scrambling_param)scrambling, st->dll, coalesce,
                            show_idle)) {
        wcx_decoder_fail(d, "out of memory starting the decoder"); /* defensive: allocation */
        return false;
    }
    return true;
}

static void pcie_fini(wcx_decoder *d, void *state) {
    (void)d;
    pcie_state *st = state;
    wcx_edge_free(&st->clk);
    pcie_detector_free(&st->det);
}

// ── edge-level transactions ────────────────────────────────────────────────

// §4.4: one error per run of X/Z edges, emitted when the run ends.
static void close_xz_run(pcie_state *st) {
    if (!st->xz_open) {
        return;
    }
    st->xz_open = false;
    wcx_arena *a = pcie_detector_arena(&st->det);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "unknown_value");
    wcx_json_add_u64(&j, "edges", st->xz_edges);
    wcx_json_add_str(&j, "error", "The data or datak bus carries X or Z.");
    pcie_detector_emit(&st->det, st->xz_start_fs, st->xz_end_fs, "X/Z on data", wcx_json_end(&j),
                       true);
}

typedef struct rxstatus_code {
    const char *label;
    const char *error; // NULL: not an error (§4.6, §10.1)
} rxstatus_code;

// PIPE RxStatus encodings (PIPE 2.00 §6.6; SPEC.md §4.6), indexed by code.
static const rxstatus_code k_rxstatus[8] = {
    {"", NULL}, // 000: no event
    {"RxStatus: SKP added", NULL},
    {"RxStatus: SKP removed", NULL},
    {"RxStatus: receiver detected", NULL},
    {"RxStatus: 8b/10b decode error", "8b/10b decode error reported by the PHY."},
    {"RxStatus: elastic buffer overflow", "Elastic buffer overflow reported by the PHY."},
    {"RxStatus: elastic buffer underflow", "Elastic buffer underflow reported by the PHY."},
    {"RxStatus: disparity error", "Disparity error reported by the PHY."},
};

// One rising edge of pclk (SPEC.md §4.2): its time, the period measured from
// the previous rising edge (0 at the first), and the end of its last lane.
typedef struct edge_times {
    uint64_t start_fs;
    uint64_t period_fs;
    uint64_t end_fs;
} edge_times;

// §4.6: at an ungated edge where rxstatus is non-zero and changed.
static void rxstatus_event(pcie_state *st, uint64_t code, const edge_times *e) {
    if (code == 0 || code == st->prev_rxstatus || code > 7u) {
        return;
    }
    const char bits[6] = {'0',
                          'b',
                          (code & 4u) != 0 ? '1' : '0',
                          (code & 2u) != 0 ? '1' : '0',
                          (code & 1u) != 0 ? '1' : '0',
                          '\0'};
    const rxstatus_code *rc = &k_rxstatus[code];
    wcx_json j;
    wcx_json_begin(&j, pcie_detector_arena(&st->det));
    wcx_json_add_str(&j, "type", "rxstatus");
    wcx_json_add_str(&j, "code", bits);
    if (rc->error != NULL) {
        wcx_json_add_str(&j, "error", rc->error);
    }
    pcie_detector_emit(&st->det, e->start_fs, e->end_fs, rc->label, wcx_json_end(&j),
                       rc->error != NULL);
}

// ── per edge ───────────────────────────────────────────────────────────────

// A bound 1-bit sideband signal at the sample point: its level, or `def`
// when unbound or X/Z (an unknown level is not "0" or "1", SPEC.md §4.3).
static bool sideband(const wcx_decoder *d, const uint8_t *at, size_t sig, bool def) {
    if (!wcx_decoder_is_bound(d, sig)) {
        return def;
    }
    bool level = false;
    bool unknown = false;
    if (!wcx_sample_read_bit(wcx_decoder_layout(d), at, sig, 0, &level, &unknown) || unknown) {
        return def;
    }
    return level;
}

static uint64_t rxstatus_value(const wcx_decoder *d, const uint8_t *at) {
    if (!wcx_decoder_is_bound(d, SIG_RXSTATUS)) {
        return 0;
    }
    uint64_t value = 0;
    bool unknown = false;
    if (!wcx_sample_read(wcx_decoder_layout(d), at, SIG_RXSTATUS, &value, &unknown) || unknown) {
        return 0; // X/Z: no event (SPEC.md is silent; treated as code 000)
    }
    return value;
}

static void feed_symbols(pcie_state *st, const uint8_t *at, const edge_times *e) {
    const wcx_layout *layout = wcx_decoder_layout(st->det.d);
    uint64_t data = 0;
    uint64_t datak = 0;
    bool data_x = false;
    bool datak_x = false;
    WCX_IGNORE(wcx_sample_read(layout, at, SIG_DATA, &data, &data_x));
    WCX_IGNORE(wcx_sample_read(layout, at, SIG_DATAK, &datak, &datak_x));
    if (data_x || datak_x) {
        if (!st->xz_open) {
            st->xz_open = true;
            st->xz_start_fs = e->start_fs;
            st->xz_edges = 0;
            pcie_detector_break(&st->det, PCIE_BREAK_UNKNOWN);
        }
        st->xz_edges++;
        st->xz_end_fs = e->end_fs;
        return;
    }
    close_xz_run(st);
    const uint64_t duration = pcie_symbol_duration(e->period_fs, st->lanes);
    for (unsigned lane = 0; lane < st->lanes; lane++) {
        pcie_sym s;
        s.byte = (uint8_t)((data >> (8u * lane)) & 0xFFu);
        s.k = ((datak >> lane) & 1u) != 0;
        s.start_fs = pcie_symbol_start(e->start_fs, e->period_fs, lane, st->lanes);
        s.end_fs = pcie_symbol_end(s.start_fs, duration);
        pcie_detector_symbol(&st->det, &s);
    }
}

// cppcheck-suppress constParameterCallback ; wcx_decoder_class fixes the callback signature
static void pcie_sample(wcx_decoder *d, void *state, const wcx_sample *s) {
    pcie_state *st = state;
    pcie_detector_begin_call(&st->det);
    if (wcx_edge_step(&st->clk, s->timestamp_fs, s->bits) != WCX_EDGE_RISING) {
        return;
    }
    // §4.2: P_k from the previous rising edge; the first edge has P = 0.
    edge_times e;
    e.start_fs = s->timestamp_fs;
    e.period_fs = st->have_edge ? e.start_fs - st->prev_edge_fs : 0u;
    e.end_fs = pcie_edge_end(e.start_fs, e.period_fs, st->lanes);
    st->have_edge = true;
    st->prev_edge_fs = e.start_fs;
    const uint8_t *at = wcx_edge_data(&st->clk);
    const uint64_t rxstatus = rxstatus_value(d, at);

    // §4.3: gating.
    pcie_break_reason why = PCIE_BREAK_RXVALID;
    bool gated = !sideband(d, at, SIG_RXVALID, true);
    if (!gated && sideband(d, at, SIG_TXELECIDLE, false)) {
        gated = true;
        why = PCIE_BREAK_TXELECIDLE;
    }
    if (gated) {
        close_xz_run(st);
        pcie_detector_break(&st->det, why);
    } else {
        if (!st->dll) {
            rxstatus_event(st, rxstatus, &e);
        }
        feed_symbols(st, at, &e);
    }
    st->prev_rxstatus = rxstatus;
}

// SPEC.md §10.6: what the pipelines hold (ordered set, coalesced runs, open
// packet, stray run), then an X/Z run, then an undecided `auto` locks.
// cppcheck-suppress constParameterCallback ; wcx_decoder_class fixes the callback signature
static void pcie_flush(wcx_decoder *d, void *state) {
    pcie_state *st = state;
    pcie_detector_begin_call(&st->det);
    pcie_detector_end_of_trace(&st->det);
    close_xz_run(st);
    pcie_detector_finish(&st->det, wcx_decoder_now(d));
}

// ── the eight classes ──────────────────────────────────────────────────────

// Transactions one call can produce: a lock emits a whole buffer
// (PCIE_AUTO_LOCK_TRANSACTIONS, SPEC.md §5.5) plus its info transaction; the
// same edge can also end an X/Z run (1), carry an RxStatus (1) and complete
// up to 8 symbols at 2 transactions each (16). 4096 + 32 covers that with
// room to spare.
#define PCIE_MAX_TX_PER_CALL (PCIE_AUTO_LOCK_TRANSACTIONS + 32u)

#define PCIE_CLASS(ident, id_str, name_str, manifest, sigs)          \
    static WcDecoderHandle ident##_create(const char *config_json);  \
    static const wcx_decoder_class ident = {                         \
        .id = (id_str),                                              \
        .display_name = (name_str),                                  \
        .manifest_json = (manifest),                                 \
        .signals = (sigs),                                           \
        .signal_count = SIG_COUNT,                                   \
        .state_size = sizeof(pcie_state),                            \
        .max_transactions_per_call = PCIE_MAX_TX_PER_CALL,           \
        .init = pcie_init,                                           \
        .on_sample = pcie_sample,                                    \
        .on_flush = pcie_flush,                                      \
        .fini = pcie_fini,                                           \
        .create = ident##_create,                                    \
    };                                                               \
    static WcDecoderHandle ident##_create(const char *config_json) { \
        return wcx_decoder_create(&(ident), config_json);            \
    }

PCIE_CLASS(k_pipe_w8, "ferrite.pcie_pipe_w8", "PCIe PIPE (8-bit)", k_manifest_pipe_w8, k_signals_w8)
PCIE_CLASS(k_pipe_w16, "ferrite.pcie_pipe_w16", "PCIe PIPE (16-bit)", k_manifest_pipe_w16,
           k_signals_w16)
PCIE_CLASS(k_pipe_w32, "ferrite.pcie_pipe_w32", "PCIe PIPE (32-bit)", k_manifest_pipe_w32,
           k_signals_w32)
PCIE_CLASS(k_pipe_w64, "ferrite.pcie_pipe_w64", "PCIe PIPE (64-bit)", k_manifest_pipe_w64,
           k_signals_w64)
PCIE_CLASS(k_dll_w8, "ferrite.pcie_dll_w8", "PCIe Data Link Layer (8-bit)", k_manifest_dll_w8,
           k_signals_w8)
PCIE_CLASS(k_dll_w16, "ferrite.pcie_dll_w16", "PCIe Data Link Layer (16-bit)", k_manifest_dll_w16,
           k_signals_w16)
PCIE_CLASS(k_dll_w32, "ferrite.pcie_dll_w32", "PCIe Data Link Layer (32-bit)", k_manifest_dll_w32,
           k_signals_w32)
PCIE_CLASS(k_dll_w64, "ferrite.pcie_dll_w64", "PCIe Data Link Layer (64-bit)", k_manifest_dll_w64,
           k_signals_w64)

static const pcie_variant k_variants[] = {
    {&k_pipe_w8, 1u, false},  {&k_pipe_w16, 2u, false}, {&k_pipe_w32, 4u, false},
    {&k_pipe_w64, 8u, false}, {&k_dll_w8, 1u, true},    {&k_dll_w16, 2u, true},
    {&k_dll_w32, 4u, true},   {&k_dll_w64, 8u, true},
};

static const pcie_variant *find_variant(const wcx_decoder *d) {
    const wcx_decoder_class *cls = wcx_decoder_class_of(d);
    for (size_t i = 0; i < sizeof k_variants / sizeof k_variants[0]; i++) {
        if (k_variants[i].cls == cls) {
            return &k_variants[i];
        }
    }
    return NULL; /* defensive: every registered class is in k_variants */
}

static const wcx_decoder_class *const k_classes[] = {
    &k_pipe_w8, &k_pipe_w16, &k_pipe_w32, &k_pipe_w64,
    &k_dll_w8,  &k_dll_w16,  &k_dll_w32,  &k_dll_w64,
};

WCX_PLUGIN("PCIe decoders (PIPE, Data Link Layer)",
           "Open source, Apache-2.0. Ferrite Engineering, "
           "github.com/Ferrite-Engineering/wavecrux-decoders",
           k_classes)
