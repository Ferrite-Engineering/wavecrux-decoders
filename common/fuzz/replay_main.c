// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// main() for <decoder>_fuzz_replay: runs a fuzz target over the files named
// on the command line (fuzz/corpus/* and fuzz/regressions/*), so every
// ordinary build and test run replays every input that ever crashed
// (testing standard §7), on every compiler, without libFuzzer. With no
// arguments it runs the empty input once.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "wcx/fuzz_driver.h"
#include "wcx/tool_io.h"

#define REPLAY_MAX_INPUT ((size_t)64u * 1024u * 1024u)

int main(int argc, char **argv) {
    static const uint8_t empty[1] = {0};
    if (argc < 2) {
        (void)LLVMFuzzerTestOneInput(empty, 0);
        (void)fputs("replayed the empty input\n", stdout);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *data = NULL;
        size_t len = 0;
        wcx_error err = {{0}};
        if (!wcx_tool_read_file(argv[i], REPLAY_MAX_INPUT, &data, &len, &err)) {
            wcx_tool_printf(stderr, "replay: %s\n", err.msg);
            return 1;
        }
        (void)LLVMFuzzerTestOneInput((const uint8_t *)data, len);
        free(data);
    }
    wcx_tool_printf(stdout, "replayed %d inputs\n", argc - 1);
    return 0;
}
