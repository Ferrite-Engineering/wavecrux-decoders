#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Chili.CHIPS
#
# Writes the generated pcie-tlp fixtures: <stem>.vcd and <stem>.bindings.json.
# The <stem>.expected.json beside them is derived by hand from SPEC.md and
# the PCIe Base Specification (testing standard §2); this script never
# writes it.
#
#   python3 decoders/pcie-tlp/tools/generate_fixtures.py [--check]
#
# Clock: rising edge k at (2k + 1) * HALF, falling edge at (2k + 2) * HALF.
# The values of beat k are set at 2k * HALF (half a period before edge k),
# or, with zero_delay, at the timestamp of edge k - 1, as a flip-flop
# output in a zero-delay RTL dump changes.

import json
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "fixtures" / "generated"

HALF = 4000  # half a clock period, in timescale units


def tlp(*dws):
    """A TLP as beats: (data, sop, eop)."""
    return [(d, int(i == 0), int(i == len(dws) - 1)) for i, d in enumerate(dws)]


def idle(n):
    return [None] * n


# Downstream requests, labels = compact, no peer.
REQUESTS = (
    idle(3)
    # CfgWr0 01:00.0 BAR0 = 0xFFFFFFFF, requester 0000, tag 01, first BE F
    + tlp(0x44000001, 0x0000010F, 0x01000010, 0xFFFFFFFF)
    + idle(2)
    # CfgRd0 01:00.0 BAR0, tag 02
    + tlp(0x04000001, 0x0000020F, 0x01000010)
    + idle(1)
    # CfgRd1 02:03.1 reg 0x100, tag 03
    + tlp(0x05000001, 0x0000030F, 0x02190100)
    # MWr32 0x80000000, 2 DW, first/last BE F/F
    + tlp(0x40000002, 0x000000FF, 0x80000000, 0x11223344, 0x55667788)
    # MRd64 0x00000001_20000004, 4 DW, tag 04
    + tlp(0x20000004, 0x000004FF, 0x00000001, 0x20000004)
    # Msg (route 4: local), code 0x7E (vendor type 0)
    + tlp(0x34000000, 0x0000007E, 0x00000000, 0x00000000)
    # Fmt 0 Type 0x1F is not a request, completion or message
    + tlp(0x1F000000, 0x00000000, 0x00000000)
    + idle(2)
)

# More requests, labels = full, span = until_next.
REQUESTS_FULL = (
    idle(3)
    # CfgWr1 03:04.2 Command = 0x00000006, requester 0100, tag 10
    + tlp(0x45000001, 0x0100100F, 0x03220004, 0x06000000)
    # CfgRd0 01:00.0 reg 0x040, tag 11
    + tlp(0x04000001, 0x0000110F, 0x01000040)
    + idle(2)
    # IORd 0x00001003 (bits [1:0] cleared to 0x00001000), tag 12, first BE 1
    + tlp(0x02000001, 0x00001201, 0x00001003)
    # IOWr 0x00002000 = 0xA5A5A5A5, tag 13
    + tlp(0x42000001, 0x0000130F, 0x00002000, 0xA5A5A5A5)
    # MRdLk32 0x90000000, 1 DW, tag 14
    + tlp(0x01000001, 0x0000140F, 0x90000000)
    # MWr64 0x00000002_00000010, 3 DW
    + tlp(0x60000003, 0x000000FF, 0x00000002, 0x00000010, 0x01020304, 0x05060708, 0x090A0B0C)
    # MRd32 0xA0000000, Length 0 = 1024 DW, tag 15
    + tlp(0x00000000, 0x000015FF, 0xA0000000)
    # MRd32 0xA0000100, 2 DW, tag 16
    + tlp(0x00000002, 0x000016FF, 0xA0000100)
    # MsgD route 0, code 0x50, 1 DW
    + tlp(0x70000001, 0x00000050, 0x00000000, 0x00000000, 0xDEADBEEF)
    + idle(3)
    # Fmt 4 (a TLP prefix) in 2 DW: no length check, but shorter than a header
    + tlp(0x9F000000, 0x00000000)
    + idle(2)
)

# Upstream completions, labels = compact, peer = the downstream requests.
PEER = (
    idle(3)
    + tlp(0x04000001, 0x0000050F, 0x01000010)                # CfgRd0 BAR0, tag 05
    + tlp(0x44000001, 0x0000060F, 0x01000004, 0x06000000)    # CfgWr0 Command, tag 06
    + tlp(0x00000001, 0x0000070F, 0x80000010)                # MRd32 0x80000010, tag 07
    + idle(12)
)
COMPLETIONS = (
    idle(8)
    # CplD SC, completer 0100, BC 4, requester 0000, tag 05: BAR0 reads 0xFFFFF008
    + tlp(0x4A000001, 0x01000004, 0x00000500, 0x08F0FFFF)
    # Cpl SC for the write, tag 06
    + tlp(0x0A000000, 0x01000000, 0x00000600)
    # CplD SC, tag 07, lower address 0x10
    + tlp(0x4A000001, 0x01000004, 0x00000710, 0xCAFEF00D)
    # Cpl UR, tag 09: nothing asked for it
    + tlp(0x0A000000, 0x01002000, 0x00000900)
    + idle(2)
)

# Completion corner cases, labels = full, with peer.
PEER_FULL = (
    idle(2)
    + tlp(0x00000002, 0x0000200F, 0x80000020)                # MRd32 0x80000020, 2 DW, tag 20
    + tlp(0x02000001, 0x0000210F, 0x00003000)                # IORd 0x00003000, tag 21
    + tlp(0x01000001, 0x0000220F, 0x90000040)                # MRdLk32 0x90000040, tag 22
    + tlp(0x04000001, 0x0000230F, 0x01000000)                # CfgRd0 ID, tag 23
    + tlp(0x04000001, 0x0000240F, 0x01000008)                # CfgRd0 Class/Rev, tag 24
    + tlp(0x00000001, 0x0200250F, 0x80000050)                # MRd32 from requester 0200, tag 25
    + [(0x77777777, 0, 0)]                                   # peer beat without SOP: skipped
    + [(0x04000001, 1, 0), ("x", 0, 0)]                      # peer TLP dropped by an x beat
    + tlp(0x42000001, 0x0000260F, 0x00004000, 0x00000001)    # IOWr 0x00004000, tag 26
    + idle(30)
)
COMPLETIONS_FULL = (
    idle(20)
    # split: CplD SC tag 20, BC 8, 1 DW: entry stays
    + tlp(0x4A000001, 0x01000008, 0x00002020, 0x00000001)
    # second part: CplD SC tag 20, BC 4, 1 DW: entry removed
    + tlp(0x4A000001, 0x01000004, 0x00002024, 0x00000002)
    # tag 20 again: no entry now
    + tlp(0x0A000000, 0x01000000, 0x00002000)
    # CplD CRS for the IO read, tag 21 (CRS is not an error)
    + tlp(0x4A000001, 0x01004004, 0x00002100, 0x12345678)
    # CplDLk CA, tag 22
    + tlp(0x4B000001, 0x01008004, 0x00002240, 0x00000000)
    # CplD rsvd status 3, tag 23 (cfg match)
    + tlp(0x4A000001, 0x01006004, 0x00002300, 0xEE10AB12)
    # CplD SC, tag 24, but requester 0001: no match
    + tlp(0x4A000001, 0x01000004, 0x00012400, 0x00000001)
    # Cpl UR tag 25 from requester 0200; Length 1 is reserved without data: no mismatch
    + tlp(0x0A000001, 0x01002000, 0x02002500)
    # Cpl SC for the IOWr, tag 26
    + tlp(0x0A000000, 0x01000000, 0x00002600)
    # CplLk SC with no entry, tag 27
    + tlp(0x0B000000, 0x01000000, 0x00002700)
    + idle(2)
)

# Framing errors, labels = full, span = packet.
ERRORS = (
    idle(3)
    + [(0x12345678, 0, 0)]                                   # data beat without SOP
    + [(0x40000001, 1, 0), (0x000000FF, 0, 0)]               # MWr32, cut short by...
    + tlp(0x00000001, 0x0000010F, 0x80000000)                # ...this MRd32, tag 01
    + tlp(0x40000002, 0x000000FF, 0x80000000, 0x00000001)    # MWr32, Length 2, 1 data DW
    + tlp(0x00000001, 0x0000020F)                            # 2 DW: shorter than a header
    + [("x", 1, 1)]                                          # x on tlp_data
    + idle(1)
    + [(0x04000001, 1, 0), (0x0000030F, 0, 0)]               # no EOP before the end
)

# Runs of unknown beats and of beats without SOP, labels = compact.
RUNS = (
    idle(3)
    + [(0x11111111, 0, 0), (0x22222222, 0, 0), (0x33333333, 0, 0)]  # 3 beats without SOP
    + [(0x04000001, 1, 0)]                                   # SOP, then...
    + [("x", 0, 0), ("x", 0, 0)]                             # ...2 beats with x data drop it
    + idle(1)
    + [(0x00000000, "x", 0)]                                 # x on tlp_sop: starts a run...
    + [("valid_x",)]                                         # ...that x on tlp_valid continues
    + [("valid_x",)]
    + tlp(0x04000001, 0x0000010F, 0x01000010)                # CfgRd0 BAR0, tag 01
    + idle(2)
)

# A zero-delay dump: data changes on the clock edge's timestamp.
ZERO_DELAY = (
    idle(3)
    + tlp(0x04000001, 0x0000010F, 0x01000010)                # CfgRd0 BAR0, tag 01
    + tlp(0x40000001, 0x000000FF, 0x80000000, 0x0000ABCD)    # MWr32 0x80000000
    + idle(2)
)

FIXTURES = {
    # stem: (main, peer, parameters, timescale, zero_delay)
    "tlp_requests_compact_1ps": (REQUESTS, None, {"labels": "compact"}, "1ps", False),
    "tlp_requests_full_until_next_1ps": (REQUESTS_FULL, None,
                                         {"labels": "full", "span": "until_next"}, "1ps", False),
    "tlp_completions_peer_1ps": (COMPLETIONS, PEER, {"labels": "compact"}, "1ps", False),
    "tlp_completions_full_peer_1ps": (COMPLETIONS_FULL, PEER_FULL, {"labels": "full"}, "1ps", False),
    "tlp_framing_errors_full_1ps": (ERRORS, None, {"labels": "full", "span": "packet"}, "1ps", False),
    "tlp_unknown_runs_1ps": (RUNS, None, {}, "1ps", False),
    "tlp_zero_delay_1ns": (ZERO_DELAY, None, {}, "1ns", True),
}

SIGS = [  # (vcd id, name, width)
    ("!", "clk", 1), ('"', "tlp_data", 32), ("#", "tlp_sop", 1),
    ("$", "tlp_eop", 1), ("%", "tlp_valid", 1), ("&", "peer_data", 32),
    ("'", "peer_sop", 1), ("(", "peer_eop", 1), (")", "peer_valid", 1),
]


def beat_values(beat, prefix):
    if beat is None:
        return {f"{prefix}data": 0, f"{prefix}sop": 0, f"{prefix}eop": 0, f"{prefix}valid": 0}
    if beat == ("valid_x",):
        return {f"{prefix}data": 0, f"{prefix}sop": 0, f"{prefix}eop": 0, f"{prefix}valid": "x"}
    d, s, e = beat
    return {f"{prefix}data": d, f"{prefix}sop": s, f"{prefix}eop": e, f"{prefix}valid": 1}


def fmt(width, v):
    if v == "x":
        return "bx " if width > 1 else "x"
    return f"b{v:b} " if width > 1 else str(v)


def vcd(main, peer, timescale, zero_delay):
    sigs = SIGS if peer is not None else SIGS[:5]
    n = max(len(main), len(peer or []))
    lines = [f"$timescale {timescale} $end", "$scope module tb $end"]
    lines += [f"$var wire {w} {i} {name} $end" if w == 1 else
              f"$var wire {w} {i} {name} [{w - 1}:0] $end" for i, name, w in sigs]
    lines += ["$upscope $end", "$enddefinitions $end"]

    def beats(k):
        vals = beat_values(main[k] if k < len(main) else None, "tlp_")
        if peer is not None:
            vals.update(beat_values(peer[k] if k < len(peer) else None, "peer_"))
        return vals

    events = {}  # time -> {name: value}, in insertion order
    events.setdefault(0, {})["clk"] = 0
    for k in range(n + 1):
        events.setdefault((2 * k + 1) * HALF, {})["clk"] = 1
        events.setdefault((2 * k + 2) * HALF, {})["clk"] = 0
        t = (2 * k - 1) * HALF if zero_delay and k > 0 else 2 * k * HALF
        events.setdefault(t, {}).update(beats(k))
    prev = {}
    for t in sorted(events):
        changes = [(i, w, events[t][name]) for i, name, w in sigs
                   if name in events[t] and prev.get(name) != events[t][name]]
        for name, v in events[t].items():
            prev[name] = v
        if changes or t == 0:
            lines.append(f"#{t}")
            lines += [f"{fmt(w, v)}{i}" for i, w, v in changes]
    lines.append(f"#{(2 * n + 3) * HALF}")  # final idle time
    return "\n".join(lines) + "\n"


def bindings(peer, params):
    names = [s[1] for s in (SIGS if peer is not None else SIGS[:5])]
    out = {"signal_bindings": {n: f"tb.{n}" for n in names}}
    if peer is not None:
        out["decoder"] = "openpcie.pcie_tlp_up"  # the completions run upstream
    if params:
        out["parameters"] = params
    return json.dumps(out, indent=2) + "\n"


def main():
    check = "--check" in sys.argv[1:]
    stale = []
    OUT.mkdir(parents=True, exist_ok=True)
    for stem, (m, p, params, ts, zd) in FIXTURES.items():
        for path, text in ((OUT / f"{stem}.vcd", vcd(m, p, ts, zd)),
                           (OUT / f"{stem}.bindings.json", bindings(p, params))):
            if check:
                if not path.exists() or path.read_text() != text:
                    stale.append(path.name)
            else:
                path.write_text(text)
    if stale:
        sys.stderr.write("stale: " + ", ".join(stale) + "\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
