// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Where each bound signal lives in a WcSample, and reading it.
//
// The layout is exactly what WaveCrux's loader builds
// (lib/services/decoders/ffi/ffi_decoder_loader_io.dart in the open core):
//
//   * Signals are taken in manifest declaration order: every entry of
//     "signals" first, then every entry of "optional_signals" (loader
//     _adaptDefinition, the attachBindings call).
//   * Only signals with a binding take space; an unbound optional signal is
//     skipped and the next signal moves down (loader decode(), the
//     activeBindings loop).
//   * A signal's width is its manifest bit_width, 1 when absent.
//   * WcSample.bit_width is the total of the bound widths (signal bits, not
//     buffer bits).
//   * Each signal bit takes two buffer bits, LSB-first: bit 2k is the level
//     (1 only for '1'), bit 2k+1 is "unknown" (set for x/X/z/Z). Signal bit
//     0 of the first bound signal is buffer bit 0 (loader
//     _packBindingValue). A signal's bits run across byte boundaries.
//   * The buffer is (2 * bit_width + 7) / 8 bytes, but never less than 32
//     (the loader's scratch minimum). Read only what the layout covers.
//
// Decoders describe their signals once, in manifest order, with
// wcx_signal_spec, and refer to them by index (an enum in the decoder).
// The decoder base (wcx/decoder.h) builds the layout, checks it against the
// manifest JSON, and checks every sample's bit_width before the decoder sees
// a single bit; decoders using this header directly must call
// wcx_layout_check_sample themselves (C coding standard §2).

#ifndef WCX_SAMPLE_H
#define WCX_SAMPLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wavecrux_decoder.h"
#include "wcx/config.h"
#include "wcx/error.h"
#include "wcx/export.h"

// Ceilings: no supported protocol binds more than 64 signals, and the widest
// bus a decoder reads is far below 4096 bits.
#define WCX_MAX_SIGNALS     64u
#define WCX_MAX_SIGNAL_BITS 4096u

// One signal, as declared in the manifest.
typedef struct wcx_signal_spec {
    const char *name;   // manifest "name"
    uint32_t bit_width; // manifest "bit_width" (write 1 when the manifest omits it)
    bool optional;      // true for entries of "optional_signals"
} wcx_signal_spec;

typedef struct wcx_layout_entry {
    bool bound;
    uint32_t offset; // first signal bit within the sample (0-based)
    uint32_t width;
} wcx_layout_entry;

typedef struct wcx_layout {
    size_t count; // number of declared signals
    wcx_layout_entry sig[WCX_MAX_SIGNALS];
    uint32_t total_bits;  // what WcSample.bit_width must equal
    size_t encoded_bytes; // (2 * total_bits + 7) / 8: the bytes a decoder may read
} wcx_layout;

// Builds the layout for `specs` (manifest order: required, then optional)
// from the bindings in `cfg`. Fails when the table is malformed (too many
// signals, a width of 0 or above WCX_MAX_SIGNAL_BITS, an optional signal
// declared before a required one) or a required signal is not bound.
WCX_NODISCARD bool wcx_layout_build(wcx_layout *l, const wcx_signal_spec *specs, size_t n,
                                    const wcx_config *cfg, wcx_error *err);

// Same, with the bound flags given directly (bound[i] for specs[i]).
WCX_NODISCARD bool wcx_layout_build_bound(wcx_layout *l, const wcx_signal_spec *specs, size_t n,
                                          const bool *bound, wcx_error *err);

// Checks a sample against the layout: non-NULL, bit_width equal to
// total_bits, and a bits pointer whenever there are bits to read.
WCX_NODISCARD bool wcx_layout_check_sample(const wcx_layout *l, const WcSample *s, wcx_error *err);

// True when signal `sig` is declared and bound.
bool wcx_signal_bound(const wcx_layout *l, size_t sig);

// Reads a whole signal of at most 64 bits: its level bits into *value
// (signal bit i in value bit i; X and Z read as 0) and whether any bit is X
// or Z into *unknown. False (value 0, unknown false) when the signal is
// unbound, out of range or wider than 64 bits.
WCX_NODISCARD bool wcx_sample_read(const wcx_layout *l, const uint8_t *bits, size_t sig,
                                   uint64_t *value, bool *unknown);

// Reads bits [lsb, lsb + width) of a signal (width 1..64), as above.
WCX_NODISCARD bool wcx_sample_read_slice(const wcx_layout *l, const uint8_t *bits, size_t sig,
                                         uint32_t lsb, uint32_t width, uint64_t *value,
                                         bool *unknown);

// Reads one bit of a signal: its level and unknown flag.
WCX_NODISCARD bool wcx_sample_read_bit(const wcx_layout *l, const uint8_t *bits, size_t sig,
                                       uint32_t bit, bool *level, bool *unknown);

#endif // WCX_SAMPLE_H
