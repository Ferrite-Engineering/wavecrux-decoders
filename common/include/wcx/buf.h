// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// A growable array with a ceiling (C coding standard §3: growth during feed
// is allowed only for buffers with a protocol-derived ceiling).
//
//   uint8_t *payload = NULL; size_t cap = 0;
//   // PCIe Base 2.1 §2.2.2: Max_Payload_Size is at most 4096 bytes.
//   if (!wcx_buf_reserve((void **)&payload, &cap, 1, len + 1, 4096)) { error }
//
// wcx_buf_reserve makes room for at least `needed` elements of `elem_size`
// bytes. It grows geometrically (so repeated calls cost amortised O(1) and
// a buffer that reached its working size never reallocates again) but never
// beyond `ceiling` elements. It returns false, leaving the buffer untouched,
// when `needed` exceeds the ceiling, when the byte size would overflow, or
// when allocation fails. Free the buffer with free().
//
// Allocate in `create` where the size is known up front; use this only for
// buffers whose size depends on the trace and has a protocol bound.

#ifndef WCX_BUF_H
#define WCX_BUF_H

#include <stdbool.h>
#include <stddef.h>

#include "wcx/export.h"

WCX_NODISCARD bool wcx_buf_reserve(void **data, size_t *cap, size_t elem_size, size_t needed,
                                   size_t ceiling);

#endif // WCX_BUF_H
