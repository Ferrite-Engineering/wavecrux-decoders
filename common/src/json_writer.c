// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "wcx/json_writer.h"

#include <string.h>

#include "wcx/size.h"
#include "wcx/str.h"

#define WCX_JSON_INITIAL 128u

// Makes room for `extra` more bytes plus the NUL.
static bool reserve(wcx_json *j, size_t extra) {
    if (j->failed) {
        return false;
    }
    size_t need = 0;
    if (!wcx_size_add(j->len, extra, &need) || !wcx_size_add(need, 1, &need)) {
        j->failed = true;
        return false;
    }
    if (need <= j->cap) {
        return true;
    }
    if (j->arena == NULL) {
        j->failed = true;
        return false;
    }
    size_t grown = j->cap * 2u;
    if (grown < need) {
        grown = need;
    }
    char *p = wcx_arena_grow(j->arena, j->buf, j->cap, grown);
    if (p == NULL) {
        j->failed = true;
        return false;
    }
    j->buf = p;
    j->cap = grown;
    return true;
}

void wcx_json_write_raw(wcx_json *j, const char *s, size_t n) {
    if (j == NULL || s == NULL || !reserve(j, n)) {
        return;
    }
    memcpy(j->buf + j->len, s, n);
    j->len += n;
    j->buf[j->len] = '\0';
}

static void put_char(wcx_json *j, char c) {
    wcx_json_write_raw(j, &c, 1);
}

void wcx_json_write_string(wcx_json *j, const char *s, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    static const char replacement[] = "\xEF\xBF\xBD"; // U+FFFD
    if (j == NULL) {
        return;
    }
    if (s == NULL) {
        n = 0;
    }
    put_char(j, '"');
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0;
    while (i < n && !j->failed) {
        const unsigned char c = u[i];
        if (c == '"' || c == '\\') {
            const char esc[2] = {'\\', (char)c};
            wcx_json_write_raw(j, esc, 2);
            i++;
        } else if (c < 0x20u) {
            const char esc[6] = {'\\', 'u', '0', '0', hex[c >> 4u], hex[c & 0xFu]};
            wcx_json_write_raw(j, esc, 6);
            i++;
        } else if (c < 0x80u) {
            put_char(j, (char)c);
            i++;
        } else {
            const size_t len = wcx_utf8_seq_len(u + i, n - i);
            if (len == 0) {
                wcx_json_write_raw(j, replacement, 3);
                i++;
            } else {
                wcx_json_write_raw(j, s + i, len);
                i += len;
            }
        }
    }
    put_char(j, '"');
}

void wcx_json_begin(wcx_json *j, wcx_arena *arena) {
    if (j == NULL) {
        return;
    }
    memset(j, 0, sizeof *j);
    j->arena = arena;
    j->buf = arena != NULL ? wcx_arena_alloc(arena, WCX_JSON_INITIAL) : NULL;
    if (j->buf == NULL) {
        j->failed = true;
        return;
    }
    j->cap = WCX_JSON_INITIAL;
    j->buf[0] = '\0';
    put_char(j, '{');
}

void wcx_json_begin_fixed(wcx_json *j, char *buf, size_t cap) {
    if (j == NULL) {
        return;
    }
    memset(j, 0, sizeof *j);
    j->buf = buf;
    j->cap = cap;
    if (buf == NULL || cap == 0) {
        j->failed = true;
        return;
    }
    j->buf[0] = '\0';
    put_char(j, '{');
}

static void begin_member(wcx_json *j, const char *key) {
    if (j->ended) {
        j->failed = true;
        return;
    }
    if (j->need_comma) {
        put_char(j, ',');
    }
    j->need_comma = true;
    wcx_json_write_string(j, key, key != NULL ? strlen(key) : 0);
    put_char(j, ':');
}

void wcx_json_add_strn(wcx_json *j, const char *key, const char *value, size_t n) {
    if (j == NULL) {
        return;
    }
    begin_member(j, key);
    wcx_json_write_string(j, value, n);
}

void wcx_json_add_str(wcx_json *j, const char *key, const char *value) {
    wcx_json_add_strn(j, key, value, value != NULL ? strlen(value) : 0);
}

void wcx_json_add_i64(wcx_json *j, const char *key, int64_t value) {
    if (j == NULL) {
        return;
    }
    char text[WCX_FMT_I64_CAP] = {0};
    const size_t n = wcx_fmt_i64(text, value);
    begin_member(j, key);
    wcx_json_write_raw(j, text, n);
}

void wcx_json_add_u64(wcx_json *j, const char *key, uint64_t value) {
    if (j == NULL) {
        return;
    }
    char text[WCX_FMT_U64_CAP] = {0};
    const size_t n = wcx_fmt_u64(text, value);
    begin_member(j, key);
    wcx_json_write_raw(j, text, n);
}

void wcx_json_add_bool(wcx_json *j, const char *key, bool value) {
    if (j == NULL) {
        return;
    }
    begin_member(j, key);
    if (value) {
        wcx_json_write_raw(j, "true", 4);
    } else {
        wcx_json_write_raw(j, "false", 5);
    }
}

void wcx_json_add_hex(wcx_json *j, const char *key, uint64_t value, unsigned digits) {
    if (j == NULL) {
        return;
    }
    char text[WCX_FMT_HEX_CAP] = {0};
    const size_t n = wcx_fmt_hex(text, value, digits);
    wcx_json_add_strn(j, key, text, n);
}

const char *wcx_json_end(wcx_json *j) {
    if (j == NULL || j->ended) {
        return NULL;
    }
    put_char(j, '}');
    j->ended = true;
    return j->failed ? NULL : j->buf;
}
