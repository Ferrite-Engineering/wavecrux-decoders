// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Per-handle string arena.
//
// Labels and fields_json handed to the host must stay valid until the next
// call on the same handle (wavecrux_decoder.h, "Lifetime and memory
// ownership"). The arena holds them: allocate freely while decoding one
// sample, and reset it when the host has copied the batch out. With the
// decoder base (wcx/decoder.h) the reset is done for you at exactly the
// right moment; never reset it yourself there.
//
// Bounded: the arena never holds more than `ceiling` bytes in total. An
// allocation that would exceed it returns NULL; the caller emits an error
// (the decoder base turns a NULL label or fields_json into the instance's
// error state automatically).
//
// No allocation per sample: memory is taken in blocks of `block_size` bytes
// and kept across resets, so once a decoder has reached its working set
// (normally after the first sample) the arena never calls malloc again.
//
// Pointers returned by the arena stay valid until wcx_arena_reset or
// wcx_arena_free; growing one allocation (wcx_arena_grow) never moves the
// others.

#ifndef WCX_ARENA_H
#define WCX_ARENA_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "wcx/export.h"

// Alignment of wcx_arena_alloc results (enough for any scalar type).
#define WCX_ARENA_ALIGN 16

typedef struct wcx_arena_block wcx_arena_block;

typedef struct wcx_arena {
    wcx_arena_block *head; // first block; blocks are kept across resets
    wcx_arena_block *cur;  // block allocations are currently served from
    size_t block_size;     // default size of a new block
    size_t ceiling;        // maximum bytes held across all blocks
    size_t reserved;       // bytes held across all blocks
} wcx_arena;

// Sets up an arena and allocates its first block. `block_size` must be > 0
// and <= `ceiling`. Returns false on bad arguments or allocation failure
// (the arena is then empty and safe to free).
WCX_NODISCARD bool wcx_arena_init(wcx_arena *a, size_t block_size, size_t ceiling);

// Releases every block. Safe on a zero-initialised or already freed arena.
void wcx_arena_free(wcx_arena *a);

// Forgets every allocation, keeping the blocks for reuse. Every pointer the
// arena returned before becomes invalid.
void wcx_arena_reset(wcx_arena *a);

// `size` bytes aligned to WCX_ARENA_ALIGN, uninitialised. NULL when the
// ceiling would be exceeded, on allocation failure, or for size 0.
WCX_NODISCARD void *wcx_arena_alloc(wcx_arena *a, size_t size);

// Grows the allocation `ptr` (of `old_size` bytes) to `new_size` bytes,
// keeping its contents. In place when `ptr` is the most recent allocation
// and its block has room; otherwise the bytes are copied to a new
// allocation (the old space is reclaimed at the next reset). Returns the
// (possibly moved) pointer, or NULL on failure, in which case `ptr` is
// unchanged and still valid. The result is only byte-aligned.
WCX_NODISCARD void *wcx_arena_grow(wcx_arena *a, void *ptr, size_t old_size, size_t new_size);

// NUL-terminated copy of `s` / of the first `n` bytes of `s`. NULL on
// failure or when `s` is NULL.
WCX_NODISCARD char *wcx_arena_strdup(wcx_arena *a, const char *s);
WCX_NODISCARD char *wcx_arena_strndup(wcx_arena *a, const char *s, size_t n);

// printf-style formatting into the arena (vsnprintf into checked space).
// Integer and string conversions only: %f depends on the locale (§9). Use
// wcx_fmt_hex / wcx_fmt_u64 or the JSON writer for numbers in output that
// must be byte-identical across platforms.
WCX_NODISCARD char *wcx_arena_printf(wcx_arena *a, const char *fmt, ...) WCX_PRINTF(2, 3);
WCX_NODISCARD char *wcx_arena_vprintf(wcx_arena *a, const char *fmt, va_list ap) WCX_PRINTF(2, 0);

// Bytes currently allocated (for tests and diagnostics).
size_t wcx_arena_used(const wcx_arena *a);

#endif // WCX_ARENA_H
