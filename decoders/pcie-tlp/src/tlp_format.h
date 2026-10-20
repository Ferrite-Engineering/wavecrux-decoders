// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Chili.CHIPS
//
// One complete TLP to its label and fields_json (SPEC.md §5). Pure: no
// decoder state, every string in the caller's arena.

#ifndef TLP_FORMAT_H
#define TLP_FORMAT_H

#include <stdbool.h>
#include <stdint.h>

#include "wcx/arena.h"

// Longest TLP the decoder keeps: a 4 DW header and 1024 DW of payload
// (PCIe Base Spec §2.2.2, Length field). Longer TLPs are counted, not kept.
#define TLP_MAX_DW (4u + 1024u)

typedef enum tlp_kind {
    TLP_KIND_CFG,   // Type 0x04, 0x05
    TLP_KIND_MEM,   // Type 0x00, 0x01, 0x02 (memory, locked memory, I/O)
    TLP_KIND_CPL,   // Type 0x0A, 0x0B
    TLP_KIND_MSG,   // Type 1_0rrr
    TLP_KIND_OTHER, // anything else
} tlp_kind;

// What a completion answers (SPEC.md §5.3, pairing).
typedef enum tlp_match_kind {
    TLP_MATCH_NONE = 0,
    TLP_MATCH_CFG_READ,
    TLP_MATCH_CFG_WRITE,
    TLP_MATCH_MEM, // memory read
    TLP_MATCH_IO_READ,
    TLP_MATCH_IO_WRITE,
} tlp_match_kind;

typedef struct tlp_match {
    tlp_match_kind kind;
    uint32_t reg;  // configuration register offset
    uint32_t addr; // memory or I/O address, low 32 bits, bits [1:0] clear
} tlp_match;

typedef struct tlp_text {
    const char *label;  // NULL when the arena ran out
    const char *fields; // NULL when the arena ran out
    bool is_error;
} tlp_text;

tlp_kind tlp_kind_of(uint32_t dw0);

// Header DWs: 3, or 4 when Fmt bit 0 is set.
uint32_t tlp_header_dw(uint32_t dw0);

// DWs the header announces, or 0 for a TLP prefix (Fmt bit 2), which is
// not checked.
uint32_t tlp_expected_dw(uint32_t dw0);

// Register name for a configuration offset; "reg 0x<3>" from the arena for
// one without a name.
const char *tlp_reg_name(wcx_arena *a, uint32_t reg);

const char *tlp_status_name(uint32_t status);

// `n` DWs were received, the first min(n, TLP_MAX_DW) are in `dw`, and n is
// at least tlp_header_dw(dw[0]). `m` is the request a completion answers
// (TLP_MATCH_NONE for none, and for every other kind).
void tlp_format(wcx_arena *a, const uint32_t *dw, uint32_t n, const tlp_match *m, bool full,
                tlp_text *out);

#endif // TLP_FORMAT_H
