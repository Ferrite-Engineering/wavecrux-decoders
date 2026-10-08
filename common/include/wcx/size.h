// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Overflow-checked size arithmetic (C coding standard §3: size computations
// fail rather than wrap).
//
// Every function returns true and stores the exact result in *out on
// success; on overflow it returns false and stores 0, so a caller that
// ignored the result would still allocate nothing rather than a short
// buffer. All are WCX_NODISCARD.
//
//   if (!wcx_size_mul(count, sizeof(item), &bytes)) { ...error... }

#ifndef WCX_SIZE_H
#define WCX_SIZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wcx/export.h"

// *out = a + b over size_t.
WCX_NODISCARD static inline bool wcx_size_add(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b) {
        *out = 0;
        return false;
    }
    *out = a + b;
    return true;
}

// *out = a * b over size_t.
WCX_NODISCARD static inline bool wcx_size_mul(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) {
        *out = 0;
        return false;
    }
    *out = a * b;
    return true;
}

// *out = a + b over uint64_t (femtosecond and tick arithmetic).
WCX_NODISCARD static inline bool wcx_u64_add(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b) {
        *out = 0;
        return false;
    }
    *out = a + b;
    return true;
}

// *out = a * b over uint64_t.
WCX_NODISCARD static inline bool wcx_u64_mul(uint64_t a, uint64_t b, uint64_t *out) {
    if (a != 0 && b > UINT64_MAX / a) {
        *out = 0;
        return false;
    }
    *out = a * b;
    return true;
}

#endif // WCX_SIZE_H
