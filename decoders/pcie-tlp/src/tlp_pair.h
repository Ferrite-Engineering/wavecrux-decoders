// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Chili.CHIPS
//
// Requests remembered until their completions (SPEC.md §5.3, pairing),
// keyed by requester ID and 10-bit tag.

#ifndef TLP_PAIR_H
#define TLP_PAIR_H

#include <stdbool.h>
#include <stdint.h>

#include "tlp_format.h"

#define TLP_PAIR_TAGS 1024u // 10-bit tags (§2.2.6.2)
#define TLP_PAIR_WAYS 4u    // requesters per tag before the oldest is replaced

typedef struct tlp_pair_entry {
    bool valid;
    uint16_t requester;
    uint64_t seq; // when the entry was made: the smallest is the oldest
    tlp_match match;
} tlp_pair_entry;

typedef struct tlp_pairs {
    tlp_pair_entry e[TLP_PAIR_TAGS][TLP_PAIR_WAYS];
    uint64_t seq; // entries made so far
} tlp_pairs;

// Remembers a complete request (`n` >= its header DWs): configuration,
// memory read or I/O. Anything else is ignored.
void tlp_pair_note(tlp_pairs *p, const uint32_t *dw, uint32_t n);

// The request a completion (`n` >= 3) answers, or TLP_MATCH_NONE. Removed
// unless more split completions follow.
tlp_match tlp_pair_take(tlp_pairs *p, const uint32_t *dw);

#endif // TLP_PAIR_H
