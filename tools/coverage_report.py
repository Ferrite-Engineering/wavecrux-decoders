#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Merge the .profraw files of a coverage build's ctest run and report.

  cmake --preset coverage && cmake --build --preset coverage
  ctest --preset coverage
  cmake --build --preset coverage --target coverage-report

Testing standard §6: line >= 95 %, branch >= 90 % for decoder sources. The
report covers common/, tools/wcxhost/ and decoders/ (tests excluded) and
writes an HTML report to <build>/coverage/html. With --fail-under-line /
--fail-under-branch and --only <path prefix> it enforces a floor for one
decoder, e.g. --only decoders/pcie/src --fail-under-line 95
--fail-under-branch 90. The percentages are summed over the files the report
lists (llvm-cov's TOTAL row ignores the --only filter). `--self-test` checks
that parser; cmake registers it as the test tools.coverage_report.parser.
"""

import argparse
import pathlib
import re
import subprocess
import sys


def objects(build: pathlib.Path) -> list:
    found = []
    for path in sorted((build / "bin").glob("*")) + sorted((build / "plugins").glob("*")):
        if path.is_file() and (path.suffix in ("", ".so", ".dylib", ".exe", ".dll")):
            found.append(str(path))
    return found


# One `llvm-cov report` file row: name, then regions, missed, %, functions,
# missed, %, lines, missed, %, branches, missed, % (a "-" where a file has
# nothing to count).
_ROW = re.compile(
    r"^(?P<name>\S.*?)\s+(?P<regions>\d+)\s+(?P<mregions>\d+)\s+\S+\s+(?P<funcs>\d+)\s+"
    r"(?P<mfuncs>\d+)\s+\S+\s+(?P<lines>\d+)\s+(?P<mlines>\d+)\s+\S+\s+(?P<branches>\d+)\s+"
    r"(?P<mbranches>\d+)\s+\S+\s*$"
)


def select_rows(report: str, only: str, source_dir: pathlib.Path) -> list:
    """The file rows of an `llvm-cov report` under the `only` prefix (every
    row when `only` is empty), excluding the TOTAL row.

    Coverage mappings name files the way the compiler saw them: relative to
    the source tree when it was built with -ffile-prefix-map (this
    repository), absolute otherwise. A row matches when either form starts
    with the prefix. The filtering is done here rather than through
    llvm-cov's SOURCES arguments, which silently match nothing against
    relative names and whose TOTAL row covers the whole tree regardless.
    """
    rows = []
    absolute = str((source_dir.resolve() / only)) if only else ""
    for row in report.splitlines():
        m = _ROW.match(row)
        if not m or m.group("name").startswith("TOTAL"):
            continue
        name = m.group("name").strip()
        if only and not (name.startswith(only) or name.startswith(absolute)):
            continue
        rows.append(row)
    return rows


def totals(rows: list):
    """Line and branch coverage (percent) summed over report rows. A set with
    no lines (or branches) counts as 100 %."""
    lines = missed_lines = branches = missed_branches = 0
    for row in rows:
        m = _ROW.match(row)
        if not m:
            continue
        lines += int(m.group("lines"))
        missed_lines += int(m.group("mlines"))
        branches += int(m.group("branches"))
        missed_branches += int(m.group("mbranches"))
    line_pct = 100.0 if lines == 0 else 100.0 * (lines - missed_lines) / lines
    branch_pct = 100.0 if branches == 0 else 100.0 * (branches - missed_branches) / branches
    return line_pct, branch_pct


_SELF_TEST_REPORT = """\
Filename   Regions  Missed Regions  Cover  Functions  Missed Functions  Executed  Lines  Missed Lines  Cover  Branches  Missed Branches  Cover
---
a/x.c           10               1  90.00%          2                 0  100.00%     40             4  90.00%        10                1  90.00%
a/y.c           10               0 100.00%          2                 0  100.00%     60             2  96.67%        30                3  90.00%
a/z.h            0               0       -          0                 0        -      0             0       -         0                0       -
---
TOTAL          500              50  90.00%         90                 9   90.00%   5000          2500  50.00%      1000              500  50.00%
"""


def self_test() -> int:
    here = pathlib.Path(".")
    # x.c and y.c: 100 lines, 6 missed -> 94.00 %; 40 branches, 4 missed ->
    # 90.00 %. The TOTAL row (50 %) must play no part; z.h adds nothing.
    line_pct, branch_pct = totals(select_rows(_SELF_TEST_REPORT, "", here))
    ok = abs(line_pct - 94.0) < 1e-9 and abs(branch_pct - 90.0) < 1e-9
    # A prefix keeps only its rows: a/y.c alone is 58/60 lines and 27/30 branches.
    y_line, y_branch = totals(select_rows(_SELF_TEST_REPORT, "a/y", here))
    ok = ok and abs(y_line - 100.0 * 58 / 60) < 1e-9 and abs(y_branch - 90.0) < 1e-9
    # An absolute prefix matches the same rows; a prefix matching nothing is 100 %.
    abs_rows = select_rows(_SELF_TEST_REPORT.replace("a/x.c", str(here.resolve() / "a/x.c")),
                           "a/x", here)
    ok = ok and len(abs_rows) == 1
    empty = totals(select_rows(_SELF_TEST_REPORT, "nowhere/", here)) == (100.0, 100.0)
    print(f"coverage_report self-test: lines {line_pct:.2f} %, branches {branch_pct:.2f} %: "
          f"{'ok' if ok and empty else 'FAILED'}")
    return 0 if ok and empty else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=pathlib.Path)
    parser.add_argument("--source-dir", type=pathlib.Path)
    parser.add_argument("--llvm-profdata")
    parser.add_argument("--llvm-cov")
    parser.add_argument("--only", default="")
    parser.add_argument("--fail-under-line", type=float, default=0.0)
    parser.add_argument("--fail-under-branch", type=float, default=0.0)
    parser.add_argument("--self-test", action="store_true",
                        help="check the report parser against a canned report and exit")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not (args.build_dir and args.source_dir and args.llvm_profdata and args.llvm_cov):
        parser.error("--build-dir, --source-dir, --llvm-profdata and --llvm-cov are required")

    build = args.build_dir.resolve()
    raws = sorted((build / "profiles").glob("*.profraw"))
    if not raws:
        print("coverage_report: no .profraw files; run ctest --preset coverage first")
        return 1
    out_dir = build / "coverage"
    out_dir.mkdir(exist_ok=True)
    profdata = out_dir / "merged.profdata"
    subprocess.run(
        [args.llvm_profdata, "merge", "-sparse", *map(str, raws), "-o", str(profdata)], check=True
    )
    objs = objects(build)
    if not objs:
        print("coverage_report: no instrumented binaries found")
        return 1
    obj_args = [objs[0]] + [f"-object={o}" for o in objs[1:]]
    ignore = r"(/tests/|/fuzz/|/third_party/|/_deps/)"
    common = [f"-instr-profile={profdata}", f"-ignore-filename-regex={ignore}"]
    # Built with -ffile-prefix-map=<source>/=, the mappings hold file names
    # relative to the source tree and a compilation directory relative to it
    # too (the build directory); llvm-cov would look for build/.../x.c. Map
    # that compilation directory back onto the source tree.
    source = args.source_dir.resolve()
    try:
        common.append(f"-path-equivalence={build.relative_to(source)},{source}")
    except ValueError:
        pass  # a build tree outside the source tree keeps absolute paths
    report = subprocess.run(
        [args.llvm_cov, "report", *obj_args, *common],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    rows = select_rows(report, args.only, args.source_dir)
    if args.only:
        print("\n".join(rows))
    else:
        print(report)
    subprocess.run(
        [args.llvm_cov, "show", *obj_args, *common, "-format=html",
         f"-output-dir={out_dir / 'html'}"],
        check=True,
    )
    # Summed over the selected files, not llvm-cov's TOTAL row (see select_rows()).
    line_pct, branch_pct = totals(rows)
    print(f"coverage_report: lines {line_pct:.2f} %, branches {branch_pct:.2f} % "
          f"over {args.only or 'the whole tree'} (html: {out_dir / 'html' / 'index.html'})")
    if line_pct < args.fail_under_line or branch_pct < args.fail_under_branch:
        print("coverage_report: below the floor")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
