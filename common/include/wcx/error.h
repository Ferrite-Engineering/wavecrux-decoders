// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// A user-facing error message, carried out of a fallible call.
//
// Messages are sentences that name the field and the expected value
// (C coding standard §10), because they end up in an error transaction a
// user reads: `parameter "lanes" must be an integer from 1 to 32, got 40`.
// They are always valid UTF-8 and at most WCX_ERROR_MAX - 1 bytes.
//
// Every function that takes a `wcx_error *` accepts NULL (the message is
// then discarded) and only writes it when it fails.

#ifndef WCX_ERROR_H
#define WCX_ERROR_H

#include "wcx/export.h"

#define WCX_ERROR_MAX 256

typedef struct wcx_error {
    char msg[WCX_ERROR_MAX];
} wcx_error;

// Formats the message (printf-style, integer and string conversions only).
// A message longer than the buffer is cut at a UTF-8 boundary.
void wcx_error_set(wcx_error *err, const char *fmt, ...) WCX_PRINTF(2, 3);

// Empties the message.
void wcx_error_clear(wcx_error *err);

#endif // WCX_ERROR_H
