# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Encoder side of the PCIe PIPE / Data Link Layer golden fixtures.

This module ENCODES: it turns protocol units (ordered sets, DLLPs, TLPs and
deliberate errors) into PIPE symbols, packs them into x1 PIPE words, writes a
VCD, and computes the transactions the decoders must report from the units it
was asked to encode (decoders/pcie/SPEC.md). It never parses a symbol stream
back into units; the only "reading" it does is a model of the WaveCrux host
(which sample a decoder sees at each clock edge), used to assert that the VCD
it wrote carries the words it meant to write.

Written from decoders/pcie/SPEC.md and decoders/pcie/docs/protocol-notes.md.
Nothing here is shared with, or derived from, the C decoders in src/.
"""

from __future__ import annotations

import json
import zlib
from dataclasses import dataclass, field

# ── 1. Protocol references (bit-serial, from protocol-notes §1, §3, §6, §7) ──

COM, STP, SDP, END, EDB, PAD, SKP, FTS, IDL, EIE = (
    0xBC, 0xFB, 0x5C, 0xFD, 0xFE, 0xF7, 0x1C, 0x3C, 0x7C, 0xFC)
VALID_K = {COM, STP, SDP, END, EDB, PAD, SKP, FTS, IDL, EIE}
TS1_ID, TS2_ID = 0x4A, 0x45


def bitrev8(b: int) -> int:
    out = 0
    for i in range(8):
        if b & (1 << i):
            out |= 1 << (7 - i)
    return out


def dllp_crc_register(data: bytes) -> int:
    """protocol-notes §6: poly 0x100B, init 0xFFFF, each byte fed bit 0
    first into an MSB-first register."""
    c = 0xFFFF
    for byte in data:
        for i in range(8):
            d = (byte >> i) & 1
            fb = ((c >> 15) & 1) ^ d
            c = (c << 1) & 0xFFFF
            if fb:
                c ^= 0x100B
    return c


def dllp_crc_wire(data4: bytes) -> bytes:
    """The two CRC bytes in wire order: bitrev8(~C[15:8]), bitrev8(~C[7:0])."""
    c = dllp_crc_register(data4) ^ 0xFFFF
    return bytes([bitrev8((c >> 8) & 0xFF), bitrev8(c & 0xFF)])


def lcrc_register(data: bytes) -> int:
    """protocol-notes §7: poly 0x04C11DB7, init 0xFFFFFFFF, bit 0 first,
    MSB-first register."""
    c = 0xFFFFFFFF
    for byte in data:
        for i in range(8):
            d = (byte >> i) & 1
            fb = ((c >> 31) & 1) ^ d
            c = (c << 1) & 0xFFFFFFFF
            if fb:
                c ^= 0x04C11DB7
    return c


def lcrc_wire(data: bytes) -> bytes:
    """LCRC over the 2 sequence bytes and the TLP, in wire order."""
    c = lcrc_register(data) ^ 0xFFFFFFFF
    return bytes(bitrev8((c >> s) & 0xFF) for s in (24, 16, 8, 0))


class Lfsr:
    """protocol-notes §3: G(x) = x^16+x^5+x^4+x^3+1, Galois form (taps 0x0039
    on a left-shifting register), seed 0xFFFF; key bit i is the register MSB
    before the i-th shift."""

    def __init__(self) -> None:
        self.state = 0xFFFF

    def reset(self) -> None:
        self.state = 0xFFFF

    def key(self) -> int:
        k = 0
        for i in range(8):
            msb = (self.state >> 15) & 1
            k |= msb << i
            self.state = (self.state << 1) & 0xFFFF
            if msb:
                self.state ^= 0x0039
        return k


def keys_after_com(n: int) -> list[int]:
    lf = Lfsr()
    return [lf.key() for _ in range(n)]


# protocol-notes §3, the first 32 keys after a COM.
KAT_KEYS = bytes.fromhex(
    "FF17C014B2E70282726E28A6BE6DBF8D"
    "BE40A7E62CD3E2B207 02772ACD34BEE0".replace(" ", ""))

# protocol-notes §6 table: DLLP bytes -> CRC on the wire.
KAT_DLLP = [
    ("40 08 03 F0", "35 BC"), ("50 08 00 01", "B1 F6"), ("60 00 00 00", "D8 92"),
    ("C0 08 03 F0", "4F C3"), ("D0 08 00 01", "CB 89"), ("E0 00 00 00", "A2 ED"),
    ("00 00 00 00", "B3 62"), ("00 00 0F FF", "25 A8"), ("10 00 00 05", "7D 70"),
    ("80 02 80 C8", "EC F8"),
]
# protocol-notes §7 table.
KAT_LCRC = [
    (b"123456789", "26 39 F4 CB"),
    (bytes.fromhex("0000" "00000001" "0100000F" "00001000"), "9A E8 F8 C2"),
]


def _reflected_crc16(data: bytes, init: int = 0xFFFF) -> int:
    """protocol-notes §6, the reflected form (poly 0xD008, LSB first). A
    second, differently-shaped implementation, used only to cross-check."""
    c = init
    for byte in data:
        c ^= byte
        for _ in range(8):
            c = (c >> 1) ^ 0xD008 if c & 1 else c >> 1
    return c


def self_check() -> list[str]:
    """Runs every known answer in protocol-notes; raises on any failure."""
    lines: list[str] = []
    fails: list[str] = []

    def check(ok: bool, text: str) -> None:
        (lines if ok else fails).append(("OK   " if ok else "FAIL ") + text)

    got = bytes(keys_after_com(32))
    check(got == KAT_KEYS, f"scrambler keys after COM: {got.hex(' ').upper()}")
    for data, want in KAT_DLLP:
        d = bytes.fromhex(data)
        w = dllp_crc_wire(d)
        check(w == bytes.fromhex(want), f"DLLP CRC-16 {data} -> {w.hex(' ').upper()} (want {want})")
        # Reflected form: xorout 0xFFFF, low byte first on the wire.
        r = _reflected_crc16(d) ^ 0xFFFF
        check(bytes([r & 0xFF, r >> 8]) == w, f"  reflected form agrees for {data}")
        # The reflected CRC (with its xorout) over all 6 bytes.
        res = _reflected_crc16(d + w) ^ 0xFFFF
        check(res == 0xAA90, f"  residue over 6 bytes = 0x{res:04X} (want 0xAA90)")
    for data, want in KAT_LCRC:
        w = lcrc_wire(data)
        check(w == bytes.fromhex(want), f"LCRC {data.hex().upper()[:24]}.. -> {w.hex(' ').upper()} (want {want})")
        z = zlib.crc32(data).to_bytes(4, "little")
        check(z == w, "  zlib.crc32 sent LSB first agrees")
        res = zlib.crc32(data + w)
        check(res == 0x2144DF1C, f"  residue 0x{res:08X} (want 0x2144DF1C)")
    nul = bytes(b ^ 0xFF for b in lcrc_wire(KAT_LCRC[1][0]))
    check(nul == bytes.fromhex("6517073D"), f"nullified LCRC {nul.hex(' ').upper()} (want 65 17 07 3D)")
    # Bit-serial LCRC against zlib over a deterministic spread of inputs.
    seed = 0x1234567
    bad = 0
    for n in range(0, 200):
        buf = bytearray()
        for _ in range(n):
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
            buf.append((seed >> 16) & 0xFF)
        if lcrc_wire(bytes(buf)) != zlib.crc32(bytes(buf)).to_bytes(4, "little"):
            bad += 1
    check(bad == 0, f"bit-serial LCRC == zlib over 200 pseudo-random inputs ({bad} mismatches)")
    if fails:
        raise SystemExit("protocol self-check FAILED:\n" + "\n".join(fails + lines))
    return lines


# ── 2. Symbols, units and the stream builder ─────────────────────────────────

@dataclass
class Sym:
    byte: int           # the value the receiver must see (after descrambling)
    k: bool
    scr: bool = False   # the transmitter XORs it with the scrambler key
    ts: bool = False    # TS1/TS2 symbol 1-15: never scrambled, still advances
    wire: int = -1      # filled by Stream.finish()
    key: int = -1       # the key the descrambler applies at this symbol
    before_com: bool = True


@dataclass
class Unit:
    kind: str
    a: int              # first symbol index (or edge index for 'xz')
    b: int              # last symbol index (or edge index)
    info: dict = field(default_factory=dict)


# DLLP intents: (label base, type, fields) are derived from these, never from
# bytes read back.
FC_KINDS = {  # kind -> (high nibble, label name)
    "initfc1_p": (0x4, "InitFC1-P"), "initfc1_np": (0x5, "InitFC1-NP"),
    "initfc1_cpl": (0x6, "InitFC1-Cpl"), "initfc2_p": (0xC, "InitFC2-P"),
    "initfc2_np": (0xD, "InitFC2-NP"), "initfc2_cpl": (0xE, "InitFC2-Cpl"),
    "updatefc_p": (0x8, "UpdateFC-P"), "updatefc_np": (0x9, "UpdateFC-NP"),
    "updatefc_cpl": (0xA, "UpdateFC-Cpl"),
}
PM_KINDS = {"pm_enter_l1": (0x20, "PM_Enter_L1"), "pm_enter_l23": (0x21, "PM_Enter_L23"),
            "pm_as_request_l1": (0x23, "PM_Active_State_Request_L1"),
            "pm_request_ack": (0x24, "PM_Request_Ack")}


@dataclass
class Dllp:
    kind: str                     # ack nak pm_* vendor initfc*/updatefc* reserved
    seq: int = 0
    vc: int = 0
    hdr: int = 0
    data: int = 0
    payload: int = 0              # vendor bytes 1-3, or reserved bytes 1-3
    byte0: int = 0                # reserved only

    def bytes4(self) -> bytes:
        if self.kind in ("ack", "nak"):
            return bytes([0x00 if self.kind == "ack" else 0x10, 0x00,
                          (self.seq >> 8) & 0x0F, self.seq & 0xFF])
        if self.kind in PM_KINDS:
            return bytes([PM_KINDS[self.kind][0], 0, 0, 0])
        if self.kind == "vendor":
            return bytes([0x30]) + self.payload.to_bytes(3, "big")
        if self.kind in FC_KINDS:
            assert 0 <= self.hdr <= 0xFF and 0 <= self.data <= 0xFFF and 0 <= self.vc <= 7
            return bytes([(FC_KINDS[self.kind][0] << 4) | self.vc,
                          (self.hdr >> 2) & 0x3F,
                          ((self.hdr & 0x3) << 6) | ((self.data >> 8) & 0x0F),
                          self.data & 0xFF])
        if self.kind == "reserved":
            return bytes([self.byte0]) + self.payload.to_bytes(3, "big")
        raise ValueError(self.kind)

    def describe(self) -> tuple[str, str, list, bool]:
        """(label, type, fields before crc, is_error) per SPEC §8.1."""
        k = self.kind
        if k in ("ack", "nak"):
            name = "Ack" if k == "ack" else "Nak"
            return f"{name} seq={self.seq}", k, [("seq", self.seq)], False
        if k in PM_KINDS:
            return PM_KINDS[k][1], k, [], False
        if k == "vendor":
            p = f"0x{self.payload:06X}"
            return f"Vendor {p}", "vendor", [("payload", p)], False
        if k in FC_KINDS:
            return (f"{FC_KINDS[k][1]} VC{self.vc} H={self.hdr} D={self.data}", k,
                    [("vc", self.vc), ("hdr_fc", self.hdr), ("data_fc", self.data)], False)
        if k == "reserved":
            b0 = f"0x{self.byte0:02X}"
            return f"Reserved DLLP {b0}", "reserved", [("byte0", b0)], True
        raise ValueError(k)


# SPEC §8.2 name table, as (fmt, type) -> name; used to BUILD byte 0 from a
# name, the encoder direction.
TLP_NAMES = {
    "MRd32": (0b000, 0b00000), "MRd64": (0b001, 0b00000),
    "MRdLk32": (0b000, 0b00001), "MRdLk64": (0b001, 0b00001),
    "MWr32": (0b010, 0b00000), "MWr64": (0b011, 0b00000),
    "IORd": (0b000, 0b00010), "IOWr": (0b010, 0b00010),
    "CfgRd0": (0b000, 0b00100), "CfgWr0": (0b010, 0b00100),
    "CfgRd1": (0b000, 0b00101), "CfgWr1": (0b010, 0b00101),
    "Msg": (0b001, 0b10000), "MsgD": (0b011, 0b10000),
    "Cpl": (0b000, 0b01010), "CplD": (0b010, 0b01010),
    "CplLk": (0b000, 0b01011), "CplDLk": (0b010, 0b01011),
}


@dataclass
class Tlp:
    """A TLP as the encoder builds it. `name` is what the fixture author says
    the decoder must call it (a SPEC §8.2 name, or "Type 0xNN" for byte 0 =
    0xNN outside the table)."""
    name: str
    seq: int
    length_dw: int = 1            # header Length field value as a count, 1..1024
    payload_dw: int | None = None  # payload actually carried (default: length)
    tc: int = 0
    td: bool = False
    ep: bool = False
    byte0: int | None = None      # only for "Type 0x.." TLPs
    msg_routing: int = 0          # rrr for Msg/MsgD
    extra_bytes: int = 0          # junk bytes appended (framing errors)
    rng_seed: int = 1

    def fmt_type(self) -> tuple[int, int]:
        if self.byte0 is not None:
            return self.byte0 >> 5, self.byte0 & 0x1F
        f, t = TLP_NAMES[self.name]
        if self.name in ("Msg", "MsgD"):
            t |= self.msg_routing & 0x7
        return f, t

    def hdr_dw(self) -> int:
        return 4 if self.fmt_type()[0] & 1 else 3

    def has_data(self) -> bool:
        return bool(self.fmt_type()[0] & 2)

    def bytes(self) -> bytes:
        fmt, typ = self.fmt_type()
        b0 = (fmt << 5) | typ
        if self.byte0 is not None:
            assert b0 == self.byte0
        length_field = self.length_dw & 0x3FF   # 1024 -> 0
        hdr = bytearray()
        hdr += bytes([b0, (self.tc & 7) << 4,
                      (0x80 if self.td else 0) | (0x40 if self.ep else 0) | (length_field >> 8),
                      length_field & 0xFF])
        rng = Prng(self.rng_seed)
        # DW1..: plausible requester/completer ID, tag, BEs and address, but
        # nothing the SPEC's light header decode reads.
        hdr += bytes([0x01, 0x00, rng.byte(), 0x0F if self.length_dw > 1 else 0x0F])
        for _ in range(self.hdr_dw() - 2):
            hdr += bytes([0x00, 0x00, 0x10, rng.byte() & 0xFC])
        pdw = self.length_dw if self.payload_dw is None else self.payload_dw
        payload = bytes(rng.byte() for _ in range(4 * pdw)) if self.has_data() or self.payload_dw else b""
        body = bytes(hdr) + payload
        if self.td:
            body += ecrc(body)
        body += bytes(rng.byte() for _ in range(self.extra_bytes))
        return body


def ecrc(tlp: bytes) -> bytes:
    """ECRC (protocol-notes §4): CRC-32 over header and data with the variant
    bits (Type bit 0, EP) forced to 1. The decoders do not check it."""
    b = bytearray(tlp)
    b[0] |= 0x01
    b[2] |= 0x40
    return lcrc_wire(bytes(b))


class Prng:
    """Deterministic LCG for payload bytes."""

    def __init__(self, seed: int) -> None:
        self.s = seed & 0x7FFFFFFF or 1

    def byte(self) -> int:
        self.s = (self.s * 1103515245 + 12345) & 0x7FFFFFFF
        return (self.s >> 16) & 0xFF


class Stream:
    """A transmitter: units in, symbols out, with each unit's symbol range.

    `scrambled` makes the transmitter scramble every D symbol outside TS1/TS2
    (SPEC §5.2). Special edges (gated, X/Z) are inserted at word boundaries.
    """

    def __init__(self, lanes: int, scrambled: bool = False) -> None:
        self.n = lanes
        self.scrambled = scrambled
        self.syms: list[Sym] = []
        self.units: list[Unit] = []
        self.specials: dict[int, list[dict]] = {}   # symbol index -> edge groups
        self.rxstatus_sched: list[tuple[int, int]] = []
        self.finished = False

    # low level
    def _put(self, byte: int, k: bool = False, ts: bool = False, scr: bool | None = None) -> int:
        if scr is None:
            scr = self.scrambled and not k and not ts
        self.syms.append(Sym(byte & 0xFF, k, scr, ts))
        return len(self.syms) - 1

    def _unit(self, kind: str, a: int, info: dict | None = None, b: int | None = None) -> Unit:
        u = Unit(kind, a, len(self.syms) - 1 if b is None else b, info or {})
        self.units.append(u)
        return u

    def pos(self) -> int:
        return len(self.syms)

    def aligned(self) -> bool:
        return len(self.syms) % self.n == 0

    def pad_idle(self, multiple: int | None = None) -> None:
        """Logical idle up to the next word boundary (or a multiple)."""
        m = multiple or self.n
        n = (-len(self.syms)) % m
        if n:
            self.idle(n)

    # ordered sets (SPEC §5.3, protocol-notes §2)
    def ts(self, kind: str = "ts1", link: int | None = None, lane: int | None = None,
           n_fts: int = 24, rate: int = 0x02, ctrl: int = 0x00, count: int = 1,
           sym6: int | None = None) -> None:
        ident = TS1_ID if kind == "ts1" else TS2_ID
        for _ in range(count):
            a = self._put(COM, True)
            body = []
            for v in (link, lane):
                if v is None:
                    body.append((PAD, True))
                else:
                    body.append((v, False))
            body += [(n_fts, False), (rate, False), (ctrl, False)]
            first_id = ident if sym6 is None else sym6
            body += [(first_id, False)] + [(ident, False)] * 9
            for byte, k in body:
                self._put(byte, k, ts=True)
            if sym6 is None:
                self._unit("ts", a, dict(kind=kind, link=link, lane=lane, n_fts=n_fts,
                                         rate=rate, ctrl=ctrl, key=tuple(body)))
            else:
                self._unit("os_unknown", a, dict(symbol=sym6))

    def skp(self, n: int = 3) -> None:
        """A SKP OS; with n = 6 the sixth SKP is an unexpected K (§10.2)."""
        a = self._put(COM, True)
        for _ in range(min(n, 5)):
            self._put(SKP, True)
        self._unit("skp", a, dict(n=min(n, 5)))
        for _ in range(n - 5):
            self.unexpected_k(SKP)

    def fts(self, count: int = 1) -> None:
        for _ in range(count):
            a = self._put(COM, True)
            for _ in range(3):
                self._put(FTS, True)
            self._unit("fts", a)

    def eios(self) -> None:
        a = self._put(COM, True)
        for _ in range(3):
            self._put(IDL, True)
        self._unit("eios", a)

    def eieos(self, count: int = 1) -> None:
        for _ in range(count):
            a = self._put(COM, True)
            for _ in range(14):
                self._put(EIE, True)
            self._put(TS1_ID, False, ts=True)   # ordered-set symbol: bypasses (§10.3)
            self._unit("eieos", a)

    def os_misfit(self, ks: list[int], misfit: int) -> None:
        """COM, the K symbols `ks`, then a D symbol that fits no ordered set.
        The unknown_os error spans COM..misfit (SPEC §5.3 'anything else')."""
        a = self._put(COM, True)
        for k in ks:
            self._put(k, True)
        self._put(misfit, False)
        self._unit("os_unknown", a, dict(symbol=misfit))

    def os_bad_start(self, kbyte: int) -> None:
        """COM followed by a K symbol that cannot start an ordered set (STP,
        SDP, END, EDB, COM): unknown_os spanning both, with the misfit
        CONSUMED as its `symbol`; decoding resumes after it (SPEC §5.3)."""
        assert kbyte in (STP, SDP, END, EDB, COM)
        a = self._put(COM, True)
        self._put(kbyte, True)
        self._unit("os_unknown", a, dict(symbol=kbyte))

    def idle(self, n: int) -> None:
        a = len(self.syms)
        for _ in range(n):
            self._put(0x00)
        self._unit("idle", a, dict(n=n))

    def pre_com(self, data: bytes) -> None:
        """D symbols before the first COM; `on` discards them (SPEC §5.2)."""
        assert all(s.byte != COM or not s.k for s in self.syms)
        a = len(self.syms)
        for b in data:
            self._put(b, scr=False)
        self._unit("pre_com", a)

    # packets (protocol-notes §4)
    def dllp(self, d: Dllp, crc: bytes | None = None, body: bytes | None = None,
             end: int = END) -> Unit:
        b4 = d.bytes4()
        good = dllp_crc_wire(b4)
        wire_crc = good if crc is None else crc
        raw = b4 + wire_crc if body is None else body
        a = self._put(SDP, True)
        for x in raw:
            self._put(x)
        self._put(end, True)
        return self._unit("dllp", a, dict(dllp=d, body=raw, crc=wire_crc, good_crc=good,
                                          end=end))

    def tlp(self, t: Tlp, lcrc: str = "good", end: int = END, seq_bytes: bytes | None = None) -> Unit:
        tb = t.bytes()
        sb = bytes([(t.seq >> 8) & 0x0F, t.seq & 0xFF]) if seq_bytes is None else seq_bytes
        good = lcrc_wire(sb + tb)
        if lcrc == "good":
            wl = good
        elif lcrc == "invert":
            wl = bytes(x ^ 0xFF for x in good)
        elif lcrc == "bad":
            wl = bytes([good[0] ^ 0x01]) + good[1:]
        else:
            raise ValueError(lcrc)
        body = sb + tb + wl
        a = self._put(STP, True)
        for x in body:
            self._put(x)
        self._put(end, True)
        return self._unit("tlp", a, dict(tlp=t, body=body, nbytes=len(tb), lcrc=wl,
                                         good_lcrc=good, lcrc_mode=lcrc, end=end))

    def raw_dllp_bytes(self, body: bytes, end: int = END) -> Unit:
        """A DLLP frame of arbitrary length (length errors)."""
        a = self._put(SDP, True)
        for x in body:
            self._put(x)
        self._put(end, True)
        return self._unit("dllp_badlen", a, dict(body=body, end=end))

    def raw_tlp_bytes(self, body: bytes, end: int = END) -> Unit:
        a = self._put(STP, True)
        for x in body:
            self._put(x)
        self._put(end, True)
        return self._unit("tlp_badlen", a, dict(body=body, end=end))

    def _upcoming_keys(self, skip: int, n: int) -> list[int]:
        """The scrambler keys the next symbols will get, after `skip` more
        non-SKP, non-COM symbols (the transmitter's own LFSR)."""
        lf = Lfsr()
        for s in self.syms:
            if s.k and s.byte == COM:
                lf.reset()
            elif not (s.k and s.byte == SKP):
                lf.key()
        for _ in range(skip):
            lf.key()
        return [lf.key() for _ in range(n)]

    def contrived_dllp(self, d: Dllp) -> Unit:
        """A DLLP whose CRC is right only in the OTHER pipeline: in a plain
        stream it is valid only when descrambled, in a scrambled stream only
        when read raw. Its intended view (this stream's own mode) is `d` with
        a CRC mismatch. Used to pin down SPEC §5.5's 'first valid packet
        wins' and 'continue with the winner only'."""
        b4 = d.bytes4()
        k = self._upcoming_keys(1, 6)       # SDP takes one key first
        x = bytes(b ^ kk for b, kk in zip(b4, k[:4]))
        c = bytes(a ^ b for a, b in zip(dllp_crc_wire(x), k[4:]))
        assert c != dllp_crc_wire(b4)
        return self.dllp(d, crc=c)

    # deliberate errors (SPEC §6)
    def invalid_k(self, byte: int) -> None:
        assert byte not in VALID_K
        a = self._put(byte, True)
        self._unit("invalid_k", a, dict(symbol=byte))

    def truncated(self, packet: str, body: bytes, by: str) -> None:
        """STP/SDP and `body`; the caller then sends what truncates it (STP,
        SDP, COM) or gates (rxvalid, txelecidle). Checked in finish()."""
        a = self._put(STP if packet == "tlp" else SDP, True)
        for x in body:
            self._put(x)
        self._unit("truncated", a, dict(packet=packet, symbols=len(body), by=by))

    def dropped(self, packet: str, body: bytes) -> None:
        """An open packet that an X/Z edge drops silently (SPEC §4.4)."""
        a = self._put(STP if packet == "tlp" else SDP, True)
        for x in body:
            self._put(x)
        self._unit("dropped", a)

    def unexpected_k(self, byte: int) -> None:
        """PAD/SKP/FTS/IDL/EIE outside an ordered set and a packet (§10.2)."""
        assert byte in (PAD, SKP, FTS, IDL, EIE)
        a = self._put(byte, True)
        self._unit("unexpected_k", a, dict(symbol=byte))

    def invalid_k_in_packet(self, packet: str, body: bytes, byte: int) -> None:
        """A non-K-code with K=1 inside a packet: invalid_k only, packet
        dropped (§10.2)."""
        assert byte not in VALID_K
        p = self._put(STP if packet == "tlp" else SDP, True)
        for x in body:
            self._put(x)
        self._unit("partial", p)
        self.invalid_k(byte)

    def open_at_end(self, packet: str, body: bytes) -> None:
        """A packet still open when the trace ends (§10.6 step 3)."""
        a = self._put(STP if packet == "tlp" else SDP, True)
        for x in body:
            self._put(x)
        self._unit("truncated", a, dict(packet=packet, symbols=len(body), by="end of trace"))

    def os_incomplete_at_end(self, rest: list[tuple[int, bool]]) -> None:
        """COM and the symbols `rest`, cut off by the end of the trace:
        unknown_os with `symbol` = the last symbol received (§10.6, §10.7)."""
        a = self._put(COM, True)
        ts = bool(rest) and (not rest[0][1] or rest[0][0] == PAD)
        for byte, k in rest:
            self._put(byte, k, ts=ts)
        last = rest[-1][0] if rest else COM
        self._unit("os_unknown", a, dict(symbol=last, at_end=True))

    def unmatched_end(self, end: int) -> None:
        a = self._put(end, True)
        self._unit("unmatched_end", a, dict(end=end))

    def k_in_packet(self, packet: str, body: bytes, kbyte: int) -> None:
        """A packet broken by a K symbol other than END/EDB/COM/STP/SDP: the
        error spans the K symbol, the packet so far is dropped, and the framer
        is outside a packet again afterwards (SPEC §10.2)."""
        assert kbyte in VALID_K and kbyte not in (COM, STP, SDP, END, EDB)
        p = self._put(STP if packet == "tlp" else SDP, True)
        for x in body:
            self._put(x)
        self._unit("partial", p)
        a = self._put(kbyte, True)
        self._unit("k_in_packet", a, dict(symbol=kbyte))

    def stray(self, data: bytes) -> None:
        assert data and all(b != 0 for b in data)
        a = len(self.syms)
        for b in data:
            self._put(b)
        self._unit("stray", a, dict(symbols=len(data), first=data[0]))

    # edge-level events (word aligned)
    def gate(self, signal: str, edges: int, bus: list[tuple[int, int]] | None = None) -> None:
        assert self.aligned(), f"gate at symbol {len(self.syms)} is not word aligned"
        self.specials.setdefault(len(self.syms), []).append(
            dict(kind="gate", signal=signal, edges=edges, bus=bus))

    def xz(self, edges: int, data: object = "x", datak: object = None) -> None:
        assert self.aligned()
        self.specials.setdefault(len(self.syms), []).append(
            dict(kind="xz", edges=edges, data=data, datak=datak))

    def rxstatus(self, code: int) -> None:
        assert self.aligned()
        self.rxstatus_sched.append((len(self.syms), code))

    # finish: scramble, validate the error constructs
    def finish(self) -> None:
        assert self.aligned(), f"stream ends mid-word ({len(self.syms)} symbols, {self.n} lanes)"
        lf = Lfsr()
        seen_com = False
        for s in self.syms:
            if s.k and s.byte == COM:
                lf.reset()
                seen_com = True
                s.key = -1
                s.before_com = False
            elif s.k and s.byte == SKP:
                s.key = -1
                s.before_com = not seen_com
            else:
                s.key = lf.key()
                s.before_com = not seen_com
            s.wire = s.byte ^ s.key if (s.scr and not s.k) else s.byte
            if s.scr:
                assert not s.before_com, "scrambled symbol before the first COM"
        # The truncating symbol after each truncated packet must be what the
        # unit says.
        for u in self.units:
            if u.kind != "truncated":
                continue
            nxt = u.b + 1
            by = u.info["by"]
            if by == "end of trace":
                assert nxt == len(self.syms) and not self.specials.get(nxt), u
            elif by in ("STP", "SDP", "COM"):
                want = {"STP": STP, "SDP": SDP, "COM": COM}[by]
                assert self.syms[nxt].k and self.syms[nxt].byte == want, (u, by)
            else:
                g = self.specials.get(nxt, [])
                assert g and g[0]["kind"] == "gate" and g[0]["signal"] == by, (u, by)
        covered = []
        for u in self.units:
            covered += list(range(u.a, u.b + 1))
        assert covered == list(range(len(self.syms))), "units must cover every symbol once, in order"
        for u in self.units:
            if u.info.get("at_end"):
                assert u is self.units[-1] and not self.specials.get(len(self.syms)), u
            if u.kind == "dropped":
                g = self.specials.get(u.b + 1, [])
                assert g and g[0]["kind"] == "xz", u
        self.finished = True


# ── 3. Edges, clocking, the VCD writer and the host model ────────────────────

UNIT_EXP = {"s": 0, "ms": -3, "us": -6, "ns": -9, "ps": -12, "fs": -15}


def fs_per_tick(factor: int, unit: str) -> int:
    return factor * 10 ** (UNIT_EXP[unit] + 15)


@dataclass
class Edge:
    kind: str                 # data, gate, xz
    syms: list[int]           # symbol indices (data edges)
    data: object              # int, or a bit string with x/z
    datak: object
    rxvalid: int = 1
    txelecidle: int = 0
    rxstatus: int = 0
    t: int = 0                # ticks
    tfs: int = 0
    pfs: int = 0


def build_edges(st: Stream, sideband_idle: dict | None = None) -> list[Edge]:
    """Lays the symbols out on PIPE words: byte lane j = j-th symbol."""
    assert st.finished
    n = st.n
    edges: list[Edge] = []
    sched = sorted(st.rxstatus_sched)
    code = 0

    def rx_at(i: int) -> int:
        c = 0
        for pos, v in sched:
            if pos <= i:
                c = v
        return c

    i = 0
    total = len(st.syms)
    while i <= total:
        for sp in st.specials.get(i, []):
            for e in range(sp["edges"]):
                if sp["kind"] == "gate":
                    bus = sp["bus"][e] if sp["bus"] else (0, 0)
                    edges.append(Edge("gate", [], bus[0], bus[1],
                                      rxvalid=0 if sp["signal"] == "rxvalid" else 1,
                                      txelecidle=1 if sp["signal"] == "txelecidle" else 0,
                                      rxstatus=rx_at(i)))
                else:
                    data = sp["data"][e] if isinstance(sp["data"], list) else sp["data"]
                    if isinstance(data, str) and len(data) == 1:
                        data = data * (8 * n)
                    dk = sp["datak"]
                    dk = dk[e] if isinstance(dk, list) else dk
                    datak = (dk * n if isinstance(dk, str) and len(dk) == 1 else dk) if dk is not None else 0
                    edges.append(Edge("xz", [], data, datak, rxstatus=rx_at(i)))
        if i == total:
            break
        idx = list(range(i, i + n))
        word = 0
        kw = 0
        for j, si in enumerate(idx):
            s = st.syms[si]
            word |= s.wire << (8 * j)
            kw |= int(s.k) << j
        edges.append(Edge("data", idx, word, kw, rxstatus=rx_at(i)))
        i += n
    del code
    return edges


@dataclass
class Clocking:
    timescale: tuple[int, str] = (1, "ns")
    period: int = 8                       # ticks; even
    first_edge: int | None = None         # ticks; default one period
    period_fn: object = None              # edge index -> period ticks (overrides)
    change: str = "zero"                  # "zero" or "delay"
    delay: int = 0                        # ticks after the previous edge
    sample_point: str = "before_edge"

    def periods(self, count: int) -> list[int]:
        out = []
        for k in range(count):
            p = self.period_fn(k) if self.period_fn else self.period
            assert p >= 2
            out.append(p)
        return out


@dataclass
class VcdNames:
    scope: tuple = ("tb", "dut")
    pclk: str = "pclk"
    data: str = "rx_data"
    datak: str = "rx_datak"
    rxvalid: str | None = None
    txelecidle: str | None = None
    rxstatus: str | None = None
    # Sidebands present in the VCD but left unbound.
    unbound: tuple = ()
    # Free-running noise signals (name, width, period ticks, phase ticks).
    noise: tuple = ()
    range_suffix: bool = True


SIG_WIDTH = {"pclk": 1, "rxvalid": 1, "txelecidle": 1, "rxstatus": 3}


def _bits(v: object, width: int) -> str:
    if isinstance(v, str):
        assert len(v) == width, (v, width)
        return v
    assert 0 <= v < (1 << width), (hex(v), width)
    return format(v, f"0{width}b")


class VcdTrace:
    """The written VCD plus its change list (for the host model)."""

    def __init__(self) -> None:
        self.text = ""
        self.changes: dict[str, list[tuple[int, str]]] = {}
        self.end_time = 0
        self.paths: dict[str, str] = {}
        self.edge_times: list[int] = []


def write_vcd(edges: list[Edge], lanes: int, clk: Clocking, names: VcdNames) -> VcdTrace:
    width = 8 * lanes
    count = len(edges)
    periods = clk.periods(count)
    t0 = clk.first_edge if clk.first_edge is not None else periods[0]
    assert t0 > 0
    times = [t0]
    for k in range(1, count):
        times.append(times[-1] + periods[k - 1])
    fpt = fs_per_tick(*clk.timescale)
    for k, e in enumerate(edges):
        e.t = times[k]
        e.tfs = times[k] * fpt
        e.pfs = 0 if k == 0 else (times[k] - times[k - 1]) * fpt
    end_time = times[-1] + periods[-1]

    # Logical signal -> (var name, width, id)
    sigs: list[tuple[str, str, int]] = [("pclk", names.pclk, 1), ("data", names.data, width),
                                        ("datak", names.datak, lanes)]
    for logical in ("rxvalid", "txelecidle", "rxstatus"):
        nm = getattr(names, logical)
        if nm:
            sigs.append((logical, nm, SIG_WIDTH[logical]))
    for logical, nm in names.unbound:
        sigs.append(("unbound_" + logical, nm, SIG_WIDTH[logical]))
    for nm, w, _, _ in names.noise:
        sigs.append(("noise_" + nm, nm, w))
    ids = {}
    for i, (logical, _, _) in enumerate(sigs):
        ids[logical] = chr(33 + i)

    events: dict[int, dict[str, str]] = {}

    def put(t: int, logical: str, w: int, v: object) -> None:
        events.setdefault(t, {})[logical] = _bits(v, w)

    init = {"pclk": 0, "data": 0, "datak": 0, "rxvalid": 1, "txelecidle": 0, "rxstatus": 0}
    for logical, _, w in sigs:
        if logical.startswith("unbound_"):
            put(0, logical, w, 1 if logical.endswith("rxvalid") else 0)
        elif logical.startswith("noise_"):
            put(0, logical, w, 0)
        else:
            put(0, logical, w, init[logical])
    # Clock.
    for k in range(count):
        put(times[k], "pclk", 1, 1)
        put(times[k] + periods[k] // 2, "pclk", 1, 0)
    # Per-edge values: when they change.
    for k, e in enumerate(edges):
        if clk.sample_point == "at_edge":
            assert clk.change == "zero"
            t = times[k]
        elif k == 0:
            t = 0
        elif clk.change == "zero":
            t = times[k - 1]
        else:
            assert 0 < clk.delay < periods[k - 1]
            t = times[k - 1] + clk.delay
        vals = {"data": (e.data, width), "datak": (e.datak, lanes), "rxvalid": (e.rxvalid, 1),
                "txelecidle": (e.txelecidle, 1), "rxstatus": (e.rxstatus, 3)}
        for logical, _, w in sigs:
            if logical in vals:
                put(t, logical, w, vals[logical][0])
    # Unbound sidebands: toggle at odd times so a wrongly bound decoder would
    # see different samples.
    for logical, _ in names.unbound:
        lg = "unbound_" + logical
        for k in range(1, count, 3):
            put(times[k] + 1, lg, SIG_WIDTH[logical], (k // 3) & 1 if logical != "rxstatus" else (k % 7))
    for nm, w, per, ph in names.noise:
        t = ph
        v = 0
        while t < end_time:
            v = (v + 1) % (1 << w)
            put(t, "noise_" + nm, w, v)
            t += per

    # Emit, writing only real changes.
    tr = VcdTrace()
    tr.end_time = end_time
    tr.edge_times = times
    last: dict[str, str] = {}
    out = []
    out.append("$version wavecrux-decoders decoders/pcie/tools/generate_fixtures.py $end\n")
    out.append(f"$timescale {clk.timescale[0]}{clk.timescale[1]} $end\n")
    for sc in names.scope:
        out.append(f"$scope module {sc} $end\n")
    for logical, nm, w in sigs:
        rng = f" [{w - 1}:0]" if (w > 1 and names.range_suffix) else ""
        kind = "reg" if logical != "pclk" else "wire"
        out.append(f"$var {kind} {w} {ids[logical]} {nm}{rng} $end\n")
        tr.paths[logical] = ".".join(names.scope + (nm,))
    for _ in names.scope:
        out.append("$upscope $end\n")
    out.append("$enddefinitions $end\n")
    for t in sorted(events):
        assert t < end_time
        lines = []
        for logical, _, w in sigs:
            if logical not in events[t]:
                continue
            v = events[t][logical]
            if last.get(logical) == v:
                continue
            last[logical] = v
            tr.changes.setdefault(logical, []).append((t, v))
            lines.append(f"{v}{ids[logical]}\n" if w == 1 else f"b{v} {ids[logical]}\n")
        if not lines:
            continue
        out.append(f"#{t}\n")
        if t == 0:
            out.append("$dumpvars\n")
            out += lines
            out.append("$end\n")
        else:
            out += lines
    out.append(f"#{end_time}\n")
    tr.text = "".join(out)
    return tr


def host_edges(tr: VcdTrace, bound: list[str], sample_point: str) -> list[tuple[int, dict]]:
    """Models the WaveCrux host (cmake/README.md 'Loader behaviour'): one
    sample per distinct time any BOUND signal changes in [0, end); a signal not
    yet changed reads as zeros. Then SPEC §4.1: a rising edge is pclk going
    0 -> 1 between consecutive samples; the other signals come from the
    previous sample (before_edge) or the same one (at_edge)."""
    stamps = sorted({t for s in bound for t, _ in tr.changes.get(s, []) if t < tr.end_time})
    cur: dict[str, str] = {}
    idx = {s: 0 for s in bound}
    samples = []
    for t in stamps:
        for s in bound:
            ch = tr.changes.get(s, [])
            while idx[s] < len(ch) and ch[idx[s]][0] <= t:
                cur[s] = ch[idx[s]][1]
                idx[s] += 1
        samples.append((t, dict(cur)))
    out = []
    for i in range(1, len(samples)):
        prev, now = samples[i - 1][1], samples[i][1]
        if prev.get("pclk", "0") == "0" and now.get("pclk", "0") == "1":
            src = prev if sample_point == "before_edge" else now
            out.append((samples[i][0], src))
    return out


# ── 4. Expected transactions (SPEC §4.2, §5.5, §6-§9) ────────────────────────

# The `error` sentences, fixed by SPEC §10.1.
ERROR_TEXT = {
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
    "rxstatus_4": "8b/10b decode error reported by the PHY.",
    "rxstatus_5": "Elastic buffer overflow reported by the PHY.",
    "rxstatus_6": "Elastic buffer underflow reported by the PHY.",
    "rxstatus_7": "Disparity error reported by the PHY.",
}


@dataclass
class Tx:
    start: int      # fs
    end: int        # fs
    label: str
    is_error: bool
    fields: list    # [(key, value)], value int/bool/str


def _s(v: object) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    return str(v)


def h2(b: int) -> str:
    return f"0x{b:02X}"


def hexb(bs: bytes) -> str:
    return "0x" + bs.hex().upper()


class Timing:
    def __init__(self, st: Stream, edges: list[Edge]) -> None:
        self.n = st.n
        self.edges = edges
        self.sym_edge: dict[int, tuple[int, int]] = {}
        for ei, e in enumerate(edges):
            for j, si in enumerate(e.syms):
                self.sym_edge[si] = (ei, j)

    def s(self, i: int) -> tuple[int, int]:
        ei, j = self.sym_edge[i]
        e = self.edges[ei]
        return e.tfs + (j * e.pfs) // self.n, e.pfs // self.n

    def span(self, a: int, b: int) -> tuple[int, int]:
        sa, _ = self.s(a)
        sb, db = self.s(b)
        return sa, sb + db

    def edge_span(self, e1: int, e2: int) -> tuple[int, int]:
        a = self.edges[e1]
        b = self.edges[e2]
        return a.tfs, b.tfs + ((self.n - 1) * b.pfs) // self.n + b.pfs // self.n


def _err(kind: str, fields: list) -> list:
    return fields + [("error", ERROR_TEXT[kind])]


def common_error_tx(u: Unit, tm: Timing) -> Tx | None:
    """SPEC §6 rows, from the unit the encoder built."""
    k = u.kind
    if k == "invalid_k":
        s, e = tm.span(u.a, u.b)
        return Tx(s, e, f"Invalid K symbol {h2(u.info['symbol'])}", True,
                  _err("invalid_k", [("type", "invalid_k"), ("symbol", h2(u.info["symbol"]))]))
    if k == "truncated":
        s, e = tm.span(u.a, u.b)
        return Tx(s, e, f"Packet truncated by {u.info['by']}", True,
                  _err("truncated", [("type", "truncated"), ("packet", u.info["packet"]),
                                     ("symbols", u.info["symbols"])]))
    if k == "unexpected_k":
        s, e = tm.span(u.a, u.b)
        return Tx(s, e, f"Unexpected K symbol {h2(u.info['symbol'])}", True,
                  _err("unexpected_k", [("type", "unexpected_k"), ("symbol", h2(u.info["symbol"]))]))
    if k == "dllp_edb":
        s, e = tm.span(u.a, u.b)
        return Tx(s, e, "DLLP ended by EDB", True,
                  _err("dllp_edb", [("type", "dllp_edb"), ("symbols", len(u.info["body"]))]))
    if k == "unmatched_end":
        s, e = tm.span(u.a, u.b)
        name = "END" if u.info["end"] == END else "EDB"
        return Tx(s, e, f"{name} without start", True,
                  _err("unmatched_end", [("type", "unmatched_end")]))
    if k == "k_in_packet":
        s, e = tm.span(u.a, u.b)
        return Tx(s, e, f"Invalid K symbol in packet {h2(u.info['symbol'])}", True,
                  _err("k_in_packet", [("type", "k_in_packet"), ("symbol", h2(u.info["symbol"]))]))
    if k == "stray":
        s, e = tm.span(u.a, u.b)
        return Tx(s, e, "Data outside a packet", True,
                  _err("stray_data", [("type", "stray_data"), ("symbols", u.info["symbols"]),
                                      ("first", h2(u.info["first"]))]))
    if k == "os_unknown":
        s, e = tm.span(u.a, u.b)
        return Tx(s, e, "Unknown ordered set", True,
                  _err("unknown_os", [("type", "unknown_os"), ("symbol", h2(u.info["symbol"]))]))
    if k == "xz":
        s, e = tm.edge_span(u.a, u.b)
        return Tx(s, e, "X/Z on data", True,
                  _err("unknown_value", [("type", "unknown_value"), ("edges", u.b - u.a + 1)]))
    return None


def dllp_tx(u: Unit, tm: Timing) -> Tx:
    d: Dllp = u.info["dllp"]
    label, typ, flds, is_err = d.describe()
    fields = [("type", typ)] + flds + [("crc", hexb(u.info["crc"]))]
    if u.info["crc"] != u.info["good_crc"]:
        label += " CRC mismatch"
        is_err = True
        # §10.7: sentences joined in label-suffix order (reserved, then CRC).
        text = ERROR_TEXT["dllp_crc"]
        if typ == "reserved":
            text = ERROR_TEXT["reserved"] + " " + text
        fields += [("expected_crc", hexb(u.info["good_crc"])), ("error", text)]
    elif typ == "reserved":
        fields += [("error", ERROR_TEXT["reserved"])]
    s, e = tm.span(u.a, u.b)
    return Tx(s, e, label, is_err, fields)


class SeqTracker:
    """SPEC §8.2 sequence tracking over good-LCRC, END-terminated TLPs."""

    def __init__(self) -> None:
        self.next: int | None = None

    def classify(self, seq: int) -> tuple[str, int | None]:
        if self.next is None:
            self.next = (seq + 1) % 4096
            return "normal", None
        if seq == self.next:
            self.next = (seq + 1) % 4096
            return "normal", None
        behind = (self.next - seq) % 4096
        if 1 <= behind <= 2048:
            return "replay", None
        exp = self.next
        self.next = (seq + 1) % 4096
        return "error", exp


def tlp_tx(u: Unit, tm: Timing, seqs: SeqTracker) -> Tx:
    t: Tlp = u.info["tlp"]
    s, e = tm.span(u.a, u.b)
    nbytes = u.info["nbytes"]
    body_len = nbytes + 6
    assert body_len >= 18 and nbytes % 4 == 0, "use raw_tlp_bytes for framing-length errors"
    end = u.info["end"]
    lcrc_s = hexb(u.info["lcrc"])
    base = [("seq", t.seq), ("tlp", t.name), ("length_dw", t.length_dw), ("tc", t.tc),
            ("td", t.td), ("ep", t.ep), ("bytes", nbytes), ("lcrc", lcrc_s)]
    if end == EDB:
        if u.info["lcrc_mode"] == "invert":
            return Tx(s, e, f"Nullified TLP seq={t.seq} {t.name}", False,
                      [("type", "tlp_nullified")] + base)
        return Tx(s, e, f"TLP ended by EDB seq={t.seq}", True,
                  [("type", "tlp_edb"), ("seq", t.seq), ("bytes", nbytes),
                   ("error", ERROR_TEXT["tlp_edb"])])
    hdr = 4 * t.hdr_dw() + (4 if t.td else 0)
    if t.has_data():
        length_ok = nbytes - hdr == 4 * t.length_dw
    else:
        length_ok = nbytes == hdr
    lcrc_ok = u.info["lcrc"] == u.info["good_lcrc"]
    # SPEC §8.2 suffix order; §10.1 one joined `error`, gained fields first.
    label = f"TLP seq={t.seq} {t.name} len={t.length_dw}"
    replay = False
    extra: list = []
    sentences: list = []
    if not lcrc_ok:
        label += " LCRC mismatch"
        extra.append(("expected_lcrc", hexb(u.info["good_lcrc"])))
        sentences.append(ERROR_TEXT["tlp_lcrc"])
    if not length_ok:
        label += " length mismatch"
        sentences.append(ERROR_TEXT["tlp_length_mismatch"])
    if lcrc_ok:
        cls, exp = seqs.classify(t.seq)
        if cls == "replay":
            label += " (replay)"
            replay = True
        elif cls == "error":
            label += " sequence error"
            extra.append(("expected_seq", exp))
            sentences.append(ERROR_TEXT["tlp_sequence"])
    if sentences:
        extra.append(("error", " ".join(sentences)))
    return Tx(s, e, label, bool(sentences), [("type", "tlp")] + base + [("replay", replay)] + extra)


def ts_tx(run: dict, tm: Timing, coalesce: bool) -> Tx:
    u: Unit = run["first"]
    i = u.info
    count = run["count"]
    link = "PAD" if i["link"] is None else str(i["link"])
    lane = "PAD" if i["lane"] is None else str(i["lane"])
    gen = "gen2" if i["rate"] & 0x04 else "gen1"
    name = "TS1" if i["kind"] == "ts1" else "TS2"
    mult = f" ×{count}" if count > 1 else ""
    label = f"{name}{mult} link={link} lane={lane} n_fts={i['n_fts']} {gen}"
    c = i["ctrl"]
    fields = [("type", i["kind"]), ("count", count), ("link", link), ("lane", lane),
              ("n_fts", i["n_fts"]), ("rate_id", h2(i["rate"])), ("training_control", h2(c)),
              ("hot_reset", bool(c & 1)), ("disable_link", bool(c & 2)),
              ("loopback", bool(c & 4)), ("disable_scrambling", bool(c & 8)),
              ("compliance_receive", bool(c & 16))]
    s, e = tm.span(run["a"], run["b"])
    return Tx(s, e, label, False, fields)


def run_tx(run: dict, tm: Timing, coalesce: bool) -> Tx:
    kind = run["kind"]
    if kind == "ts":
        return ts_tx(run, tm, coalesce)
    s, e = tm.span(run["a"], run["b"])
    count = run["count"]
    name = "FTS" if kind == "fts" else "EIEOS"
    label = f"{name} ×{count}" if count > 1 else name   # §10.3
    return Tx(s, e, label, False, [("type", kind), ("count", count)])


def pipe_engine(st: Stream, tm: Timing, coalesce: bool, show_idle: bool) -> list[Tx]:
    out: list[Tx] = []
    run: dict | None = None
    idle: dict | None = None

    def end_run() -> None:
        nonlocal run
        if run is not None:
            out.append(run_tx(run, tm, coalesce))
            run = None

    def end_idle() -> None:
        nonlocal idle
        if idle is not None:
            s, e = tm.span(idle["a"], idle["b"])
            label = f"Idle ×{idle['n']}" if idle["n"] > 1 else "Idle"
            out.append(Tx(s, e, label, False,
                          [("type", "idle"), ("symbols", idle["n"])]))
            idle = None

    for u in st.units:
        if u.kind == "idle":
            end_run()
            if show_idle:
                if idle is not None and idle["b"] + 1 == u.a:
                    idle["b"] = u.b
                    idle["n"] += u.info["n"]
                else:
                    end_idle()
                    idle = dict(a=u.a, b=u.b, n=u.info["n"])
            continue
        end_idle()
        if u.kind in ("ts", "fts", "eieos"):
            key = (u.kind, u.info.get("kind"), u.info.get("key"))
            if coalesce and run is not None and run["key"] == key and run["b"] + 1 == u.a:
                run["b"] = u.b
                run["count"] += 1
                continue
            end_run()
            run = dict(kind=u.kind, key=key, a=u.a, b=u.b, count=1, first=u)
            continue
        end_run()
        if u.kind == "skp":
            n = u.info["n"]
            s, e = tm.span(u.a, u.b)
            out.append(Tx(s, e, "SKP" if n == 3 else f"SKP ×{n}", False,
                          [("type", "skp"), ("skp_symbols", n)]))
        elif u.kind == "eios":
            s, e = tm.span(u.a, u.b)
            out.append(Tx(s, e, "EIOS", False, [("type", "eios")]))
        elif u.kind in ("dllp", "dllp_badlen") and u.info["end"] == EDB:
            out.append(common_error_tx(Unit("dllp_edb", u.a, u.b, u.info), tm))
        elif u.kind in ("dllp", "dllp_badlen"):
            s, e = tm.span(u.a, u.b)
            n = len(u.info["body"])
            assert n >= 2
            out.append(Tx(s, e, f"DLLP frame ({n} symbols)", False,
                          [("type", "dllp_frame"), ("symbols", n), ("end", "END")]))
        elif u.kind in ("tlp", "tlp_badlen"):
            s, e = tm.span(u.a, u.b)
            body = u.info["body"]
            n = len(body)
            assert n >= 2
            seq = ((body[0] & 0x0F) << 8) | body[1]
            endn = "END" if u.info["end"] == END else "EDB"
            tail = "" if endn == "END" else ", EDB"
            out.append(Tx(s, e, f"TLP frame seq={seq} ({n} symbols{tail})", False,
                          [("type", "tlp_frame"), ("seq", seq), ("symbols", n), ("end", endn)]))
        elif u.kind in ("pre_com", "dropped", "partial"):
            pass
        else:
            tx = common_error_tx(u, tm)
            assert tx is not None, u.kind
            out.append(tx)
    end_run()
    end_idle()
    return out


def dll_engine(st: Stream, tm: Timing) -> list[Tx]:
    out: list[Tx] = []
    seqs = SeqTracker()
    for u in st.units:
        if u.kind in ("ts", "skp", "fts", "eios", "eieos", "idle", "pre_com", "dropped", "partial"):
            continue
        if u.kind in ("dllp", "dllp_badlen") and u.info["end"] == EDB:
            out.append(common_error_tx(Unit("dllp_edb", u.a, u.b, u.info), tm))
        elif u.kind == "dllp":
            out.append(dllp_tx(u, tm))
        elif u.kind == "dllp_badlen":
            n = len(u.info["body"])
            assert n != 6
            s, e = tm.span(u.a, u.b)
            out.append(Tx(s, e, f"DLLP length {n}, expected 6", True,
                          _err("dllp_length", [("type", "dllp_length"), ("symbols", n)])))
        elif u.kind == "tlp":
            out.append(tlp_tx(u, tm, seqs))
        elif u.kind == "tlp_badlen":
            n = len(u.info["body"])
            assert n < 18 or (n - 6) % 4 != 0
            s, e = tm.span(u.a, u.b)
            out.append(Tx(s, e, f"TLP length {n} symbols is not valid", True,
                          _err("tlp_length", [("type", "tlp_length"), ("symbols", n)])))
        else:
            tx = common_error_tx(u, tm)
            assert tx is not None, u.kind
            out.append(tx)
    return out


RXSTATUS = {1: ("SKP added", False), 2: ("SKP removed", False), 3: ("receiver detected", False),
            4: ("8b/10b decode error", True), 5: ("elastic buffer overflow", True),
            6: ("elastic buffer underflow", True), 7: ("disparity error", True)}


def rxstatus_txs(tm: Timing) -> list[Tx]:
    out = []
    prev = 0
    for ei, e in enumerate(tm.edges):
        code = e.rxstatus
        gated = e.kind == "gate"
        if not gated and code != 0 and code != prev:
            name, err = RXSTATUS[code]
            s, en = tm.edge_span(ei, ei)
            f = [("type", "rxstatus"), ("code", "0b" + format(code, "03b"))]
            if err:
                f.append(("error", ERROR_TEXT[f"rxstatus_{code}"]))
            out.append(Tx(s, en, f"RxStatus: {name}", err, f))
        prev = code
    return out


def view_byte(s: Sym, view: str) -> int | None:
    """What a receiver in `view` mode sees for this symbol (SPEC §5.2): used
    to assert which pipeline a CRC check passes in, never to build labels."""
    if s.k or view == "off" or s.ts:
        return s.wire
    if s.before_com:
        return None
    return s.wire ^ s.key


def packet_valid(st: Stream, u: Unit, view: str) -> bool:
    if u.kind not in ("dllp", "tlp", "dllp_badlen", "tlp_badlen"):
        return False
    if u.info["end"] != END:
        return False
    body = [view_byte(st.syms[i], view) for i in range(u.a + 1, u.b)]
    if any(b is None for b in body):
        return False
    body = bytes(body)
    if u.kind.startswith("dllp"):
        return len(body) == 6 and dllp_crc_wire(body[:4]) == body[4:]
    return len(body) >= 6 and lcrc_wire(body[:-4]) == body[-4:]


def check_view(st: Stream, view: str) -> None:
    """The engine reads intents, so every symbol the winning pipeline sees must
    be the symbol the encoder meant."""
    for u in st.units:
        if u.kind == "pre_com":
            assert view == "on", "D symbols before the first COM decode as data in off mode"
            continue
        if u.kind == "xz":
            continue
        for i in range(u.a, u.b + 1):
            s = st.syms[i]
            assert view_byte(s, view) == s.byte, (view, u.kind, i, s)


def unit_span(u: Unit, tm: Timing) -> tuple[int, int]:
    return tm.span(u.a, u.b)


def expected(st: Stream, edges: list[Edge], decoder: str, params: dict,
             rxstatus_bound: bool = False) -> list[Tx]:
    """SPEC §5.5: the winning pipeline's transactions plus the lock notice."""
    tm = Timing(st, edges)
    # X/Z units are edge ranges; add them now that edges exist.
    xz_units = []
    ei = 0
    while ei < len(edges):
        if edges[ei].kind == "xz":
            e2 = ei
            while e2 + 1 < len(edges) and edges[e2 + 1].kind == "xz":
                e2 += 1
            xz_units.append(Unit("xz", ei, e2))
            ei = e2 + 1
        else:
            ei += 1

    mode = params.get("scrambling", "auto")
    coalesce = params.get("coalesce", True)
    show_idle = params.get("show_idle", False)
    pipe = decoder.startswith("ferrite.pcie_pipe")

    def run(view: str) -> list[Tx]:
        check_view(st, view)
        txs = pipe_engine(st, tm, coalesce, show_idle) if pipe else dll_engine(st, tm)
        txs += [common_error_tx(u, tm) for u in xz_units]
        if pipe and rxstatus_bound:
            txs += rxstatus_txs(tm)
        return txs

    if mode in ("off", "on"):
        return run(mode)
    # auto
    lock = None
    for u in sorted(st.units, key=lambda x: x.b):
        if u.kind == "ts" and u.info["ctrl"] & 0x08:
            lock = ("ds", "off", u)
            break
        off_ok = packet_valid(st, u, "off")
        on_ok = packet_valid(st, u, "on")
        if off_ok or on_ok:
            lock = ("crc", "off" if off_ok else "on", u)
            break
    if lock is None:
        txs = run("off")
        if txs:
            last = txs[-1]
            s, e = last.start, last.end
        else:
            s, e = tm.span(len(st.syms) - 1, len(st.syms) - 1)
        return txs + [Tx(s, e, "Scrambling: off (undetermined)", False,
                         [("type", "scrambling_detected"), ("mode", "off")])]
    how, view, u = lock
    txs = run(view)
    if how == "ds":
        # §10.3: spans the first TS that carried Disable Scrambling.
        s, e = unit_span(u, tm)
        return txs + [Tx(s, e, "Scrambling: off (training sets)", False,
                         [("type", "scrambling_detected"), ("mode", "off")])]
    s, e = unit_span(u, tm)
    return txs + [Tx(s, e, f"Scrambling: {view} (detected)", False,
                     [("type", "scrambling_detected"), ("mode", view)])]


# ── 5. Canonical output (tools/wcxhost/src/output.c, dart.c) ─────────────────

def dart_string(s: str) -> str:
    """Dart jsonEncode string escaping (wcxh_dart_string)."""
    out = ['"']
    for ch in s:
        c = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\b":
            out.append("\\b")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\f":
            out.append("\\f")
        elif ch == "\r":
            out.append("\\r")
        elif c < 0x20:
            out.append(f"\\u{c:04x}")
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def canonical(txs: list[Tx], fpt: int) -> str:
    rows = []
    for t in txs:
        st = t.start // fpt
        en = -(-t.end // fpt)
        fields = "{" + ",".join(f"{dart_string(k)}:{dart_string(_s(v))}" for k, v in t.fields) + "}"
        rows.append((st, en, t.label, t.is_error, fields))
    rows.sort(key=lambda r: (r[0], r[1], r[2].encode(), r[3], r[4].encode()))
    lines = [f'{{"startTime":{a},"endTime":{b},"label":{dart_string(c)},'
             f'"isError":{"true" if d else "false"},"fields":{f}}}' for a, b, c, d, f in rows]
    if not lines:
        return "[\n]\n"
    return "[\n" + ",\n".join(lines) + "\n]\n"


def bindings_json(decoder: str, paths: dict, params: dict) -> str:
    obj = {"decoder": decoder, "signal_bindings": paths}
    if params:
        obj["parameters"] = params
    return json.dumps(obj, indent=2, ensure_ascii=False) + "\n"
