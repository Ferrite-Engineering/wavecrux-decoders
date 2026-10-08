// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "sb.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wcx/buf.h"
#include "wcx/size.h"
#include "wcx/str.h"

#define SB_CEILING ((size_t)1u << 30u)

void wcxh_sb_put(wcxh_sb *sb, const char *s, size_t n) {
    if (sb == NULL || sb->failed || (s == NULL && n > 0)) {
        if (sb != NULL) {
            sb->failed = true;
        }
        return;
    }
    size_t need = 0;
    if (!wcx_size_add(sb->len, n, &need) || !wcx_size_add(need, 1u, &need) ||
        !wcx_buf_reserve((void **)&sb->data, &sb->cap, 1, need, SB_CEILING)) {
        sb->failed = true;
        return;
    }
    if (n > 0) {
        memcpy(sb->data + sb->len, s, n);
    }
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void wcxh_sb_puts(wcxh_sb *sb, const char *s) {
    wcxh_sb_put(sb, s, s != NULL ? strlen(s) : 0);
}

void wcxh_sb_putc(wcxh_sb *sb, char c) {
    wcxh_sb_put(sb, &c, 1);
}

void wcxh_sb_u64(wcxh_sb *sb, uint64_t v) {
    char text[WCX_FMT_U64_CAP] = {0};
    const size_t n = wcx_fmt_u64(text, v);
    wcxh_sb_put(sb, text, n);
}

void wcxh_sb_printf(wcxh_sb *sb, const char *fmt, ...) {
    char line[4096] = {0};
    va_list ap;
    va_start(ap, fmt);
    const bool ok = wcx_str_vformat(line, sizeof line, fmt, ap);
    va_end(ap);
    if (!ok && sb != NULL) {
        sb->failed = true;
        return;
    }
    wcxh_sb_puts(sb, line);
}

void wcxh_sb_free(wcxh_sb *sb) {
    if (sb != NULL) {
        free(sb->data);
        memset(sb, 0, sizeof *sb);
    }
}

char *wcxh_sb_take(wcxh_sb *sb) {
    if (sb == NULL) {
        return NULL;
    }
    if (sb->failed) {
        wcxh_sb_free(sb);
        return NULL;
    }
    if (sb->data == NULL) {
        wcxh_sb_put(sb, "", 0);
    }
    char *out = sb->data;
    memset(sb, 0, sizeof *sb);
    return out;
}
