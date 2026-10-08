// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// banned.h is force-included before this file. Including every standard
// header (and the POSIX/Windows headers the tools use) after it must still
// compile: that is what keeps the poison from breaking system headers.

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "wavecrux_decoder.h"
#include "wcx/test.h"

WCX_TEST(banned_header_is_active) {
    // WCX_BANNED_H is defined only if the build force-included banned.h.
#ifdef WCX_BANNED_H
    WCX_CHECK(true);
#else
    WCX_FAIL("banned.h was not force-included");
#endif
}

WCX_TEST(allowed_replacements_work) {
    char buf[16] = {0};
    WCX_CHECK(snprintf(buf, sizeof buf, "%d", 42) == 2);
    WCX_CHECK_STR_EQ(buf, "42");
    errno = 0;
    char *end = NULL;
    const long v = strtol("123x", &end, 10);
    WCX_CHECK(v == 123 && errno == 0 && *end == 'x');
    memcpy(buf, "ok", 3);
    WCX_CHECK_STR_EQ(buf, "ok");
    WCX_CHECK(fputs("", stdout) >= 0);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, banned_header_is_active);
    WCX_RUN(&t, allowed_replacements_work);
    return wcx_test_finish(&t);
}
