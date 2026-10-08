// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Shared libFuzzer driver for decoder plugins (testing standard §7).
//
// A decoder's fuzz target is two lines:
//
//   #include "wcx/fuzz_driver.h"
//   int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
//       return wcx_fuzz_run(data, size, wavecrux_decoder_register);
//   }
//
// The driver plays the host: it registers the plugin (two-call pattern),
// picks a decoder, creates it with a fuzzed or structurally valid
// config_json, feeds a fuzzed sample stream (fuzzed widths, timestamps that
// go backwards, X/Z everywhere), flushes and destroys. It checks the ABI
// contract on every call and aborts (a crash libFuzzer reports) when the
// plugin breaks it: a NEED_MORE_SLOTS whose count does not exceed the slots
// offered, a retry that does not deliver exactly that count, a NULL or
// ill-formed UTF-8 label, a fields_json that is not a JSON object, a
// non-zero reserved field.
//
// Input format (all integers little-endian):
//
//   u8   decoder selector (index into the registered decoders, modulo count)
//   u8   flags: WCX_FUZZ_RAW_CONFIG   the next L bytes are config_json verbatim
//                                     (otherwise they are the "parameters" value
//                                     of a valid config that binds every signal)
//              WCX_FUZZ_BIND_OPTIONAL bind optional signals too (template config)
//              WCX_FUZZ_TINY_SLOTS    offer 1 slot on every call (forces retries)
//   u16  L, then L bytes of config (clipped to what is left)
//   records until the input ends (at most WCX_FUZZ_MAX_SAMPLES):
//     u8      record flags: WCX_FUZZ_WIDTH_DELTA  next i8 is added to the expected width
//                           WCX_FUZZ_WIDTH_ABS    next u32 is the width (mod WCX_FUZZ_MAX_WIDTH)
//                           WCX_FUZZ_BACKWARDS    the timestamp moves back by dt
//     varint  dt in femtoseconds (LEB128, at most 10 bytes)
//     bytes   the packed bits for the claimed width, (2 * width + 7) / 8 bytes
//             (fewer if the input ends; the rest are zero). The buffer is
//             never smaller than 32 bytes, like WaveCrux's.
//
// wcx_fuzz_seed_* write that format, so a fixture can become a seed
// (`wcxhost ... --fuzz-seed <file>`).
//
// The same target source also links against wcx_fuzz_replay_main, which
// replays corpus and regression files in every ordinary build and test run.

#ifndef WCX_FUZZ_DRIVER_H
#define WCX_FUZZ_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wavecrux_decoder.h"

#define WCX_FUZZ_RAW_CONFIG    0x01u
#define WCX_FUZZ_BIND_OPTIONAL 0x02u
#define WCX_FUZZ_TINY_SLOTS    0x04u

#define WCX_FUZZ_WIDTH_DELTA 0x01u
#define WCX_FUZZ_WIDTH_ABS   0x02u
#define WCX_FUZZ_BACKWARDS   0x04u

#define WCX_FUZZ_MAX_SAMPLES 4096u
#define WCX_FUZZ_MAX_WIDTH   65536u

typedef int32_t (*wcx_register_fn)(WcDecoderDef *out_defs, size_t *inout_count);

// libFuzzer's entry point, defined by each decoder's fuzz target.
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// Runs one input against the plugin whose register function is given.
// Always returns 0 (libFuzzer's convention); contract violations abort.
int wcx_fuzz_run(const uint8_t *data, size_t size, wcx_register_fn register_fn);

// Seed writer.
typedef struct wcx_fuzz_seed {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool failed;
} wcx_fuzz_seed;

void wcx_fuzz_seed_begin(wcx_fuzz_seed *s, uint8_t selector, uint8_t flags, const char *config,
                         size_t config_len);
void wcx_fuzz_seed_sample(wcx_fuzz_seed *s, uint64_t dt_fs, const uint8_t *bits, size_t nbytes);
void wcx_fuzz_seed_free(wcx_fuzz_seed *s);

#endif // WCX_FUZZ_DRIVER_H
