// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Chili.CHIPS
//
// Request/completion pairing (SPEC.md §5.3). A completion names its
// requester and tag (PCIe Base Spec §2.2.9), so the two together find the
// request; split completions follow the rules of §2.3.1.1.

#include "tlp_pair.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tlp_format.h"

void tlp_pair_note(tlp_pairs *p, const uint32_t *dw, uint32_t n) {
    const uint32_t dw0 = dw[0];
    if (n < tlp_header_dw(dw0)) {
        return;
    }
    const uint32_t type = (dw0 >> 24) & 0x1Fu;
    const bool has_data = ((dw0 >> 29) & 0x2u) != 0u;
    tlp_match m = {TLP_MATCH_NONE, 0u, 0u};
    if (tlp_kind_of(dw0) == TLP_KIND_CFG) {
        m.kind = has_data ? TLP_MATCH_CFG_WRITE : TLP_MATCH_CFG_READ;
        m.reg = dw[2] & 0xFFCu;
    } else if (tlp_kind_of(dw0) == TLP_KIND_MEM && type == 0x02u) {
        m.kind = has_data ? TLP_MATCH_IO_WRITE : TLP_MATCH_IO_READ;
        m.addr = dw[2] & ~3u;
    } else if (tlp_kind_of(dw0) == TLP_KIND_MEM && !has_data) {
        m.kind = TLP_MATCH_MEM;
        m.addr = (tlp_header_dw(dw0) == 4u ? dw[3] : dw[2]) & ~3u;
    } else {
        return;
    }
    const uint32_t tag = tlp_tag(dw0, dw[1] >> 8);
    const uint16_t requester = (uint16_t)(dw[1] >> 16);
    tlp_pair_entry *ways = p->e[tag];
    size_t slot = TLP_PAIR_WAYS;
    for (size_t w = 0; w < TLP_PAIR_WAYS; w++) {
        if (ways[w].valid && ways[w].requester == requester) {
            slot = w; // the same requester reuses its tag: replace
            break;
        }
        if (!ways[w].valid && slot == TLP_PAIR_WAYS) {
            slot = w;
        }
    }
    if (slot == TLP_PAIR_WAYS) {
        slot = 0; // all ways taken: replace the oldest
        for (size_t w = 1; w < TLP_PAIR_WAYS; w++) {
            if (ways[w].seq < ways[slot].seq) {
                slot = w;
            }
        }
    }
    if (!ways[slot].valid || ways[slot].requester != requester) {
        ways[slot].seq = ++p->seq; // a replaced request keeps its place
    }
    ways[slot].valid = true;
    ways[slot].requester = requester;
    ways[slot].match = m;
}

tlp_match tlp_pair_take(tlp_pairs *p, const uint32_t *dw) {
    const tlp_match none = {TLP_MATCH_NONE, 0u, 0u};
    const uint32_t dw0 = dw[0];
    const uint32_t tag = tlp_tag(dw0, dw[2] >> 8);
    const uint16_t requester = (uint16_t)(dw[2] >> 16);
    tlp_pair_entry *ways = p->e[tag];
    for (size_t w = 0; w < TLP_PAIR_WAYS; w++) {
        tlp_pair_entry *e = &ways[w];
        if (!e->valid || e->requester != requester) {
            continue;
        }
        // The last part of a split read: its byte count, counted from the
        // lower address's offset in the first DW, fits its payload.
        const uint32_t len = dw0 & 0x3FFu;
        const uint32_t payload = 4u * (len != 0u ? len : 1024u);
        const bool has_data = ((dw0 >> 29) & 0x2u) != 0u;
        const uint32_t needed = tlp_byte_count(dw[1]) + (dw[2] & 0x3u);
        if (!has_data || needed <= payload) {
            e->valid = false;
        }
        return e->match;
    }
    return none;
}
