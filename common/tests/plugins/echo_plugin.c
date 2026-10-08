// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Test plugin for the lifecycle tests, the host emulator and the fuzz
// driver. Built and tested by wcx_add_decoder exactly like a real decoder,
// so it also proves the build machinery.
//
// wcx.test.echo   one transaction per rising clock edge carrying the data
//                 bus value at the configured sample point (before the edge
//                 by default). Parameters:
//                   sample_point  "before_edge" | "at_edge"
//                   repeat        1..64 transactions per edge (drives
//                                 NEED_MORE_SLOTS: more than the host's 16
//                                 default slots in one call)
//                   prefix        label prefix, up to 31 bytes (exercises
//                                 escaping)
//                   emit_summary  a summary transaction at flush
//                 When the optional `valid` signal is bound, edges where it
//                 is not 1 are skipped. Data that is X or Z at the sample
//                 point gives an is_error transaction (and decoding goes on).
//
// wcx.test.count  counts samples and rising edges; emits one transaction at
//                 flush. Exercises multi-decoder registration.
//
// Transaction times: start_fs is the edge time, end_fs is one femtosecond
// later, so the host's ceil() rounding of end times is visible in goldens.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wavecrux_decoder.h"
#include "wcx/arena.h"
#include "wcx/decoder.h"
#include "wcx/edge.h"
#include "wcx/json_writer.h"
#include "wcx/sample.h"
#include "wcx/str.h"

// ── wcx.test.echo ───────────────────────────────────────────────────────────

enum { ECHO_SIG_CLK, ECHO_SIG_DATA, ECHO_SIG_VALID, ECHO_SIG_COUNT };

#define ECHO_MAX_REPEAT 64
#define ECHO_PREFIX_CAP 32

static const wcx_signal_spec k_echo_signals[ECHO_SIG_COUNT] = {
    {"clk", 1, false},
    {"data", 8, false},
    {"valid", 1, true},
};

static const char k_echo_manifest[] =
    "{\"description\":\"Test plugin: one transaction per rising clock edge with the data value "
    "at the sample point.\","
    "\"category\":\"userPlugin\","
    "\"signals\":["
    "{\"name\":\"clk\",\"bit_width\":1,\"description\":\"Clock\"},"
    "{\"name\":\"data\",\"bit_width\":8,\"description\":\"Data bus\"}],"
    "\"optional_signals\":["
    "{\"name\":\"valid\",\"bit_width\":1,\"description\":\"Qualifies data when bound\"}],"
    "\"parameters\":["
    "{\"name\":\"sample_point\",\"kind\":\"enum\",\"default\":\"before_edge\","
    "\"enum_values\":[\"before_edge\",\"at_edge\"],"
    "\"enum_labels\":{\"before_edge\":\"Before the edge\",\"at_edge\":\"At the edge\"}},"
    "{\"name\":\"repeat\",\"kind\":\"int\",\"default\":1},"
    "{\"name\":\"prefix\",\"kind\":\"string\",\"default\":\"D\"},"
    "{\"name\":\"emit_summary\",\"kind\":\"bool\",\"default\":true}]}";

static const char *const k_sample_points[] = {"before_edge", "at_edge"};

typedef struct echo_state {
    wcx_edge clk;
    int64_t repeat;
    bool emit_summary;
    char prefix[ECHO_PREFIX_CAP];
    uint64_t edges;
    uint64_t samples;
    uint64_t first_edge_fs;
} echo_state;

static bool echo_init(wcx_decoder *d, void *state) {
    echo_state *st = state;
    size_t point = 0;
    if (!wcx_param_enum(d, "sample_point", k_sample_points, 2, 0, &point) ||
        !wcx_param_int(d, "repeat", 1, 1, ECHO_MAX_REPEAT, &st->repeat) ||
        !wcx_param_string(d, "prefix", "D", st->prefix, sizeof st->prefix) ||
        !wcx_param_bool(d, "emit_summary", true, &st->emit_summary)) {
        return false;
    }
    const wcx_sample_point sp = point == 1 ? WCX_SAMPLE_AT_EDGE : WCX_SAMPLE_BEFORE_EDGE;
    if (!wcx_edge_init(&st->clk, wcx_decoder_layout(d), ECHO_SIG_CLK, sp)) {
        wcx_decoder_fail(d, "out of memory starting the decoder");
        return false;
    }
    return true;
}

static void echo_emit_data(wcx_decoder *d, echo_state *st, uint64_t ts, uint64_t data) {
    wcx_arena *a = wcx_decoder_arena(d);
    char hex[WCX_FMT_HEX_CAP] = {0};
    (void)wcx_fmt_hex(hex, data, 2);
    for (int64_t r = 0; r < st->repeat; r++) {
        const char *label = st->repeat > 1
                                ? wcx_arena_printf(a, "%s=%s #%lld", st->prefix, hex, (long long)r)
                                : wcx_arena_printf(a, "%s=%s", st->prefix, hex);
        wcx_json j;
        wcx_json_begin(&j, a);
        wcx_json_add_str(&j, "data", hex);
        wcx_json_add_u64(&j, "edge", st->edges);
        wcx_json_add_i64(&j, "repeat", r);
        if (!wcx_emit(d, ts, ts + 1u, label, wcx_json_end(&j), false)) {
            return;
        }
    }
}

static void echo_sample(wcx_decoder *d, void *state, const wcx_sample *s) {
    echo_state *st = state;
    st->samples++;
    if (wcx_edge_step(&st->clk, s->timestamp_fs, s->bits) != WCX_EDGE_RISING) {
        return;
    }
    st->edges++;
    if (st->edges == 1) {
        st->first_edge_fs = s->timestamp_fs;
    }
    const uint8_t *at = wcx_edge_data(&st->clk);
    if (wcx_decoder_is_bound(d, ECHO_SIG_VALID)) {
        bool valid = false;
        bool valid_x = false;
        if (!wcx_sample_read_bit(s->layout, at, ECHO_SIG_VALID, 0, &valid, &valid_x) || !valid ||
            valid_x) {
            return;
        }
    }
    uint64_t data = 0;
    bool unknown = false;
    if (!wcx_sample_read(s->layout, at, ECHO_SIG_DATA, &data, &unknown)) {
        wcx_decoder_fail(d, "internal decoder error: data signal unreadable");
        return;
    }
    if (unknown) {
        wcx_json j;
        wcx_json_begin(&j, wcx_decoder_arena(d));
        wcx_json_add_u64(&j, "edge", st->edges);
        (void)wcx_emit(d, s->timestamp_fs, s->timestamp_fs + 1u,
                       "data is X or Z at the rising edge", wcx_json_end(&j), true);
        return;
    }
    echo_emit_data(d, st, s->timestamp_fs, data);
}

static void echo_flush(wcx_decoder *d, void *state) {
    echo_state *st = state;
    if (!st->emit_summary) {
        return;
    }
    wcx_arena *a = wcx_decoder_arena(d);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_u64(&j, "edges", st->edges);
    wcx_json_add_u64(&j, "samples", st->samples);
    (void)wcx_emit(d, st->first_edge_fs, wcx_decoder_now(d),
                   wcx_arena_printf(a, "edges=%llu", (unsigned long long)st->edges),
                   wcx_json_end(&j), false);
}

static void echo_fini(wcx_decoder *d, void *state) {
    (void)d;
    echo_state *st = state;
    wcx_edge_free(&st->clk);
}

static WcDecoderHandle echo_create(const char *config_json);

static const wcx_decoder_class k_echo_class = {
    .id = "wcx.test.echo",
    .display_name = "Test echo",
    .manifest_json = k_echo_manifest,
    .signals = k_echo_signals,
    .signal_count = ECHO_SIG_COUNT,
    .state_size = sizeof(echo_state),
    // ECHO_MAX_REPEAT transactions per rising edge, one edge per sample.
    .max_transactions_per_call = ECHO_MAX_REPEAT,
    .init = echo_init,
    .on_sample = echo_sample,
    .on_flush = echo_flush,
    .fini = echo_fini,
    .create = echo_create,
};

static WcDecoderHandle echo_create(const char *config_json) {
    return wcx_decoder_create(&k_echo_class, config_json);
}

// ── wcx.test.count ──────────────────────────────────────────────────────────

enum { COUNT_SIG_CLK, COUNT_SIG_COUNT };

static const wcx_signal_spec k_count_signals[COUNT_SIG_COUNT] = {{"clk", 1, false}};

static const char k_count_manifest[] =
    "{\"description\":\"Test plugin: counts samples and rising edges.\","
    "\"signals\":[{\"name\":\"clk\"}],\"parameters\":[]}";

typedef struct count_state {
    wcx_edge clk;
    uint64_t samples;
    uint64_t rising;
} count_state;

static bool count_init(wcx_decoder *d, void *state) {
    count_state *st = state;
    if (!wcx_edge_init(&st->clk, wcx_decoder_layout(d), COUNT_SIG_CLK, WCX_SAMPLE_AT_EDGE)) {
        wcx_decoder_fail(d, "out of memory starting the decoder");
        return false;
    }
    return true;
}

static void count_sample(wcx_decoder *d, void *state, const wcx_sample *s) {
    (void)d;
    count_state *st = state;
    st->samples++;
    if (wcx_edge_step(&st->clk, s->timestamp_fs, s->bits) == WCX_EDGE_RISING) {
        st->rising++;
    }
}

static void count_flush(wcx_decoder *d, void *state) {
    count_state *st = state;
    wcx_arena *a = wcx_decoder_arena(d);
    wcx_json j;
    wcx_json_begin(&j, a);
    wcx_json_add_u64(&j, "samples", st->samples);
    wcx_json_add_u64(&j, "rising", st->rising);
    (void)wcx_emit(d, 0, wcx_decoder_now(d),
                   wcx_arena_printf(a, "samples=%llu rising=%llu", (unsigned long long)st->samples,
                                    (unsigned long long)st->rising),
                   wcx_json_end(&j), false);
}

static void count_fini(wcx_decoder *d, void *state) {
    (void)d;
    count_state *st = state;
    wcx_edge_free(&st->clk);
}

static WcDecoderHandle count_create(const char *config_json);

static const wcx_decoder_class k_count_class = {
    .id = "wcx.test.count",
    .display_name = "Test count",
    .manifest_json = k_count_manifest,
    .signals = k_count_signals,
    .signal_count = COUNT_SIG_COUNT,
    .state_size = sizeof(count_state),
    .max_transactions_per_call = 1, // only flush emits, once
    .init = count_init,
    .on_sample = count_sample,
    .on_flush = count_flush,
    .fini = count_fini,
    .create = count_create,
};

static WcDecoderHandle count_create(const char *config_json) {
    return wcx_decoder_create(&k_count_class, config_json);
}

// ── entry points ────────────────────────────────────────────────────────────

static const wcx_decoder_class *const k_classes[] = {&k_echo_class, &k_count_class};

WCX_PLUGIN("WaveCrux decoders test plugin", "Test fixture; not for use in WaveCrux.", k_classes)
