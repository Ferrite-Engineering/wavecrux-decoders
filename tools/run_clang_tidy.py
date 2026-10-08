#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Run clang-tidy over every repository C file in compile_commands.json.

  run_clang_tidy.py --clang-tidy <bin> --build-dir <dir> --source-dir <root>

Uses the repository's .clang-tidy (warnings as errors). Files outside the
source tree, in third_party/, or generated into the build tree are skipped.
Runs files in parallel; exits non-zero if any file has a finding.
"""

import argparse
import concurrent.futures
import json
import os
import pathlib
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--clang-tidy", required=True)
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    parser.add_argument("--source-dir", required=True, type=pathlib.Path)
    parser.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    if not args.clang_tidy or args.clang_tidy.endswith("NOTFOUND"):
        print("run_clang_tidy: clang-tidy not found (set WCX_CLANG_TIDY)")
        return 1
    root = args.source_dir.resolve()
    build = args.build_dir.resolve()
    db = json.loads((build / "compile_commands.json").read_text())
    files = set()
    for entry in db:
        path = pathlib.Path(entry["file"])
        if not path.is_absolute():
            path = pathlib.Path(entry["directory"]) / path
        path = path.resolve()
        if root not in path.parents or build in path.parents:
            continue
        rel = path.relative_to(root).as_posix()
        if rel.startswith("third_party/") or rel.endswith("banned_rejects.c"):
            continue
        files.add(str(path))

    extra = []
    if sys.platform == "darwin" and not any("-isysroot" in e.get("command", "") for e in db):
        # A clang-tidy from another LLVM than the compiler (Homebrew's next to
        # Apple's clang) does not know the SDK location; hand it over.
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True)
        if sdk.returncode == 0 and sdk.stdout.strip():
            extra = [f"--extra-arg=-isysroot{sdk.stdout.strip()}"]

    def tidy(path: str):
        return path, subprocess.run(
            [args.clang_tidy, "-p", str(build), "--quiet", "--warnings-as-errors=*", *extra, path],
            capture_output=True,
            text=True,
        )

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for path, result in pool.map(tidy, sorted(files)):
            if result.returncode != 0:
                failed += 1
                sys.stdout.write(result.stdout)
                sys.stdout.write(result.stderr)
    print(f"run_clang_tidy: {len(files)} files, {failed} with findings")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
