#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Rebuilds fixtures/captured/ from openCologne-PCIE's own DLL testbench.

    git clone https://github.com/chili-chips-ba/openCologne-PCIE
    git -C openCologne-PCIE checkout ef0147959f1ecf888d756ce5133cd37c88edb936
    python3 decoders/pcie/tools/make_captured.py --opencologne openCologne-PCIE

Needs Icarus Verilog (iverilog, vvp) on PATH. Compiles the unmodified
testbench and RTL, runs it, deletes the VCD's $date section (the only part
that changes from run to run), writes the trace and bindings for each
captured fixture, and computes each expected file with pcie_ref_decoder.py.
See fixtures/captured/PROVENANCE.md.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import capture_check  # noqa: E402

CAPTURED = os.path.join(os.path.dirname(HERE), "fixtures", "captured")
UPSTREAM_SHA = "ef0147959f1ecf888d756ce5133cd37c88edb936"

BINDINGS = {
    # The DLL-to-MAC transmit stream: PIPE-shaped, 8 symbols per phy_clk.
    "opencologne_dll_tb_w64_100fs": (
        '{\n  "decoder": "ferrite.pcie_dll_w64",\n  "signal_bindings": {\n'
        '    "pclk": "tb_dll_logic.phy_clk",\n    "data": "tb_dll_logic.phy_tx_data",\n'
        '    "datak": "tb_dll_logic.phy_tx_data_k"\n  }\n}\n'),
    "opencologne_dll_tb_pipe_w64_100fs": (
        '{\n  "decoder": "ferrite.pcie_pipe_w64",\n  "signal_bindings": {\n'
        '    "pclk": "tb_dll_logic.phy_clk",\n    "data": "tb_dll_logic.phy_tx_data",\n'
        '    "datak": "tb_dll_logic.phy_tx_data_k"\n  },\n  "parameters": {\n'
        '    "scrambling": "off"\n  }\n}\n'),
}


def run(cmd: list[str], cwd: str) -> str:
    print("+ (cd " + os.path.basename(cwd) + ") " + " ".join(cmd))
    p = subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True)
    return p.stdout


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--opencologne", required=True, help="openCologne-PCIE checkout")
    ap.add_argument("--expected-only", action="store_true",
                    help="only recompute the expected files from the committed VCDs")
    args = ap.parse_args()
    src = os.path.abspath(args.opencologne)
    if not args.expected_only:
        sha = subprocess.run(["git", "-C", src, "rev-parse", "HEAD"], check=True, capture_output=True,
                             text=True).stdout.strip()
        if sha != UPSTREAM_SHA:
            print(f"warning: checkout is at {sha}, PROVENANCE.md records {UPSTREAM_SHA}")
        dll = os.path.join(src, "2.rtl", "4.dll")
        with tempfile.TemporaryDirectory() as work:
            os.makedirs(os.path.join(work, "sim"))
            rtl = sorted(glob.glob(os.path.join(dll, "dll_src", "*.v")))
            run(["iverilog", "-g2012", "-Wall", "-o", "tb_dll_logic.vvp", "-s", "tb_dll_logic",
                 os.path.join(dll, "sim", "tb_dll_logic.v")] + rtl, work)
            log = run(["vvp", "-n", "tb_dll_logic.vvp", "-vcd"], work)
            print(log)
            if "TEST PASSED" not in log:
                raise SystemExit("the openCologne testbench did not pass")
            # The testbench names its dump sim/dll_sim.fst; with -vcd it is VCD text.
            with open(os.path.join(work, "sim", "dll_sim.fst"), encoding="utf-8") as f:
                text = f.read()
        text = re.sub(r"\$date.*?\$end\s*", "", text, count=1, flags=re.S)
        os.makedirs(CAPTURED, exist_ok=True)
        for stem in BINDINGS:
            with open(os.path.join(CAPTURED, stem + ".vcd"), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
    for stem, b in BINDINGS.items():
        with open(os.path.join(CAPTURED, stem + ".bindings.json"), "w", encoding="utf-8", newline="\n") as f:
            f.write(b)
        p = os.path.join(CAPTURED, stem)
        out = capture_check.ref_output(p + ".vcd", p + ".bindings.json")
        with open(p + ".expected.json", "w", encoding="utf-8", newline="\n") as f:
            f.write(out)
        print(f"wrote captured/{stem}.* ({len(out.splitlines()) - 2} transactions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
