// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// What the two front ends share: the error transactions of SPEC.md §6 and
// a few formatting helpers.

#ifndef PCIE_FE_COMMON_H
#define PCIE_FE_COMMON_H

#include <stdbool.h>
#include <stdint.h>

#include "pcie_pipeline.h"
#include "pcie_sink.h"

// U+00D7 MULTIPLICATION SIGN, as UTF-8, for "×N" labels. Kept as escapes so
// the source stays plain ASCII for every compiler's default source charset.
#define PCIE_TIMES "\xC3\x97"

// Emits the §6 transaction for an INVALID_K, TRUNCATED, UNMATCHED_END,
// K_IN_PACKET, UNEXPECTED_K, UNKNOWN_OS or STRAY_RUN event.
void pcie_fe_emit_error(pcie_sink *sink, const pcie_event *ev);

// §10.2, §10.7: a DLLP ended by EDB is one `dllp_edb` error in both
// decoders (`ev` is a PCIE_EV_PACKET with tlp = false and edb = true).
void pcie_fe_emit_dllp_edb(pcie_sink *sink, const pcie_event *ev);

// An arena string for a "%s" argument: "" when the arena returned NULL.
// Passing NULL to %s is undefined behaviour; once the arena is exhausted
// the final label or fields_json is NULL too, so the base (or the detector)
// still sees the failure and the substitute never reaches a user.
static inline const char *pcie_str(const char *s) {
    return s != NULL ? s : "";
}

// The 12-bit TLP sequence number from the first two body bytes (§8.2).
uint32_t pcie_tlp_seq(const uint8_t head[PCIE_HEAD_KEEP]);

#endif // PCIE_FE_COMMON_H
