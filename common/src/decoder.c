// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The call state machine is the one documented in wcx/emit.h, plus the
// phases below. One handle is used by one thread at a time (ABI threading
// model); nothing here is global.

#include "wcx/decoder.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "wcx/assert.h"
#include "wcx/config.h"
#include "wcx/emit.h"
#include "wcx/json_parse.h"
#include "wcx/json_writer.h"
#include "wcx/manifest.h"
#include "wcx/str.h"

#define WCX_DECODER_MAGIC 0x57435844u // "WCXD"

// Escaping can expand each message byte to at most 6 bytes.
#define ERR_FIELDS_CAP (WCX_ERROR_MAX * 6u + 32u)

typedef enum call_phase {
    PHASE_IDLE,          // ready for the next sample
    PHASE_PENDING_FEED,  // a feed returned NEED_MORE_SLOTS; expecting its retry
    PHASE_PENDING_FLUSH, // a flush returned NEED_MORE_SLOTS; expecting its retry
    PHASE_FLUSHED        // flush delivered; the stream is over
} call_phase;

struct wcx_decoder {
    uint32_t magic;
    const wcx_decoder_class *cls;
    void *state;
    wcx_config config; // live during create only
    wcx_layout layout;
    wcx_arena arena;
    wcx_emit_queue queue;
    call_phase phase;
    uint64_t pending_fp; // fingerprint of the sample whose batch is pending
    bool have_ts;
    uint64_t last_ts;
    uint64_t now_fs;
    bool in_init;
    bool in_call;
    bool batch_delivered; // begin_call found no pending batch (see wcx_decoder_batch_delivered)
    bool init_called;
    bool failed;
    bool error_emitted;
    char err_label[WCX_ERROR_MAX];
    char err_fields[ERR_FIELDS_CAP];
};

static const char k_fallback_fields[] = "{\"error\":\"decoder error\"}";

static wcx_decoder *from_handle(WcDecoderHandle h) {
    wcx_decoder *d = h;
    if (d == NULL || d->magic != WCX_DECODER_MAGIC) {
        return NULL;
    }
    return d;
}

// ── error state ─────────────────────────────────────────────────────────────

static void emit_error_if_needed(wcx_decoder *d) {
    if (!d->failed || d->error_emitted || !d->in_call) {
        return;
    }
    d->error_emitted =
        wcx_emit_push_reserved(&d->queue, d->now_fs, d->now_fs, d->err_label, d->err_fields, true);
}

static void fail_with(wcx_decoder *d, const char *msg) {
    if (d->failed) {
        return;
    }
    d->failed = true;
    WCX_IGNORE(wcx_str_copy(d->err_label, sizeof d->err_label, msg));
    wcx_json j;
    wcx_json_begin_fixed(&j, d->err_fields, sizeof d->err_fields);
    wcx_json_add_str(&j, "error", d->err_label);
    if (wcx_json_end(&j) == NULL) {
        WCX_IGNORE(wcx_str_copy(d->err_fields, sizeof d->err_fields, k_fallback_fields));
    }
    emit_error_if_needed(d);
}

void wcx_decoder_fail(wcx_decoder *d, const char *fmt, ...) {
    if (d == NULL || d->failed) {
        return;
    }
    char msg[WCX_ERROR_MAX] = {0};
    va_list ap;
    va_start(ap, fmt);
    WCX_IGNORE(wcx_str_vformat(msg, sizeof msg, fmt, ap));
    va_end(ap);
    fail_with(d, msg);
}

bool wcx_decoder_failed(const wcx_decoder *d) {
    return d == NULL || d->failed;
}

// ── create / destroy ────────────────────────────────────────────────────────

static void destroy_decoder(wcx_decoder *d) {
    if (d->init_called && d->cls->fini != NULL) {
        d->cls->fini(d, d->state);
    }
    wcx_config_free(&d->config);
    wcx_emit_free(&d->queue);
    wcx_arena_free(&d->arena);
    free(d->state);
    d->magic = 0;
    free(d);
}

// The C signal table must say what manifest_json tells the host.
static void check_manifest(wcx_decoder *d) {
    wcx_manifest *m = calloc(1, sizeof *m);
    if (m == NULL) {
        fail_with(d, "out of memory checking the decoder manifest");
        return;
    }
    wcx_error err = {{0}};
    if (!wcx_manifest_parse(m, d->cls->manifest_json, &err) ||
        !wcx_manifest_check_signals(m, d->cls->signals, d->cls->signal_count, &err)) {
        wcx_decoder_fail(d, "internal decoder error: %s", err.msg);
    }
    wcx_manifest_free(m);
    free(m);
}

static void configure(wcx_decoder *d, const char *config_json) {
    wcx_error err = {{0}};
    check_manifest(d);
    if (d->failed) {
        return;
    }
    if (!wcx_config_parse(&d->config, config_json, &err)) {
        fail_with(d, err.msg);
        return;
    }
    if (wcx_config_has_decoder_id(&d->config) &&
        !wcx_config_decoder_id_is(&d->config, d->cls->id)) {
        wcx_decoder_fail(d, "configuration names a different decoder than \"%s\"", d->cls->id);
        return;
    }
    if (!wcx_layout_build(&d->layout, d->cls->signals, d->cls->signal_count, &d->config, &err)) {
        fail_with(d, err.msg);
        return;
    }
    if (d->cls->init != NULL) {
        d->init_called = true;
        d->in_init = true;
        const bool ok = d->cls->init(d, d->state);
        d->in_init = false;
        if (!ok && !d->failed) {
            fail_with(d, "the decoder could not start with this configuration");
        }
    }
}

WcDecoderHandle wcx_decoder_create(const wcx_decoder_class *cls, const char *config_json) {
    if (cls == NULL || cls->on_sample == NULL) {
        return NULL;
    }
    wcx_decoder *d = calloc(1, sizeof *d);
    if (d == NULL) {
        return NULL;
    }
    d->magic = WCX_DECODER_MAGIC;
    d->cls = cls;
    d->config.decoder_id = WCX_JSON_NONE;
    d->config.bindings = WCX_JSON_NONE;
    d->config.parameters = WCX_JSON_NONE;
    d->config.options = WCX_JSON_NONE;
    const size_t ceiling = cls->max_transactions_per_call != 0 ? cls->max_transactions_per_call
                                                               : WCX_DEFAULT_MAX_TRANSACTIONS;
    const size_t block =
        cls->arena_block_bytes != 0 ? cls->arena_block_bytes : WCX_DEFAULT_ARENA_BLOCK;
    const size_t max = cls->arena_max_bytes != 0 ? cls->arena_max_bytes : WCX_DEFAULT_ARENA_MAX;
    d->state = calloc(1, cls->state_size != 0 ? cls->state_size : 1u);
    if (d->state == NULL || !wcx_emit_init(&d->queue, ceiling) ||
        !wcx_arena_init(&d->arena, block < max ? block : max, max)) {
        destroy_decoder(d);
        return NULL;
    }
    configure(d, config_json);
    wcx_config_free(&d->config);
    return d;
}

void wcx_decoder_destroy(WcDecoderHandle handle) {
    wcx_decoder *d = from_handle(handle);
    if (d != NULL) {
        destroy_decoder(d);
    }
}

// ── feed / flush ────────────────────────────────────────────────────────────

// FNV-1a over what identifies a sample: timestamp, width and, when the width
// is the expected one, the bytes the layout covers (never more, so a lying
// bit_width cannot make this read past the buffer).
static uint64_t fingerprint(const wcx_decoder *d, const WcSample *s) {
    uint64_t h = 0xcbf29ce484222325u;
    if (s == NULL) {
        return h;
    }
    unsigned char head[12] = {0};
    for (unsigned i = 0; i < 8u; i++) {
        head[i] = (unsigned char)(s->timestamp_fs >> (8u * i));
    }
    for (unsigned i = 0; i < 4u; i++) {
        head[8u + i] = (unsigned char)(s->bit_width >> (8u * i));
    }
    for (size_t i = 0; i < sizeof head; i++) {
        h = (h ^ head[i]) * 0x100000001b3u;
    }
    if (s->bit_width == d->layout.total_bits && s->bits_ptr != NULL) {
        for (size_t i = 0; i < d->layout.encoded_bytes; i++) {
            h = (h ^ s->bits_ptr[i]) * 0x100000001b3u;
        }
    }
    return h;
}

// Starts a call that is not a retry: when the host has copied the previous
// batch, the arena is reset now, and only now. The decoder can ask whether
// that happened (wcx_decoder_batch_delivered) to time the release of any
// storage of its own that the pending batch points into.
static void begin_call(wcx_decoder *d) {
    d->batch_delivered = wcx_emit_begin(&d->queue);
    if (d->batch_delivered) {
        wcx_arena_reset(&d->arena);
    }
    d->in_call = true;
}

static bool validate_sample(wcx_decoder *d, const WcSample *s) {
    if (d->failed) {
        return false;
    }
    if (d->phase == PHASE_FLUSHED) {
        fail_with(d, "the host fed a sample after flush");
        return false;
    }
    wcx_error err = {{0}};
    if (s == NULL || !wcx_layout_check_sample(&d->layout, s, &err)) {
        fail_with(d, s == NULL ? "the host passed no sample" : err.msg);
        return false;
    }
    if (d->have_ts && s->timestamp_fs < d->last_ts) {
        wcx_decoder_fail(d, "sample timestamps went backwards (%llu fs after %llu fs)",
                         (unsigned long long)s->timestamp_fs, (unsigned long long)d->last_ts);
        return false;
    }
    return true;
}

int32_t wcx_decoder_feed(WcDecoderHandle handle, const WcSample *sample,
                         WcTransaction *out_transactions, size_t *inout_count) {
    wcx_decoder *d = from_handle(handle);
    if (d == NULL || inout_count == NULL) {
        return WC_DECODER_ERR;
    }
    if (d->phase == PHASE_PENDING_FEED && fingerprint(d, sample) == d->pending_fp) {
        // The host's retry with a larger buffer: deliver, do not decode.
        const int32_t rc = wcx_emit_drain(&d->queue, out_transactions, inout_count);
        d->phase = rc == WC_DECODER_NEED_MORE_SLOTS ? PHASE_PENDING_FEED : PHASE_IDLE;
        return rc;
    }
    begin_call(d);
    if (sample != NULL) {
        d->now_fs = sample->timestamp_fs;
    }
    if (validate_sample(d, sample) && sample != NULL) {
        d->last_ts = sample->timestamp_fs;
        d->have_ts = true;
        const wcx_sample view = {sample->timestamp_fs, sample->bits_ptr, &d->layout};
        d->cls->on_sample(d, d->state, &view);
    }
    emit_error_if_needed(d);
    d->in_call = false;
    const int32_t rc = wcx_emit_drain(&d->queue, out_transactions, inout_count);
    if (rc == WC_DECODER_NEED_MORE_SLOTS) {
        d->phase = PHASE_PENDING_FEED;
        d->pending_fp = fingerprint(d, sample);
    } else if (d->phase != PHASE_FLUSHED) {
        d->phase = PHASE_IDLE;
    }
    return rc;
}

int32_t wcx_decoder_flush(WcDecoderHandle handle, WcTransaction *out_transactions,
                          size_t *inout_count) {
    wcx_decoder *d = from_handle(handle);
    if (d == NULL || inout_count == NULL) {
        return WC_DECODER_ERR;
    }
    if (d->phase != PHASE_PENDING_FLUSH) {
        const bool already_flushed = d->phase == PHASE_FLUSHED;
        begin_call(d);
        d->now_fs = d->last_ts;
        if (!already_flushed && !d->failed && d->cls->on_flush != NULL) {
            d->cls->on_flush(d, d->state);
        }
        emit_error_if_needed(d);
        d->in_call = false;
    }
    const int32_t rc = wcx_emit_drain(&d->queue, out_transactions, inout_count);
    d->phase = rc == WC_DECODER_NEED_MORE_SLOTS ? PHASE_PENDING_FLUSH : PHASE_FLUSHED;
    return rc;
}

int32_t wcx_register(const wcx_decoder_class *const *classes, size_t count, WcDecoderDef *out_defs,
                     size_t *inout_count) {
    if (inout_count == NULL || (count > 0 && classes == NULL)) {
        return WC_DECODER_ERR;
    }
    if (out_defs == NULL || *inout_count < count) {
        *inout_count = count;
        return WC_DECODER_NEED_MORE_SLOTS;
    }
    for (size_t i = 0; i < count; i++) {
        const wcx_decoder_class *cls = classes[i];
        if (cls == NULL || cls->create == NULL) {
            return WC_DECODER_ERR;
        }
        memset(&out_defs[i], 0, sizeof out_defs[i]);
        out_defs[i].id = cls->id;
        out_defs[i].display_name = cls->display_name;
        out_defs[i].manifest_json = cls->manifest_json;
        out_defs[i].create = cls->create;
        out_defs[i].feed = wcx_decoder_feed;
        out_defs[i].flush = wcx_decoder_flush;
        out_defs[i].destroy = wcx_decoder_destroy;
    }
    *inout_count = count;
    return WC_DECODER_OK;
}

// ── helpers for callbacks ───────────────────────────────────────────────────

// The string itself when it is well-formed UTF-8, else an arena copy with
// each bad byte replaced by U+FFFD; NULL when that copy cannot be made.
static const char *clean_utf8(wcx_decoder *d, const char *s) {
    const size_t n = strlen(s);
    if (wcx_utf8_valid(s, n)) {
        return s;
    }
    char *out = wcx_arena_alloc(&d->arena, n * 3u + 1u);
    if (out == NULL) {
        return NULL;
    }
    size_t w = 0;
    size_t i = 0;
    while (i < n) {
        const size_t len = wcx_utf8_seq_len((const unsigned char *)s + i, n - i);
        if (len == 0) {
            memcpy(out + w, "\xEF\xBF\xBD", 3);
            w += 3;
            i++;
        } else {
            memcpy(out + w, s + i, len);
            w += len;
            i += len;
        }
    }
    out[w] = '\0';
    return out;
}

bool wcx_emit(wcx_decoder *d, uint64_t start_fs, uint64_t end_fs, const char *label,
              const char *fields_json, bool is_error) {
    if (d == NULL || d->failed) {
        return false;
    }
    if (!d->in_call) {
        fail_with(d, "internal decoder error: wcx_emit called outside on_sample/on_flush");
        return false;
    }
    if (label == NULL || fields_json == NULL) {
        wcx_decoder_fail(d, "the decoder ran out of string space (limit %lu bytes per sample)",
                         (unsigned long)d->arena.ceiling);
        return false;
    }
    label = clean_utf8(d, label);
    fields_json = clean_utf8(d, fields_json);
    if (label == NULL || fields_json == NULL) {
        wcx_decoder_fail(d, "the decoder ran out of string space (limit %lu bytes per sample)",
                         (unsigned long)d->arena.ceiling);
        return false;
    }
#ifdef WCX_ENABLE_ASSERTS
    {
        size_t ntok = 0;
        const size_t flen = strlen(fields_json);
        WCX_ASSERT(wcx_json_tokenize(fields_json, flen, NULL, SIZE_MAX, &ntok, NULL));
        WCX_ASSERT(flen > 0 && fields_json[0] == '{');
    }
#endif
    if (!wcx_emit_push(&d->queue, start_fs, end_fs, label, fields_json, is_error)) {
        wcx_decoder_fail(d, "more than %lu transactions from one sample; decoding stopped",
                         (unsigned long)d->queue.ceiling);
        return false;
    }
    return true;
}

static bool param_guard(wcx_decoder *d) {
    if (d == NULL) {
        return false;
    }
    if (!d->in_init) {
        fail_with(d, "internal decoder error: parameters can only be read in init");
        return false;
    }
    return !d->failed;
}

bool wcx_param_int(wcx_decoder *d, const char *name, int64_t def, int64_t min, int64_t max,
                   int64_t *out) {
    wcx_error err = {{0}};
    if (!param_guard(d)) {
        return false;
    }
    if (!wcx_config_int(&d->config, name, def, min, max, out, &err)) {
        fail_with(d, err.msg);
        return false;
    }
    return true;
}

bool wcx_param_bool(wcx_decoder *d, const char *name, bool def, bool *out) {
    wcx_error err = {{0}};
    if (!param_guard(d)) {
        return false;
    }
    if (!wcx_config_bool(&d->config, name, def, out, &err)) {
        fail_with(d, err.msg);
        return false;
    }
    return true;
}

bool wcx_param_string(wcx_decoder *d, const char *name, const char *def, char *buf, size_t cap) {
    wcx_error err = {{0}};
    if (!param_guard(d)) {
        return false;
    }
    if (!wcx_config_string(&d->config, name, def, buf, cap, &err)) {
        fail_with(d, err.msg);
        return false;
    }
    return true;
}

bool wcx_param_enum(wcx_decoder *d, const char *name, const char *const *values, size_t count,
                    size_t def_index, size_t *out_index) {
    wcx_error err = {{0}};
    if (!param_guard(d)) {
        return false;
    }
    if (!wcx_config_enum(&d->config, name, values, count, def_index, out_index, &err)) {
        fail_with(d, err.msg);
        return false;
    }
    return true;
}

const wcx_layout *wcx_decoder_layout(const wcx_decoder *d) {
    return d != NULL ? &d->layout : NULL;
}

wcx_arena *wcx_decoder_arena(wcx_decoder *d) {
    return d != NULL ? &d->arena : NULL;
}

bool wcx_decoder_is_bound(const wcx_decoder *d, size_t sig) {
    return d != NULL && wcx_signal_bound(&d->layout, sig);
}

uint64_t wcx_decoder_now(const wcx_decoder *d) {
    return d != NULL ? d->now_fs : 0;
}

bool wcx_decoder_batch_delivered(const wcx_decoder *d) {
    return d != NULL && d->batch_delivered;
}

const wcx_decoder_class *wcx_decoder_class_of(const wcx_decoder *d) {
    return d != NULL ? d->cls : NULL;
}
