// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Only built into debug, sanitizer and fuzz builds (WCX_ENABLE_ASSERTS), where
// an invariant violation should crash loudly (C coding standard §2). Release
// plugins contain no abort and write nothing to stderr.

#include "wcx/assert.h"

#ifdef WCX_ENABLE_ASSERTS

#include <stdio.h>
#include <stdlib.h>

#include "wcx/str.h"

_Noreturn void wcx_assert_fail(const char *file, int line, const char *expr) {
    char msg[512] = {0};
    WCX_IGNORE(wcx_str_format(msg, sizeof msg, "%s:%d: WCX_ASSERT(%s) failed\n", file, line, expr));
    (void)fputs(msg, stderr);
    (void)fflush(stderr);
    abort();
}

#else

// ISO C forbids an empty translation unit.
typedef int wcx_assert_translation_unit_is_not_empty;

#endif
