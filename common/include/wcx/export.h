// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Compiler-specific attributes, wrapped once so decoder code stays ISO C17
// (C coding standard §1: symbol export is the one extension common/ may wrap).
//
//   WCX_EXPORT        on the definition of an ABI entry point. Every other
//                     symbol is hidden (-fvisibility=hidden); the
//                     <decoder>.exports test checks that the library exports
//                     exactly the four entry points of wavecrux_decoder.h.
//   WCX_NODISCARD     on every fallible common/ function: ignoring its result
//                     is a compiler warning, and warnings are errors (§4).
//   WCX_PRINTF(f, a)  on printf-like functions: argument f is the format, a
//                     the first variadic argument, so -Wformat checks callers.
//   WCX_IGNORE(expr)  discards a WCX_NODISCARD result on purpose (a cut-short
//                     diagnostic message, say). GCC does not accept a plain
//                     (void) cast for warn_unused_result; negating the value
//                     first uses it. Say why at the call site when not obvious.

#ifndef WCX_EXPORT_H
#define WCX_EXPORT_H

#ifdef _MSC_VER
// Empty on MSVC: the vendored ABI header declares the entry points without
// __declspec(dllexport), and MSVC rejects adding it on the definition
// (C2375). wcx_add_decoder() exports them through a generated .def file.
#define WCX_EXPORT
#elif defined(_WIN32) || defined(__CYGWIN__)
#define WCX_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define WCX_EXPORT __attribute__((visibility("default")))
#else
#define WCX_EXPORT
#endif

#if defined(__GNUC__) || defined(__clang__)
#define WCX_NODISCARD    __attribute__((warn_unused_result))
#define WCX_PRINTF(f, a) __attribute__((format(__printf__, f, a)))
#define WCX_MAYBE_UNUSED __attribute__((unused))
#elif defined(_MSC_VER)
// Empty on MSVC: SAL's _Check_return_ must then appear on every definition
// as well as the declaration (C28251 under /analyze), and GCC and Clang
// already enforce unused-result checks on every CI run.
#define WCX_NODISCARD
#define WCX_PRINTF(f, a)
#define WCX_MAYBE_UNUSED
#else
#define WCX_NODISCARD
#define WCX_PRINTF(f, a)
#define WCX_MAYBE_UNUSED
#endif

#define WCX_IGNORE(expr) ((void)!(expr))

#endif // WCX_EXPORT_H
