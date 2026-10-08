// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The decode loop of _PluginProtocolDecoder.decode()
// (lib/services/decoders/ffi/ffi_decoder_loader_io.dart lines 902-1125).

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "dart.h"
#include "host.h"
#include "sb.h"
#include "wcx/buf.h"
#include "wcx/str.h"

// The loader retries until a call fits; a plugin that keeps asking would
// hang WaveCrux. Past this many retries in one call, wcxhost reports it.
#define MAX_RETRIES 64

// Largest buffer offered for a claimed (wrong) bit_width.
#define MAX_OVERRIDE_BYTES ((size_t)1024u * 1024u)

typedef struct session {
    const wcxh_input *in;
    const wcxh_run_opts *o;
    wcxh_run *r;
    WcDecoderHandle handle;
    WcTransaction *tx_buf;
    size_t tx_len;
    uint8_t *bits;
    size_t bits_len;
    size_t cursors[WCX_MAX_SIGNALS];
    bool stopped;
} session;

static void note(char *slot, size_t cap, unsigned *counter, const char *fmt, va_list ap)
    WCX_PRINTF(4, 0);
static void note(char *slot, size_t cap, unsigned *counter, const char *fmt, va_list ap) {
    if (*counter == 0) {
        WCX_IGNORE(wcx_str_vformat(slot, cap, fmt, ap));
    }
    (*counter)++;
}

static void violation(wcxh_run *r, const char *fmt, ...) WCX_PRINTF(2, 3);
static void violation(wcxh_run *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    note(r->violation, sizeof r->violation, &r->violations, fmt, ap);
    va_end(ap);
}

static void warning(wcxh_run *r, const char *fmt, ...) WCX_PRINTF(2, 3);
static void warning(wcxh_run *r, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    note(r->warning, sizeof r->warning, &r->warnings, fmt, ap);
    va_end(ap);
}

// A breach WaveCrux tolerates: a violation in strict (lifecycle) runs, a
// warning otherwise.
static void soft_issue(session *s, const char *fmt, ...) WCX_PRINTF(2, 3);
static void soft_issue(session *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (s->o->strict) {
        note(s->r->violation, sizeof s->r->violation, &s->r->violations, fmt, ap);
    } else {
        note(s->r->warning, sizeof s->r->warning, &s->r->warnings, fmt, ap);
    }
    va_end(ap);
}

void wcxh_run_opts_default(wcxh_run_opts *o) {
    memset(o, 0, sizeof *o);
    o->slots = 16; // decode() line 980: `var txBufferLen = 16;`
    o->max_samples = SIZE_MAX;
}

static bool alloc_tx(session *s, size_t len) {
    free(s->tx_buf);
    s->tx_len = len;
    s->tx_buf = calloc(len > 0 ? len : 1u, sizeof *s->tx_buf);
    return s->tx_buf != NULL;
}

static char *dup_str(const char *src) {
    const size_t n = strlen(src);
    char *out = malloc(n + 1u);
    if (out != NULL) {
        memcpy(out, src, n + 1u);
    }
    return out;
}

// _drainTransactions (lines 1276-1297): copy the strings out at once, and
// convert femtoseconds back to ticks.
static bool copy_transactions(session *s, size_t count, const char *call) {
    wcxh_txlist *l = &s->r->tx;
    for (size_t i = 0; i < count; i++) {
        const WcTransaction *t = &s->tx_buf[i];
        if (t->label == NULL || t->fields_json == NULL) {
            violation(s->r,
                      "%s returned a transaction with a NULL label or fields_json "
                      "(WaveCrux would crash)",
                      call);
            return false;
        }
        if (!wcx_utf8_valid(t->label, strlen(t->label)) ||
            !wcx_utf8_valid(t->fields_json, strlen(t->fields_json))) {
            violation(s->r,
                      "%s returned ill-formed UTF-8 (WaveCrux drops every transaction "
                      "of the decode)",
                      call);
        }
        if (t->_reserved0 != 0) {
            soft_issue(s, "WcTransaction._reserved0 is not zero");
        }
        wcxh_sb fields = {0};
        const unsigned flags = wcxh_dart_fields(&fields, t->fields_json);
        if ((flags & WCXH_FIELDS_NOT_JSON) != 0) {
            soft_issue(s, "fields_json is not a JSON object (WaveCrux shows {}): %.60s",
                       t->fields_json);
        }
        if ((flags & (WCXH_FIELDS_INEXACT | WCXH_FIELDS_DUPLICATE)) != 0) {
            warning(s->r,
                    "fields_json %.60s has a value WaveCrux shows differently (nested, "
                    "non-integer or duplicate)",
                    t->fields_json);
        }
        if (!wcx_buf_reserve((void **)&l->items, &l->cap, sizeof *l->items, l->count + 1u,
                             SIZE_MAX / sizeof *l->items)) {
            wcxh_sb_free(&fields);
            return false;
        }
        wcxh_tx *out = &l->items[l->count];
        out->start = wcxh_fs_to_ticks_floor(t->start_fs, s->in->fs_per_tick);
        out->end = wcxh_fs_to_ticks_ceil(t->end_fs, s->in->fs_per_tick);
        out->is_error = t->is_error != 0;
        out->label = dup_str(t->label);
        out->fields = wcxh_sb_take(&fields);
        if (out->label == NULL || out->fields == NULL) {
            free(out->label);
            free(out->fields);
            return false;
        }
        l->count++;
    }
    return true;
}

// One feed (sample != NULL) or flush with the loader's retry loop
// (feed: lines 1036-1069; flush: lines 1073-1106).
static void call_plugin(session *s, const WcSample *sample) {
    const char *name = sample != NULL ? "feed" : "flush";
    if (s->o->reset_slots && !alloc_tx(s, s->o->slots)) {
        s->stopped = true;
        return;
    }
    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        size_t count = s->tx_len;
        s->r->calls++;
        const int32_t rc = sample != NULL ? s->in->def->feed(s->handle, sample, s->tx_buf, &count)
                                          : s->in->def->flush(s->handle, s->tx_buf, &count);
        if (rc == WC_DECODER_NEED_MORE_SLOTS) {
            s->r->need_more++;
            if (count <= s->tx_len) {
                violation(s->r,
                          "%s returned NEED_MORE_SLOTS asking for %lu slots after being "
                          "offered %lu",
                          name, (unsigned long)count, (unsigned long)s->tx_len);
            }
            // `txBufferLen = txCountPtr.value <= 0 ? txBufferLen * 2 : value`
            if (!alloc_tx(s, count == 0 ? s->tx_len * 2u : count)) {
                break;
            }
            continue;
        }
        if (rc != WC_DECODER_OK) {
            // The loader logs and returns what it has (lines 1056-1061).
            s->r->stop_rc = rc;
            s->r->stop_call = name;
            s->stopped = true;
            if (rc != WC_DECODER_ERR) {
                violation(s->r, "%s returned %d, which is not a WC_DECODER_* code", name, (int)rc);
            }
            return;
        }
        if (count > s->tx_len) {
            violation(s->r, "%s reported %lu transactions in a buffer of %lu", name,
                      (unsigned long)count, (unsigned long)s->tx_len);
            count = s->tx_len;
        }
        if (!copy_transactions(s, count, name)) {
            s->stopped = true;
        }
        return;
    }
    violation(s->r, "%s kept returning NEED_MORE_SLOTS (WaveCrux would loop forever)", name);
    s->stopped = true;
}

// decode() lines 1003-1034: zero the buffer, pack every bound signal's
// value at `ts` in manifest order, convert the time to femtoseconds.
static void feed_sample(session *s, size_t index) {
    const wcxh_input *in = s->in;
    const uint64_t ts = in->timeline[index];
    const uint32_t claimed = s->o->override_width ? s->o->width : in->bits_per_sample;
    size_t want = wcxh_scratch_bytes(claimed);
    if (s->o->override_width && want > MAX_OVERRIDE_BYTES) {
        want = MAX_OVERRIDE_BYTES;
    }
    if (s->bits == NULL || s->o->fresh_bits || s->bits_len != want) {
        free(s->bits);
        s->bits = calloc(1, want);
        s->bits_len = want;
        if (s->bits == NULL) {
            s->stopped = true;
            return;
        }
    }
    memset(s->bits, 0, s->bits_len);
    uint32_t offset = 0;
    for (size_t sig = 0; sig < in->manifest.signal_count; sig++) {
        if (!in->bound[sig]) {
            continue;
        }
        const uint32_t width = in->manifest.signals[sig].bit_width;
        const char *value = wcxh_vcd_value_at(&in->vcd, in->vcd_sig[sig], ts, &s->cursors[sig]);
        wcxh_pack_value(s->bits, s->bits_len, offset, width, value);
        offset += width;
    }
    WcSample sample;
    memset(&sample, 0, sizeof sample);
    sample.timestamp_fs = wcxh_ticks_to_fs(ts, in->fs_per_tick);
    sample.bits_ptr = s->bits;
    sample.bit_width = claimed;
    call_plugin(s, &sample);
    if (s->o->fresh_bits) {
        free(s->bits);
        s->bits = NULL;
    }
}

static bool session_open(session *s, const wcxh_input *in, const wcxh_run_opts *o, wcxh_run *r) {
    memset(s, 0, sizeof *s);
    memset(r, 0, sizeof *r);
    s->in = in;
    s->o = o;
    s->r = r;
    if (!alloc_tx(s, o->slots)) {
        return false;
    }
    const char *config = o->null_config ? NULL : (o->config != NULL ? o->config : in->config_json);
    s->handle = in->def->create(config);
    r->created = s->handle != NULL;
    return r->created;
}

static void session_close(session *s) {
    if (s->handle != NULL) {
        if (!s->stopped && !s->o->skip_flush) {
            call_plugin(s, NULL);
        }
        // destroy runs exactly once for every handle create returned
        // (decode()'s finally, lines 1107-1116).
        s->in->def->destroy(s->handle);
        s->handle = NULL;
    }
    free(s->tx_buf);
    free(s->bits);
    s->tx_buf = NULL;
    s->bits = NULL;
}

static size_t sample_count(const wcxh_input *in, const wcxh_run_opts *o) {
    return o->max_samples < in->timeline_count ? o->max_samples : in->timeline_count;
}

void wcxh_run_decode(const wcxh_input *in, const wcxh_run_opts *o, wcxh_run *r) {
    session s;
    if (session_open(&s, in, o, r)) {
        const size_t n = sample_count(in, o);
        for (size_t i = 0; i < n && !s.stopped; i++) {
            feed_sample(&s, i);
        }
    }
    session_close(&s);
}

void wcxh_run_pair(const wcxh_input *in, const wcxh_run_opts *o, wcxh_run *a, wcxh_run *b) {
    session sa;
    session sb;
    const bool oa = session_open(&sa, in, o, a);
    const bool ob = session_open(&sb, in, o, b);
    if (oa && ob) {
        const size_t n = sample_count(in, o);
        for (size_t i = 0; i < n; i++) {
            if (!sa.stopped) {
                feed_sample(&sa, i);
            }
            if (!sb.stopped) {
                feed_sample(&sb, i);
            }
        }
    }
    session_close(&sa);
    session_close(&sb);
}

void wcxh_txlist_free(wcxh_txlist *l) {
    if (l == NULL) {
        return;
    }
    for (size_t i = 0; i < l->count; i++) {
        free(l->items[i].label);
        free(l->items[i].fields);
    }
    free(l->items);
    memset(l, 0, sizeof *l);
}

void wcxh_run_free(wcxh_run *r) {
    if (r != NULL) {
        wcxh_txlist_free(&r->tx);
    }
}
