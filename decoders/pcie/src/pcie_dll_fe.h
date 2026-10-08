// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The Data Link Layer decoder's front end (SPEC.md §8): closed packets
// become DLLP and TLP transactions with CRC-16 / LCRC checks, the light TLP
// header decode and sequence-number tracking. Ordered sets and idle produce
// nothing; the §6 errors are emitted as in the PIPE decoder.

#ifndef PCIE_DLL_FE_H
#define PCIE_DLL_FE_H

#include <stdbool.h>
#include <stdint.h>

#include "pcie_pipeline.h"
#include "pcie_sink.h"

typedef struct pcie_dll_fe {
    pcie_sink *sink;
    // Sequence tracking (§8.2): the sequence number the next good TLP
    // should carry, once a good TLP has been seen.
    bool have_next;
    uint32_t next_seq;
} pcie_dll_fe;

void pcie_dll_fe_init(pcie_dll_fe *fe, pcie_sink *sink);

// pcie_event_fn for a pipeline whose ctx is the pcie_dll_fe.
void pcie_dll_fe_event(void *ctx, const pcie_event *ev);

// The TLP name of SPEC.md §8.2 for a header byte 0 (Fmt in bits 7:5, Type
// in bits 4:0), or NULL for a type the table does not list.
const char *pcie_tlp_name(uint8_t byte0);

#endif // PCIE_DLL_FE_H
