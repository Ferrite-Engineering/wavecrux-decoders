#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Run cppcheck with the settings of C coding standard §8.

  run_cppcheck.py --cppcheck <bin> --build-dir <dir> --source-dir <root> [--misra]

Default: --enable=warning,style,performance,portability --inconclusive
--error-exitcode=1 over every repository C file in compile_commands.json.
--misra: the bundled MISRA C:2012 addon in advisory mode (reports, never
fails), for ferrite.* decoders.

Suppressions are inline only (`// cppcheck-suppress <id>` with a reason on
the line above the finding); this script adds none beyond
missingIncludeSystem (cppcheck does not read system headers).
"""

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cppcheck", required=True)
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    parser.add_argument("--source-dir", required=True, type=pathlib.Path)
    parser.add_argument("--misra", action="store_true")
    args = parser.parse_args()
    if not args.cppcheck or args.cppcheck.endswith("NOTFOUND"):
        print("run_cppcheck: cppcheck not found (set WCX_CPPCHECK)")
        return 1
    root = args.source_dir.resolve()
    build = args.build_dir.resolve()
    db = json.loads((build / "compile_commands.json").read_text())
    kept = []
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
        kept.append(entry)
    with tempfile.TemporaryDirectory() as tmp:
        project = pathlib.Path(tmp) / "compile_commands.json"
        project.write_text(json.dumps(kept))
        cmd = [
            args.cppcheck,
            f"--project={project}",
            "--std=c11",
            "--enable=warning,style,performance,portability",
            "--inconclusive",
            "--inline-suppr",
            "--suppress=missingIncludeSystem",
            "--suppress=unmatchedSuppression",
            "--quiet",
            f"--cppcheck-build-dir={tmp}",
            "-j",
            "8",
        ]
        if args.misra:
            cmd += ["--addon=misra"]
        else:
            cmd += ["--error-exitcode=1"]
        result = subprocess.run(cmd, cwd=root)
    label = "MISRA (advisory)" if args.misra else "cppcheck"
    print(f"run_cppcheck: {label} over {len(kept)} files: "
          f"{'findings' if result.returncode else 'clean'}")
    return 0 if args.misra else result.returncode


if __name__ == "__main__":
    sys.exit(main())
