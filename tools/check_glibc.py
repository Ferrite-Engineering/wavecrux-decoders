#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Fail if a Linux plugin needs a newer glibc than the operations standard allows.

The standard (docs/standards/operations-standard.md §1) promises that every
plugin runs on glibc 2.35, below WaveCrux's own floor. A promise that lives
only in documentation is not enforced by anything; WaveCrux's Linux builds once
raised their floor to 2.38 for a year without anyone noticing. This measures
it.

Usage: check_glibc.py [--max 2.35] <library.so> [...]
"""

import argparse
import re
import subprocess
import sys

_VERSION = re.compile(r"GLIBC_(\d+)\.(\d+)(?:\.(\d+))?")


def required_versions(path: str) -> set[tuple[int, ...]]:
    """Every GLIBC_x.y symbol version the library's dynamic symbols require."""
    result = subprocess.run(
        ["objdump", "-T", path], check=True, capture_output=True, text=True
    )
    found = set()
    for line in result.stdout.splitlines():
        # Only undefined symbols are requirements; defined ones are exports.
        if "*UND*" not in line:
            continue
        for match in _VERSION.finditer(line):
            found.add(tuple(int(part) for part in match.groups() if part is not None))
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--max", default="2.35", help="highest allowed glibc version")
    parser.add_argument("libraries", nargs="+")
    args = parser.parse_args()
    limit = tuple(int(part) for part in args.max.split("."))

    failed = False
    for path in args.libraries:
        versions = required_versions(path)
        highest = max(versions) if versions else None
        shown = ".".join(map(str, highest)) if highest else "none"
        if highest is not None and highest > limit:
            print(f"FAIL {path}: needs glibc {shown}, the floor is {args.max}")
            failed = True
        else:
            print(f"ok   {path}: needs glibc {shown} (floor {args.max})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
