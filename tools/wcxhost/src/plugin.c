// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "plugin.h"

#include <stdlib.h>
#include <string.h>

#include "platform.h"

typedef uint32_t (*abi_version_fn)(void);
typedef int32_t (*register_fn)(WcDecoderDef *, size_t *);
typedef const char *(*string_fn)(void);

// More decoders than any plugin ships (the SigRok bridge has about 130).
#define MAX_DECODERS 100000u

static bool invoke_register(wcxh_plugin *p, register_fn reg, wcx_error *err) {
    // Lines 531-576: the sizing call passes NULL and a count of 0.
    size_t count = 0;
    int32_t rc = reg(NULL, &count);
    if (rc != WC_DECODER_OK && rc != WC_DECODER_NEED_MORE_SLOTS) {
        wcx_error_set(err, "register returned rc=%d on the sizing call", (int)rc);
        return false;
    }
    if (count == 0 || count > MAX_DECODERS) {
        wcx_error_set(err, "register reported %lu decoders (line 397: zero means nothing to load)",
                      (unsigned long)count);
        return false;
    }
    p->defs = calloc(count, sizeof *p->defs);
    if (p->defs == NULL) {
        wcx_error_set(err, "out of memory");
        return false;
    }
    size_t populated = count;
    rc = reg(p->defs, &populated);
    if (rc != WC_DECODER_OK) {
        wcx_error_set(err, "register returned rc=%d on the populate call", (int)rc);
        return false;
    }
    if (populated == 0 || populated > count) {
        wcx_error_set(err, "register populated %lu of %lu slots", (unsigned long)populated,
                      (unsigned long)count);
        return false;
    }
    p->count = populated;
    for (size_t i = 0; i < populated; i++) {
        const WcDecoderDef *d = &p->defs[i];
        // _RawDecoderDef.fromStruct dereferences all three strings and the
        // adapter calls all four callbacks: NULL would crash WaveCrux.
        if (d->id == NULL || d->display_name == NULL || d->manifest_json == NULL ||
            d->create == NULL || d->feed == NULL || d->flush == NULL || d->destroy == NULL) {
            wcx_error_set(err, "decoder #%lu has a NULL string or callback", (unsigned long)i);
            return false;
        }
    }
    return true;
}

bool wcxh_plugin_load(wcxh_plugin *p, const char *path, wcx_error *err) {
    memset(p, 0, sizeof *p);
    char why[512] = {0};
    p->lib = wcxh_dl_open(path, why, sizeof why);
    if (p->lib == NULL) {
        wcx_error_set(err, "cannot open \"%s\": %s", path, why);
        return false;
    }
    const abi_version_fn version =
        (abi_version_fn)wcxh_dl_sym(p->lib, "wavecrux_decoder_abi_version");
    const register_fn reg = (register_fn)wcxh_dl_sym(p->lib, "wavecrux_decoder_register");
    if (version == NULL || reg == NULL) {
        wcx_error_set(err,
                      "\"%s\" does not export wavecrux_decoder_abi_version and "
                      "wavecrux_decoder_register",
                      path);
        return false;
    }
    p->abi_version = version();
    // Lines 350-365.
    if (WAVECRUX_DECODER_ABI_GET_MAJOR(p->abi_version) != WAVECRUX_DECODER_ABI_MAJOR) {
        wcx_error_set(err, "plugin reports ABI major %u; host requires %u",
                      (unsigned)WAVECRUX_DECODER_ABI_GET_MAJOR(p->abi_version),
                      (unsigned)WAVECRUX_DECODER_ABI_MAJOR);
        return false;
    }
    if (!invoke_register(p, reg, err)) {
        return false;
    }
    // Lines 471-480: optional ABI 1.1 identity.
    const string_fn name = (string_fn)wcxh_dl_sym(p->lib, "wavecrux_decoder_plugin_name");
    const string_fn desc = (string_fn)wcxh_dl_sym(p->lib, "wavecrux_decoder_plugin_description");
    p->name = name != NULL ? name() : NULL;
    p->description = desc != NULL ? desc() : NULL;
    return true;
}

const WcDecoderDef *wcxh_plugin_find(const wcxh_plugin *p, const char *id, size_t *index) {
    for (size_t i = 0; i < p->count; i++) {
        if (strcmp(p->defs[i].id, id) == 0) {
            if (index != NULL) {
                *index = i;
            }
            return &p->defs[i];
        }
    }
    return NULL;
}

void wcxh_plugin_unload(wcxh_plugin *p) {
    if (p == NULL) {
        return;
    }
    free(p->defs);
    // Deliberately not dlclose'd: like WaveCrux, which keeps every loaded
    // library mapped (_LoadedPlugin), and so LeakSanitizer can symbolise
    // anything the plugin leaked.
    memset(p, 0, sizeof *p);
}
