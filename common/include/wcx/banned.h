// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Banned functions (C coding standard §6). CMake force-includes this header
// first in every translation unit (-include on GCC/Clang, /FI on MSVC), so
// using any of these is a compile error everywhere in the tree:
//
//   strcpy strcat strncpy strncat sprintf vsprintf gets strtok
//   atoi atol atof asctime ctime gmtime localtime tmpnam rand
//   printf puts fprintf system
//
// Each one's safe use depends on the caller getting a length (or a global
// state, or a locale) right somewhere else. Use instead: wcx_str_* and
// snprintf into a checked buffer, memcpy with a checked length,
// strtol/strtoull with errno and end-pointer checks, fputs/fwrite for
// output in tools.
//
// How it stays compatible with system headers. A poisoned identifier is an
// error wherever it appears afterwards, including in a system header that
// declares it. So this header first includes every standard header that
// declares a banned function; their include guards make later includes no-ops
// and the declarations are already behind us. Then it undefines any
// fortification macro of the same name (macOS's _FORTIFY_SOURCE defines
// strcpy, sprintf, ... as macros; a banned function needs no fortification)
// and poisons the names. common/tests/test_banned.c compiles a broad set of
// system headers after this one to keep that true, and the
// common.banned.rejects test proves a use of strcpy fails to compile.
//
// MSVC has no poison; it gets #pragma deprecated, which raises C4995, and the
// build makes C4995 and C4996 errors (/we4995, /sdl).

#ifndef WCX_BANNED_H
#define WCX_BANNED_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__GNUC__) || defined(__clang__)

#ifdef strcpy
#undef strcpy
#endif
#ifdef strcat
#undef strcat
#endif
#ifdef strncpy
#undef strncpy
#endif
#ifdef strncat
#undef strncat
#endif
#ifdef sprintf
#undef sprintf
#endif
#ifdef vsprintf
#undef vsprintf
#endif
#ifdef printf
#undef printf
#endif
#ifdef fprintf
#undef fprintf
#endif
#ifdef puts
#undef puts
#endif

#pragma GCC poison strcpy strcat strncpy strncat sprintf vsprintf gets strtok
#pragma GCC poison atoi atol atof asctime ctime gmtime localtime tmpnam rand
#pragma GCC poison printf puts fprintf system

#elif defined(_MSC_VER)

// No #pragma deprecated list on MSVC: it poisons identifiers globally, and
// Windows SDK headers use some of these names as struct members (ctime in
// objidlbase.h), which breaks every translation unit that includes them.
// MSVC builds instead get /sdl with /we4996, which makes the CRT's unsafe
// functions (strcpy, sprintf, strtok, gets, ...) errors; GCC and Clang
// enforce the full list above on every CI run.

#endif

#endif // WCX_BANNED_H
