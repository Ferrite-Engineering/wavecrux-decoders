// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "wcx/arena.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wcx/size.h"

struct wcx_arena_block {
    wcx_arena_block *next;
    unsigned char *data; // separate allocation, malloc-aligned
    size_t cap;
    size_t used;
    size_t last; // offset of the most recent allocation (for wcx_arena_grow)
};

// The number of blocks is bounded by ceiling / 1 byte in theory and by
// ceiling / block_size in practice; every walk below stops at the list end.

static wcx_arena_block *arena_new_block(wcx_arena *a, size_t cap) {
    size_t total = 0;
    if (!wcx_size_add(a->reserved, cap, &total) || total > a->ceiling) {
        return NULL;
    }
    wcx_arena_block *b = calloc(1, sizeof *b);
    if (b == NULL) {
        return NULL;
    }
    b->data = malloc(cap);
    if (b->data == NULL) {
        free(b);
        return NULL;
    }
    b->cap = cap;
    a->reserved = total;
    return b;
}

bool wcx_arena_init(wcx_arena *a, size_t block_size, size_t ceiling) {
    if (a == NULL) {
        return false;
    }
    memset(a, 0, sizeof *a);
    if (block_size == 0 || block_size > ceiling) {
        return false;
    }
    a->block_size = block_size;
    a->ceiling = ceiling;
    a->head = arena_new_block(a, block_size);
    a->cur = a->head;
    return a->head != NULL;
}

void wcx_arena_free(wcx_arena *a) {
    if (a == NULL) {
        return;
    }
    wcx_arena_block *b = a->head;
    while (b != NULL) {
        wcx_arena_block *next = b->next;
        free(b->data);
        free(b);
        b = next;
    }
    memset(a, 0, sizeof *a);
}

void wcx_arena_reset(wcx_arena *a) {
    if (a == NULL) {
        return;
    }
    for (wcx_arena_block *b = a->head; b != NULL; b = b->next) {
        b->used = 0;
        b->last = 0;
    }
    a->cur = a->head;
}

// Offset in `b` where an allocation of `size` bytes aligned to `align` would
// start, or SIZE_MAX if it does not fit.
static size_t block_fit(const wcx_arena_block *b, size_t size, size_t align) {
    size_t start = b->used;
    const size_t rem = start % align;
    if (rem != 0 && !wcx_size_add(start, align - rem, &start)) {
        return SIZE_MAX;
    }
    size_t end = 0;
    if (!wcx_size_add(start, size, &end) || end > b->cap) {
        return SIZE_MAX;
    }
    return start;
}

static void *arena_alloc_aligned(wcx_arena *a, size_t size, size_t align) {
    if (a == NULL || a->cur == NULL || size == 0) {
        return NULL;
    }
    // Serve from the current block or a later block kept from before the
    // last reset; otherwise append a new block.
    for (wcx_arena_block *b = a->cur; b != NULL; b = b->next) {
        const size_t start = block_fit(b, size, align);
        if (start != SIZE_MAX) {
            a->cur = b;
            b->last = start;
            b->used = start + size;
            return b->data + start;
        }
    }
    const size_t cap = size > a->block_size ? size : a->block_size;
    wcx_arena_block *nb = arena_new_block(a, cap);
    if (nb == NULL) {
        return NULL;
    }
    wcx_arena_block *tail = a->cur;
    while (tail->next != NULL) {
        tail = tail->next;
    }
    tail->next = nb;
    a->cur = nb;
    nb->last = 0;
    nb->used = size;
    return nb->data;
}

void *wcx_arena_alloc(wcx_arena *a, size_t size) {
    return arena_alloc_aligned(a, size, WCX_ARENA_ALIGN);
}

void *wcx_arena_grow(wcx_arena *a, void *ptr, size_t old_size, size_t new_size) {
    if (a == NULL || a->cur == NULL || ptr == NULL || new_size < old_size) {
        return NULL;
    }
    wcx_arena_block *b = a->cur;
    const unsigned char *p = ptr;
    if (p == b->data + b->last && b->last + old_size == b->used) {
        size_t end = 0;
        if (wcx_size_add(b->last, new_size, &end) && end <= b->cap) {
            b->used = end;
            return ptr;
        }
    }
    void *moved = arena_alloc_aligned(a, new_size, 1);
    if (moved == NULL) {
        return NULL;
    }
    memcpy(moved, ptr, old_size);
    return moved;
}

char *wcx_arena_strndup(wcx_arena *a, const char *s, size_t n) {
    if (s == NULL) {
        return NULL;
    }
    size_t bytes = 0;
    if (!wcx_size_add(n, 1, &bytes)) {
        return NULL;
    }
    char *out = arena_alloc_aligned(a, bytes, 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

char *wcx_arena_strdup(wcx_arena *a, const char *s) {
    if (s == NULL) {
        return NULL;
    }
    return wcx_arena_strndup(a, s, strlen(s));
}

char *wcx_arena_vprintf(wcx_arena *a, const char *fmt, va_list ap) {
    if (a == NULL || fmt == NULL) {
        return NULL;
    }
    va_list measure;
    va_copy(measure, ap);
    const int n = vsnprintf(NULL, 0, fmt, measure);
    va_end(measure);
    if (n < 0) {
        return NULL;
    }
    size_t bytes = 0;
    if (!wcx_size_add((size_t)n, 1, &bytes)) {
        return NULL;
    }
    char *out = arena_alloc_aligned(a, bytes, 1);
    if (out == NULL) {
        return NULL;
    }
    const int written = vsnprintf(out, bytes, fmt, ap);
    if (written != n) {
        return NULL;
    }
    return out;
}

char *wcx_arena_printf(wcx_arena *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *out = wcx_arena_vprintf(a, fmt, ap);
    va_end(ap);
    return out;
}

size_t wcx_arena_used(const wcx_arena *a) {
    if (a == NULL) {
        return 0;
    }
    size_t used = 0;
    for (const wcx_arena_block *b = a->head; b != NULL; b = b->next) {
        used += b->used;
    }
    return used;
}
