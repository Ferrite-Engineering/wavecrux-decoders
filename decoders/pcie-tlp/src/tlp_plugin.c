// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Chili.CHIPS
//
// openPCIE's TLP decoder: one TLP DW per clock, SOP/EOP framed, optionally
// with the opposite link direction to pair completions with requests.
// SPEC.md is the output contract; tlp_format.c builds each TLP's text.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tlp_format.h"
#include "wavecrux_decoder.h"
#include "wcx/arena.h"
#include "wcx/decoder.h"
#include "wcx/edge.h"
#include "wcx/json_writer.h"
#include "wcx/sample.h"
#include "wcx/str.h"

enum {
    SIG_CLK,
    SIG_DATA,
    SIG_SOP,
    SIG_EOP,
    SIG_VALID,
    SIG_PEER_DATA,
    SIG_PEER_SOP,
    SIG_PEER_EOP,
    SIG_PEER_VALID,
    SIG_COUNT
};

static const wcx_signal_spec k_signals[SIG_COUNT] = {
    {"clk", 1, false},     {"tlp_data", 32, false}, {"tlp_sop", 1, false},
    {"tlp_eop", 1, false}, {"tlp_valid", 1, false}, {"peer_data", 32, true},
    {"peer_sop", 1, true}, {"peer_eop", 1, true},   {"peer_valid", 1, true},
};

static const char k_manifest[] =
    "{\"description\":\"PCI Express Transaction Layer Packets, one 32-bit DW per clock with "
    "SOP/EOP framing (openPCIE). Memory, I/O, Configuration, Completion and Message TLPs. Bind "
    "the opposite direction to peer_* to label each Completion with the request it answers.\","
    "\"category\":\"userPlugin\","
    "\"signals\":["
    "{\"name\":\"clk\",\"bit_width\":1,\"description\":\"Stream clock; the rising edge samples "
    "it\"},"
    "{\"name\":\"tlp_data\",\"bit_width\":32,\"description\":\"One TLP DW per beat, byte 0 "
    "(Fmt/Type) in bits [31:24]\"},"
    "{\"name\":\"tlp_sop\",\"bit_width\":1,\"description\":\"First DW of a TLP\"},"
    "{\"name\":\"tlp_eop\",\"bit_width\":1,\"description\":\"Last DW of a TLP\"},"
    "{\"name\":\"tlp_valid\",\"bit_width\":1,\"description\":\"tlp_data carries a DW this "
    "cycle\"}],"
    "\"optional_signals\":["
    "{\"name\":\"peer_data\",\"bit_width\":32,\"description\":\"Opposite direction: DW. Unbound "
    "(any of the four): completions are not paired\"},"
    "{\"name\":\"peer_sop\",\"bit_width\":1,\"description\":\"Opposite direction: first DW\"},"
    "{\"name\":\"peer_eop\",\"bit_width\":1,\"description\":\"Opposite direction: last DW\"},"
    "{\"name\":\"peer_valid\",\"bit_width\":1,\"description\":\"Opposite direction: DW "
    "valid\"}],"
    "\"parameters\":["
    "{\"name\":\"labels\",\"kind\":\"enum\",\"default\":\"compact\","
    "\"enum_values\":[\"compact\",\"full\"],"
    "\"enum_labels\":{\"compact\":\"Compact\",\"full\":\"Full (bus/device, tag, status)\"},"
    "\"display_name\":\"Labels\","
    "\"description\":\"Compact leaves out what a single-device link does not need -- "
    "bus/device/function 01:00.0, the tag, a Successful status -- so the label fits its box. "
    "Everything stays in the transaction's fields.\"},"
    "{\"name\":\"span\",\"kind\":\"enum\",\"default\":\"packet\","
    "\"enum_values\":[\"packet\",\"until_next\"],"
    "\"enum_labels\":{\"packet\":\"Packet duration\",\"until_next\":\"Stretch to next TLP "
    "(overview)\"},"
    "\"display_name\":\"Box width\","
    "\"description\":\"Packet duration draws each TLP as long as it is on the stream. Stretch "
    "to next TLP widens each box up to the next one, so labels stay readable when zoomed "
    "out.\"}]}";

static const char *const k_labels[] = {"compact", "full"};
static const char *const k_spans[] = {"packet", "until_next"};

// What `what` says for an unknown beat, by the first X/Z signal (SPEC.md §4).
static const char *const k_unknown_what[] = {
    "tlp_valid is X or Z",
    "tlp_data is X or Z",
    "tlp_sop is X or Z",
    "tlp_eop is X or Z",
};

#define HELD_LABEL_CAP  256u
#define HELD_FIELDS_CAP 1024u

typedef struct tlp_stream {
    bool open;
    uint64_t start_fs; // SOP edge
    uint64_t last_fs;  // last beat's edge
    uint32_t n;        // DWs received (only the first TLP_MAX_DW are kept)
    uint32_t dw[TLP_MAX_DW];
} tlp_stream;

typedef struct tlp_request {
    bool valid;
    uint32_t requester;
    tlp_match match;
} tlp_request;

typedef enum run_kind { RUN_NONE = 0, RUN_UNKNOWN, RUN_NO_SOP } run_kind;

typedef struct tlp_run {
    run_kind kind;
    const char *what; // static
    uint64_t start_fs;
    uint64_t end_fs;
    uint64_t count;
} tlp_run;

typedef struct tlp_held {
    bool active;
    bool is_error;
    uint64_t start_fs;
    uint64_t end_fs;
    char label[HELD_LABEL_CAP];
    char fields[HELD_FIELDS_CAP];
} tlp_held;

typedef struct tlp_state {
    wcx_edge clk;
    bool full;
    bool stretch;
    bool peer_bound;
    tlp_stream main;
    tlp_stream peer;
    tlp_request req[256]; // by tag (8-bit tags, §2.2.6.2)
    tlp_run run;
    tlp_held held;
} tlp_state;

typedef enum beat_kind { BEAT_NONE, BEAT_UNKNOWN, BEAT_OK } beat_kind;

typedef struct tlp_beat {
    beat_kind kind;
    size_t unknown; // index into k_unknown_what
    uint32_t data;
    bool sop;
    bool eop;
} tlp_beat;

// ── output ──────────────────────────────────────────────────────────────────

// Emits now, or (span = until_next) holds this one and releases the last.
static void commit(wcx_decoder *d, tlp_state *st, uint64_t start_fs, uint64_t end_fs,
                   const char *label, const char *fields, bool is_error) {
    if (!st->stretch) {
        (void)wcx_emit(d, start_fs, end_fs, label, fields, is_error);
        return;
    }
    if (label == NULL || fields == NULL) {
        wcx_decoder_fail(d, "out of memory formatting a TLP");
        return;
    }
    tlp_held *h = &st->held;
    if (h->active) {
        wcx_arena *a = wcx_decoder_arena(d);
        const uint64_t end = start_fs > h->end_fs ? start_fs : h->end_fs;
        if (!wcx_emit(d, h->start_fs, end, wcx_arena_strdup(a, h->label),
                      wcx_arena_strdup(a, h->fields), h->is_error)) {
            return;
        }
    }
    if (!wcx_str_copy(h->label, sizeof h->label, label) ||
        !wcx_str_copy(h->fields, sizeof h->fields, fields)) {
        wcx_decoder_fail(d, "internal decoder error: a TLP label is too long to hold");
        return;
    }
    h->active = true;
    h->is_error = is_error;
    h->start_fs = start_fs;
    h->end_fs = end_fs;
}

// An error with one extra numeric field (SPEC.md §4); `key2` may be NULL.
static void error_tx(wcx_decoder *d, tlp_state *st, uint64_t start_fs, uint64_t end_fs,
                     const char *what, const char *label_suffix, const char *key1, uint64_t v1,
                     const char *key2, uint64_t v2) {
    wcx_arena *a = wcx_decoder_arena(d);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_str(&j, "type", "error");
    wcx_json_add_str(&j, "error", what);
    wcx_json_add_u64(&j, key1, v1);
    if (key2 != NULL) {
        wcx_json_add_u64(&j, key2, v2);
    }
    commit(d, st, start_fs, end_fs, wcx_arena_printf(a, "TLP error: %s%s", what, label_suffix),
           wcx_json_end(&j), true);
}

static void run_end(wcx_decoder *d, tlp_state *st) {
    tlp_run *r = &st->run;
    if (r->kind == RUN_NONE) {
        return;
    }
    const char *suffix = r->count > 1u ? wcx_arena_printf(wcx_decoder_arena(d), " (%llu beats)",
                                                          (unsigned long long)r->count)
                                       : "";
    error_tx(d, st, r->start_fs, r->end_fs, r->what, suffix != NULL ? suffix : "", "occurrences",
             r->count, NULL, 0u);
    r->kind = RUN_NONE;
}

static void run_step(wcx_decoder *d, tlp_state *st, run_kind kind, const char *what, uint64_t t) {
    tlp_run *r = &st->run;
    if (r->kind == kind) {
        r->end_fs = t;
        r->count++;
        return;
    }
    run_end(d, st);
    r->kind = kind;
    r->what = what;
    r->start_fs = t;
    r->end_fs = t;
    r->count = 1u;
}

// ── pairing (SPEC.md §5.3) ──────────────────────────────────────────────────

static void note_request(tlp_state *st, const tlp_stream *s) {
    const uint32_t dw0 = s->dw[0];
    if (s->n < tlp_header_dw(dw0)) {
        return;
    }
    const uint32_t type = (dw0 >> 24) & 0x1Fu;
    const bool has_data = ((dw0 >> 29) & 0x2u) != 0u;
    tlp_match m = {TLP_MATCH_NONE, 0u, 0u};
    if (tlp_kind_of(dw0) == TLP_KIND_CFG) {
        m.kind = has_data ? TLP_MATCH_CFG_WRITE : TLP_MATCH_CFG_READ;
        m.reg = s->dw[2] & 0xFFCu;
    } else if (type == 0x02u) {
        m.kind = has_data ? TLP_MATCH_IO_WRITE : TLP_MATCH_IO_READ;
        m.addr = s->dw[2] & ~3u;
    } else if (tlp_kind_of(dw0) == TLP_KIND_MEM && !has_data) {
        m.kind = TLP_MATCH_MEM;
        m.addr = (tlp_header_dw(dw0) == 4u ? s->dw[3] : s->dw[2]) & ~3u;
    } else {
        return;
    }
    tlp_request *r = &st->req[(s->dw[1] >> 8) & 0xFFu];
    r->valid = true;
    r->requester = s->dw[1] >> 16;
    r->match = m;
}

// The request a completion answers, removed unless more split completions
// follow (byte count beyond this completion's payload).
static tlp_match take_match(tlp_state *st, const tlp_stream *s) {
    const tlp_match none = {TLP_MATCH_NONE, 0u, 0u};
    const uint32_t dw0 = s->dw[0];
    tlp_request *r = &st->req[(s->dw[2] >> 8) & 0xFFu];
    if (!r->valid || r->requester != (s->dw[2] >> 16)) {
        return none;
    }
    const uint32_t len = dw0 & 0x3FFu;
    const uint32_t payload = 4u * (len != 0u ? len : 1024u);
    const bool has_data = ((dw0 >> 29) & 0x2u) != 0u;
    if (!has_data || (s->dw[1] & 0xFFFu) <= payload) {
        r->valid = false;
    }
    return r->match;
}

// ── one TLP ─────────────────────────────────────────────────────────────────

static void decode(wcx_decoder *d, tlp_state *st) {
    const tlp_stream *s = &st->main;
    const uint32_t hdr = tlp_header_dw(s->dw[0]);
    if (s->n < hdr) {
        error_tx(d, st, s->start_fs, s->last_fs, "TLP shorter than its header", "", "expected", hdr,
                 "actual", s->n);
        return;
    }
    tlp_match m = {TLP_MATCH_NONE, 0u, 0u};
    if (tlp_kind_of(s->dw[0]) == TLP_KIND_CPL) {
        m = take_match(st, s);
    }
    tlp_text text;
    tlp_format(wcx_decoder_arena(d), s->dw, s->n, &m, st->full, &text);
    commit(d, st, s->start_fs, s->last_fs, text.label, text.fields, text.is_error);
}

// Adds a beat; true when it ended a TLP.
static bool stream_push(tlp_stream *s, const tlp_beat *b, uint64_t t) {
    if (b->sop) {
        s->open = true;
        s->start_fs = t;
        s->n = 0u;
    }
    if (s->n < TLP_MAX_DW) {
        s->dw[s->n] = b->data;
    }
    if (s->n < UINT32_MAX) {
        s->n++;
    }
    s->last_fs = t;
    if (b->eop) {
        s->open = false;
        return true;
    }
    return false;
}

// ── one edge ────────────────────────────────────────────────────────────────

// `sig` is {valid, data, sop, eop}, the order X/Z is reported in.
static void read_beat(const wcx_layout *l, const uint8_t *bits, const size_t sig[4], tlp_beat *b) {
    uint64_t v[4] = {0};
    bool x[4] = {false};
    for (size_t i = 0; i < 4u; i++) {
        if (!wcx_sample_read(l, bits, sig[i], &v[i], &x[i])) {
            x[i] = true;
        }
    }
    b->kind = BEAT_OK;
    if (x[0]) {
        b->kind = BEAT_UNKNOWN;
        b->unknown = 0;
        return;
    }
    if (v[0] == 0u) {
        b->kind = BEAT_NONE;
        return;
    }
    for (size_t i = 1; i < 4u; i++) {
        if (x[i]) {
            b->kind = BEAT_UNKNOWN;
            b->unknown = i;
            return;
        }
    }
    b->data = (uint32_t)v[1];
    b->sop = v[2] != 0u;
    b->eop = v[3] != 0u;
}

static void peer_edge(tlp_state *st, const wcx_layout *l, const uint8_t *bits, uint64_t t) {
    static const size_t k_sig[4] = {SIG_PEER_VALID, SIG_PEER_DATA, SIG_PEER_SOP, SIG_PEER_EOP};
    tlp_beat b;
    read_beat(l, bits, k_sig, &b);
    if (b.kind == BEAT_UNKNOWN) {
        st->peer.open = false;
        return;
    }
    if (b.kind == BEAT_NONE || (!b.sop && !st->peer.open)) {
        return;
    }
    if (stream_push(&st->peer, &b, t)) {
        note_request(st, &st->peer);
    }
}

static void main_edge(wcx_decoder *d, tlp_state *st, const wcx_layout *l, const uint8_t *bits,
                      uint64_t t) {
    static const size_t k_sig[4] = {SIG_VALID, SIG_DATA, SIG_SOP, SIG_EOP};
    tlp_beat b;
    read_beat(l, bits, k_sig, &b);
    if (b.kind == BEAT_NONE) {
        run_end(d, st);
        return;
    }
    if (b.kind == BEAT_UNKNOWN) {
        run_step(d, st, RUN_UNKNOWN, k_unknown_what[b.unknown], t);
        st->main.open = false;
        return;
    }
    if (!b.sop && !st->main.open) {
        run_step(d, st, RUN_NO_SOP, "data beat without SOP", t);
        return;
    }
    run_end(d, st);
    if (b.sop && st->main.open) {
        error_tx(d, st, st->main.start_fs, st->main.last_fs, "SOP before EOP", "", "dw_count",
                 st->main.n, NULL, 0u);
    }
    if (stream_push(&st->main, &b, t)) {
        note_request(st, &st->main);
        decode(d, st);
    }
}

// ── callbacks ───────────────────────────────────────────────────────────────

static bool tlp_init(wcx_decoder *d, void *state) {
    tlp_state *st = state;
    size_t labels = 0;
    size_t span = 0;
    if (!wcx_param_enum(d, "labels", k_labels, 2, 0, &labels) ||
        !wcx_param_enum(d, "span", k_spans, 2, 0, &span)) {
        return false;
    }
    st->full = labels == 1u;
    st->stretch = span == 1u;
    st->peer_bound =
        wcx_decoder_is_bound(d, SIG_PEER_DATA) && wcx_decoder_is_bound(d, SIG_PEER_SOP) &&
        wcx_decoder_is_bound(d, SIG_PEER_EOP) && wcx_decoder_is_bound(d, SIG_PEER_VALID);
    if (!wcx_edge_init(&st->clk, wcx_decoder_layout(d), SIG_CLK, WCX_SAMPLE_BEFORE_EDGE)) {
        wcx_decoder_fail(d, "out of memory starting the decoder");
        return false;
    }
    return true;
}

static void tlp_sample(wcx_decoder *d, void *state, const wcx_sample *s) {
    tlp_state *st = state;
    if (wcx_edge_step(&st->clk, s->timestamp_fs, s->bits) != WCX_EDGE_RISING) {
        return;
    }
    const uint8_t *bits = wcx_edge_data(&st->clk);
    if (st->peer_bound) {
        peer_edge(st, s->layout, bits, s->timestamp_fs);
    }
    main_edge(d, st, s->layout, bits, s->timestamp_fs);
}

static void tlp_flush(wcx_decoder *d, void *state) {
    tlp_state *st = state;
    run_end(d, st);
    if (st->main.open) {
        error_tx(d, st, st->main.start_fs, st->main.last_fs, "missing EOP at end of capture", "",
                 "dw_count", st->main.n, NULL, 0u);
        st->main.open = false;
    }
    tlp_held *h = &st->held;
    if (h->active) {
        wcx_arena *a = wcx_decoder_arena(d);
        (void)wcx_emit(d, h->start_fs, h->end_fs, wcx_arena_strdup(a, h->label),
                       wcx_arena_strdup(a, h->fields), h->is_error);
        h->active = false;
    }
}

static void tlp_fini(wcx_decoder *d, void *state) {
    (void)d;
    tlp_state *st = state;
    wcx_edge_free(&st->clk);
}

// Per sample: a run that ends, a SOP-before-EOP error and the TLP itself
// (SPEC.md §4, §5). At flush: a run, a missing EOP and the held one.
#define TLP_MAX_PER_CALL 3u

static WcDecoderHandle tlp_down_create(const char *config_json);
static WcDecoderHandle tlp_up_create(const char *config_json);

// The same decoder twice, once per link direction, so the two rows read
// "TLP Downstream" and "TLP Upstream" (a decoder row is named after its
// decoder and cannot be renamed).
static const wcx_decoder_class k_down = {
    .id = "openpcie.pcie_tlp_down",
    .display_name = "TLP Downstream",
    .manifest_json = k_manifest,
    .signals = k_signals,
    .signal_count = SIG_COUNT,
    .state_size = sizeof(tlp_state),
    .max_transactions_per_call = TLP_MAX_PER_CALL,
    .init = tlp_init,
    .on_sample = tlp_sample,
    .on_flush = tlp_flush,
    .fini = tlp_fini,
    .create = tlp_down_create,
};

static const wcx_decoder_class k_up = {
    .id = "openpcie.pcie_tlp_up",
    .display_name = "TLP Upstream",
    .manifest_json = k_manifest,
    .signals = k_signals,
    .signal_count = SIG_COUNT,
    .state_size = sizeof(tlp_state),
    .max_transactions_per_call = TLP_MAX_PER_CALL,
    .init = tlp_init,
    .on_sample = tlp_sample,
    .on_flush = tlp_flush,
    .fini = tlp_fini,
    .create = tlp_up_create,
};

static WcDecoderHandle tlp_down_create(const char *config_json) {
    return wcx_decoder_create(&k_down, config_json);
}

static WcDecoderHandle tlp_up_create(const char *config_json) {
    return wcx_decoder_create(&k_up, config_json);
}

static const wcx_decoder_class *const k_classes[] = {&k_down, &k_up};

WCX_PLUGIN("openPCIE PCIe TLP decoder",
           "Apache-2.0, Chili.CHIPS -- github.com/Ferrite-Engineering/wavecrux-decoders", k_classes)
