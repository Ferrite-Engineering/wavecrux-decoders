// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// The two serial line decoder classes (SPEC.md §11), registered by
// pcie_plugin.c beside the eight PIPE classes.

#ifndef PCIE_SERIAL_PLUGIN_H
#define PCIE_SERIAL_PLUGIN_H

#include "wcx/decoder.h"

extern const wcx_decoder_class pcie_serial_class;     // ferrite.pcie_serial
extern const wcx_decoder_class pcie_serial_dll_class; // ferrite.pcie_serial_dll

#endif // PCIE_SERIAL_PLUGIN_H
