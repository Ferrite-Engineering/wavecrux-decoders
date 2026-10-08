// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// File input/output for the test tools (wcxhost, the fuzz replay runner).
// Never linked into a plugin: plugins do no I/O (C coding standard §2).
//
// fopen is wrapped because MSVC's /sdl turns its deprecation into an error
// (fopen_s is the MSVC spelling); output helpers exist because printf and
// fprintf are banned (§6).

#ifndef WCX_TOOL_IO_H
#define WCX_TOOL_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "wcx/error.h"
#include "wcx/export.h"

// fopen / fopen_s.
FILE *wcx_tool_fopen(const char *path, const char *mode);

// Reads a whole file (at most `max` bytes) into a malloc'd, NUL-terminated
// buffer. The NUL is not counted in *len.
WCX_NODISCARD bool wcx_tool_read_file(const char *path, size_t max, char **data, size_t *len,
                                      wcx_error *err);

// Writes `len` bytes to `path` (replacing it).
WCX_NODISCARD bool wcx_tool_write_file(const char *path, const void *data, size_t len,
                                       wcx_error *err);

// printf-style output to a stream (one line is at most 4 KiB).
void wcx_tool_printf(FILE *f, const char *fmt, ...) WCX_PRINTF(2, 3);

#endif // WCX_TOOL_IO_H
