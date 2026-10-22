#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Chili.CHIPS
#
# Writes the captured pcie-tlp fixtures from openPCIE's co-simulation trace
# (5.sim/tb.vcd, see fixtures/captured/PROVENANCE.md):
#
#   python3 decoders/pcie-tlp/tools/make_captured.py --vcd <openpcie>/5.sim/tb.vcd
#
# Keeps only the decoder's signal set, scope tb.tlp_view (tx_* and rx_*),
# trimmed to the TLP traffic with MARGIN clock periods on each side. The
# values at the window start become the $dumpvars block. The $date section,
# the only part that changes between runs, is dropped. One trace serves both
# fixtures. The <stem>.expected.json files are not written here: they come
# from SPEC.md (PROVENANCE.md; derive_captured_expected.py reproduces them).

import argparse
import json
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "fixtures" / "captured"

SCOPE = ["tb", "tlp_view"]
MARGIN = 8           # clock periods kept before the first and after the last beat
PERIOD = 16000       # tlp_view clocks: 62.5 MHz, timescale 1 ps
SIGNALS = ["clk", "tlp_data", "tlp_sop", "tlp_eop", "tlp_valid",
           "peer_data", "peer_sop", "peer_eop", "peer_valid"]

# stem, decoder, prefix, parameters
FIXTURES = [
    ("openpcie_rc_down_peer_1ps", "openpcie.pcie_tlp_down", "tx_", {}),
    ("openpcie_rc_up_full_peer_1ps", "openpcie.pcie_tlp_up", "rx_", {"labels": "full"}),
]


def read_header(lines):
    """The tlp_view variables, as (id, width, name), and the timescale."""
    path, keep, timescale, i = [], [], None, 0
    while i < len(lines):
        tok = lines[i].split()
        if tok and tok[0] == "$timescale":
            j = i
            while "$end" not in lines[j]:
                j += 1
            timescale = " ".join(" ".join(lines[i:j + 1]).split()[1:-1])
            i = j
        elif tok[:1] == ["$scope"]:
            path.append(tok[2])
        elif tok[:1] == ["$upscope"]:
            path.pop()
        elif tok[:1] == ["$var"] and path == SCOPE:
            keep.append((tok[3], int(tok[2]), tok[4]))
        elif tok[:1] == ["$enddefinitions"]:
            return keep, timescale, i + 1
        i += 1
    sys.exit("make_captured: no $enddefinitions")


def read_changes(lines, start, ids):
    """[(time, [(id, value)])] for the kept ids, in file order."""
    out, now, cur = [], 0, []
    for line in lines[start:]:
        line = line.strip()
        if not line or line.startswith("$"):
            continue
        if line[0] == "#":
            if cur:
                out.append((now, cur))
            now, cur = int(line[1:]), []
            continue
        if line[0] in "bBrR":
            value, vid = line.split()
        else:
            value, vid = line[0], line[1:]
        if vid in ids:
            cur.append((vid, value))
    if cur:
        out.append((now, cur))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--vcd", required=True, help="openPCIE 5.sim/tb.vcd")
    args = ap.parse_args()

    lines = Path(args.vcd).read_text().splitlines()
    keep, timescale, start = read_header(lines)
    if timescale != "1ps" or len(keep) != 18:
        sys.exit(f"make_captured: expected 18 tlp_view signals at 1ps, got {len(keep)} at {timescale}")
    ids = {vid for vid, _, _ in keep}
    changes = read_changes(lines, start, ids)

    valid = {vid for vid, _, name in keep if name.endswith("_valid")}
    busy = [t for t, ch in changes if any(v in valid and val == "1" for v, val in ch)]
    if not busy:
        sys.exit("make_captured: no TLP traffic in the trace")
    t0 = max(0, busy[0] - MARGIN * PERIOD)
    t1 = busy[-1] + MARGIN * PERIOD

    state = {}
    body = []
    for t, ch in changes:
        if t <= t0:
            state.update(ch)
        elif t <= t1:
            body.append((t, ch))

    text = ["$version openPCIE 5.sim (Vivado xsim 2026.1), tb.tlp_view only $end",
            f"$timescale {timescale} $end",
            "$scope module tb $end"]
    text += [f"$var wire {w} {vid} {name}{f' [{w - 1}:0]' if w > 1 else ''} $end"
             for vid, w, name in keep]
    text += ["$upscope $end", "$enddefinitions $end", f"#{t0}", "$dumpvars"]
    for vid, w, _ in keep:
        v = state.get(vid, "x")
        text.append(f"{v} {vid}" if w > 1 else f"{v}{vid}")
    text.append("$end")
    for t, ch in body:
        text.append(f"#{t}")
        text += [f"{val} {vid}" if val[0] in "bB" else f"{val}{vid}" for vid, val in ch]
    vcd = "\n".join(text) + "\n"

    OUT.mkdir(parents=True, exist_ok=True)
    for stem, decoder, prefix, params in FIXTURES:
        (OUT / f"{stem}.vcd").write_text(vcd, newline="\n")
        binding = {
            "signal_bindings": {s: f"tb.{prefix}{s}" for s in SIGNALS},
            "decoder": decoder,
            "parameters": params,
        }
        (OUT / f"{stem}.bindings.json").write_text(json.dumps(binding, indent=2) + "\n", newline="\n")
    print(f"make_captured: {len(body)} timestamps, #{t0}..#{t1}, {len(vcd)} bytes per fixture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
