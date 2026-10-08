// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The PIPE decoder's front end (SPEC.md §7): pipeline events become ordered
// set, idle and frame transactions, with runs of identical TS1, TS2, FTS and
// EIEOS coalesced into one transaction when `coalesce` is on.

#ifndef PCIE_PIPE_FE_H
#define PCIE_PIPE_FE_H

#include <stdbool.h>
#include <stdint.h>

#include "pcie_pipeline.h"
#include "pcie_sink.h"

typedef struct pcie_pipe_fe {
    pcie_sink *sink;
    bool coalesce;
    bool show_idle;
    // The coalesced run not yet emitted (§7 "Coalescing").
    bool pending;
    pcie_event_kind pending_kind; // PCIE_EV_TS, PCIE_EV_FTS or PCIE_EV_EIEOS
    pcie_ts pending_ts;
    uint64_t pending_count;
    uint64_t pending_start_fs;
    uint64_t pending_end_fs;
} pcie_pipe_fe;

void pcie_pipe_fe_init(pcie_pipe_fe *fe, pcie_sink *sink, bool coalesce, bool show_idle);

// pcie_event_fn for a pipeline whose ctx is the pcie_pipe_fe.
void pcie_pipe_fe_event(void *ctx, const pcie_event *ev);

#endif // PCIE_PIPE_FE_H
