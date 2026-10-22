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
    # Fmt 4 (a TLP prefix) in 2 DW: "other", 1 DW header, no length check
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
    + tlp(0x0A000000, 0x01000004, 0x00000600)
    # CplD SC, tag 07, lower address 0x10
    + tlp(0x4A000001, 0x01000004, 0x00000710, 0xCAFEF00D)
    # Cpl UR, tag 09: nothing asked for it
    + tlp(0x0A000000, 0x01002004, 0x00000900)
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
    + tlp(0x0A000000, 0x01000004, 0x00002000)
    # CplD CRS for the IO read, tag 21 (CRS is not an error)
    + tlp(0x4A000001, 0x01004004, 0x00002100, 0x12345678)
    # CplDLk CA, tag 22
    + tlp(0x4B000001, 0x01008004, 0x00002240, 0x00000000)
    # CplD rsvd status 3, tag 23 (cfg match)
    + tlp(0x4A000001, 0x01006004, 0x00002300, 0xEE10AB12)
    # CplD SC, tag 24, but requester 0001: no match
    + tlp(0x4A000001, 0x01000004, 0x00012400, 0x00000001)
    # Cpl UR tag 25 from requester 0200; Length 1 is reserved without data: no mismatch
    + tlp(0x0A000001, 0x01002004, 0x02002500)
    # Cpl SC for the IOWr, tag 26
    + tlp(0x0A000000, 0x01000004, 0x00002600)
    # CplLk SC with no entry, tag 27
    + tlp(0x0B000000, 0x01000004, 0x00002700)
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

# ECRC (TD) and runs of framing errors, labels = full.
ECRC_RUNS = (
    idle(3)
    # MWr32 0x80000000, TD = 1: 1 data DW + ECRC, 5 DW as expected
    + tlp(0x40008001, 0x0000000F, 0x80000000, 0x12345678, 0xAABBCCDD)
    # MRd32 0x80000010, TD = 1, tag 30: header + ECRC, 4 DW as expected
    + tlp(0x00008001, 0x0000300F, 0x80000010, 0x11111111)
    # MRd32 0x80000020, TD = 1, tag 31, ECRC missing: 3 DW, 4 expected
    + tlp(0x00008001, 0x0000310F, 0x80000020)
    + idle(1)
    # tlp_sop stuck at 1 for 4 beats: 3 times SOP before EOP...
    + [(0x04000001, 1, 0)] * 4
    # ...then the last one completes: CfgRd0 BAR0, tag 01
    + [(0x0000010F, 0, 0), (0x01000010, 0, 1)]
    + idle(1)
    # tlp_sop and tlp_eop stuck at 1 for 5 beats: 5 TLPs of 1 DW
    + [(0x00000001, 1, 1)] * 5
    + idle(2)
    # both on one beat: a TLP is open, then sop + eop
    + [(0x04000001, 1, 0), (0x00000001, 1, 1)]
    + idle(2)
)

# Byte Count 0 = 4096: a 4 KB read split into completions, compact, with peer.
BC_PEER = (
    idle(2)
    + tlp(0x00000000, 0x000030FF, 0x80001000)                # MRd32 0x80001000, Length 0, tag 30
    + idle(50)
)
BC_COMPLETIONS = (
    idle(8)
    # CplD SC, Length 16, BC field 0 (4096), tag 30, lower address 0x00
    + tlp(0x4A000010, 0x01000000, 0x00003000, *range(0x01, 0x11))
    # CplD SC, Length 16, BC 4032, tag 30, lower address 0x40: still matched
    + tlp(0x4A000010, 0x01000FC0, 0x00003040, *range(0x11, 0x21))
    + idle(2)
)

# Request-side corners, labels = full, span = until_next, a 10ps timescale.
CORNER_REQUESTS = (
    idle(3)
    # MRd64 in 3 DW: shorter than its 4 DW header
    + tlp(0x20000001, 0x0000400F, 0x00000001)
    # x on tlp_eop drops the open CfgRd0
    + [(0x04000001, 1, 0), (0x0000010F, 0, "x")]
    + idle(1)
    # MWr64 with a high address of 0
    + tlp(0x60000001, 0x0000000F, 0x00000000, 0x80000000, 0xCAFEBABE)
    # a TLP prefix (Fmt 100), then an MRd32: shown as one "other" frame
    + tlp(0x90000000, 0x00000001, 0x0000500F, 0x80000000)
    # MWrLk (Type 0x01 with data): reserved, "other"
    + tlp(0x41000001, 0x0000000F, 0x80000000, 0x11111111)
    # CfgRd0 with a 4 DW header: reserved, "other"
    + tlp(0x24000001, 0x0000600F, 0x01000010, 0x00000000)
    # MRd32 0x80000040 with T8 set (DW0 bit 19): tag 0x131
    + tlp(0x00080001, 0x0000310F, 0x80000040)
    + idle(2)
)

# Pairing corners, labels = full, with peer.
CORNER_PEER = (
    idle(2)
    + tlp(0x20000001, 0x0000410F, 0x00000001, 0x20000000)    # MRd64 0x00000001_20000000, tag 41
    + tlp(0x44000001, 0x0000420F, 0x01000004, 0x06000000)    # CfgWr0 Command, tag 42
    + [(0x04000001, 1, 0)]                                   # cut short by SOP (peer: no error)...
    + tlp(0x04000001, 0x0000430F, 0x01000010)                # ...CfgRd0 BAR0, tag 43
    + [(0x04000001, 1, 0), ("valid_x",)]                     # dropped by x on peer_valid
    + tlp(0x00080001, 0x0000310F, 0x80000040)                # MRd32 0x80000040, tag 0x131 (T8)
    + tlp(0x04000001, 0x0000050F, 0x01000010)                # CfgRd0 BAR0, tag 05, requester 0000
    + tlp(0x00000002, 0x00004418, 0x8000003C)                # MRd32 0x8000003C, 2 DW, BE 1/8, tag 44
    + tlp(0x00000001, 0x0000450F, 0x80000100)                # MRd32 0x80000100, tag 45
    + idle(60)
)
CORNER_COMPLETIONS = (
    idle(30)
    # the EP's own MRd32, tag 05, requester 01:00.0: a second tag-05 entry
    + tlp(0x00000001, 0x0100050F, 0x90000000)
    # CplD tag 05 for requester 0000: still finds the peer's CfgRd0 BAR0
    + tlp(0x4A000001, 0x01000004, 0x00000500, 0x08F0FFFF)
    # CplD tag 41: answers the 64-bit read
    + tlp(0x4A000001, 0x01000004, 0x00004100, 0x12345678)
    # Cpl tag 42: answers the CfgWr0
    + tlp(0x0A000000, 0x01000004, 0x00004200)
    # CplD tag 43: answers the CfgRd0 after the peer's restart
    + tlp(0x4A000001, 0x01000004, 0x00004300, 0x0000000C)
    # CplD tag 0x131 (T8 in DW0 bit 19), lower address 0x40
    + tlp(0x4A080001, 0x01000004, 0x00003140, 0xAAAAAAAA)
    # split at the 64-byte boundary, tag 44: part 1, BC 2, lower address 0x3F: kept
    + tlp(0x4A000001, 0x01000002, 0x0000443F, 0x11000000)
    # part 2, BC 1, lower address 0x40: removed
    + tlp(0x4A000001, 0x01000001, 0x00004440, 0x00000022)
    # tag 44 again: no entry
    + tlp(0x0A000000, 0x01000004, 0x00004400)
    # CplD UR, tag 45, Length 2 with one data DW: the length mismatch wins
    + tlp(0x4A000002, 0x01002008, 0x00004500, 0x00000000)
    + idle(2)
)

# Only peer_data and peer_sop bound: the peer is not read.
PARTIAL_PEER = (
    idle(2)
    + tlp(0x04000001, 0x0000010F, 0x01000010)                # CfgRd0 BAR0, tag 01
    + idle(10)
)
PARTIAL_COMPLETIONS = (
    idle(8)
    + tlp(0x4A000001, 0x01000004, 0x00000100, 0x08F0FFFF)    # CplD tag 01: no match
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
    "tlp_ecrc_and_error_runs_full_1ps": (ECRC_RUNS, None, {"labels": "full"}, "1ps", False),
    "tlp_corner_requests_full_until_next_10ps": (CORNER_REQUESTS, None,
                                                 {"labels": "full", "span": "until_next"},
                                                 "10ps", False),
    "tlp_corner_pairing_full_peer_1ps": (CORNER_COMPLETIONS, CORNER_PEER, {"labels": "full"},
                                         "1ps", False),
    "tlp_partial_peer_1ps": (PARTIAL_COMPLETIONS, PARTIAL_PEER, {}, "1ps", False),
    "tlp_byte_count_4096_peer_1ps": (BC_COMPLETIONS, BC_PEER, {"labels": "compact"}, "1ps", False),
}

# Per-fixture options: `half` (half a clock period in timescale units, so a
# 10ps fixture keeps the 8 ns clock) and `bound` (the bound signals, when not
# all of the VCD's).
OPTIONS = {
    "tlp_corner_requests_full_until_next_10ps": {"half": 400},
    "tlp_partial_peer_1ps": {"bound": ["clk", "tlp_data", "tlp_sop", "tlp_eop", "tlp_valid",
                                       "peer_data", "peer_sop"]},
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


def vcd(main, peer, timescale, zero_delay, half=HALF):
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
        events.setdefault((2 * k + 1) * half, {})["clk"] = 1
        events.setdefault((2 * k + 2) * half, {})["clk"] = 0
        t = (2 * k - 1) * half if zero_delay and k > 0 else 2 * k * half
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
    lines.append(f"#{(2 * n + 3) * half}")  # final idle time
    return "\n".join(lines) + "\n"


def bindings(peer, params, bound=None):
    names = bound or [s[1] for s in (SIGS if peer is not None else SIGS[:5])]
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
        o = OPTIONS.get(stem, {})
        for path, text in ((OUT / f"{stem}.vcd", vcd(m, p, ts, zd, o.get("half", HALF))),
                           (OUT / f"{stem}.bindings.json", bindings(p, params, o.get("bound")))):
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
