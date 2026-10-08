// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The canonical transaction format (cmake/README.md, "Expected files"):
//
//   [
//   {"startTime":100,"endTime":101,"label":"D=0x5A","isError":false,"fields":{"data":"0x5A"}},
//   ...
//   ]
//
// One transaction per line, keys in that order, times in ticks of the VCD's
// timescale (as WaveCrux places DecodedTransaction), every field value a
// string (WaveCrux's _parseFieldsJson makes them Map<String, String>),
// strings escaped as Dart's jsonEncode does. An expected file may be laid
// out any way (the WaveCrux example's pretty-printed files work as is); it is
// parsed and compared transaction by transaction in this canonical form.

#include <stdlib.h>
#include <string.h>

#include "dart.h"
#include "host.h"
#include "sb.h"
#include "wcx/buf.h"
#include "wcx/fuzz_driver.h"
#include "wcx/str.h"
#include "wcx/tool_io.h"

#define EXPECTED_MAX_TOKENS ((size_t)1u << 26u)

static void put_line(wcxh_sb *sb, const wcxh_tx *tx) {
    wcxh_sb_puts(sb, "{\"startTime\":");
    wcxh_sb_u64(sb, tx->start);
    wcxh_sb_puts(sb, ",\"endTime\":");
    wcxh_sb_u64(sb, tx->end);
    wcxh_sb_puts(sb, ",\"label\":");
    wcxh_dart_string(sb, tx->label, strlen(tx->label));
    wcxh_sb_puts(sb, tx->is_error ? ",\"isError\":true" : ",\"isError\":false");
    wcxh_sb_puts(sb, ",\"fields\":");
    wcxh_sb_puts(sb, tx->fields);
    wcxh_sb_putc(sb, '}');
}

char *wcxh_tx_line(const wcxh_tx *tx) {
    wcxh_sb sb = {0};
    put_line(&sb, tx);
    return wcxh_sb_take(&sb);
}

char *wcxh_format(const wcxh_txlist *l) {
    wcxh_sb sb = {0};
    wcxh_sb_puts(&sb, "[\n");
    for (size_t i = 0; i < l->count; i++) {
        put_line(&sb, &l->items[i]);
        wcxh_sb_puts(&sb, i + 1u < l->count ? ",\n" : "\n");
    }
    wcxh_sb_puts(&sb, "]\n");
    return wcxh_sb_take(&sb);
}

static bool tx_equal(const wcxh_tx *a, const wcxh_tx *b) {
    return a->start == b->start && a->end == b->end && a->is_error == b->is_error &&
           strcmp(a->label, b->label) == 0 && strcmp(a->fields, b->fields) == 0;
}

size_t wcxh_first_difference(const wcxh_txlist *a, const wcxh_txlist *b) {
    const size_t n = a->count < b->count ? a->count : b->count;
    for (size_t i = 0; i < n; i++) {
        if (!tx_equal(&a->items[i], &b->items[i])) {
            return i;
        }
    }
    return a->count == b->count ? SIZE_MAX : n;
}

static int cmp_u64(uint64_t x, uint64_t y) {
    return (x > y) - (x < y);
}

// A total order over every field, so equal keys mean equal transactions and
// qsort's instability cannot show.
static int cmp_tx(const void *pa, const void *pb) {
    const wcxh_tx *a = pa;
    const wcxh_tx *b = pb;
    int c = cmp_u64(a->start, b->start);
    if (c == 0) {
        c = cmp_u64(a->end, b->end);
    }
    if (c == 0) {
        c = strcmp(a->label, b->label);
    }
    if (c == 0) {
        c = (a->is_error ? 1 : 0) - (b->is_error ? 1 : 0);
    }
    if (c == 0) {
        c = strcmp(a->fields, b->fields);
    }
    return c;
}

void wcxh_txlist_sort(wcxh_txlist *l) {
    if (l->count > 1) {
        qsort(l->items, l->count, sizeof *l->items, cmp_tx);
    }
}

static char *decode_string(const wcx_jdoc *d, size_t tok) {
    const size_t n = wcx_jdoc_string_len(d, tok);
    char *s = malloc(n + 1u);
    if (s != NULL && !wcx_jdoc_string(d, tok, s, n + 1u, NULL)) {
        free(s);
        return NULL;
    }
    return s;
}

static bool fields_are_strings(const wcx_jdoc *d, size_t obj) {
    size_t k = obj + 1u;
    for (uint32_t i = 0; i < d->toks[obj].size; i++) {
        if (d->toks[k + 1u].type != WCX_JT_STRING) {
            return false;
        }
        k = d->toks[k + 1u].next;
    }
    return true;
}

static bool expected_member(const wcx_jdoc *d, size_t key, size_t value, wcxh_tx *tx,
                            unsigned *seen) {
    const wcx_jtype t = d->toks[value].type;
    if (wcx_jdoc_string_equals(d, key, "startTime")) {
        *seen |= 1u;
        return wcx_jdoc_uint64(d, value, &tx->start);
    }
    if (wcx_jdoc_string_equals(d, key, "endTime")) {
        *seen |= 2u;
        return wcx_jdoc_uint64(d, value, &tx->end);
    }
    if (wcx_jdoc_string_equals(d, key, "label")) {
        *seen |= 4u;
        free(tx->label);
        tx->label = t == WCX_JT_STRING ? decode_string(d, value) : NULL;
        return tx->label != NULL;
    }
    if (wcx_jdoc_string_equals(d, key, "isError")) {
        *seen |= 8u;
        tx->is_error = t == WCX_JT_TRUE;
        return t == WCX_JT_TRUE || t == WCX_JT_FALSE;
    }
    if (wcx_jdoc_string_equals(d, key, "fields")) {
        if (t != WCX_JT_OBJECT || !fields_are_strings(d, value)) {
            return false;
        }
        size_t len = 0;
        const char *raw = wcx_jdoc_raw(d, value, &len);
        char *copy = malloc(len + 1u);
        if (copy == NULL) {
            return false;
        }
        memcpy(copy, raw, len);
        copy[len] = '\0';
        wcxh_sb sb = {0};
        (void)wcxh_dart_fields(&sb, copy);
        free(copy);
        free(tx->fields);
        tx->fields = wcxh_sb_take(&sb);
        return tx->fields != NULL;
    }
    return false;
}

static bool parse_one(const wcx_jdoc *d, size_t obj, wcxh_tx *tx, size_t index, wcx_error *err) {
    memset(tx, 0, sizeof *tx);
    unsigned seen = 0;
    bool ok = d->toks[obj].type == WCX_JT_OBJECT;
    size_t k = obj + 1u;
    for (uint32_t i = 0; ok && i < d->toks[obj].size; i++) {
        ok = expected_member(d, k, k + 1u, tx, &seen);
        k = d->toks[k + 1u].next;
    }
    if (ok && tx->fields == NULL) {
        tx->fields = malloc(3);
        ok = tx->fields != NULL;
        if (ok) {
            memcpy(tx->fields, "{}", 3);
        }
    }
    if (!ok || seen != 15u) {
        wcx_error_set(err,
                      "expected transaction #%lu must be {\"startTime\": int, \"endTime\": int, "
                      "\"label\": string, \"isError\": bool, \"fields\"?: {string: string}}",
                      (unsigned long)index);
        free(tx->label);
        free(tx->fields);
        memset(tx, 0, sizeof *tx);
        return false;
    }
    return true;
}

bool wcxh_parse_expected(const char *text, size_t len, wcxh_txlist *out, wcx_error *err) {
    memset(out, 0, sizeof *out);
    wcx_jdoc d;
    wcx_error inner = {{0}};
    if (!wcx_jdoc_parse(&d, text, len, EXPECTED_MAX_TOKENS, &inner) ||
        d.toks[0].type != WCX_JT_ARRAY) {
        wcx_error_set(err, "expected file is not a JSON array (%s)", inner.msg);
        wcx_jdoc_free(&d);
        return false;
    }
    const size_t n = d.toks[0].size;
    bool ok = n == 0 || wcx_buf_reserve((void **)&out->items, &out->cap, sizeof *out->items, n,
                                        SIZE_MAX / sizeof *out->items);
    size_t e = 1;
    for (size_t i = 0; ok && i < n; i++) {
        ok = parse_one(&d, e, &out->items[i], i, err);
        if (ok) {
            out->count++;
        }
        e = d.toks[e].next;
    }
    wcx_jdoc_free(&d);
    if (!ok) {
        wcxh_txlist_free(out);
    }
    return ok;
}

bool wcxh_write_fuzz_seed(const wcxh_input *in, const char *path, wcx_error *err) {
    wcx_fuzz_seed seed;
    const size_t clen = strlen(in->config_json);
    if (clen > 0xFFFFu) {
        wcx_error_set(err, "config is too long for a fuzz seed");
        return false;
    }
    wcx_fuzz_seed_begin(&seed, (uint8_t)(in->def_index & 0xFFu), WCX_FUZZ_RAW_CONFIG,
                        in->config_json, clen);
    const size_t nbytes = ((size_t)in->bits_per_sample * 2u + 7u) / 8u;
    uint8_t *bits = calloc(1, wcxh_scratch_bytes(in->bits_per_sample));
    size_t cursors[WCX_MAX_SIGNALS] = {0};
    uint64_t prev_fs = 0;
    for (size_t i = 0; bits != NULL && i < in->timeline_count && i < WCX_FUZZ_MAX_SAMPLES; i++) {
        memset(bits, 0, wcxh_scratch_bytes(in->bits_per_sample));
        uint32_t offset = 0;
        for (size_t sig = 0; sig < in->manifest.signal_count; sig++) {
            if (!in->bound[sig]) {
                continue;
            }
            const uint32_t width = in->manifest.signals[sig].bit_width;
            wcxh_pack_value(
                bits, wcxh_scratch_bytes(in->bits_per_sample), offset, width,
                wcxh_vcd_value_at(&in->vcd, in->vcd_sig[sig], in->timeline[i], &cursors[sig]));
            offset += width;
        }
        const uint64_t fs = wcxh_ticks_to_fs(in->timeline[i], in->fs_per_tick);
        wcx_fuzz_seed_sample(&seed, fs - prev_fs, bits, nbytes);
        prev_fs = fs;
    }
    const bool ok =
        bits != NULL && !seed.failed && wcx_tool_write_file(path, seed.data, seed.len, err);
    if (bits == NULL || seed.failed) {
        wcx_error_set(err, "out of memory writing the fuzz seed");
    }
    free(bits);
    wcx_fuzz_seed_free(&seed);
    return ok;
}
