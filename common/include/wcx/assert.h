// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// WCX_ASSERT(cond): an internal invariant.
//
// In debug, sanitizer and fuzz builds (WCX_ENABLE_ASSERTS, set by
// cmake/WcxHardening.cmake) a false condition prints the file, line and
// expression to stderr and aborts: there, crashing loudly is the point.
//
// In release builds it compiles to nothing (the condition is not evaluated),
// because a plugin must never abort the host (C coding standard §2). Release
// code must therefore also handle the violation: route it to the instance's
// error state (wcx_decoder_fail) right after the assertion.
//
// The condition must have no side effects.

#ifndef WCX_ASSERT_H
#define WCX_ASSERT_H

#ifdef WCX_ENABLE_ASSERTS

_Noreturn void wcx_assert_fail(const char *file, int line, const char *expr);

#define WCX_ASSERT(cond)                                \
    do {                                                \
        if (!(cond)) {                                  \
            wcx_assert_fail(__FILE__, __LINE__, #cond); \
        }                                               \
    } while (0)

#else

#define WCX_ASSERT(cond) ((void)sizeof((cond) ? 1 : 0))

#endif

#endif // WCX_ASSERT_H
