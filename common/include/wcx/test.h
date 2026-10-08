// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// A tiny, dependency-free unit test framework for ctest.
//
//   #include "wcx/test.h"
//
//   WCX_TEST(crc_of_empty_input_is_seed) {
//       WCX_CHECK_EQ_U64(crc16(NULL, 0), 0xFFFFu);   // spec §x.y, Table z
//   }
//
//   int main(void) {
//       wcx_test t = WCX_TEST_INIT;
//       WCX_RUN(&t, crc_of_empty_input_is_seed);
//       return wcx_test_finish(&t);
//   }
//
// Each check reports file:line and the values on failure, and the test goes
// on (so one run shows every failure); WCX_REQUIRE* return from the test
// function instead, for checks the rest of the test depends on.
// wcx_test_finish prints a summary and returns 0 when everything passed and
// 1 otherwise, which is what ctest reads.
//
// No global state: every check talks to the `wcx_test *t` parameter that
// WCX_TEST gives each test function. Output goes to stderr via fputs (printf
// is banned, C coding standard §6).

#ifndef WCX_TEST_H
#define WCX_TEST_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wcx/export.h"

typedef struct wcx_test {
    const char *current;
    unsigned tests_run;
    unsigned tests_failed;
    unsigned checks;
    unsigned failures;
    bool current_failed;
} wcx_test;

#define WCX_TEST_INIT {NULL, 0u, 0u, 0u, 0u, false}

#define WCX_TEST(name) static void name(wcx_test *t)

static inline void wcx_test_vreport(wcx_test *t, const char *file, int line, const char *fmt,
                                    va_list ap) WCX_PRINTF(4, 0);
static inline void wcx_test_vreport(wcx_test *t, const char *file, int line, const char *fmt,
                                    va_list ap) {
    char msg[1024] = {0};
    char head[512] = {0};
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    (void)snprintf(head, sizeof head, "%s:%d: [%s] ", file, line,
                   t->current != NULL ? t->current : "?");
    (void)fputs(head, stderr);
    (void)fputs(msg, stderr);
    (void)fputs("\n", stderr);
    t->failures++;
    t->current_failed = true;
}

static inline void wcx_test_report(wcx_test *t, const char *file, int line, const char *fmt, ...)
    WCX_PRINTF(4, 5);
static inline void wcx_test_report(wcx_test *t, const char *file, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    wcx_test_vreport(t, file, line, fmt, ap);
    va_end(ap);
}

static inline bool wcx_test_str_eq(const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

#define WCX_FAIL(...) wcx_test_report(t, __FILE__, __LINE__, __VA_ARGS__)

#define WCX_CHECK(cond)                          \
    do {                                         \
        t->checks++;                             \
        if (!(cond)) {                           \
            WCX_FAIL("CHECK(%s) failed", #cond); \
        }                                        \
    } while (0)

#define WCX_REQUIRE(cond)                          \
    do {                                           \
        t->checks++;                               \
        if (!(cond)) {                             \
            WCX_FAIL("REQUIRE(%s) failed", #cond); \
            return;                                \
        }                                          \
    } while (0)

#define WCX_CHECK_EQ_U64(a, b)                                                                  \
    do {                                                                                        \
        t->checks++;                                                                            \
        const unsigned long long wcx_a_ = (unsigned long long)(a);                              \
        const unsigned long long wcx_b_ = (unsigned long long)(b);                              \
        if (wcx_a_ != wcx_b_) {                                                                 \
            WCX_FAIL("%s == %s failed: %llu (0x%llX) != %llu (0x%llX)", #a, #b, wcx_a_, wcx_a_, \
                     wcx_b_, wcx_b_);                                                           \
        }                                                                                       \
    } while (0)

#define WCX_CHECK_EQ_I64(a, b)                                                 \
    do {                                                                       \
        t->checks++;                                                           \
        const long long wcx_a_ = (long long)(a);                               \
        const long long wcx_b_ = (long long)(b);                               \
        if (wcx_a_ != wcx_b_) {                                                \
            WCX_FAIL("%s == %s failed: %lld != %lld", #a, #b, wcx_a_, wcx_b_); \
        }                                                                      \
    } while (0)

#define WCX_CHECK_STR_EQ(a, b)                                                                \
    do {                                                                                      \
        t->checks++;                                                                          \
        const char *wcx_a_ = (a);                                                             \
        const char *wcx_b_ = (b);                                                             \
        if (!wcx_test_str_eq(wcx_a_, wcx_b_)) {                                               \
            WCX_FAIL("%s == %s failed:\n    got:      %s\n    expected: %s", #a, #b,          \
                     wcx_a_ != NULL ? wcx_a_ : "(null)", wcx_b_ != NULL ? wcx_b_ : "(null)"); \
        }                                                                                     \
    } while (0)

#define WCX_CHECK_MEM_EQ(a, b, n)                                                \
    do {                                                                         \
        t->checks++;                                                             \
        if (memcmp((a), (b), (n)) != 0) {                                        \
            WCX_FAIL("%s == %s (%lu bytes) failed", #a, #b, (unsigned long)(n)); \
        }                                                                        \
    } while (0)

#define WCX_RUN(tp, fn)                 \
    do {                                \
        wcx_test *wcx_t_ = (tp);        \
        wcx_t_->current = #fn;          \
        wcx_t_->current_failed = false; \
        wcx_t_->tests_run++;            \
        fn(wcx_t_);                     \
        if (wcx_t_->current_failed) {   \
            wcx_t_->tests_failed++;     \
        }                               \
    } while (0)

static inline int wcx_test_finish(const wcx_test *t) {
    char line[256] = {0};
    (void)snprintf(line, sizeof line, "%u tests, %u checks, %u failed tests (%u failed checks)\n",
                   t->tests_run, t->checks, t->tests_failed, t->failures);
    (void)fputs(line, t->tests_failed == 0 ? stdout : stderr);
    return t->tests_failed == 0 && t->tests_run > 0 ? 0 : 1;
}

#endif // WCX_TEST_H
