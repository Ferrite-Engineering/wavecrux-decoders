// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Growable string builder for wcxhost. Failure (allocation, or the 1 GiB
// ceiling) is sticky: check `failed` once at the end.

#ifndef WCXH_SB_H
#define WCXH_SB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wcx/export.h"

typedef struct wcxh_sb {
    char *data; // NUL-terminated while not failed
    size_t len;
    size_t cap;
    bool failed;
} wcxh_sb;

void wcxh_sb_put(wcxh_sb *sb, const char *s, size_t n);
void wcxh_sb_puts(wcxh_sb *sb, const char *s);
void wcxh_sb_putc(wcxh_sb *sb, char c);
void wcxh_sb_u64(wcxh_sb *sb, uint64_t v);
void wcxh_sb_printf(wcxh_sb *sb, const char *fmt, ...) WCX_PRINTF(2, 3);
void wcxh_sb_free(wcxh_sb *sb);

// Takes the buffer (NULL if failed); the builder is left empty.
char *wcxh_sb_take(wcxh_sb *sb);

#endif // WCXH_SB_H
