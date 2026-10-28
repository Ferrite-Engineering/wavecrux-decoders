// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The Data Link Layer decoder's front end (SPEC.md §8): closed packets
// become DLLP and TLP transactions with CRC-16 / LCRC checks, the light TLP
// header decode and sequence-number tracking. Ordered sets and idle produce
// nothing; the §6 errors are emitted as in the PIPE decoder. With
// `coalesce` on, runs of identical DLLPs and of identical InitFC triplets
// become one transaction with a count (§8.3).

#ifndef PCIE_DLL_FE_H
#define PCIE_DLL_FE_H

#include <stdbool.h>
#include <stdint.h>

#include "pcie_pipeline.h"
#include "pcie_sink.h"

#define PCIE_DLLP_BYTES   6u // 4 bytes + CRC-16 (§8.1)
#define PCIE_TRIPLET_DLLP 3u // InitFC P, NP and Cpl (§8.3)

// One coalescing DLLP (§8.3): its six bytes and its span.
typedef struct pcie_dllp_item {
    uint8_t b[PCIE_DLLP_BYTES];
    uint64_t start_fs;
    uint64_t end_fs;
} pcie_dllp_item;

typedef struct pcie_dll_fe {
    pcie_sink *sink;
    bool coalesce;
    // Sequence tracking (§8.2): the sequence number the next good TLP
    // should carry, once a good TLP has been seen.
    bool have_next;
    uint32_t next_seq;
    // §8.3: DLLPs that may still begin an InitFC triplet (at most two; a
    // third completes or breaks it), and the run not yet emitted: `run_n`
    // DLLPs per unit (0: no run, 1: single, 3: triplet), in wire order.
    unsigned held;
    pcie_dllp_item hold[PCIE_TRIPLET_DLLP];
    unsigned run_n;
    uint8_t run_bytes[PCIE_TRIPLET_DLLP * PCIE_DLLP_BYTES];
    uint64_t run_count;
    uint64_t run_start_fs;
    uint64_t run_end_fs;
} pcie_dll_fe;

void pcie_dll_fe_init(pcie_dll_fe *fe, pcie_sink *sink, bool coalesce);

// pcie_event_fn for a pipeline whose ctx is the pcie_dll_fe.
void pcie_dll_fe_event(void *ctx, const pcie_event *ev);

// The TLP name of SPEC.md §8.2 for a header byte 0 (Fmt in bits 7:5, Type
// in bits 4:0), or NULL for a type the table does not list.
const char *pcie_tlp_name(uint8_t byte0);

#endif // PCIE_DLL_FE_H
