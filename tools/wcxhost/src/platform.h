// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Shared-library loading: dlopen/dlsym on POSIX, LoadLibrary/GetProcAddress
// on Windows, the same calls Dart's DynamicLibrary.open and lookup make.

#ifndef WCXH_PLATFORM_H
#define WCXH_PLATFORM_H

#include <stddef.h>

// Generic function pointer (casting from void(*)(void) to any other
// function-pointer type is exempt from -Wcast-function-type).
typedef void (*wcxh_fn)(void);

// Opens a library; NULL on failure with a message in err (capacity cap).
void *wcxh_dl_open(const char *path, char *err, size_t cap);

// Looks up a symbol; NULL when absent.
wcxh_fn wcxh_dl_sym(void *lib, const char *name);

void wcxh_dl_close(void *lib);

#endif // WCXH_PLATFORM_H
