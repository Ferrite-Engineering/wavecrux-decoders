// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "wcx/buf.h"

#include <stdlib.h>

#include "wcx/size.h"

bool wcx_buf_reserve(void **data, size_t *cap, size_t elem_size, size_t needed, size_t ceiling) {
    if (data == NULL || cap == NULL || elem_size == 0) {
        return false;
    }
    if (needed <= *cap) {
        return true;
    }
    if (needed > ceiling) {
        return false;
    }
    size_t grown = *cap < 8u ? 8u : *cap;
    // Doubling from >= 8 reaches any needed <= ceiling within 64 steps.
    for (int step = 0; step < 64 && grown < needed; step++) {
        if (!wcx_size_mul(grown, 2u, &grown)) {
            grown = ceiling;
            break;
        }
    }
    if (grown > ceiling) {
        grown = ceiling;
    }
    if (grown < needed) {
        grown = needed;
    }
    size_t bytes = 0;
    // grown >= needed > *cap >= 0 and elem_size > 0, so bytes > 0 on success.
    if (!wcx_size_mul(grown, elem_size, &bytes) || bytes == 0) {
        return false;
    }
    void *p = realloc(*data, bytes);
    if (p == NULL) {
        return false;
    }
    *data = p;
    *cap = grown;
    return true;
}
