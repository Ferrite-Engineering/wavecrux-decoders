#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Run clang-format over the repository's C sources.

  run_clang_format.py --clang-format <bin> [--check] <repo root>

The files are git's tracked and untracked-but-not-ignored *.c and *.h (so a
new file is checked before its first commit), minus the vendored ABI header
and third_party/. --check is `clang-format --dry-run --Werror`; without it
the files are rewritten in place.
"""

import argparse
import pathlib
import subprocess
import sys

EXCLUDED_PREFIXES = ("third_party/", "include/wavecrux_decoder.h")


def sources(root: pathlib.Path) -> list:
    out = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "--", "*.c", "*.h"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    files = sorted({line for line in out.splitlines() if line and not line.startswith(EXCLUDED_PREFIXES)})
    return [f for f in files if (root / f).exists()]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clang-format", required=True)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("root", type=pathlib.Path)
    args = parser.parse_args()
    if not args.clang_format or args.clang_format.endswith("NOTFOUND"):
        print("run_clang_format: clang-format not found (set WCX_CLANG_FORMAT)")
        return 1
    files = sources(args.root)
    mode = ["--dry-run", "--Werror"] if args.check else ["-i"]
    failed = False
    for chunk in range(0, len(files), 50):
        batch = files[chunk : chunk + 50]
        result = subprocess.run([args.clang_format, "--style=file", *mode, *batch], cwd=args.root)
        failed = failed or result.returncode != 0
    verb = "checked" if args.check else "formatted"
    print(f"run_clang_format: {verb} {len(files)} files{'; differences found' if failed else ''}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
