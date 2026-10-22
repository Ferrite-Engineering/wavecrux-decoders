// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Chili.CHIPS
//
// libFuzzer target for the pcie_tlp plugin.

#include <stddef.h>
#include <stdint.h>

#include "wavecrux_decoder.h"
#include "wcx/fuzz_driver.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    return wcx_fuzz_run(data, size, wavecrux_decoder_register);
}
