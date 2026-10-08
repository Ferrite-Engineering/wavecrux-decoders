#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Check that a decoder plugin exports exactly the WaveCrux ABI entry points.

C coding standard §2: everything but the four entry points of
include/wavecrux_decoder.h is static or hidden. Reads the dynamic symbol
table with nm (Linux, macOS) or dumpbin /exports (Windows).

Symbols the toolchain itself adds are ignored: the ELF linker's markers
(_init, _fini, _edata, ...) and the runtime hooks of sanitizer and coverage
instrumentation, which only appear in instrumented builds.

usage: check_exports.py [--nm NM] [--dumpbin DUMPBIN] <library>
"""

import argparse
import pathlib
import re
import subprocess
import sys

EXPECTED = {
    "wavecrux_decoder_abi_version",
    "wavecrux_decoder_register",
    "wavecrux_decoder_plugin_name",
    "wavecrux_decoder_plugin_description",
}

TOOLCHAIN_SYMBOLS = {"_init", "_fini", "_edata", "_end", "__bss_start", "_DYNAMIC", "_etext"}
INSTRUMENTATION_PREFIXES = (
    "__asan",
    "__ubsan",
    "__lsan",
    "__sanitizer",
    "__sancov",
    "__odr_asan",
    "__llvm_profile",
    "__llvm_prf",
    "___asan",
    # The ELF linker defines __start_<section> / __stop_<section> for every
    # section whose name is a C identifier; instrumented builds create such
    # sections (asan_globals, __sancov_*, __llvm_prf_*). Measured on Ubuntu
    # 24.04 with clang 18; release builds have none.
    "__start_asan_globals",
    "__stop_asan_globals",
    "__start___sancov",
    "__stop___sancov",
    "__start___llvm_prf",
    "__stop___llvm_prf",
)


def symbols_nm(nm: str, lib: pathlib.Path) -> set:
    if sys.platform == "darwin":
        args = [nm, "-gUj", str(lib)]
    else:
        args = [nm, "-D", "--defined-only", "--format=posix", str(lib)]
    out = subprocess.run(args, check=True, capture_output=True, text=True).stdout
    names = set()
    for line in out.splitlines():
        line = line.strip()
        if not line or line.endswith(":"):
            continue
        name = line.split()[0]
        if sys.platform == "darwin" and name.startswith("_"):
            name = name[1:]  # Mach-O prefixes C symbols with an underscore
        names.add(name)
    return names


def symbols_dumpbin(dumpbin: str, lib: pathlib.Path) -> set:
    out = subprocess.run(
        [dumpbin, "/nologo", "/exports", str(lib)], check=True, capture_output=True, text=True
    ).stdout
    names = set()
    # "    ordinal hint RVA      name"
    for match in re.finditer(r"^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)", out, re.M):
        names.add(match.group(1))
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--dumpbin", default="")
    parser.add_argument("library", type=pathlib.Path)
    args = parser.parse_args()

    if args.library.suffix.lower() == ".dll":
        if not args.dumpbin:
            print("check_exports: dumpbin not found; cannot read a .dll export table")
            return 1
        found = symbols_dumpbin(args.dumpbin, args.library)
    else:
        found = symbols_nm(args.nm, args.library)

    found = {
        s
        for s in found
        if s not in TOOLCHAIN_SYMBOLS and not s.startswith(INSTRUMENTATION_PREFIXES)
    }
    missing = sorted(EXPECTED - found)
    extra = sorted(found - EXPECTED)
    if missing or extra:
        print(f"check_exports: {args.library.name} does not export exactly the ABI entry points")
        for name in missing:
            print(f"  missing: {name}")
        for name in extra:
            print(f"  extra:   {name}  (make it static, or it is a hidden-visibility leak)")
        return 1
    print(f"check_exports: {args.library.name} exports exactly the {len(EXPECTED)} entry points")
    return 0


if __name__ == "__main__":
    sys.exit(main())
