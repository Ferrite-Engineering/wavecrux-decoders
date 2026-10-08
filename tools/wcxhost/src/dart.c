// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Line references: lib/services/decoders/ffi/ffi_decoder_loader_io.dart in
// the WaveCrux open core.

#include "dart.h"

#include <stdlib.h>
#include <string.h>

#include "wcx/str.h"

#define FIELDS_MAX_TOKENS 1000000u
#define FIELDS_MAX_KEYS   4096u

uint64_t wcxh_fs_per_tick(bool has_timescale, uint64_t factor, int exponent) {
    // Line 1149: `if (timescale == null) return 1000000;`
    if (!has_timescale) {
        return 1000000u;
    }
    // Lines 1155-1156: shift = exp + 15; below zero falls back to 1 ns.
    const int shift = exponent + 15;
    if (shift < 0) {
        return 1000000u;
    }
    // Lines 1157-1161: multiplier = 10^shift; factor * multiplier, in
    // Dart's wrapping int arithmetic.
    uint64_t multiplier = 1;
    for (int i = 0; i < shift && i < 64; i++) {
        multiplier *= 10u;
    }
    return factor * multiplier;
}

uint64_t wcxh_ticks_to_fs(uint64_t ticks, uint64_t fs_per_tick) {
    // Line 1027: `final tsFs = ts < 0 ? 0 : ts * fsPerTick;`
    if (ticks > (uint64_t)INT64_MAX) {
        return 0;
    }
    return ticks * fs_per_tick;
}

uint64_t wcxh_fs_to_ticks_floor(uint64_t fs, uint64_t fs_per_tick) {
    // Lines 1166-1170: `fs ~/ fsPerTick`, unsigned for values above 2^63.
    return fs_per_tick == 0 ? 0 : fs / fs_per_tick;
}

uint64_t wcxh_fs_to_ticks_ceil(uint64_t fs, uint64_t fs_per_tick) {
    // Lines 1173-1180: exact ? floor : floor + 1.
    if (fs_per_tick == 0) {
        return 0;
    }
    const uint64_t floor = fs / fs_per_tick;
    return fs % fs_per_tick == 0 ? floor : floor + 1u;
}

size_t wcxh_scratch_bytes(uint32_t bits_per_sample) {
    // Lines 974-975.
    const size_t encoded = ((size_t)bits_per_sample * 2u + 7u) >> 3u;
    return encoded < 32u ? 32u : encoded;
}

void wcxh_pack_value(uint8_t *buffer, size_t buffer_len, uint32_t bit_offset, uint32_t bit_width,
                     const char *value) {
    // Line 1199.
    if (bit_width == 0 || buffer == NULL) {
        return;
    }
    const size_t raw_len = value != NULL ? strlen(value) : 0;
    for (uint32_t i = 0; i < bit_width; i++) {
        // Lines 1205-1220: signal bit i is raw[raw.length - 1 - i].
        bool level = false;
        bool unknown = false;
        if (raw_len > i) {
            const char c = value[raw_len - 1u - i];
            if (c == '1') {
                level = true;
            } else if (c == 'x' || c == 'X' || c == 'z' || c == 'Z') {
                unknown = true;
            }
        }
        // Lines 1222-1225.
        const size_t encoded_pos = ((size_t)bit_offset + i) * 2u;
        const size_t byte_index = encoded_pos >> 3u;
        const unsigned shift = (unsigned)(encoded_pos & 7u);
        if (byte_index >= buffer_len) {
            return;
        }
        // Lines 1226-1239. shift is even, so shift + 1 < 8 always holds and
        // the loader's byte-straddling branch is never taken.
        unsigned b = buffer[byte_index];
        if (level) {
            b |= 1u << shift;
        }
        if (unknown && shift + 1u < 8u) {
            b |= 1u << (shift + 1u);
        } else if (unknown && byte_index + 1u < buffer_len) {
            buffer[byte_index + 1u] = (uint8_t)(buffer[byte_index + 1u] | 1u);
        }
        buffer[byte_index] = (uint8_t)b;
    }
}

static int cmp_u64(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

size_t wcxh_sort_unique(uint64_t *stamps, size_t count, uint64_t start_time) {
    // Lines 1261-1265: an empty set becomes {startTime}; otherwise sorted.
    if (count == 0) {
        stamps[0] = start_time;
        return 1;
    }
    qsort(stamps, count, sizeof *stamps, cmp_u64);
    size_t w = 1;
    for (size_t i = 1; i < count; i++) {
        if (stamps[i] != stamps[w - 1u]) {
            stamps[w++] = stamps[i];
        }
    }
    return w;
}

void wcxh_dart_string(wcxh_sb *sb, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    wcxh_sb_putc(sb, '"');
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        switch (c) {
            case '"':
                esc = "\\\"";
                break;
            case '\\':
                esc = "\\\\";
                break;
            case '\b':
                esc = "\\b";
                break;
            case '\t':
                esc = "\\t";
                break;
            case '\n':
                esc = "\\n";
                break;
            case '\f':
                esc = "\\f";
                break;
            case '\r':
                esc = "\\r";
                break;
            default:
                break;
        }
        if (esc != NULL) {
            wcxh_sb_puts(sb, esc);
        } else if (c < 0x20u) {
            const char u[6] = {'\\', 'u', '0', '0', hex[c >> 4u], hex[c & 0xFu]};
            wcxh_sb_put(sb, u, sizeof u);
        } else {
            wcxh_sb_putc(sb, (char)c);
        }
    }
    wcxh_sb_putc(sb, '"');
}

// A decoded string token, written with Dart's escaping.
static void put_string_token(wcxh_sb *sb, const wcx_jdoc *doc, size_t tok) {
    const size_t len = wcx_jdoc_string_len(doc, tok);
    char *buf = malloc(len + 1u);
    if (buf == NULL || !wcx_jdoc_string(doc, tok, buf, len + 1u, NULL)) {
        sb->failed = true;
        free(buf);
        return;
    }
    wcxh_dart_string(sb, buf, len);
    free(buf);
}

// A scalar's jsonEncode text; false when not reproduced exactly.
static bool put_scalar(wcxh_sb *sb, const wcx_jdoc *doc, size_t tok) {
    size_t len = 0;
    const char *raw = wcx_jdoc_raw(doc, tok, &len);
    switch (doc->toks[tok].type) {
        case WCX_JT_STRING:
            put_string_token(sb, doc, tok);
            return true;
        case WCX_JT_NUMBER: {
            int64_t v = 0;
            if (wcx_jdoc_int64(doc, tok, &v)) {
                char text[WCX_FMT_I64_CAP] = {0};
                wcxh_sb_put(sb, text, wcx_fmt_i64(text, v));
                return true;
            }
            wcxh_sb_put(sb, raw, len);
            return false;
        }
        case WCX_JT_TRUE:
        case WCX_JT_FALSE:
        case WCX_JT_NULL:
        case WCX_JT_OBJECT:
        case WCX_JT_ARRAY:
        case WCX_JT_NONE:
        default:
            wcxh_sb_put(sb, raw, len);
            return true;
    }
}

bool wcxh_dart_reencode(wcxh_sb *sb, const wcx_jdoc *doc, size_t tok) {
    if (doc == NULL || tok >= doc->count) {
        return false;
    }
    struct {
        bool object;
        uint32_t remaining; // children still to start (objects: keys and values)
        uint32_t started;
    } stack[WCX_JSON_MAX_DEPTH];
    size_t depth = 0;
    bool exact = true;
    const size_t end = doc->toks[tok].next;
    for (size_t i = tok; i < end; i++) {
        if (depth > 0) {
            const bool value_of_key = stack[depth - 1].object && stack[depth - 1].started % 2u;
            if (stack[depth - 1].started > 0) {
                wcxh_sb_putc(sb, value_of_key ? ':' : ',');
            }
            stack[depth - 1].started++;
            stack[depth - 1].remaining--;
        }
        const wcx_jtok *t = &doc->toks[i];
        if (t->type == WCX_JT_OBJECT || t->type == WCX_JT_ARRAY) {
            const bool object = t->type == WCX_JT_OBJECT;
            wcxh_sb_putc(sb, object ? '{' : '[');
            stack[depth].object = object;
            stack[depth].remaining = object ? t->size * 2u : t->size;
            stack[depth].started = 0;
            depth++;
        } else {
            exact = put_scalar(sb, doc, i) && exact;
        }
        while (depth > 0 && stack[depth - 1].remaining == 0) {
            depth--;
            wcxh_sb_putc(sb, stack[depth].object ? '}' : ']');
        }
    }
    return exact;
}

// One field value as Dart's `value?.toString() ?? ''` renders it.
static unsigned field_value(wcxh_sb *val, const wcx_jdoc *doc, size_t tok) {
    size_t len = 0;
    const char *raw = wcx_jdoc_raw(doc, tok, &len);
    switch (doc->toks[tok].type) {
        case WCX_JT_STRING: {
            const size_t n = wcx_jdoc_string_len(doc, tok);
            char *buf = malloc(n + 1u);
            if (buf == NULL || !wcx_jdoc_string(doc, tok, buf, n + 1u, NULL)) {
                val->failed = true;
            } else {
                wcxh_sb_put(val, buf, n);
            }
            free(buf);
            return 0;
        }
        case WCX_JT_NULL:
            return 0;
        case WCX_JT_TRUE:
        case WCX_JT_FALSE:
            wcxh_sb_put(val, raw, len);
            return 0;
        case WCX_JT_NUMBER: {
            int64_t v = 0;
            if (wcx_jdoc_int64(doc, tok, &v)) {
                char text[WCX_FMT_I64_CAP] = {0};
                wcxh_sb_put(val, text, wcx_fmt_i64(text, v));
                return 0;
            }
            wcxh_sb_put(val, raw, len);
            return WCXH_FIELDS_INEXACT;
        }
        case WCX_JT_OBJECT:
        case WCX_JT_ARRAY:
        case WCX_JT_NONE:
        default:
            (void)wcxh_dart_reencode(val, doc, tok);
            return WCXH_FIELDS_INEXACT;
    }
}

typedef struct field_entry {
    char *key;
    size_t key_len;
    char *value;
    size_t value_len;
} field_entry;

static void free_entries(field_entry *e, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(e[i].key);
        free(e[i].value);
    }
    free(e);
}

// Collects members in first-occurrence order, the last value winning, as
// a Dart LinkedHashMap does.
static unsigned collect_fields(const wcx_jdoc *doc, field_entry *entries, size_t *count) {
    unsigned flags = 0;
    size_t n = 0;
    size_t k = 1;
    for (uint32_t m = 0; m < doc->toks[0].size && n < FIELDS_MAX_KEYS; m++) {
        const size_t v = k + 1u;
        const size_t klen = wcx_jdoc_string_len(doc, k);
        char *key = malloc(klen + 1u);
        wcxh_sb val = {0};
        flags |= field_value(&val, doc, v);
        if (key == NULL || !wcx_jdoc_string(doc, k, key, klen + 1u, NULL) || val.failed) {
            free(key);
            wcxh_sb_free(&val);
            break;
        }
        size_t at = n;
        for (size_t i = 0; i < n; i++) {
            if (entries[i].key_len == klen && memcmp(entries[i].key, key, klen) == 0) {
                at = i;
                flags |= WCXH_FIELDS_DUPLICATE;
                break;
            }
        }
        if (at == n) {
            entries[n].key = key;
            entries[n].key_len = klen;
            n++;
        } else {
            free(key);
            free(entries[at].value);
        }
        entries[at].value_len = val.len;
        entries[at].value = wcxh_sb_take(&val);
        k = doc->toks[v].next;
    }
    *count = n;
    return flags;
}

unsigned wcxh_dart_fields(wcxh_sb *sb, const char *fields_json) {
    const size_t len = fields_json != NULL ? strlen(fields_json) : 0;
    wcx_jdoc doc;
    // Line 1300: an empty string is an empty map; lines 1302-1303 and
    // 1311-1312: anything that is not a JSON object is an empty map too.
    if (len == 0) {
        wcxh_sb_puts(sb, "{}");
        return 0;
    }
    if (!wcx_jdoc_parse(&doc, fields_json, len, FIELDS_MAX_TOKENS, NULL) ||
        doc.toks[0].type != WCX_JT_OBJECT) {
        wcx_jdoc_free(&doc);
        wcxh_sb_puts(sb, "{}");
        return WCXH_FIELDS_NOT_JSON;
    }
    const size_t members = doc.toks[0].size;
    field_entry *entries = calloc(members > 0 ? members : 1u, sizeof *entries);
    if (entries == NULL) {
        wcx_jdoc_free(&doc);
        sb->failed = true;
        return 0;
    }
    size_t n = 0;
    const unsigned flags = collect_fields(&doc, entries, &n);
    wcxh_sb_putc(sb, '{');
    for (size_t i = 0; i < n; i++) {
        if (i > 0) {
            wcxh_sb_putc(sb, ',');
        }
        wcxh_dart_string(sb, entries[i].key, entries[i].key_len);
        wcxh_sb_putc(sb, ':');
        wcxh_dart_string(sb, entries[i].value != NULL ? entries[i].value : "",
                         entries[i].value_len);
    }
    wcxh_sb_putc(sb, '}');
    free_entries(entries, members > 0 ? members : 1u);
    wcx_jdoc_free(&doc);
    return flags;
}
