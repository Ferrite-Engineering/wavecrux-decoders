#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Writes decoders/pcie/stimulus/: Data Link Layer receive stimulus for RTL
testbenches, as $readmemh files.

    python3 decoders/pcie/tools/export_stimulus.py            # write
    python3 decoders/pcie/tools/export_stimulus.py --check    # CI: fail on drift
    python3 decoders/pcie/tools/export_stimulus.py --lanes 4 --out /tmp/w32

Each case is built by the fixture encoder (pcie_fixture_lib.py), so DLLP CRCs
and TLP LCRCs are computed from the packet contents, never copied from a
decoder. Every case is then read back from its .hex file and run through the
reference decoder (pcie_ref_decoder.py); the export fails unless the decoder
finds exactly the packets the manifest lists, at the positions it lists.

One .hex line is one PCLK: {datak[N-1:0], data[8N-1:0]}, byte lane j in
data[8j+7:8j] (lane 0 first in time) and its K flag in datak[j]. Scrambling
is off; logical idle is D 0x00. Standard library only and deterministic.
"""

from __future__ import annotations

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from generate_fixtures import ALL_DLLPS, Seq, tlp_catalogue, to_lane, walk_lanes  # noqa: E402
from pcie_fixture_lib import EDB, Dllp, Stream, Tlp  # noqa: E402
from pcie_ref_decoder import Pipeline  # noqa: E402

STIMULUS = os.path.join(os.path.dirname(HERE), "stimulus")


# ── cases ────────────────────────────────────────────────────────────────────

def every_lane(st: Stream) -> None:
    """DLLP, TLP, DLLP, TLP back to back, starting at each byte lane in turn."""
    walk_lanes(st, Seq(0))


def back_to_back(st: Stream) -> None:
    """Every defined DLLP type and every defined TLP type, no idle between
    them. Every packet is a multiple of 4 symbols, so with no idle they start
    only at lanes 0 and 4; every_lane and errors cover the other lanes."""
    seq = Seq(0)
    dllps = [d for d in ALL_DLLPS if d.kind != "reserved"]
    tlps = [t for t in tlp_catalogue(seq) if t.byte0 is None]
    for i in range(max(len(dllps), len(tlps))):
        if i < len(dllps):
            st.dllp(dllps[i])
        if i < len(tlps):
            st.tlp(tlps[i])


def errors(st: Stream) -> None:
    """At each starting lane: a DLLP with a bad CRC, a good TLP, a TLP with a
    bad LCRC and its retransmission, a nullified TLP and its retransmission.
    A rejected or nullified TLP does not use up its sequence number, so the
    retransmission carries the same one."""
    seq = Seq(0)
    for lane in range(st.n):
        to_lane(st, lane)
        good = Dllp("updatefc_p", vc=0, hdr=lane + 1, data=16 * lane).bytes4()
        st.dllp(Dllp("updatefc_p", vc=0, hdr=lane + 1, data=16 * lane),
                crc=bytes([good[0] ^ 0x5A, good[1]]))
        st.tlp(Tlp("MWr32", seq(), length_dw=1 + lane % 4, rng_seed=200 + lane))
        n = seq()
        st.tlp(Tlp("MRd32", n, length_dw=1, rng_seed=220 + lane), lcrc="bad")
        st.tlp(Tlp("MRd32", n, length_dw=1, rng_seed=220 + lane))
        n = seq()
        st.tlp(Tlp("CplD", n, length_dw=2, rng_seed=240 + lane), lcrc="invert", end=EDB)
        st.tlp(Tlp("CplD", n, length_dw=2, rng_seed=240 + lane))


CASES = [("every_lane", every_lane), ("back_to_back", back_to_back), ("errors", errors)]


# ── expectations, from the encoder's units ───────────────────────────────────

def packets(st: Stream) -> list[dict]:
    out = []
    for u in st.units:
        if u.kind == "dllp":
            d = u.info["dllp"]
            ok = u.info["crc"] == u.info["good_crc"]
            label = d.describe()[0] + ("" if ok else " CRC mismatch")
            out.append(dict(a=u.a, b=u.b, kind="DLLP", seq="-", expect="accept" if ok else "drop",
                            why="good CRC" if ok else "bad CRC", label=label))
        elif u.kind == "tlp":
            t = u.info["tlp"]
            mode = u.info["lcrc_mode"]
            if mode == "invert" and u.info["end"] == EDB:
                out.append(dict(a=u.a, b=u.b, kind="TLP", seq=t.seq, expect="drop", why="nullified",
                                label=f"Nullified TLP seq={t.seq} {t.name}"))
                continue
            ok = mode == "good"
            label = f"TLP seq={t.seq} {t.name} len={t.length_dw}" + ("" if ok else " LCRC mismatch")
            out.append(dict(a=u.a, b=u.b, kind="TLP", seq=t.seq, expect="accept" if ok else "nak",
                            why="good LCRC" if ok else "bad LCRC", label=label))
        elif u.kind != "idle":
            raise AssertionError(f"unexpected unit {u.kind}")
    return out


# ── output ───────────────────────────────────────────────────────────────────

def hex_text(name: str, st: Stream) -> str:
    n = st.n
    dig = 2 * n
    lines = [f"// wcx-pcie DLL receive stimulus: {name}, {n} lanes, scrambling off",
             f"// one line per PCLK: {{datak[{n - 1}:0], data[{8 * n - 1}:0]}}, byte lane j = data[8j+7:8j]",
             "// generated by decoders/pcie/tools/export_stimulus.py; do not edit"]
    for w in range(len(st.syms) // n):
        word = st.syms[w * n:(w + 1) * n]
        k = sum(1 << j for j, s in enumerate(word) if s.k)
        d = sum(s.wire << (8 * j) for j, s in enumerate(word))
        lines.append(f"{k:0{(n + 3) // 4}X}{d:0{dig}X}")
    return "\n".join(lines) + "\n"


def manifest_text(name: str, doc: str, st: Stream, pk: list[dict]) -> str:
    n = st.n
    words = len(st.syms) // n
    lines = [f"# {name}, {n} lanes: {words} words, {len(pk)} packets",
             "# " + " ".join(doc.split()),
             "# word.lane = first and last symbol of the packet (word = .hex data line, from 0)",
             "# expect: accept = passed up; nak = rejected, receiver schedules a Nak;",
             "#         drop = discarded silently (bad DLLP CRC, nullified TLP)",
             f"{'start':>7} {'end':>7}  {'kind':4} {'seq':>4}  {'expect':6}  {'why':9}  label"]
    for p in pk:
        s = f"{p['a'] // n}.{p['a'] % n}"
        e = f"{p['b'] // n}.{p['b'] % n}"
        lines.append(f"{s:>7} {e:>7}  {p['kind']:4} {p['seq']!s:>4}  {p['expect']:6}  {p['why']:9}  {p['label']}")
    return "\n".join(lines) + "\n"


def read_hex(text: str, lanes: int) -> list[tuple[int, bool]]:
    syms = []
    for line in text.splitlines():
        line = line.split("//", 1)[0].strip()
        if not line:
            continue
        v = int(line, 16)
        k, d = v >> (8 * lanes), v & ((1 << (8 * lanes)) - 1)
        for j in range(lanes):
            syms.append(((d >> (8 * j)) & 0xFF, bool((k >> j) & 1)))
    return syms


def verify(name: str, text: str, lanes: int, pk: list[dict]) -> list[str]:
    """Decode the written .hex with the reference decoder and compare."""
    pl = Pipeline("dll", "off", False, False)
    for i, (byte, k) in enumerate(read_hex(text, lanes)):
        pl.symbol(byte, k, i, 1)
    pl.flush()
    got = [(tx.start, tx.label) for tx in pl.out]
    want = [(p["a"], p["label"]) for p in pk]
    if got == want:
        return []
    bad = [f"{name}: reference decoder disagrees with the manifest"]
    for i in range(max(len(got), len(want))):
        g = got[i] if i < len(got) else None
        w = want[i] if i < len(want) else None
        if g != w:
            bad.append(f"  first difference at packet {i}: decoded {g}, expected {w}")
            break
    return bad


def build(lanes: int) -> tuple[dict[str, str], list[str]]:
    files, bad = {}, []
    for name, fn in CASES:
        st = Stream(lanes)
        fn(st)
        st.pad_idle()
        st.finish()
        pk = packets(st)
        base = f"{name}_w{8 * lanes}"
        hx = hex_text(name, st)
        bad += verify(base, hx, lanes, pk)
        files[base + ".hex"] = hx
        files[base + ".txt"] = manifest_text(name, fn.__doc__ or "", st, pk)
    return files, bad


def write(out: str, files: dict[str, str]) -> None:
    os.makedirs(out, exist_ok=True)
    for fname, text in files.items():
        with open(os.path.join(out, fname), "w", encoding="utf-8", newline="\n") as f:
            f.write(text)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="fail if stimulus/ would change")
    ap.add_argument("--lanes", type=int, default=8, choices=(1, 2, 4, 8),
                    help="bytes per PCLK (default 8: a 64-bit PIPE data bus)")
    ap.add_argument("--out", default=STIMULUS, help="output directory (default: decoders/pcie/stimulus)")
    args = ap.parse_args()
    files, bad = build(8 if args.check else args.lanes)
    if bad:
        print("\n".join(bad))
        return 1
    if args.check:
        drift = []
        for fname, text in sorted(files.items()):
            try:
                with open(os.path.join(STIMULUS, fname), encoding="utf-8", newline="") as f:
                    if f.read() != text:
                        drift.append(f"changed: {fname}")
            except FileNotFoundError:
                drift.append(f"missing: {fname}")
        for fname in sorted(os.listdir(STIMULUS)) if os.path.isdir(STIMULUS) else []:
            if fname.endswith((".hex", ".txt")) and fname not in files:
                drift.append(f"stale (not generated any more): {fname}")
        if drift:
            print("stimulus is out of date:\n  " + "\n  ".join(drift))
            return 1
        print(f"--check: {len(files)} stimulus files are up to date and decode as listed")
        return 0
    write(args.out, files)
    print(f"wrote {len(files)} files to {args.out}; the reference decoder agrees with every manifest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
