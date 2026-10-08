// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "platform.h"

#include <string.h>

#include "wcx/str.h"

#ifdef _WIN32

#include <windows.h>

void *wcxh_dl_open(const char *path, char *err, size_t cap) {
    HMODULE h = LoadLibraryA(path);
    if (h == NULL) {
        WCX_IGNORE(wcx_str_format(err, cap, "LoadLibrary failed (error %lu)",
                                  (unsigned long)GetLastError()));
    }
    return (void *)h;
}

wcxh_fn wcxh_dl_sym(void *lib, const char *name) {
    FARPROC p = GetProcAddress((HMODULE)lib, name);
    wcxh_fn fn = NULL;
    memcpy((void *)&fn, (const void *)&p, sizeof fn);
    return fn;
}

void wcxh_dl_close(void *lib) {
    if (lib != NULL) {
        (void)FreeLibrary((HMODULE)lib);
    }
}

#else

#include <dlfcn.h>

_Static_assert(sizeof(void *) == sizeof(wcxh_fn), "object and function pointers differ in size");

void *wcxh_dl_open(const char *path, char *err, size_t cap) {
    // RTLD_NOW: an unresolved symbol fails here rather than mid-decode.
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        const char *why = dlerror();
        WCX_IGNORE(wcx_str_copy(err, cap, why != NULL ? why : "dlopen failed"));
    }
    return h;
}

wcxh_fn wcxh_dl_sym(void *lib, const char *name) {
    const void *p = dlsym(lib, name);
    // ISO C has no conversion from object to function pointer; POSIX
    // guarantees the representations match (checked above).
    wcxh_fn fn = NULL;
    memcpy((void *)&fn, (const void *)&p, sizeof fn);
    return fn;
}

void wcxh_dl_close(void *lib) {
    if (lib != NULL) {
        (void)dlclose(lib);
    }
}

#endif
