// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "wcx/fuzz_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wcx/buf.h"
#include "wcx/json_parse.h"
#include "wcx/json_writer.h"
#include "wcx/manifest.h"
#include "wcx/str.h"

// WaveCrux's scratch-buffer minimum (loader decode(): `encodedBytes < 32 ? 32`).
#define HOST_MIN_SCRATCH 32u
#define DEFAULT_SLOTS    16u
#define MAX_DECODERS     256u

typedef struct reader {
    const uint8_t *data;
    size_t size;
    size_t pos;
} reader;

typedef struct session {
    const WcDecoderDef *def;
    WcDecoderHandle handle;
    bool tiny_slots;
    bool stopped; // the plugin returned WC_DECODER_ERR; a host stops feeding
} session;

// _Noreturn, so static analysers (MSVC /analyze in particular) know a check
// that calls this ends the path.
_Noreturn static void violation(const char *what) {
    (void)fputs("wcx fuzz driver: ABI contract violation: ", stderr);
    (void)fputs(what, stderr);
    (void)fputs("\n", stderr);
    abort();
}

static uint8_t read_u8(reader *r) {
    if (r->pos >= r->size) {
        return 0;
    }
    return r->data[r->pos++];
}

static uint64_t read_varint(reader *r) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 10u && r->pos < r->size; i++) {
        const uint8_t b = r->data[r->pos++];
        v |= (uint64_t)(b & 0x7Fu) << (7u * i);
        if ((b & 0x80u) == 0) {
            break;
        }
    }
    return v;
}

static void check_transactions(const WcTransaction *tx, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (tx[i].label == NULL || tx[i].fields_json == NULL) {
            violation("NULL label or fields_json");
        }
        if (tx[i]._reserved0 != 0) {
            violation("WcTransaction._reserved0 is not zero");
        }
        const size_t llen = strlen(tx[i].label);
        const size_t flen = strlen(tx[i].fields_json);
        if (!wcx_utf8_valid(tx[i].label, llen) || !wcx_utf8_valid(tx[i].fields_json, flen)) {
            violation("label or fields_json is not valid UTF-8");
        }
        size_t count = 0;
        if (!wcx_json_tokenize(tx[i].fields_json, flen, NULL, SIZE_MAX, &count, NULL) ||
            tx[i].fields_json[0] != '{') {
            violation("fields_json is not a JSON object");
        }
    }
}

// One feed (sample != NULL) or flush, honouring NEED_MORE_SLOTS like the
// host: the retry offers exactly the count asked for and must get exactly
// that many transactions.
static void call_plugin(session *s, const WcSample *sample) {
    if (s->stopped) {
        return;
    }
    size_t slots = s->tiny_slots ? 1u : DEFAULT_SLOTS;
    for (int attempt = 0; attempt < 2; attempt++) {
        WcTransaction *out = calloc(slots > 0 ? slots : 1u, sizeof *out);
        if (out == NULL) {
            abort();
        }
        size_t count = slots;
        const int32_t rc = sample != NULL ? s->def->feed(s->handle, sample, out, &count)
                                          : s->def->flush(s->handle, out, &count);
        if (rc == WC_DECODER_OK) {
            if (count > slots || (attempt == 1 && count != slots)) {
                violation("a retry did not deliver exactly the count NEED_MORE_SLOTS asked for");
            }
            check_transactions(out, count);
            free(out);
            return;
        }
        free(out);
        if (rc == WC_DECODER_ERR) {
            s->stopped = true;
            return;
        }
        if (rc != WC_DECODER_NEED_MORE_SLOTS || attempt == 1 || count <= slots) {
            violation("NEED_MORE_SLOTS must ask for more slots than offered, once");
        }
        slots = count;
    }
}

// The template config: every required signal (and, on request, every
// optional one) bound, `params` as the "parameters" value.
static char *template_config(const WcDecoderDef *def, const wcx_manifest *m, bool optional,
                             const char *params, size_t params_len) {
    // A 255-byte name escapes to at most 6 bytes per byte.
    const size_t cap = params_len + 2048u + m->signal_count * 1600u;
    char *buf = malloc(cap);
    if (buf == NULL) {
        abort();
    }
    wcx_json j;
    wcx_json_begin_fixed(&j, buf, cap);
    wcx_json_write_raw(&j, "\"decoder_id\":", 13);
    wcx_json_write_string(&j, def->id, strlen(def->id));
    wcx_json_write_raw(&j, ",\"signal_bindings\":{", 20);
    bool first = true;
    for (size_t i = 0; i < m->signal_count; i++) {
        if (m->signals[i].optional && !optional) {
            continue;
        }
        char name[256] = {0};
        char ref[WCX_FMT_U64_CAP] = {0};
        WCX_IGNORE(wcx_manifest_text(m, m->signals[i].name, name, sizeof name));
        const size_t rlen = wcx_fmt_u64(ref, i);
        wcx_json_write_raw(&j, first ? "" : ",", first ? 0u : 1u);
        first = false;
        wcx_json_write_string(&j, name, strlen(name));
        wcx_json_write_raw(&j, ":", 1);
        wcx_json_write_string(&j, ref, rlen);
    }
    wcx_json_write_raw(&j, "},\"parameters\":", 15);
    if (params_len > 0) {
        wcx_json_write_raw(&j, params, params_len);
    } else {
        wcx_json_write_raw(&j, "{}", 2);
    }
    wcx_json_write_raw(&j, ",\"options\":{}", 13);
    if (wcx_json_end(&j) == NULL) {
        abort();
    }
    return buf;
}

static uint32_t expected_width(const wcx_manifest *m, bool optional) {
    uint32_t total = 0;
    for (size_t i = 0; i < m->signal_count; i++) {
        if (!m->signals[i].optional || optional) {
            total += m->signals[i].bit_width;
        }
    }
    return total;
}

static void feed_records(session *s, reader *r, uint32_t width) {
    uint64_t ts = 0;
    for (unsigned n = 0; n < WCX_FUZZ_MAX_SAMPLES && r->pos < r->size && !s->stopped; n++) {
        const uint8_t flags = read_u8(r);
        const uint64_t dt = read_varint(r);
        uint64_t w = width;
        if ((flags & WCX_FUZZ_WIDTH_ABS) != 0) {
            uint32_t abs = 0;
            for (unsigned k = 0; k < 4u; k++) {
                abs |= (uint32_t)read_u8(r) << (8u * k);
            }
            w = abs % WCX_FUZZ_MAX_WIDTH;
        } else if ((flags & WCX_FUZZ_WIDTH_DELTA) != 0) {
            const int delta = (int)(int8_t)read_u8(r);
            w = delta < 0 && (uint64_t)(-delta) > w ? 0u : (uint64_t)((int64_t)w + delta);
        }
        if ((flags & WCX_FUZZ_BACKWARDS) != 0) {
            ts = dt > ts ? 0u : ts - dt;
        } else {
            ts = dt > UINT64_MAX - ts ? UINT64_MAX : ts + dt;
        }
        const size_t nbytes = (size_t)((w * 2u + 7u) / 8u);
        const size_t bytes = nbytes < HOST_MIN_SCRATCH ? HOST_MIN_SCRATCH : nbytes;
        uint8_t *bits = calloc(1, bytes);
        if (bits == NULL) {
            abort();
        }
        const size_t avail = r->size - r->pos;
        const size_t take = avail < nbytes ? avail : nbytes;
        if (take > 0) {
            memcpy(bits, r->data + r->pos, take);
        }
        r->pos += take;
        WcSample sample;
        memset(&sample, 0, sizeof sample);
        sample.timestamp_fs = ts;
        sample.bits_ptr = bits;
        sample.bit_width = (uint32_t)w;
        call_plugin(s, &sample);
        free(bits);
    }
}

static void run_decoder(const WcDecoderDef *def, reader *r, uint8_t flags) {
    wcx_manifest *m = calloc(1, sizeof *m);
    if (m == NULL) {
        abort();
    }
    if (!wcx_manifest_parse(m, def->manifest_json, NULL)) {
        violation("the decoder's manifest_json does not parse with WaveCrux's rules");
    }
    size_t len = (size_t)read_u8(r);
    len |= (size_t)read_u8(r) << 8u;
    if (len > r->size - r->pos) {
        len = r->size - r->pos;
    }
    const char *cfg_bytes = (const char *)r->data + r->pos;
    r->pos += len;
    const bool optional = (flags & WCX_FUZZ_BIND_OPTIONAL) != 0;
    char *config = NULL;
    if ((flags & WCX_FUZZ_RAW_CONFIG) != 0) {
        config = calloc(1, len + 1u);
        if (config == NULL) {
            abort();
        }
        if (len > 0) {
            memcpy(config, cfg_bytes, len);
        }
    } else {
        config = template_config(def, m, optional, cfg_bytes, len);
    }
    session s = {def, def->create(config), (flags & WCX_FUZZ_TINY_SLOTS) != 0, false};
    free(config);
    if (s.handle != NULL) {
        feed_records(&s, r, expected_width(m, optional));
        call_plugin(&s, NULL);
        def->destroy(s.handle);
    }
    wcx_manifest_free(m);
    free(m);
}

int wcx_fuzz_run(const uint8_t *data, size_t size, wcx_register_fn register_fn) {
    reader r = {data, size, 0};
    size_t count = 0;
    int32_t rc = register_fn(NULL, &count);
    if ((rc != WC_DECODER_OK && rc != WC_DECODER_NEED_MORE_SLOTS) || count == 0 ||
        count > MAX_DECODERS) {
        violation("register sizing call failed");
    }
    WcDecoderDef *defs = calloc(count, sizeof *defs);
    if (defs == NULL) {
        abort();
    }
    size_t filled = count;
    rc = register_fn(defs, &filled);
    if (rc != WC_DECODER_OK || filled == 0 || filled > count) {
        violation("register populate call failed");
    }
    const uint8_t selector = read_u8(&r);
    const uint8_t flags = read_u8(&r);
    run_decoder(&defs[selector % filled], &r, flags);
    free(defs);
    return 0;
}

// ── seed writer ─────────────────────────────────────────────────────────────

static void seed_put(wcx_fuzz_seed *s, const void *p, size_t n) {
    if (s->failed || n == 0) {
        return;
    }
    if (!wcx_buf_reserve((void **)&s->data, &s->cap, 1, s->len + n, (size_t)1u << 30u)) {
        s->failed = true;
        return;
    }
    memcpy(s->data + s->len, p, n);
    s->len += n;
}

void wcx_fuzz_seed_begin(wcx_fuzz_seed *s, uint8_t selector, uint8_t flags, const char *config,
                         size_t config_len) {
    memset(s, 0, sizeof *s);
    const size_t len = config_len > 0xFFFFu ? 0xFFFFu : config_len;
    const uint8_t head[4] = {selector, flags, (uint8_t)(len & 0xFFu), (uint8_t)(len >> 8u)};
    seed_put(s, head, sizeof head);
    seed_put(s, config, len);
}

void wcx_fuzz_seed_sample(wcx_fuzz_seed *s, uint64_t dt_fs, const uint8_t *bits, size_t nbytes) {
    uint8_t rec[11] = {0};
    size_t n = 1; // record flags: 0
    uint64_t v = dt_fs;
    for (unsigned i = 0; i < 10u; i++) {
        const uint8_t b = (uint8_t)(v & 0x7Fu);
        v >>= 7u;
        rec[n++] = (uint8_t)(v != 0 ? (b | 0x80u) : b);
        if (v == 0) {
            break;
        }
    }
    seed_put(s, rec, n);
    seed_put(s, bits, nbytes);
}

void wcx_fuzz_seed_free(wcx_fuzz_seed *s) {
    if (s != NULL) {
        free(s->data);
        memset(s, 0, sizeof *s);
    }
}
