#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""A small reference decoder for the PCIe PIPE and DLL decoders, written from
decoders/pcie/SPEC.md in the DECODING direction.

It exists for two jobs:

1. the expected output of the captured fixtures (fixtures/captured/), which
   have no encoder behind them;
2. a cross-check of the generator's own expectations (--cross-check in
   capture_check.py): two independent directions written by the same author
   catch the author's slips before the C decoders are compared at all.

It shares no code with pcie_fixture_lib.py (the encoder) and none with the
C decoders. Its CRCs are table-free reflected implementations and zlib, not
the encoder's bit-serial ones.

    python3 pcie_ref_decoder.py --vcd X.vcd --bindings X.bindings.json
"""

from __future__ import annotations

import argparse
import json
import sys
import zlib

K_NAMES = {0xBC: "COM", 0xFB: "STP", 0x5C: "SDP", 0xFD: "END", 0xFE: "EDB", 0xF7: "PAD",
           0x1C: "SKP", 0x3C: "FTS", 0x7C: "IDL", 0xFC: "EIE"}
COM, STP, SDP, END, EDB, PAD, SKP, FTS, IDL, EIE = (0xBC, 0xFB, 0x5C, 0xFD, 0xFE, 0xF7, 0x1C,
                                                    0x3C, 0x7C, 0xFC)

ERROR_TEXT = {  # SPEC §10.1
    "invalid_k": "Not a PCIe control symbol.",
    "unexpected_k": "A control symbol appeared outside an ordered set or packet.",
    "truncated": "The packet ended without END or EDB.",
    "unmatched_end": "END or EDB arrived with no packet open.",
    "k_in_packet": "A control symbol other than END or EDB appeared inside a packet.",
    "stray_data": "Data outside a packet or ordered set; logical idle is 0x00.",
    "unknown_os": "COM was not followed by a recognised ordered set.",
    "unknown_value": "The data or datak bus carries X or Z.",
    "dllp_length": "A DLLP is 6 symbols between SDP and END.",
    "dllp_edb": "A DLLP cannot be ended by EDB.",
    "reserved": "This DLLP type is reserved in PCIe Gen1 and Gen2.",
    "dllp_crc": "DLLP CRC-16 does not match its contents.",
    "tlp_length": "A TLP body is at least 18 symbols, and a multiple of 4 between the sequence number and the LCRC.",
    "tlp_edb": "A TLP ended by EDB must carry the inverted LCRC of a nullified TLP.",
    "tlp_lcrc": "TLP LCRC does not match its contents.",
    "tlp_length_mismatch": "The header Length field does not match the framed payload.",
    "tlp_sequence": "The sequence number is neither the next expected one nor a replay.",
}
RXSTATUS_ERROR = {4: "8b/10b decode error reported by the PHY.", 5: "Elastic buffer overflow reported by the PHY.",
                  6: "Elastic buffer underflow reported by the PHY.", 7: "Disparity error reported by the PHY."}


# ── CRCs (reflected forms; protocol-notes §6 and §7) ─────────────────────────

def crc16_reflected(data: bytes) -> int:
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xD008 if c & 1 else c >> 1
    return c ^ 0xFFFF


def dllp_crc_ok(body6: bytes) -> bool:
    c = crc16_reflected(body6[:4])
    return bytes([c & 0xFF, c >> 8]) == body6[4:6]


def dllp_crc_of(body4: bytes) -> bytes:
    c = crc16_reflected(body4)
    return bytes([c & 0xFF, c >> 8])


def lcrc_of(data: bytes) -> bytes:
    return zlib.crc32(data).to_bytes(4, "little")


def hx(bs: bytes) -> str:
    return "0x" + bs.hex().upper()


def h2(b: int) -> str:
    return f"0x{b:02X}"


# ── VCD + host model ─────────────────────────────────────────────────────────

def read_vcd(path: str, wanted: dict[str, str]):
    """Returns (fs_per_tick, end_time, {logical: [(time, bits)]})."""
    units = {"s": 0, "ms": -3, "us": -6, "ns": -9, "ps": -12, "fs": -15}
    with open(path, encoding="utf-8") as f:
        toks = f.read().split()
    i = 0
    scope: list[str] = []
    fpt = 10 ** 6
    var_of_id: dict[str, list[tuple[str, int]]] = {}
    want_path = {}
    for logical, p in wanted.items():
        base, _, rng = p.partition("[")
        want_path[logical] = (base, ("[" + rng) if rng else None)
    while i < len(toks):
        t = toks[i]
        if t == "$timescale":
            j = i + 1
            spec = ""
            while toks[j] != "$end":
                spec += toks[j]
                j += 1
            num = "".join(c for c in spec if c.isdigit())
            unit = spec[len(num):]
            fpt = int(num) * 10 ** (units[unit] + 15)
            i = j + 1
        elif t == "$scope":
            scope.append(toks[i + 2])
            i += 4
        elif t == "$upscope":
            scope.pop()
            i += 2
        elif t == "$var":
            width = int(toks[i + 2])
            code = toks[i + 3]
            name = toks[i + 4]
            j = i + 5
            rng = ""
            while toks[j] != "$end":
                rng += toks[j]
                j += 1
            if "[" in name:
                name, _, r = name.partition("[")
                rng = "[" + r + rng
            full = ".".join(scope + [name])
            for logical, (base, r) in want_path.items():
                if full == base and (r is None or r == rng):
                    var_of_id.setdefault(code, []).append((logical, width))
            i = j + 1
        elif t == "$enddefinitions":
            i += 2
            break
        else:
            i += 1
    changes: dict[str, list[tuple[int, str]]] = {k: [] for k in wanted}
    widths = {}
    for code, lst in var_of_id.items():
        for logical, w in lst:
            widths[logical] = w
    now = 0
    end = 0
    while i < len(toks):
        t = toks[i]
        if t[0] == "#":
            now = int(t[1:])
            end = max(end, now)
            i += 1
            continue
        if t[0] == "$":
            i += 1
            continue
        if t[0] in "bB":
            val, code = t[1:], toks[i + 1]
            i += 2
        else:
            val, code = t[0], t[1:]
            i += 1
        for logical, w in var_of_id.get(code, []):
            v = val.lower()
            if len(v) < w:
                v = (v[0] if v[0] in "xz" else "0") * (w - len(v)) + v
            changes[logical].append((now, v))
    return fpt, end, changes, widths


def host_samples(changes: dict, end: int):
    stamps = sorted({t for lst in changes.values() for t, _ in lst if t < end})
    idx = {k: 0 for k in changes}
    cur: dict[str, str] = {}
    for t in stamps:
        for k, lst in changes.items():
            while idx[k] < len(lst) and lst[idx[k]][0] <= t:
                cur[k] = lst[idx[k]][1]
                idx[k] += 1
        yield t, dict(cur)


# ── the decoder ──────────────────────────────────────────────────────────────

class Lfsr:
    """Byte-parallel form of the scrambler: key = bitrev8(lfsr[15:8]),
    then 8 serial steps (protocol-notes §3, pcieVHost's byte form)."""

    def __init__(self) -> None:
        self.r = 0xFFFF

    def next_key(self) -> int:
        hi = (self.r >> 8) & 0xFF
        key = int(f"{hi:08b}"[::-1], 2)
        for _ in range(8):
            m = self.r & 0x8000
            self.r = (self.r << 1) & 0xFFFF
            if m:
                self.r ^= 0x0039
        return key


class Tx:
    __slots__ = ("start", "end", "label", "err", "fields")

    def __init__(self, start, end, label, err, fields):
        self.start, self.end, self.label, self.err, self.fields = start, end, label, err, fields


class Pipeline:
    def __init__(self, kind: str, mode: str, coalesce: bool, show_idle: bool) -> None:
        self.kind = kind            # "pipe" or "dll"
        self.mode = mode            # "off" or "on"
        self.coalesce = coalesce
        self.show_idle = show_idle
        self.out: list[Tx] = []
        self.lfsr = Lfsr()
        self.synced = False
        self.state = "idle"
        self.os: list = []          # [(byte, k, s, d)] incl. COM
        self.os_kind = None
        self.pkt: list = []
        self.pkt_start = None
        self.pkt_kind = None
        self.run = None             # pending coalesced OS run
        self.idle_run = None
        self.stray_run = None
        self.xz_run = None
        self.next_seq = None
        self.valid_close = None     # span of a packet with good CRC just closed
        self.ds_seen = False
        self.last_sym = None

    # emission helpers
    def emit(self, tx: Tx) -> None:
        self.out.append(tx)

    def end_runs(self, keep_os_run: bool = False) -> None:
        if self.idle_run:
            a, b, n = self.idle_run
            self.idle_run = None
            if self.kind == "pipe" and self.show_idle:
                self.emit(Tx(a, b, f"Idle ×{n}" if n > 1 else "Idle", False, [("type", "idle"), ("symbols", n)]))
        if self.stray_run:
            a, b, n, first = self.stray_run
            self.stray_run = None
            self.emit(Tx(a, b, "Data outside a packet", True,
                         [("type", "stray_data"), ("symbols", n), ("first", h2(first)),
                          ("error", ERROR_TEXT["stray_data"])]))
        if not keep_os_run:
            self.end_os_run()

    def end_os_run(self) -> None:
        if self.run is None:
            return
        r = self.run
        self.run = None
        if self.kind != "pipe":
            return
        kind, ident, a, b, count = r["kind"], r["ident"], r["a"], r["b"], r["count"]
        if kind in ("ts1", "ts2"):
            syms = ident
            link = "PAD" if syms[0] == (PAD, True) else str(syms[0][0])
            lane = "PAD" if syms[1] == (PAD, True) else str(syms[1][0])
            nfts, rate, ctrl = syms[2][0], syms[3][0], syms[4][0]
            mult = f" ×{count}" if count > 1 else ""
            label = f"{kind.upper()}{mult} link={link} lane={lane} n_fts={nfts} " + \
                    ("gen2" if rate & 4 else "gen1")
            self.emit(Tx(a, b, label, False,
                         [("type", kind), ("count", count), ("link", link), ("lane", lane),
                          ("n_fts", nfts), ("rate_id", h2(rate)), ("training_control", h2(ctrl)),
                          ("hot_reset", bool(ctrl & 1)), ("disable_link", bool(ctrl & 2)),
                          ("loopback", bool(ctrl & 4)), ("disable_scrambling", bool(ctrl & 8)),
                          ("compliance_receive", bool(ctrl & 16))]))
        else:
            name = kind.upper()
            label = f"{name} ×{count}" if count > 1 else name
            self.emit(Tx(a, b, label, False, [("type", kind), ("count", count)]))

    # edge-level events
    def gate(self, reason: str) -> None:
        self.end_xz()
        self.end_runs()
        if self.state == "pkt":
            self.truncate(reason)
        self.state = "idle"
        self.os = []

    def xz_edge(self, a: int, b: int) -> None:
        self.end_runs()
        if self.state == "pkt":
            self.pkt = []
        self.state = "idle"
        self.os = []
        if self.xz_run:
            self.xz_run[1] = b
            self.xz_run[2] += 1
        else:
            self.xz_run = [a, b, 1]

    def end_xz(self) -> None:
        if self.xz_run:
            a, b, n = self.xz_run
            self.xz_run = None
            self.emit(Tx(a, b, "X/Z on data", True, [("type", "unknown_value"), ("edges", n),
                                                     ("error", ERROR_TEXT["unknown_value"])]))

    def truncate(self, by: str) -> None:
        a = self.pkt_start[0]
        b = self.pkt[-1][2] if self.pkt else self.pkt_start[1]
        self.emit(Tx(a, b, f"Packet truncated by {by}", True,
                     [("type", "truncated"), ("packet", self.pkt_kind), ("symbols", len(self.pkt)),
                      ("error", ERROR_TEXT["truncated"])]))
        self.pkt = []
        self.state = "idle"

    # symbols
    def symbol(self, byte: int, k: bool, s: int, d: int) -> None:
        self.end_xz()
        self.valid_close = None
        self.last_sym = (s, s + d)
        # descrambler bookkeeping
        # §10.3: every ordered-set symbol bypasses (TS 1-15, EIEOS 15).
        bypass = self.state == "os" and (
            self.os_kind == "ts" or (self.os_kind is None and len(self.os) == 1 and (not k or byte == PAD))
            or (self.os_kind == "eieos" and len(self.os) == 15))
        if k and byte == COM:
            self.lfsr = Lfsr()
            self.synced = True
            key = None
        elif k and byte == SKP:
            key = None
        else:
            key = self.lfsr.next_key()
        if not k and self.mode == "on":
            if not self.synced:
                return
            if not bypass:
                byte ^= key
        self.process(byte, k, s, d)

    def process(self, byte: int, k: bool, s: int, d: int) -> None:
        e = s + d
        if self.state == "os":
            if self.os_step(byte, k, s, e):
                return
        if self.state == "pkt":
            if k and byte in (END, EDB):
                self.close_packet(byte, e)
                return
            if k and byte in (STP, SDP):
                self.truncate("STP" if byte == STP else "SDP")
                self.open_packet(byte, s, e)
                return
            if k and byte == COM:
                self.truncate("COM")
                self.start_os(s, e)
                return
            if k and byte not in K_NAMES:
                self.emit(Tx(s, e, f"Invalid K symbol {h2(byte)}", True,
                             [("type", "invalid_k"), ("symbol", h2(byte)), ("error", ERROR_TEXT["invalid_k"])]))
                self.pkt = []
                self.state = "idle"
                return
            if k:
                self.emit(Tx(s, e, f"Invalid K symbol in packet {h2(byte)}", True,
                             [("type", "k_in_packet"), ("symbol", h2(byte)),
                              ("error", ERROR_TEXT["k_in_packet"])]))
                self.pkt = []
                self.state = "idle"
                return
            self.pkt.append((byte, s, e))
            return
        # outside
        if not k:
            if byte == 0:
                self.end_runs_except("idle")
                if self.idle_run:
                    self.idle_run[1] = e
                    self.idle_run[2] += 1
                else:
                    self.idle_run = [s, e, 1]
                return
            self.end_runs_except("stray")
            if self.stray_run:
                self.stray_run[1] = e
                self.stray_run[2] += 1
            else:
                self.stray_run = [s, e, 1, byte]
            return
        self.end_runs(keep_os_run=(byte == COM))
        if byte == COM:
            self.start_os(s, e)
        elif byte in (STP, SDP):
            self.end_os_run()
            self.open_packet(byte, s, e)
        elif byte in (END, EDB):
            self.end_os_run()
            self.emit(Tx(s, e, f"{K_NAMES[byte]} without start", True,
                         [("type", "unmatched_end"), ("error", ERROR_TEXT["unmatched_end"])]))
        elif byte not in K_NAMES:
            self.end_os_run()
            self.emit(Tx(s, e, f"Invalid K symbol {h2(byte)}", True,
                         [("type", "invalid_k"), ("symbol", h2(byte)), ("error", ERROR_TEXT["invalid_k"])]))
        else:
            self.end_os_run()   # a lone PAD/SKP/FTS/IDL/EIE outside (§10.2)
            self.emit(Tx(s, e, f"Unexpected K symbol {h2(byte)}", True,
                         [("type", "unexpected_k"), ("symbol", h2(byte)), ("error", ERROR_TEXT["unexpected_k"])]))

    def end_runs_except(self, keep: str) -> None:
        if keep != "idle" and self.idle_run:
            ir = self.idle_run
            self.idle_run = None
            if self.kind == "pipe" and self.show_idle:
                self.emit(Tx(ir[0], ir[1], f"Idle ×{ir[2]}" if ir[2] > 1 else "Idle", False,
                             [("type", "idle"), ("symbols", ir[2])]))
        if keep != "stray" and self.stray_run:
            a, b, n, first = self.stray_run
            self.stray_run = None
            self.emit(Tx(a, b, "Data outside a packet", True,
                         [("type", "stray_data"), ("symbols", n), ("first", h2(first)),
                          ("error", ERROR_TEXT["stray_data"])]))
        self.end_os_run()

    # ordered sets
    def start_os(self, s: int, e: int) -> None:
        self.state = "os"
        self.os = [(COM, True, s, e)]
        self.os_kind = None

    def os_step(self, byte: int, k: bool, s: int, e: int) -> bool:
        """True if the symbol was consumed by the ordered set."""
        os = self.os
        if len(os) == 1:
            if not k or byte == PAD:
                self.os_kind = "ts"
            elif byte == SKP:
                self.os_kind = "skp"
            elif byte == FTS:
                self.os_kind = "fts"
            elif byte == IDL:
                self.os_kind = "eios"
            elif byte == EIE:
                self.os_kind = "eieos"
            else:
                self.os_unknown(byte, e)
                self.state = "idle"
                return True     # §5.3: the misfit is consumed; resume at the next symbol
        kind = self.os_kind
        if kind == "skp":
            if k and byte == SKP:
                os.append((byte, k, s, e))
                if len(os) == 6:
                    self.os_done()
                return True
            self.os_done()
            return False
        os.append((byte, k, s, e))
        if kind == "ts":
            if len(os) == 16:
                self.os_done()
            return True
        need = {"fts": [FTS] * 3, "eios": [IDL] * 3, "eieos": [EIE] * 14}[kind]
        pos = len(os) - 1
        if kind == "eieos" and pos == 15:
            if not k and byte == 0x4A:
                self.os_done()
            else:
                os.pop()
                self.os_unknown(byte, e)
                self.state = "idle"
            return True
        if not (k and byte == need[pos - 1]):
            os.pop()
            self.os_unknown(byte, e)
            self.state = "idle"
            return True         # §5.3: consumed as the unknown_os symbol
        if kind != "eieos" and pos == 3:
            self.os_done()
        return True

    def os_unknown(self, sym: int, e: int) -> None:
        self.end_os_run()
        self.emit(Tx(self.os[0][2], e, "Unknown ordered set", True,
                     [("type", "unknown_os"), ("symbol", h2(sym)), ("error", ERROR_TEXT["unknown_os"])]))

    def os_done(self) -> None:
        os = self.os
        self.state = "idle"
        a, b = os[0][2], os[-1][3]
        kind = self.os_kind
        if kind == "ts":
            ident = tuple((x[0], x[1]) for x in os[1:16])
            sym6 = os[6][0] if not os[6][1] else None
            if sym6 == 0x4A:
                kind = "ts1"
            elif sym6 == 0x45:
                kind = "ts2"
            else:
                self.os_unknown(os[6][0], b)
                return
            if ident[4][0] & 0x08 and not self.ds_seen:
                self.ds_seen = (a, b)
            self.add_run(kind, ident, a, b)
        elif kind in ("fts", "eieos"):
            self.add_run(kind, None, a, b)
        elif kind == "skp":
            self.end_os_run()
            n = len(os) - 1
            if self.kind == "pipe":
                self.emit(Tx(a, b, "SKP" if n == 3 else f"SKP ×{n}", False,
                             [("type", "skp"), ("skp_symbols", n)]))
        elif kind == "eios":
            self.end_os_run()
            if self.kind == "pipe":
                self.emit(Tx(a, b, "EIOS", False, [("type", "eios")]))

    def add_run(self, kind, ident, a, b) -> None:
        # Anything processed between two ordered sets has already ended the
        # run (end_runs / end_os_run), so a live run is always adjacent.
        r = self.run
        if self.coalesce and r and r["kind"] == kind and r["ident"] == ident:
            r["b"] = b
            r["count"] += 1
        else:
            self.end_os_run()
            self.run = dict(kind=kind, ident=ident, a=a, b=b, count=1)
        if not self.coalesce:
            self.end_os_run()

    # packets
    def open_packet(self, byte: int, s: int, e: int) -> None:
        self.state = "pkt"
        self.pkt = []
        self.pkt_start = (s, e)
        self.pkt_kind = "tlp" if byte == STP else "dllp"

    def close_packet(self, endsym: int, e: int) -> None:
        body = bytes(x[0] for x in self.pkt)
        a = self.pkt_start[0]
        kind = self.pkt_kind
        self.state = "idle"
        self.pkt = []
        endname = "END" if endsym == END else "EDB"
        if kind == "dllp":
            valid = endsym == END and len(body) == 6 and dllp_crc_ok(body)
        else:
            valid = endsym == END and len(body) >= 6 and lcrc_of(body[:-4]) == body[-4:]
        if valid:
            self.valid_close = (a, e)
        if kind == "dllp" and endsym == EDB:
            self.emit(Tx(a, e, "DLLP ended by EDB", True,
                         [("type", "dllp_edb"), ("symbols", len(body)), ("error", ERROR_TEXT["dllp_edb"])]))
            return
        if self.kind == "pipe":
            n = len(body)
            if kind == "dllp":
                self.emit(Tx(a, e, f"DLLP frame ({n} symbols)", False,
                             [("type", "dllp_frame"), ("symbols", n), ("end", endname)]))
            else:
                tail = "" if endname == "END" else ", EDB"
                if n >= 2:
                    seq = ((body[0] & 0xF) << 8) | body[1]
                    self.emit(Tx(a, e, f"TLP frame seq={seq} ({n} symbols{tail})", False,
                                 [("type", "tlp_frame"), ("seq", seq), ("symbols", n), ("end", endname)]))
                else:
                    self.emit(Tx(a, e, f"TLP frame ({n} symbols{tail})", False,
                                 [("type", "tlp_frame"), ("symbols", n), ("end", endname)]))
            return
        if kind == "dllp":
            self.emit(decode_dllp(body, a, e))
        else:
            self.emit(self.decode_tlp(body, endsym, a, e))

    def decode_tlp(self, body: bytes, endsym: int, a: int, e: int) -> Tx:
        n = len(body)
        if n < 18 or (n - 6) % 4:
            return Tx(a, e, f"TLP length {n} symbols is not valid", True,
                      [("type", "tlp_length"), ("symbols", n), ("error", ERROR_TEXT["tlp_length"])])
        seq = ((body[0] & 0xF) << 8) | body[1]
        tlp, lcrc = body[2:-4], body[-4:]
        good = lcrc_of(body[:-4])
        b0 = tlp[0]
        fmt, typ = b0 >> 5, b0 & 0x1F
        length = ((tlp[2] & 3) << 8) | tlp[3] or 1024
        td, ep, tc = bool(tlp[2] & 0x80), bool(tlp[2] & 0x40), (tlp[1] >> 4) & 7
        name = tlp_name(fmt, typ, b0)
        base = [("seq", seq), ("tlp", name), ("length_dw", length), ("tc", tc), ("td", td),
                ("ep", ep), ("bytes", len(tlp)), ("lcrc", hx(lcrc))]
        if endsym == EDB:
            if lcrc == bytes(x ^ 0xFF for x in good):
                return Tx(a, e, f"Nullified TLP seq={seq} {name}", False, [("type", "tlp_nullified")] + base)
            return Tx(a, e, f"TLP ended by EDB seq={seq}", True,
                      [("type", "tlp_edb"), ("seq", seq), ("bytes", len(tlp)), ("error", ERROR_TEXT["tlp_edb"])])
        hdr = 4 if fmt & 1 else 3
        extra_dw = 4 * hdr + (4 if td else 0)
        if fmt & 2:
            length_ok = len(tlp) - extra_dw == 4 * length
        else:
            length_ok = len(tlp) == extra_dw
        label = f"TLP seq={seq} {name} len={length}"
        extra = []
        errs = []
        replay = False
        if lcrc != good:
            label += " LCRC mismatch"
            extra += [("expected_lcrc", hx(good))]
            errs.append(ERROR_TEXT["tlp_lcrc"])
        if not length_ok:
            label += " length mismatch"
            errs.append(ERROR_TEXT["tlp_length_mismatch"])
        if lcrc == good:
            if self.next_seq is None:
                self.next_seq = (seq + 1) % 4096
            elif seq == self.next_seq:
                self.next_seq = (seq + 1) % 4096
            elif 1 <= (self.next_seq - seq) % 4096 <= 2048:
                label += " (replay)"
                replay = True
            else:
                label += " sequence error"
                extra += [("expected_seq", self.next_seq)]
                errs.append(ERROR_TEXT["tlp_sequence"])
                self.next_seq = (seq + 1) % 4096
        if errs:
            extra.append(("error", " ".join(errs)))
        return Tx(a, e, label, bool(errs), [("type", "tlp")] + base + [("replay", replay)] + extra)

    def flush(self) -> None:
        """SPEC §10.6 order: open OS, runs, open packet, stray/X-Z runs."""
        if self.state == "os":
            complete = (self.os_kind == "skp" and len(self.os) > 1) or \
                (self.os_kind in ("fts", "eios") and len(self.os) == 4) or len(self.os) == 16
            if complete:
                self.os_done()
            else:
                self.end_os_run()
                o = self.os
                self.emit(Tx(o[0][2], o[-1][3], "Unknown ordered set", True,
                             [("type", "unknown_os"), ("symbol", h2(o[-1][0])), ("error", ERROR_TEXT["unknown_os"])]))
                self.state = "idle"
        self.end_os_run()
        if self.idle_run:
            self.end_runs_except("stray")
        if self.state == "pkt":
            self.truncate("end of trace")
        self.end_runs()
        self.end_xz()


def tlp_name(fmt: int, typ: int, b0: int) -> str:
    table = {(0, 0): "MRd32", (1, 0): "MRd64", (0, 1): "MRdLk32", (1, 1): "MRdLk64",
             (2, 0): "MWr32", (3, 0): "MWr64", (0, 2): "IORd", (2, 2): "IOWr",
             (0, 4): "CfgRd0", (2, 4): "CfgWr0", (0, 5): "CfgRd1", (2, 5): "CfgWr1",
             (0, 10): "Cpl", (2, 10): "CplD", (0, 11): "CplLk", (2, 11): "CplDLk"}
    if (fmt, typ) in table:
        return table[(fmt, typ)]
    if typ >> 3 == 0b10 and fmt in (1, 3):
        return "Msg" if fmt == 1 else "MsgD"
    return f"Type {h2(b0)}"


def decode_dllp(body: bytes, a: int, e: int) -> Tx:
    if len(body) != 6:
        return Tx(a, e, f"DLLP length {len(body)}, expected 6", True,
                  [("type", "dllp_length"), ("symbols", len(body)), ("error", ERROR_TEXT["dllp_length"])])
    b0, b1, b2, b3 = body[:4]
    err = False
    fc = {0x4: ("InitFC1-P", "initfc1_p"), 0x5: ("InitFC1-NP", "initfc1_np"), 0x6: ("InitFC1-Cpl", "initfc1_cpl"),
          0xC: ("InitFC2-P", "initfc2_p"), 0xD: ("InitFC2-NP", "initfc2_np"), 0xE: ("InitFC2-Cpl", "initfc2_cpl"),
          0x8: ("UpdateFC-P", "updatefc_p"), 0x9: ("UpdateFC-NP", "updatefc_np"),
          0xA: ("UpdateFC-Cpl", "updatefc_cpl")}
    pm = {0x20: ("PM_Enter_L1", "pm_enter_l1"), 0x21: ("PM_Enter_L23", "pm_enter_l23"),
          0x23: ("PM_Active_State_Request_L1", "pm_as_request_l1"), 0x24: ("PM_Request_Ack", "pm_request_ack")}
    if b0 in (0x00, 0x10):
        seq = ((b2 & 0xF) << 8) | b3
        label, typ, f = (f"{'Ack' if b0 == 0 else 'Nak'} seq={seq}", "ack" if b0 == 0 else "nak", [("seq", seq)])
    elif b0 in pm:
        label, typ, f = pm[b0][0], pm[b0][1], []
    elif b0 == 0x30:
        p = hx(body[1:4])
        label, typ, f = f"Vendor {p}", "vendor", [("payload", p)]
    elif (b0 >> 4) in fc and not b0 & 0x08:
        vc, hdr, data = b0 & 7, ((b1 & 0x3F) << 2) | (b2 >> 6), ((b2 & 0xF) << 8) | b3
        name, typ = fc[b0 >> 4]
        label, f = f"{name} VC{vc} H={hdr} D={data}", [("vc", vc), ("hdr_fc", hdr), ("data_fc", data)]
    else:
        label, typ, f, err = f"Reserved DLLP {h2(b0)}", "reserved", [("byte0", h2(b0))], True
    fields = [("type", typ)] + f + [("crc", hx(body[4:6]))]
    if not dllp_crc_ok(body):
        label += " CRC mismatch"
        err = True
        text = ERROR_TEXT["dllp_crc"]
        if typ == "reserved":
            text = ERROR_TEXT["reserved"] + " " + text
        fields += [("expected_crc", hx(dllp_crc_of(body[:4]))), ("error", text)]
    elif typ == "reserved":
        fields += [("error", ERROR_TEXT["reserved"])]
    return Tx(a, e, label, err, fields)


def decode(vcd: str, bindings: dict) -> tuple[int, list[Tx]]:
    decoder = bindings["decoder"]
    kind = "pipe" if decoder.startswith("ferrite.pcie_pipe") else "dll"
    n = int(decoder.rsplit("_w", 1)[1]) // 8
    params = {"scrambling": "auto", "sample_point": "before_edge", "coalesce": True, "show_idle": False}
    params.update(bindings.get("parameters", {}))
    sb = bindings["signal_bindings"]
    fpt, end, changes, _ = read_vcd(vcd, sb)
    width = {"pclk": 1, "data": 8 * n, "datak": n, "rxvalid": 1, "txelecidle": 1, "rxstatus": 3}

    def val(sample: dict, sig: str) -> str:
        v = sample.get(sig)
        w = width[sig]
        if v is None:
            return "0" * w
        if len(v) > w:
            v = v[-w:]
        return v

    mode = params["scrambling"]
    mk = lambda m: Pipeline(kind, m, params["coalesce"], params["show_idle"])  # noqa: E731
    pipes = {"off": mk("off"), "on": mk("on")} if mode == "auto" else {mode: mk(mode)}
    locked = None if mode == "auto" else mode
    final: list[Tx] = []

    def lock(view: str, span, why: str = "detected") -> None:
        nonlocal locked
        locked = view
        final.extend(pipes[view].out)
        pipes[view].out = []
        if span is not None:
            final.append(Tx(span[0], span[1], f"Scrambling: {view} ({why})", False,
                            [("type", "scrambling_detected"), ("mode", view)]))
        for k in list(pipes):
            if k != view:
                del pipes[k]

    prev = None
    prev_t = None
    prev_rx = 0
    for t, sample in host_samples(changes, end):
        if prev is not None and val(prev, "pclk") == "0" and val(sample, "pclk") == "1":
            src = prev if params["sample_point"] == "before_edge" else sample
            tfs = t * fpt
            p = 0 if prev_t is None else tfs - prev_t
            prev_t = tfs
            gated = None
            if "rxvalid" in sb and val(src, "rxvalid") == "0":
                gated = "rxvalid"
            elif "txelecidle" in sb and val(src, "txelecidle") == "1":
                gated = "txelecidle"
            d, dk = val(src, "data"), val(src, "datak")
            span_e = (tfs, tfs + ((n - 1) * p) // n + p // n)
            if kind == "pipe" and "rxstatus" in sb:
                code = int(val(src, "rxstatus").replace("x", "0").replace("z", "0"), 2)
                if gated is None and code and code != prev_rx:
                    names = {1: "SKP added", 2: "SKP removed", 3: "receiver detected", 4: "8b/10b decode error",
                             5: "elastic buffer overflow", 6: "elastic buffer underflow", 7: "disparity error"}
                    fl = [("type", "rxstatus"), ("code", "0b" + format(code, "03b"))]
                    if code >= 4:
                        fl.append(("error", RXSTATUS_ERROR[code]))
                    for pl in pipes.values():
                        pl.emit(Tx(span_e[0], span_e[1], f"RxStatus: {names[code]}", code >= 4, fl))
                prev_rx = code
            if gated:
                for pl in pipes.values():
                    pl.gate(gated)
            elif any(c in "xz" for c in d + dk):
                for pl in pipes.values():
                    pl.xz_edge(*span_e)
            else:
                dv, kv = int(d, 2), int(dk, 2)
                for j in range(n):
                    s = tfs + (j * p) // n
                    for pl in pipes.values():
                        pl.symbol((dv >> (8 * j)) & 0xFF, bool((kv >> j) & 1), s, p // n)
                    if locked is None:
                        if pipes["off"].ds_seen:
                            lock("off", pipes["off"].ds_seen, "training sets")
                        elif pipes["off"].valid_close or pipes["on"].valid_close:
                            v = "off" if pipes["off"].valid_close else "on"
                            lock(v, pipes[v].valid_close)
        prev = sample
    for pl in pipes.values():
        pl.flush()
    if locked is None:
        off = pipes["off"]
        if off.out:
            last = off.out[-1]
            span = (last.start, last.end)
        else:
            span = off.last_sym
        final.extend(off.out)
        final.append(Tx(span[0], span[1], "Scrambling: off (undetermined)", False,
                        [("type", "scrambling_detected"), ("mode", "off")]))
    else:
        final.extend(pipes[locked].out)
    return fpt, final


def dart_string(s: str) -> str:
    esc = {'"': '\\"', "\\": "\\\\", "\b": "\\b", "\t": "\\t", "\n": "\\n", "\f": "\\f", "\r": "\\r"}
    return '"' + "".join(esc.get(c, f"\\u{ord(c):04x}" if ord(c) < 0x20 else c) for c in s) + '"'


def render(fpt: int, txs: list[Tx]) -> str:
    def sv(v):
        return ("true" if v else "false") if isinstance(v, bool) else str(v)
    rows = []
    for t in txs:
        f = "{" + ",".join(f"{dart_string(k)}:{dart_string(sv(v))}" for k, v in t.fields) + "}"
        rows.append((t.start // fpt, -(-t.end // fpt), t.label, t.err, f))
    rows.sort(key=lambda r: (r[0], r[1], r[2].encode(), r[3], r[4].encode()))
    if not rows:
        return "[\n]\n"
    return "[\n" + ",\n".join(
        f'{{"startTime":{a},"endTime":{b},"label":{dart_string(c)},"isError":{"true" if d else "false"},'
        f'"fields":{e}}}' for a, b, c, d, e in rows) + "\n]\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vcd", required=True)
    ap.add_argument("--bindings", required=True)
    args = ap.parse_args()
    with open(args.bindings, encoding="utf-8") as f:
        b = json.load(f)
    fpt, txs = decode(args.vcd, b)
    sys.stdout.write(render(fpt, txs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
