// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "wcx/tool_io.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "wcx/buf.h"
#include "wcx/str.h"

FILE *wcx_tool_fopen(const char *path, const char *mode) {
    if (path == NULL || mode == NULL) {
        return NULL;
    }
#ifdef _MSC_VER
    FILE *f = NULL;
    if (fopen_s(&f, path, mode) != 0) {
        return NULL;
    }
    return f;
#else
    return fopen(path, mode);
#endif
}

bool wcx_tool_read_file(const char *path, size_t max, char **data, size_t *len, wcx_error *err) {
    *data = NULL;
    *len = 0;
    FILE *f = wcx_tool_fopen(path, "rb");
    if (f == NULL) {
        wcx_error_set(err, "cannot open \"%s\"", path);
        return false;
    }
    char *buf = NULL;
    size_t cap = 0;
    size_t used = 0;
    bool ok = true;
    bool done = false;
    // Each iteration fills at least 64 KiB of the buffer or stops at a short
    // read (end of file or an error), so `max` bounds the iterations.
    const size_t max_iterations = max / 65536u + 2u;
    for (size_t iter = 0; iter < max_iterations && ok && !done; iter++) {
        if (!wcx_buf_reserve((void **)&buf, &cap, 1, used + 65536u + 1u, max + 65536u + 1u)) {
            wcx_error_set(err, "\"%s\" is larger than %lu bytes", path, (unsigned long)max);
            ok = false;
            break;
        }
        const size_t room = cap - used - 1u;
        const size_t got = fread(buf + used, 1, room, f);
        // fread never returns more than asked; the clamp makes that visible
        // to the analyzer, which treats its result as untrusted.
        const size_t n = got < room ? got : room;
        used += n;
        // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): used <= cap - 1, since n <= room.
        buf[used] = '\0';
        if (used > max) {
            wcx_error_set(err, "\"%s\" is larger than %lu bytes", path, (unsigned long)max);
            ok = false;
        } else if (n < room) {
            done = true;
            if (ferror(f) != 0) {
                wcx_error_set(err, "error reading \"%s\"", path);
                ok = false;
            }
        }
    }
    if (ok && !done) {
        wcx_error_set(err, "error reading \"%s\"", path);
        ok = false;
    }
    (void)fclose(f);
    if (!ok || buf == NULL) {
        free(buf);
        return false;
    }
    *data = buf;
    *len = used;
    return true;
}

bool wcx_tool_write_file(const char *path, const void *data, size_t len, wcx_error *err) {
    FILE *f = wcx_tool_fopen(path, "wb");
    if (f == NULL) {
        wcx_error_set(err, "cannot create \"%s\"", path);
        return false;
    }
    const size_t n = len > 0 ? fwrite(data, 1, len, f) : 0;
    const int closed = fclose(f);
    if (n != len || closed != 0) {
        wcx_error_set(err, "error writing \"%s\"", path);
        return false;
    }
    return true;
}

void wcx_tool_printf(FILE *f, const char *fmt, ...) {
    char line[4096] = {0};
    va_list ap;
    va_start(ap, fmt);
    WCX_IGNORE(wcx_str_vformat(line, sizeof line, fmt, ap));
    va_end(ap);
    (void)fputs(line, f);
}
