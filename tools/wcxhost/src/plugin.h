// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Loads a plugin library the way WaveCrux's FfiDecoderLoader._loadOne does
// (lib/services/decoders/ffi/ffi_decoder_loader_io.dart lines 298-497):
// open, bind wavecrux_decoder_abi_version and wavecrux_decoder_register
// (both required), reject a different ABI major, call register twice (a
// sizing call with no buffer, then a populate call), and read the optional
// ABI 1.1 name and description.

#ifndef WCXH_PLUGIN_H
#define WCXH_PLUGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wavecrux_decoder.h"
#include "wcx/error.h"
#include "wcx/export.h"

typedef struct wcxh_plugin {
    void *lib;
    uint32_t abi_version;
    WcDecoderDef *defs;
    size_t count;
    const char *name;        // NULL when not provided
    const char *description; // NULL when not provided
} wcxh_plugin;

WCX_NODISCARD bool wcxh_plugin_load(wcxh_plugin *p, const char *path, wcx_error *err);

// The decoder with `id`, its index in register order in *index.
const WcDecoderDef *wcxh_plugin_find(const wcxh_plugin *p, const char *id, size_t *index);

void wcxh_plugin_unload(wcxh_plugin *p);

#endif // WCXH_PLUGIN_H
