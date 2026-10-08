#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Fail if a tracked file points at something an outside reader cannot see.

This repository is written to be public. A reference to a private repository,
an internal document or a maintainer's machine is a dead end for a reader and
can leak more than intended, so CI rejects it. Scans the files git tracks
plus any staged as intent-to-add (`git add -N`), because a new untracked file
would otherwise pass every local run and fail only after it is committed.

Usage: check_private_refs.py [repo root]
"""

import pathlib
import re
import subprocess
import sys

FORBIDDEN = [
    (re.compile(r"\bedacrux/(docs|tool)\b|\bedacrux ADR\b|\bADR 0\d{3}\b"), "internal edacrux document"),
    (re.compile(r"\bwavecrux-pro\b|\bnetcrux-pro\b|\blintcrux-pro\b|\bsimcrux-pro\b|\bcrux-shared-pro\b"), "private repository"),
    (re.compile(r"/Users/[A-Za-z]|/home/[a-z]+/Development|/private/tmp/"), "local machine path"),
    (re.compile(r"\bscratchpad\b"), "session scratch directory"),
]

SELF = pathlib.Path(__file__).resolve()


def tracked_files(root: pathlib.Path) -> list[pathlib.Path]:
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--cached"],
        check=True, capture_output=True,
    ).stdout
    return [root / name for name in out.decode("utf-8").split("\0") if name]


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
    problems = []
    for path in tracked_files(root):
        if path.resolve() == SELF or not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue  # binary fixtures and corpora
        for number, line in enumerate(text.splitlines(), start=1):
            for pattern, why in FORBIDDEN:
                if pattern.search(line):
                    problems.append(f"{path.relative_to(root)}:{number}: {why}: {line.strip()[:120]}")
    for problem in problems:
        print(problem)
    if problems:
        print(f"check_private_refs: {len(problems)} reference(s) an outside reader cannot follow")
        return 1
    print("check_private_refs: clean")
    return 0


if __name__ == "__main__":
    sys.exit(main())
