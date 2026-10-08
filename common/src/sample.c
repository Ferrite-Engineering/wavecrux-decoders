// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "wcx/sample.h"

#include <string.h>

bool wcx_layout_build_bound(wcx_layout *l, const wcx_signal_spec *specs, size_t n,
                            const bool *bound, wcx_error *err) {
    if (l == NULL) {
        return false;
    }
    memset(l, 0, sizeof *l);
    if (n > WCX_MAX_SIGNALS || (n > 0 && (specs == NULL || bound == NULL))) {
        wcx_error_set(err, "decoder declares more than %u signals", WCX_MAX_SIGNALS);
        return false;
    }
    bool seen_optional = false;
    uint32_t offset = 0;
    for (size_t i = 0; i < n; i++) {
        const wcx_signal_spec *s = &specs[i];
        const char *name = s->name != NULL ? s->name : "";
        if (s->bit_width == 0 || s->bit_width > WCX_MAX_SIGNAL_BITS) {
            wcx_error_set(err, "signal \"%s\" must be 1 to %u bits wide", name,
                          WCX_MAX_SIGNAL_BITS);
            return false;
        }
        if (seen_optional && !s->optional) {
            wcx_error_set(err,
                          "signal \"%s\" is required but declared after an optional signal; "
                          "WaveCrux packs required signals first",
                          name);
            return false;
        }
        seen_optional = seen_optional || s->optional;
        if (!bound[i]) {
            if (!s->optional) {
                wcx_error_set(err, "required signal \"%s\" is not bound", name);
                return false;
            }
            continue;
        }
        l->sig[i].bound = true;
        l->sig[i].offset = offset;
        l->sig[i].width = s->bit_width;
        // At most 64 signals of at most 4096 bits: no overflow.
        offset += s->bit_width;
    }
    l->count = n;
    l->total_bits = offset;
    l->encoded_bytes = ((size_t)offset * 2u + 7u) / 8u;
    return true;
}

bool wcx_layout_build(wcx_layout *l, const wcx_signal_spec *specs, size_t n, const wcx_config *cfg,
                      wcx_error *err) {
    bool bound[WCX_MAX_SIGNALS] = {false};
    if (n > WCX_MAX_SIGNALS) {
        wcx_error_set(err, "decoder declares more than %u signals", WCX_MAX_SIGNALS);
        if (l != NULL) {
            memset(l, 0, sizeof *l);
        }
        return false;
    }
    for (size_t i = 0; i < n && specs != NULL; i++) {
        bound[i] = wcx_config_is_bound(cfg, specs[i].name);
    }
    return wcx_layout_build_bound(l, specs, n, bound, err);
}

bool wcx_layout_check_sample(const wcx_layout *l, const WcSample *s, wcx_error *err) {
    if (l == NULL || s == NULL) {
        wcx_error_set(err, "the host passed no sample");
        return false;
    }
    if (s->bit_width != l->total_bits) {
        wcx_error_set(err,
                      "sample is %lu bits wide but the bound signals need %lu bits; "
                      "decoding stopped",
                      (unsigned long)s->bit_width, (unsigned long)l->total_bits);
        return false;
    }
    if (l->total_bits > 0 && s->bits_ptr == NULL) {
        wcx_error_set(err, "sample has no data buffer; decoding stopped");
        return false;
    }
    return true;
}

bool wcx_signal_bound(const wcx_layout *l, size_t sig) {
    return l != NULL && sig < l->count && l->sig[sig].bound;
}

bool wcx_sample_read_slice(const wcx_layout *l, const uint8_t *bits, size_t sig, uint32_t lsb,
                           uint32_t width, uint64_t *value, bool *unknown) {
    if (value != NULL) {
        *value = 0;
    }
    if (unknown != NULL) {
        *unknown = false;
    }
    if (!wcx_signal_bound(l, sig) || bits == NULL || value == NULL || unknown == NULL ||
        width == 0 || width > 64u || lsb >= l->sig[sig].width || width > l->sig[sig].width - lsb) {
        return false;
    }
    uint64_t v = 0;
    bool any_unknown = false;
    const size_t first = (size_t)l->sig[sig].offset + lsb;
    for (uint32_t i = 0; i < width; i++) {
        // Two buffer bits per signal bit; the pair never straddles a byte
        // because it starts at an even bit position.
        const size_t pos = (first + i) * 2u;
        const unsigned byte = bits[pos >> 3u];
        const unsigned shift = (unsigned)(pos & 7u);
        v |= (uint64_t)((byte >> shift) & 1u) << i;
        any_unknown = any_unknown || ((byte >> (shift + 1u)) & 1u) != 0;
    }
    *value = v;
    *unknown = any_unknown;
    return true;
}

bool wcx_sample_read(const wcx_layout *l, const uint8_t *bits, size_t sig, uint64_t *value,
                     bool *unknown) {
    if (!wcx_signal_bound(l, sig)) {
        if (value != NULL) {
            *value = 0;
        }
        if (unknown != NULL) {
            *unknown = false;
        }
        return false;
    }
    return wcx_sample_read_slice(l, bits, sig, 0, l->sig[sig].width, value, unknown);
}

bool wcx_sample_read_bit(const wcx_layout *l, const uint8_t *bits, size_t sig, uint32_t bit,
                         bool *level, bool *unknown) {
    uint64_t v = 0;
    bool u = false;
    const bool ok = wcx_sample_read_slice(l, bits, sig, bit, 1, &v, &u);
    if (level != NULL) {
        *level = v != 0;
    }
    if (unknown != NULL) {
        *unknown = u;
    }
    return ok;
}
