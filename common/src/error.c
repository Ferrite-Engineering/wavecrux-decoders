// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "wcx/error.h"

#include <stdarg.h>

#include "wcx/str.h"

void wcx_error_set(wcx_error *err, const char *fmt, ...) {
    if (err == NULL) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    // A truncated message is still a useful message; the cut is at a UTF-8
    // boundary either way.
    WCX_IGNORE(wcx_str_vformat(err->msg, sizeof err->msg, fmt, ap));
    va_end(ap);
}

void wcx_error_clear(wcx_error *err) {
    if (err != NULL) {
        err->msg[0] = '\0';
    }
}
