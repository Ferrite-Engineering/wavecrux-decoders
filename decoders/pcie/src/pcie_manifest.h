// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Manifest fragments shared by the PIPE decoders (pcie_plugin.c) and the
// serial line decoders (pcie_serial_plugin.c), so the same parameter reads
// the same in both.

#ifndef PCIE_MANIFEST_H
#define PCIE_MANIFEST_H

#define MANIFEST_PARAM_SCRAMBLING                                                               \
    "{\"name\":\"scrambling\",\"kind\":\"enum\",\"default\":\"auto\","                          \
    "\"display_name\":\"Scrambling\","                                                          \
    "\"description\":\"Detect: run descrambled and plain decoding side by side until a packet " \
    "with a correct CRC settles it. On: descramble (Gen1/Gen2 LFSR, reset by COM). Off: the "   \
    "symbols are already plain, as in RTL simulation\","                                        \
    "\"enum_values\":[\"auto\",\"on\",\"off\"],"                                                \
    "\"enum_labels\":{\"auto\":\"Detect\",\"on\":\"On (descramble)\",\"off\":\"Off\"}}"

#define MANIFEST_PARAM_COALESCE                                                              \
    "{\"name\":\"coalesce\",\"kind\":\"bool\",\"default\":true,"                             \
    "\"display_name\":\"Coalesce repeated ordered sets\","                                   \
    "\"description\":\"One transaction per run of identical TS1, TS2, FTS or EIEOS ordered " \
    "sets, with a count\"}"

#define MANIFEST_PARAM_SHOW_IDLE                                   \
    "{\"name\":\"show_idle\",\"kind\":\"bool\",\"default\":false," \
    "\"display_name\":\"Show logical idle\","                      \
    "\"description\":\"One transaction per run of logical idle symbols between packets\"}"

#endif // PCIE_MANIFEST_H
