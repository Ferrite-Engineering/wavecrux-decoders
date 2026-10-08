#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Regenerates decoders/pcie/fixtures/generated/ (and checks captured/).

    python3 decoders/pcie/tools/generate_fixtures.py           # write
    python3 decoders/pcie/tools/generate_fixtures.py --check   # CI: fail on drift
    python3 decoders/pcie/tools/generate_fixtures.py --list    # what each proves

Every fixture is built by ENCODING protocol units into PIPE symbols; its
expected transactions are computed from those units by the rules of
decoders/pcie/SPEC.md (pcie_fixture_lib.py), never by decoding. Standard
library only and deterministic: no clock, and payload bytes come from a seeded
generator.
"""

from __future__ import annotations

import argparse
import os
import sys
import tempfile
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import pcie_fixture_lib as L  # noqa: E402
from pcie_fixture_lib import (COM, EDB, EIE, END, FTS, IDL, PAD, SDP, SKP, STP,  # noqa: E402
                              Clocking, Dllp, Stream, Tlp, VcdNames)

FIXTURES_DIR = os.path.join(os.path.dirname(HERE), "fixtures")
GENERATED = os.path.join(FIXTURES_DIR, "generated")
CAPTURED = os.path.join(FIXTURES_DIR, "captured")


@dataclass
class Fixture:
    name: str
    proves: str
    decoder: str
    lanes: int
    build: object
    clk: Clocking
    names: VcdNames = field(default_factory=VcdNames)
    params: dict = field(default_factory=dict)
    scrambled: bool = False


FIXTURES: list[Fixture] = []


def fixture(name: str, proves: str, kind: str, lanes: int, clk: Clocking, names: VcdNames | None = None,
            params: dict | None = None, scrambled: bool = False):
    def wrap(fn):
        FIXTURES.append(Fixture(name, proves, f"ferrite.pcie_{kind}_w{8 * lanes}", lanes, fn, clk,
                                names or VcdNames(), params or {}, scrambled))
        return fn
    return wrap


# ── building blocks ──────────────────────────────────────────────────────────

class Seq:
    def __init__(self, start: int = 0) -> None:
        self.v = start

    def __call__(self) -> int:
        v = self.v
        self.v = (self.v + 1) % 4096
        return v


ALL_DLLPS = [
    Dllp("ack", seq=12), Dllp("ack", seq=0), Dllp("ack", seq=4095), Dllp("nak", seq=5),
    Dllp("pm_enter_l1"), Dllp("pm_enter_l23"), Dllp("pm_as_request_l1"), Dllp("pm_request_ack"),
    Dllp("vendor", payload=0x123456), Dllp("vendor", payload=0x00ABCD),
    Dllp("initfc1_p", vc=0, hdr=32, data=1008), Dllp("initfc1_np", vc=0, hdr=32, data=1),
    Dllp("initfc1_cpl", vc=0, hdr=0, data=0), Dllp("initfc2_p", vc=1, hdr=255, data=4095),
    Dllp("initfc2_np", vc=2, hdr=1, data=2), Dllp("initfc2_cpl", vc=7, hdr=128, data=2048),
    Dllp("updatefc_p", vc=0, hdr=10, data=200), Dllp("updatefc_np", vc=3, hdr=3, data=0),
    Dllp("updatefc_cpl", vc=0, hdr=64, data=768),
    Dllp("reserved", byte0=0x01), Dllp("reserved", byte0=0x31), Dllp("reserved", byte0=0x48),
    Dllp("reserved", byte0=0xFF, payload=0x010203),
]


def tlp_catalogue(seq: Seq) -> list[Tlp]:
    """Every SPEC §8.2 name, plus TD/EP/TC variety and two unknown types."""
    return [
        Tlp("MRd32", seq(), length_dw=1, rng_seed=11),
        Tlp("MRd64", seq(), length_dw=4, tc=2, rng_seed=12),
        Tlp("MRdLk32", seq(), length_dw=1, rng_seed=13),
        Tlp("MRdLk64", seq(), length_dw=2, rng_seed=14),
        Tlp("MWr32", seq(), length_dw=1, rng_seed=15),
        Tlp("MWr64", seq(), length_dw=8, tc=7, rng_seed=16),
        Tlp("MWr32", seq(), length_dw=2, td=True, rng_seed=17),          # TD=1, ECRC
        Tlp("MWr32", seq(), length_dw=1, ep=True, tc=5, rng_seed=18),
        Tlp("IORd", seq(), length_dw=1, rng_seed=19),
        Tlp("IOWr", seq(), length_dw=1, rng_seed=20),
        Tlp("CfgRd0", seq(), length_dw=1, rng_seed=21),
        Tlp("CfgWr0", seq(), length_dw=1, rng_seed=22),
        Tlp("CfgRd1", seq(), length_dw=1, rng_seed=23),
        Tlp("CfgWr1", seq(), length_dw=1, td=True, rng_seed=24),
        Tlp("Msg", seq(), length_dw=1024, msg_routing=4, rng_seed=25),    # Length 0 -> 1024
        Tlp("MsgD", seq(), length_dw=1, msg_routing=0, rng_seed=26),
        Tlp("Cpl", seq(), length_dw=1, rng_seed=27),
        Tlp("CplD", seq(), length_dw=3, rng_seed=28),
        Tlp("CplLk", seq(), length_dw=1, rng_seed=29),
        Tlp("CplDLk", seq(), length_dw=1, rng_seed=30),
        Tlp("MRd64", seq(), length_dw=1, td=True, rng_seed=31),
        Tlp("Type 0x7F", seq(), length_dw=1, byte0=0x7F, rng_seed=32),     # Fmt 011, Type 11111
        Tlp("Type 0x90", seq(), length_dw=1, byte0=0x90, rng_seed=33),     # Fmt 100 (prefix)
        Tlp("Type 0x18", seq(), length_dw=1, byte0=0x18, rng_seed=34),     # Fmt 000, Type 11000
    ]


def to_lane(st: Stream, lane: int) -> None:
    """Idle symbols so that the next unit starts at byte lane `lane`."""
    n = (lane - st.pos()) % st.n
    if n:
        st.idle(n)


def end_with(st: Stream, size: int, min_idle: int = 0) -> None:
    """Idle so that a final unit of `size` symbols ends on a word boundary;
    with show_idle the idle run must be 0 or >= 2 symbols."""
    n = (-(st.pos() + size)) % st.n
    while 0 < n < min_idle:
        n += st.n
    if n:
        st.idle(n)


def walk_lanes(st: Stream, seq: Seq | None, rng0: int = 100) -> None:
    """For every starting byte lane: DLLP, TLP, DLLP back to back (no idle
    between them)."""
    seq = seq or Seq(0)
    for lane in range(st.n):
        to_lane(st, lane)
        st.dllp(Dllp("ack", seq=lane))
        st.tlp(Tlp("MWr32", seq(), length_dw=1 + (lane % 3), rng_seed=rng0 + lane))
        st.dllp(Dllp("updatefc_p", vc=0, hdr=lane + 1, data=8 * lane + 3))
        st.tlp(Tlp("MRd64", seq(), length_dw=2, rng_seed=rng0 + 50 + lane))


# ── PIPE decoder fixtures ────────────────────────────────────────────────────

# TS1/TS2 decode (PAD/PAD, link set, lane set), coalescing into ×N runs, runs ended by a SKP and by a content change.
@fixture("pipe_training_coalesce_w8_1ns", "TS1/TS2 fields and coalescing; runs end at SKP and at a changed TS",
         "pipe", 1, Clocking((1, "ns"), period=8), params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.ts("ts1", None, None, n_fts=24, count=4)
    st.ts("ts1", 0, None, n_fts=24, count=2)
    st.ts("ts1", 0, 0, n_fts=24, count=2)
    st.ts("ts2", 0, 0, n_fts=24, count=8)
    st.skp()
    st.ts("ts2", 0, 0, n_fts=24, count=2)
    st.ts("ts1", 0, 0, n_fts=40, count=1)
    st.ts("ts1", 0, 0, n_fts=24, count=1)
    st.ts("ts1", 255, 31, n_fts=0, count=3)
    st.skp()


# coalesce=false: every TS/FTS/EIEOS is its own transaction, count 1, no ×1; data changes mid-cycle.
@fixture("pipe_training_nocoalesce_w16_1ps", "coalesce=false: one transaction per ordered set, count 1, no ×1",
         "pipe", 2, Clocking((1, "ps"), period=4000, change="delay", delay=1300),
         params={"scrambling": "off", "coalesce": False})
def _(st: Stream) -> None:
    st.ts("ts1", None, None, n_fts=24, count=3)
    st.ts("ts2", 3, 0, n_fts=128, rate=0x06, count=2)
    st.fts(3)
    st.eios()
    st.eieos(2)
    st.skp()
    st.ts("ts1", 3, 0, n_fts=128, rate=0x06, ctrl=0x01, count=1)
    st.skp(2)
    st.pad_idle()


# SKP OS with 1-5 SKPs, FTS ×N, EIOS, EIEOS ×N, gen2 rate bits, every training-control bit; 10 ps timescale.
@fixture("pipe_ordered_sets_w32_10ps", "SKP x1..x5, FTS/EIEOS runs, EIOS, gen1/gen2 rate ids, every training-control bit",
         "pipe", 4, Clocking((10, "ps"), period=1600, change="delay", delay=400), params={"scrambling": "off"})
def _(st: Stream) -> None:
    for n in (1, 2, 3, 4, 5):
        st.skp(n)
    st.fts(4)
    st.skp()
    st.ts("ts1", 7, 0, n_fts=255, rate=0x06, count=2)
    st.ts("ts1", 7, 0, n_fts=255, rate=0x86, count=1)
    st.ts("ts2", 7, 0, n_fts=255, rate=0x06, ctrl=0x01)
    st.ts("ts1", 7, 0, n_fts=255, rate=0x02, ctrl=0x02)
    st.ts("ts1", 7, 0, n_fts=255, rate=0x02, ctrl=0x04)
    st.ts("ts1", 7, 0, n_fts=255, rate=0x02, ctrl=0x08)
    st.ts("ts1", 7, 0, n_fts=255, rate=0x02, ctrl=0x10)
    st.ts("ts2", None, None, n_fts=1, rate=0x02, ctrl=0x1F)
    st.eios()
    st.eios()
    st.eieos(3)
    st.fts(2)
    st.pad_idle()


# show_idle=true: idle runs (one starting on the very first edge, where P_1 = 0), broken by SKP and packets; odd period at 100 fs.
@fixture("pipe_idle_show_w64_100fs", "show_idle=true idle runs, first-edge zero-duration symbols, floor(j*P/N) at an odd period",
         "pipe", 8, Clocking((100, "fs"), period=4001), params={"scrambling": "off", "show_idle": True})
def _(st: Stream) -> None:
    st.idle(11)
    st.skp()
    st.idle(5)
    st.dllp(Dllp("ack", seq=1))
    st.idle(2)
    st.tlp(Tlp("MRd32", 0, length_dw=1, rng_seed=5))
    st.idle(9)
    st.skp(2)
    st.idle(3)
    st.ts("ts1", 0, 0, count=2)
    st.idle(4)
    st.dllp(Dllp("updatefc_np", hdr=4, data=40))
    end_with(st, 4, min_idle=2)
    st.eios()


# DLLP and TLP frames starting at every byte lane of a 64-bit word, back to back; EDB frames; frames of invalid length.
@fixture("pipe_frames_lanes_w64_1ns", "frames at every byte lane, back to back; EDB frames; PIPE decoder does not check CRC or length",
         "pipe", 8, Clocking((1, "ns"), period=4), params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.skp()
    walk_lanes(st, Seq(10))
    st.tlp(Tlp("MWr32", 30, length_dw=1, rng_seed=7), lcrc="invert", end=EDB)
    st.tlp(Tlp("MWr32", 31, length_dw=1, rng_seed=8), lcrc="bad", end=EDB)
    st.tlp(Tlp("MWr32", 32, length_dw=1, rng_seed=9), lcrc="bad")
    st.dllp(Dllp("ack", seq=7), crc=b"\x00\x00")
    st.raw_dllp_bytes(bytes([0x00, 0x00, 0x00, 0x01, 0x02]))
    st.raw_tlp_bytes(bytes(range(1, 18)))
    st.pad_idle()


# Same frames at 32 bits with data changing between edges (testbench-with-delays style).
@fixture("pipe_frames_lanes_w32_delayed_1ps", "frames at every lane of a 32-bit word, data changing mid-cycle",
         "pipe", 4, Clocking((1, "ps"), period=16000, change="delay", delay=3000), params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.skp()
    walk_lanes(st, Seq(4090))
    st.pad_idle()


# sample_point=at_edge with data changing on the clock edge: each word is captured at the edge it changes on.
@fixture("pipe_at_edge_w16_1ns", "sample_point=at_edge reads the sample in which pclk rose",
         "pipe", 2, Clocking((1, "ns"), period=8, sample_point="at_edge"),
         params={"scrambling": "off", "sample_point": "at_edge"})
def _(st: Stream) -> None:
    st.ts("ts1", None, None, count=2)
    st.skp()
    st.idle(3)
    st.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    st.tlp(Tlp("CfgRd0", 1, rng_seed=3))
    st.idle(1)
    st.dllp(Dllp("ack", seq=1))
    st.pad_idle()


# scrambling=on: D symbols before the first COM discarded; TS symbols unscrambled; SKP does not advance the LFSR; descrambled idle shown.
@fixture("pipe_scrambled_on_w16_1ps", "scrambling=on: pre-COM data discarded, TS bypass, SKP inside idle, descrambled idle runs",
         "pipe", 2, Clocking((1, "ps"), period=8000), params={"scrambling": "on", "show_idle": True}, scrambled=True)
def _(st: Stream) -> None:
    st.pre_com(bytes([0x12, 0x34, 0x00, 0x56]))
    st.skp()
    st.ts("ts1", 0, 0, count=2)
    st.idle(6)
    st.skp()
    st.idle(4)
    st.dllp(Dllp("ack", seq=3))
    st.idle(2)
    st.tlp(Tlp("MWr32", 4, length_dw=2, rng_seed=44))
    st.idle(3)
    st.stray(bytes([0x11, 0x22, 0x33]))
    st.idle(3)
    st.skp(1)
    st.idle(2)
    st.dllp(Dllp("updatefc_cpl", hdr=2, data=64))
    end_with(st, 4, min_idle=2)
    st.eios()


# auto: a scrambled stream locks to on at its first valid DLLP; the earlier ordered sets come from the on buffer.
@fixture("pipe_auto_on_w32_1ns", "auto locks to on at the first packet valid only when descrambled",
         "pipe", 4, Clocking((1, "ns"), period=16), scrambled=True)
def _(st: Stream) -> None:
    st.skp()
    st.ts("ts1", 0, 0, count=4)
    st.ts("ts2", 0, 0, count=2)
    st.idle(8)
    st.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    st.idle(4)
    st.dllp(Dllp("initfc1_np", hdr=32, data=1))
    st.tlp(Tlp("MRd32", 0, rng_seed=9))
    st.idle(4)
    st.skp()
    st.pad_idle()


# auto on a plain stream: locks to off at the first valid TLP; a later packet valid only when descrambled changes nothing.
@fixture("pipe_auto_off_w8_1ps", "auto locks to off at the first valid packet (a TLP), then runs off only",
         "pipe", 1, Clocking((1, "ps"), period=4000))
def _(st: Stream) -> None:
    st.skp()
    st.ts("ts1", 0, 0, count=2)
    st.idle(3)
    st.dllp(Dllp("ack", seq=1), crc=b"\x12\x34")
    st.tlp(Tlp("MWr32", 0, rng_seed=2))
    st.idle(2)
    st.contrived_dllp(Dllp("ack", seq=0))
    st.dllp(Dllp("ack", seq=0))
    st.skp()


# auto: a TS1 with Disable Scrambling locks to off before any packet (notice spans that TS), though the next DLLP is valid only descrambled.
@fixture("pipe_auto_ds_w16_10ps", "auto: Training Control bit 3 (Disable Scrambling) locks to off: 'Scrambling: off (training sets)'",
         "pipe", 2, Clocking((10, "ps"), period=800))
def _(st: Stream) -> None:
    st.ts("ts1", 0, 0, ctrl=0x08, count=2)
    st.contrived_dllp(Dllp("ack", seq=9))
    st.skp()
    st.idle(2)
    st.dllp(Dllp("ack", seq=10))
    st.pad_idle()


# auto with no valid packet anywhere: off (undetermined) at flush, spanning the last buffered transaction (an EIOS).
@fixture("pipe_auto_undetermined_w8_1ns", "auto never locks without a valid packet: off (undetermined) spans the last transaction",
         "pipe", 1, Clocking((1, "ns"), period=4))
def _(st: Stream) -> None:
    st.ts("ts1", None, None, count=2)
    st.skp()
    st.idle(2)
    st.dllp(Dllp("ack", seq=2), crc=b"\xAA\xBB")
    st.tlp(Tlp("MRd32", 1, rng_seed=6), lcrc="bad")
    st.idle(2)
    st.eios()
    st.idle(3)


# Errors common to both decoders, seen by the PIPE decoder.
@fixture("pipe_errors_w8_1ns", "invalid K, truncation by STP/SDP/COM, END/EDB without start, K in packet, stray data, unknown OS",
         "pipe", 1, Clocking((1, "ns"), period=8), params={"scrambling": "off"})
def _(st: Stream) -> None:
    common_errors(st)


def common_errors(st: Stream) -> None:
    st.skp()
    st.idle(2)
    st.invalid_k(0x9C)
    st.idle(1)
    st.invalid_k(0xDC)
    st.truncated("tlp", bytes([0x00, 0x05, 0x40, 0x00]), by="STP")
    st.tlp(Tlp("MWr32", 5, rng_seed=51))
    st.truncated("dllp", bytes([0x00, 0x00]), by="SDP")
    st.dllp(Dllp("ack", seq=5))
    st.truncated("tlp", bytes([0x00, 0x06, 0x00, 0x00, 0x00, 0x01]), by="COM")
    st.skp()
    st.idle(2)
    st.unmatched_end(END)
    st.idle(1)
    st.unmatched_end(EDB)
    st.k_in_packet("dllp", bytes([0x00, 0x00]), PAD)
    st.dllp(Dllp("ack", seq=6))
    st.k_in_packet("tlp", bytes([0x00, 0x07, 0x00]), SKP)
    st.idle(2)
    st.stray(bytes([0x12, 0x34, 0x56]))
    st.idle(2)
    st.stray(bytes([0xA5]))
    st.dllp(Dllp("ack", seq=7))
    st.ts("ts1", 0, 0, sym6=0x00)
    st.idle(2)
    st.os_misfit([FTS, FTS], 0x00)
    st.idle(3)
    st.truncated("dllp", b"", by="SDP")
    st.dllp(Dllp("nak", seq=6))
    st.skp()
    st.pad_idle()


# X/Z on data or datak: one error per run of edges; an open packet is dropped silently.
@fixture("pipe_xz_w16_1ps", "X/Z runs (whole bus, one bit, datak only) and the silent drop of an open packet",
         "pipe", 2, Clocking((1, "ps"), period=4000), params={"scrambling": "off"})
def _(st: Stream) -> None:
    xz_body(st)


def xz_body(st: Stream) -> None:
    w = 8 * st.n
    st.skp()
    st.dllp(Dllp("ack", seq=1))
    st.pad_idle()
    to_lane(st, st.n - 3 if st.n >= 3 else 0)
    st.dropped("tlp", bytes([0x00, 0x01, 0x40])[: (-(st.pos() + 1)) % st.n or st.n])
    assert st.aligned()
    st.xz(3, data="x")
    st.idle(st.n * 2)
    st.dllp(Dllp("ack", seq=2))
    st.pad_idle()
    st.xz(1, data=0, datak="z")
    st.idle(st.n)
    st.xz(2, data=["0" * (w - 4) + "x000", "z" + "1" * (w - 1)])
    st.dllp(Dllp("ack", seq=3))
    st.pad_idle()


# rxvalid gating: gated edges contribute no symbols; an open TLP is truncated by rxvalid.
@fixture("pipe_gating_rxvalid_w32_1ns", "rxvalid=0 edges are ignored; an open TLP is reported truncated by rxvalid",
         "pipe", 4, Clocking((1, "ns"), period=16), names=VcdNames(rxvalid="rx_valid"),
         params={"scrambling": "off"})
def _(st: Stream) -> None:
    gating_body(st, "rxvalid")


def gating_body(st: Stream, sig: str) -> None:
    n = st.n
    st.skp()
    st.idle(4)
    st.dllp(Dllp("ack", seq=1))
    st.pad_idle()
    junk = [(0xFDFDFDFD_FDFDFDFD & ((1 << (8 * n)) - 1), (1 << n) - 1),
            (0x5C5C5C5C_5C5C5C5C & ((1 << (8 * n)) - 1), (1 << n) - 1)]
    st.gate(sig, 2, bus=junk)
    st.dllp(Dllp("ack", seq=2))
    t = Tlp("MWr32", 9, length_dw=4, rng_seed=77).bytes()
    body = bytes([0x00, 0x09]) + t
    cut = (-(st.pos() + 1)) % n + n          # STP + cut bytes end on a word boundary
    st.truncated("tlp", body[:cut], by=sig)
    mask = (1 << (8 * n)) - 1
    st.gate(sig, 3, bus=[(0x11223344 & mask, 0), (0, (1 << n) - 1), (0xFDFDFDFD_FDFDFDFD & mask, (1 << n) - 1)])
    st.idle(2)
    st.dllp(Dllp("ack", seq=3))
    st.pad_idle()


# txelecidle gating after an EIOS, with a DLLP truncated by txelecidle; 10 ps timescale.
@fixture("pipe_gating_txelecidle_w8_10ps", "txelecidle=1 edges are ignored; an open DLLP is truncated by txelecidle",
         "pipe", 1, Clocking((10, "ps"), period=400), names=VcdNames(data="tx_data", datak="tx_datak",
                                                                    txelecidle="tx_elec_idle"),
         params={"scrambling": "off"})
def _(st: Stream) -> None:
    txelecidle_body(st)


def txelecidle_body(st: Stream) -> None:
    st.skp()
    st.dllp(Dllp("ack", seq=4))
    st.eios()
    st.gate("txelecidle", 6, bus=[(0, 0)] * 6)
    st.fts(2)
    st.skp()
    st.idle(2)
    st.truncated("dllp", bytes([0x00, 0x00, 0x00]), by="txelecidle")
    st.gate("txelecidle", 3, bus=[(0x04, 0), (0xFD, 1), (0x00, 0)])
    st.idle(2)
    st.dllp(Dllp("ack", seq=5))
    st.skp()


# RxStatus codes 001-111 (non-zero and changed from the previous edge), with repeats that must not re-report.
@fixture("pipe_rxstatus_w16_1ns", "RxStatus 001-111 labels and is_error; equal consecutive codes report once",
         "pipe", 2, Clocking((1, "ns"), period=8, change="delay", delay=2),
         names=VcdNames(rxstatus="rx_status"), params={"scrambling": "off"})
def _(st: Stream) -> None:
    rxstatus_body(st)


def rxstatus_body(st: Stream) -> None:
    st.skp()
    st.idle(2)
    for code, hold in ((1, 2), (0, 1), (2, 1), (3, 3), (4, 1), (0, 2), (4, 1), (5, 1), (6, 1),
                       (7, 2), (1, 1), (0, 1)):
        st.rxstatus(code)
        st.idle(2 * hold)
    st.dllp(Dllp("ack", seq=1))
    st.rxstatus(0)
    st.skp()


# A Gen1 -> Gen2 speed change halves the PIPE clock period: P_k is the measured interval; odd periods at 100 fs.
@fixture("pipe_rate_change_w64_100fs", "symbol times follow the measured period across a 2:1 clock change (odd fs periods)",
         "pipe", 8, Clocking((100, "fs"), period_fn=lambda k: 40001 if k < 9 else 20001, first_edge=3),
         params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.skp()
    st.ts("ts1", 0, 0, rate=0x06, count=2)
    st.ts("ts2", 0, 0, rate=0x86, count=2)
    st.idle(4)
    st.eios()
    st.ts("ts1", 0, 0, rate=0x06, count=2)        # straddles the period change
    st.dllp(Dllp("updatefc_p", hdr=1, data=1))
    st.tlp(Tlp("MRd32", 0, rng_seed=1))
    st.skp()
    st.pad_idle()


# ── DLL decoder fixtures ─────────────────────────────────────────────────────

# Every DLLP type (Ack, Nak, PM x4, vendor, InitFC1/2 and UpdateFC x P/NP/Cpl, reserved) under default auto.
@fixture("dll_dllp_types_w8_1ns", "every DLLP type incl. vendor and reserved; default auto locks off at the first DLLP",
         "dll", 1, Clocking((1, "ns"), period=4))
def _(st: Stream) -> None:
    st.skp()
    for d in ALL_DLLPS:
        st.dllp(d)
        st.idle(1)
    st.skp()


# DLLP CRC mismatches (label suffix, expected_crc, error) and wrong DLLP lengths; data changing mid-cycle.
@fixture("dll_dllp_errors_w16_1ps", "DLLP CRC mismatch fields and DLLP length errors",
         "dll", 2, Clocking((1, "ps"), period=8000, change="delay", delay=2500), params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.skp()
    st.dllp(Dllp("ack", seq=12), crc=b"\x00\x00")
    st.dllp(Dllp("initfc1_p", hdr=32, data=1008), crc=b"\x35\xBD")
    st.dllp(Dllp("vendor", payload=0x123456), crc=b"\xFF\xFF")
    st.dllp(Dllp("nak", seq=5))
    st.raw_dllp_bytes(bytes([0x00, 0x00, 0x00, 0x05, 0x12]))
    st.raw_dllp_bytes(bytes([0x00, 0x00, 0x00, 0x05, 0x12, 0x34, 0x56]))
    st.raw_dllp_bytes(bytes([0x10]))
    st.dllp(Dllp("pm_request_ack"), crc=b"\x01\x02")
    st.dllp(Dllp("ack", seq=13))
    st.pad_idle()


# Every TLP name in SPEC §8.2 plus TD=1/ECRC, EP, TC, Length 0 = 1024 and unknown types; sequence increments.
@fixture("dll_tlp_types_w32_1ns", "TLP names, header fields, TD/ECRC, Length 0 = 1024, unknown types",
         "dll", 4, Clocking((1, "ns"), period=16), params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.skp()
    seq = Seq(0)
    for i, t in enumerate(tlp_catalogue(seq)):
        st.tlp(t)
        if i % 3 == 0:
            st.dllp(Dllp("ack", seq=t.seq))
        st.idle(i % 4)
    st.pad_idle()


# TLP error paths: length mismatch, invalid framing length, nullified, EDB with bad LCRC, LCRC mismatch; 10 ps.
@fixture("dll_tlp_errors_w64_10ps", "length mismatch, tlp_length, nullified TLP, EDB with bad LCRC, LCRC mismatch",
         "dll", 8, Clocking((10, "ps"), period=400), params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.skp()
    st.tlp(Tlp("MWr32", 0, length_dw=2, rng_seed=1))
    st.tlp(Tlp("MWr32", 1, length_dw=4, payload_dw=2, rng_seed=2))          # too little data
    st.tlp(Tlp("MRd32", 2, length_dw=1, payload_dw=1, rng_seed=3))          # data on a read
    st.tlp(Tlp("MWr64", 3, length_dw=1, td=True, payload_dw=3, rng_seed=4))  # TD and short
    st.raw_tlp_bytes(bytes([0x00, 0x04]) + bytes(8) + bytes(4))             # 14 < 18
    st.raw_tlp_bytes(bytes([0x00, 0x04]) + bytes(11) + bytes(4))            # 17: % 4
    st.raw_tlp_bytes(bytes([0x00, 0x04]) + bytes(17) + bytes(4))            # 23: % 4
    st.tlp(Tlp("MWr32", 4, rng_seed=5), lcrc="invert", end=EDB)               # nullified
    st.tlp(Tlp("CplD", 4, length_dw=2, rng_seed=6), lcrc="bad", end=EDB)     # EDB, bad LCRC
    st.tlp(Tlp("CplD", 4, length_dw=2, rng_seed=6), lcrc="good", end=EDB)    # EDB, good LCRC
    st.tlp(Tlp("MRd64", 4, rng_seed=7), lcrc="bad")                          # LCRC mismatch
    st.tlp(Tlp("MRd64", 4, rng_seed=7))
    st.pad_idle()


# Sequence tracking: increment, wrap 4095 -> 0, replay 1 and exactly 2048 behind, error 2049 behind and ahead.
@fixture("dll_sequence_w16_1ns", "sequence tracking: wrap, replay window edge (2048 / 2049), ahead, untracked TLPs",
         "dll", 2, Clocking((1, "ns"), period=8), params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.skp()
    plan = [4094, 4095, 0, 1, 1, 2, 2051, 2050, 2051, 2060, 2061]
    for i, s in enumerate(plan):
        st.tlp(Tlp("MWr32", s, rng_seed=200 + i))
        if i % 4 == 3:
            st.dllp(Dllp("ack", seq=s))
    st.tlp(Tlp("MWr32", 5, rng_seed=300), lcrc="bad")             # not tracked
    st.tlp(Tlp("MWr32", 2062, rng_seed=301), lcrc="invert", end=EDB)  # not tracked
    st.tlp(Tlp("MWr32", 2062, rng_seed=302))                       # still the next one
    st.tlp(Tlp("MWr32", 2062, length_dw=2, payload_dw=1, rng_seed=303))  # length mismatch + replay
    st.tlp(Tlp("MWr32", 2063, rng_seed=304))
    st.dllp(Dllp("nak", seq=2063))
    st.tlp(Tlp("MWr32", 1015, rng_seed=305))                       # 3144 behind = 952 ahead: error
    st.tlp(Tlp("MWr32", 1016, rng_seed=306))
    st.pad_idle()


# DLLPs and TLPs starting at every byte lane of a 64-bit word, back to back; zero-delay RTL dump.
@fixture("dll_lanes_w64_1ps", "DLL framing at every byte lane, back to back (64-bit, zero-delay)",
         "dll", 8, Clocking((1, "ps"), period=4000), params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.skp()
    walk_lanes(st, Seq(100))
    st.pad_idle()


# The same at 32 bits with data changing mid-cycle.
@fixture("dll_lanes_w32_delayed_1ns", "DLL framing at every lane of a 32-bit word, data changing mid-cycle",
         "dll", 4, Clocking((1, "ns"), period=16, change="delay", delay=5), params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.skp()
    walk_lanes(st, Seq(4093))
    st.pad_idle()


# The same at 16 bits on a 100 fs timescale.
@fixture("dll_lanes_w16_100fs", "DLL framing at both lanes of a 16-bit word (100 fs ticks)",
         "dll", 2, Clocking((100, "fs"), period=80000), params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.skp()
    walk_lanes(st, Seq(7))
    st.pad_idle()


# scrambling=on for the DLL decoder: pre-COM data discarded, descrambled packets, SKPs between idles, stray data.
@fixture("dll_scrambled_on_w32_1ps", "scrambling=on: descrambled DLLPs and TLPs, SKP not advancing the LFSR",
         "dll", 4, Clocking((1, "ps"), period=16000), params={"scrambling": "on"}, scrambled=True)
def _(st: Stream) -> None:
    st.pre_com(bytes([0xFB, 0x00, 0x5C, 0x01, 0xFD, 0x99]))
    st.skp()
    st.ts("ts1", 0, 0, count=2)
    st.idle(5)
    st.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    st.idle(3)
    st.skp(2)
    st.idle(1)
    seq = Seq(0)
    st.tlp(Tlp("MWr64", seq(), length_dw=3, rng_seed=61))
    st.dllp(Dllp("ack", seq=0))
    st.skp()
    st.tlp(Tlp("CplD", seq(), length_dw=1, rng_seed=62))
    st.idle(2)
    st.stray(bytes([0x0F, 0xF0]))
    st.idle(2)
    st.dllp(Dllp("ack", seq=1), crc=b"\x00\x01")
    st.pad_idle()


# auto on a scrambled stream (64-bit): on wins; errors before the lock come from the on buffer.
@fixture("dll_auto_on_w64_1ns", "auto locks to on; pre-lock transactions are the on pipeline's",
         "dll", 8, Clocking((1, "ns"), period=8), scrambled=True)
def _(st: Stream) -> None:
    st.skp()
    st.ts("ts1", 0, 0, count=2)
    st.idle(5)
    st.stray(bytes([0x42]))
    st.idle(2)
    st.dllp(Dllp("ack", seq=0), crc=b"\x00\x00")
    st.idle(3)
    st.tlp(Tlp("MRd32", 0, rng_seed=81))
    st.dllp(Dllp("ack", seq=0))
    st.skp()
    st.tlp(Tlp("MRd32", 1, rng_seed=82))
    st.pad_idle()


# auto on a plain stream (16-bit, 100 fs): off wins at the first good DLLP; a later on-only-valid DLLP is a CRC mismatch.
@fixture("dll_auto_off_w16_100fs", "auto locks to off; afterwards only the off pipeline runs",
         "dll", 2, Clocking((100, "fs"), period=40000))
def _(st: Stream) -> None:
    st.skp()
    st.idle(2)
    st.dllp(Dllp("ack", seq=1), crc=b"\x99\x99")
    st.stray(bytes([0x07, 0x08]))
    st.dllp(Dllp("initfc1_cpl"))
    st.contrived_dllp(Dllp("initfc2_cpl"))
    st.tlp(Tlp("MWr32", 0, rng_seed=91))
    st.pad_idle()


# auto, scrambled stream: the descrambled-valid DLLP comes first, so on wins even though a later DLLP is valid raw.
@fixture("dll_auto_first_valid_on_w8_1ps", "auto: the first pipeline to close a valid packet wins (on first, off-valid later)",
         "dll", 1, Clocking((1, "ps"), period=2000), scrambled=True)
def _(st: Stream) -> None:
    st.skp()
    st.idle(4)
    st.dllp(Dllp("ack", seq=100))             # valid descrambled: closes first
    st.idle(2)
    st.contrived_dllp(Dllp("ack", seq=101))   # valid only raw: on already won
    st.dllp(Dllp("ack", seq=102))
    st.skp()


# auto, plain stream: the raw-valid DLLP closes before the descrambled-valid one, so off wins.
@fixture("dll_auto_first_valid_off_w32_10ps", "auto: a raw-valid DLLP before a descrambled-valid one locks off",
         "dll", 4, Clocking((10, "ps"), period=1600))
def _(st: Stream) -> None:
    st.skp()
    st.idle(4)
    st.dllp(Dllp("ack", seq=200))
    st.contrived_dllp(Dllp("ack", seq=201))
    st.dllp(Dllp("ack", seq=202))
    st.pad_idle()


# auto with only ordered sets: no transaction is ever buffered, so off (undetermined) spans the last symbol.
@fixture("dll_auto_undetermined_os_only_w32_10ps", "auto with nothing buffered: off (undetermined) spans the last symbol",
         "dll", 4, Clocking((10, "ps"), period=1600))
def _(st: Stream) -> None:
    st.ts("ts1", None, None, count=4)
    st.ts("ts2", 0, 0, count=2)
    st.skp()
    st.fts(3)
    st.eios()


# auto with bad packets only: off (undetermined) spans the last buffered transaction.
@fixture("dll_auto_undetermined_w8_1ns", "auto with only bad packets: off (undetermined) spans the last buffered error",
         "dll", 1, Clocking((1, "ns"), period=4))
def _(st: Stream) -> None:
    st.skp()
    st.dllp(Dllp("ack", seq=1), crc=b"\x01\x01")
    st.tlp(Tlp("MRd32", 0, rng_seed=12), lcrc="bad")
    st.raw_dllp_bytes(bytes([0, 0, 0]))
    st.dllp(Dllp("nak", seq=1), crc=b"\x02\x02")
    st.idle(4)
    st.skp()


# auto: Disable Scrambling in TS1 locks off; the following DLLP is valid only descrambled and so reports a CRC mismatch.
@fixture("dll_auto_ds_w16_10ps", "auto: Disable Scrambling locks to off before any packet",
         "dll", 2, Clocking((10, "ps"), period=800))
def _(st: Stream) -> None:
    st.ts("ts1", 0, 0, ctrl=0x08, count=2)
    st.contrived_dllp(Dllp("ack", seq=9))
    st.skp()
    st.idle(2)
    st.dllp(Dllp("ack", seq=10))
    st.pad_idle()


# Common errors seen by the DLL decoder (32-bit).
@fixture("dll_errors_w32_1ns", "the SPEC §6 common errors through the DLL decoder",
         "dll", 4, Clocking((1, "ns"), period=16), params={"scrambling": "off"})
def _(st: Stream) -> None:
    common_errors(st)


# X/Z on the DLL decoder, with an open TLP dropped silently (8-bit, 1 ps).
@fixture("dll_xz_w8_1ps", "X/Z runs on the DLL decoder; an open packet is dropped without a truncation error",
         "dll", 1, Clocking((1, "ps"), period=4000), params={"scrambling": "off"})
def _(st: Stream) -> None:
    xz_body(st)


# Gating on the DLL decoder: rxvalid and txelecidle both bound; packets truncated by each.
@fixture("dll_gating_w16_1ns", "rxvalid and txelecidle gating on the DLL decoder, truncating open packets",
         "dll", 2, Clocking((1, "ns"), period=8), names=VcdNames(rxvalid="rx_valid", txelecidle="tx_elec_idle"),
         params={"scrambling": "off"})
def _(st: Stream) -> None:
    gating_body(st, "rxvalid")
    txelecidle_body(st)
    st.pad_idle()


# sample_point=at_edge on the DLL decoder: 64-bit, 100 fs, odd period.
@fixture("dll_at_edge_w64_100fs", "sample_point=at_edge on the DLL decoder (64-bit, odd period at 100 fs)",
         "dll", 8, Clocking((100, "fs"), period=8001, sample_point="at_edge"),
         params={"scrambling": "off", "sample_point": "at_edge"})
def _(st: Stream) -> None:
    st.skp()
    walk_lanes(st, Seq(0))
    st.pad_idle()


# rxstatus bound on the DLL decoder: it declares the signal and ignores it.
@fixture("dll_rxstatus_ignored_w16_1ns", "the DLL decoder ignores rxstatus",
         "dll", 2, Clocking((1, "ns"), period=8, change="delay", delay=2),
         names=VcdNames(rxstatus="rx_status"), params={"scrambling": "off"})
def _(st: Stream) -> None:
    rxstatus_body(st)


# ── cases settled by SPEC §10 ────────────────────────────────────────────────

# Lone coalescable units with coalesce=true (FTS, EIEOS, TS1, Idle ×1 show no ×1), a sixth SKP, lone K symbols outside; ends on a SKP OS.
@fixture("pipe_lone_units_w16_1ns", "§10.3 no ×1 for lone FTS/EIEOS/Idle; §10.2 sixth SKP and lone K are unexpected_k; trailing SKP OS",
         "pipe", 2, Clocking((1, "ns"), period=8), params={"scrambling": "off", "show_idle": True})
def _(st: Stream) -> None:
    st.skp()
    st.fts(1)
    st.idle(1)
    st.eieos(1)
    st.idle(1)
    st.ts("ts1", 0, 0, count=1)
    st.idle(2)
    st.skp(6)                      # COM + 5 SKP, then the 6th SKP outside
    st.idle(1)
    for k in (PAD, IDL, FTS, EIE):
        st.unexpected_k(k)
        st.idle(1)
    st.fts(2)
    st.idle(1)
    st.dllp(Dllp("ack", seq=1))
    st.idle(1)
    end_with(st, 3)
    st.skp(2)                      # still open at the end of the trace: emitted at flush


def frame_errors(st: Stream) -> None:
    seq = Seq(40)
    st.skp()
    st.idle(2)
    st.invalid_k_in_packet("dllp", bytes([0x00, 0x00]), 0x9C)   # invalid_k only, packet dropped
    st.dllp(Dllp("ack", seq=1))
    st.k_in_packet("dllp", bytes([0x00, 0x00]), IDL)            # dropped; the rest is stray data
    st.stray(bytes([0x0F, 0x22]))
    st.unmatched_end(END)
    st.invalid_k_in_packet("tlp", bytes([0x00, 0x05, 0x40]), 0xDC)
    st.stray(bytes([0x01, 0x02]))
    st.idle(1)
    st.dllp(Dllp("ack", seq=2), end=EDB)                        # dllp_edb
    st.raw_dllp_bytes(bytes([0x10, 0x00, 0x00]), end=EDB)       # dllp_edb, any length
    st.unexpected_k(SKP)
    st.idle(2)
    st.tlp(Tlp("MWr32", seq(), length_dw=1, rng_seed=401))       # seq 40 sets next = 41
    st.tlp(Tlp("MWr32", seq(), length_dw=2, payload_dw=1, rng_seed=402), lcrc="bad")  # LCRC + length
    st.tlp(Tlp("MWr32", 47, length_dw=2, payload_dw=1, rng_seed=403))  # length + sequence error
    st.tlp(Tlp("MWr32", 47, length_dw=4, payload_dw=1, rng_seed=404))  # length + replay
    st.tlp(Tlp("MWr32", 48, rng_seed=405))
    st.idle(1)
    st.dllp(Dllp("reserved", byte0=0x02))
    t = Tlp("MRd32", 49, rng_seed=406).bytes()
    body = bytes([0x00, 49]) + t
    cut = (-(st.pos() + 1)) % st.n or st.n
    st.open_at_end("tlp", body[:cut + st.n])                     # still open at the end


# SPEC §10.1/§10.2 framing errors through the DLL decoder: invalid K in a packet, data after a dropped packet, dllp_edb, multi-problem TLPs, open packet at end.
@fixture("dll_frame_errors_w8_1ps", "invalid K in a packet, dropped-packet leftovers, dllp_edb, joined TLP errors, packet open at end of trace",
         "dll", 1, Clocking((1, "ps"), period=4000), params={"scrambling": "off"})
def _(st: Stream) -> None:
    frame_errors(st)


# The same through the PIPE decoder (32-bit, 10 ps): the frame-level errors and the end-of-trace truncation.
@fixture("pipe_frame_errors_w32_10ps", "the §10.2 framing errors through the PIPE decoder; packet open at end of trace",
         "pipe", 4, Clocking((10, "ps"), period=1600, change="delay", delay=300), params={"scrambling": "off"})
def _(st: Stream) -> None:
    frame_errors(st)


# The same through the DLL decoder at 64 bits under auto: locks off at the first good packet.
@fixture("dll_frame_errors_w64_1ns", "the §10.2 framing errors at 64 bits under default auto",
         "dll", 8, Clocking((1, "ns"), period=8))
def _(st: Stream) -> None:
    frame_errors(st)


# EIEOS in a scrambled stream: its final D10.2 bypasses the descrambler (§10.3); lone and coalesced EIEOS.
@fixture("pipe_eieos_scrambled_w32_1ps", "scrambling=on: EIEOS symbol 15 is not descrambled; lone and coalesced EIEOS",
         "pipe", 4, Clocking((1, "ps"), period=16000), params={"scrambling": "on", "show_idle": True}, scrambled=True)
def _(st: Stream) -> None:
    st.skp()
    st.eieos(2)
    st.ts("ts1", 0, 0, rate=0x06, count=2)
    st.idle(3)
    st.eieos(1)
    st.idle(5)
    st.dllp(Dllp("ack", seq=4))
    st.idle(4)
    st.eieos(1)
    st.skp()


# The DLL decoder under auto on a scrambled stream with EIEOS: a misread symbol 15 would surface as unknown_os.
@fixture("dll_eieos_scrambled_auto_w64_1ns", "auto locks on across EIEOS whose symbol 15 bypasses the descrambler",
         "dll", 8, Clocking((1, "ns"), period=8), scrambled=True)
def _(st: Stream) -> None:
    st.eieos(1)
    st.ts("ts1", 0, 0, rate=0x06, count=2)
    st.idle(4)
    st.dllp(Dllp("updatefc_p", hdr=8, data=64))
    st.eieos(2)
    st.idle(3)
    st.tlp(Tlp("MWr32", 0, rng_seed=77))
    st.eieos(1)
    st.pad_idle()


# A coalesced TS1 run still in progress at the end of the trace is emitted at flush (§10.6 step 2).
@fixture("pipe_flush_ts_run_w8_1ns", "flush emits a coalesced TS1 run in progress",
         "pipe", 1, Clocking((1, "ns"), period=4), params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.skp()
    st.ts("ts2", 0, 0, count=2)
    st.ts("ts1", None, None, count=5)


# auto with no valid packet and a packet open at the end: the truncation is the last buffered transaction (§10.6 steps 3 and 5).
@fixture("dll_flush_undetermined_w16_10ps", "flush order: end-of-trace truncation, then off (undetermined) spanning it",
         "dll", 2, Clocking((10, "ps"), period=800))
def _(st: Stream) -> None:
    st.skp()
    st.dllp(Dllp("ack", seq=3), crc=b"\x00\x00")
    st.idle(2)
    st.open_at_end("dllp", bytes([0x00, 0x00, 0x00]))


# §10.7: a reserved DLLP with a CRC mismatch (joined sentences); an incomplete TS cut off by the end of the trace.
@fixture("dll_reserved_crc_os_end_w32_1ns", "reserved DLLP + CRC mismatch joins both sentences; partial TS1 at end is unknown_os",
         "dll", 4, Clocking((1, "ns"), period=16), params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.skp()
    st.dllp(Dllp("reserved", byte0=0x01), crc=b"\x00\x00")
    st.dllp(Dllp("reserved", byte0=0x48))
    st.dllp(Dllp("ack", seq=7))
    end_with(st, 8)
    st.os_incomplete_at_end([(PAD, True), (PAD, True), (24, False), (0x02, False), (0x00, False),
                             (0x4A, False), (0x4A, False)])       # COM + 7 of a TS1's 15


# §10.6/§10.7 through the PIPE decoder: COM FTS FTS cut off by the end of the trace; a reserved DLLP frame with a bad CRC is just a frame.
@fixture("pipe_os_incomplete_end_w16_1ps", "incomplete FTS OS at end of trace: unknown_os, symbol = last symbol received",
         "pipe", 2, Clocking((1, "ps"), period=8000, change="delay", delay=1000), params={"scrambling": "off"})
def _(st: Stream) -> None:
    st.ts("ts1", 0, 0, count=2)
    st.fts(2)
    st.dllp(Dllp("reserved", byte0=0x01), crc=b"\x00\x00")
    end_with(st, 3)
    st.os_incomplete_at_end([(FTS, True), (FTS, True)])


def com_misfits(st: Stream) -> None:
    """COM then STP/SDP/COM/END/EDB: each misfit is consumed, so what the
    packet would have held is stray data and its END has no start."""
    st.skp()
    st.idle(2)
    st.os_bad_start(STP)
    st.stray(bytes([0x0A, 0x0B, 0x0C, 0x0D, 0x0E]))
    st.unmatched_end(END)
    st.idle(2)
    st.os_bad_start(SDP)
    st.stray(bytes([0x10, 0x20, 0x30]))
    st.unmatched_end(END)
    st.idle(1)
    st.os_bad_start(COM)          # the second COM is the misfit, not a new OS
    st.idle(3)
    st.os_bad_start(END)
    st.idle(1)
    st.os_bad_start(EDB)
    st.idle(2)
    st.dllp(Dllp("ack", seq=8))   # framing is back to normal
    st.os_bad_start(COM)
    st.skp()                       # after a consumed COM, a real SKP OS
    st.idle(2)


# §5.3: COM followed by STP, SDP, COM, END or EDB is unknown_os and the misfit is consumed (PIPE, off).
@fixture("pipe_com_misfit_w8_1ns", "COM + STP/SDP/COM/END/EDB: unknown_os consumes the misfit; leftovers are stray/unmatched",
         "pipe", 1, Clocking((1, "ns"), period=8), params={"scrambling": "off"})
def _(st: Stream) -> None:
    com_misfits(st)
    st.pad_idle()


# The same through the DLL decoder under default auto (locks off at the Ack), 32-bit, 1 ps.
@fixture("dll_com_misfit_w32_1ps", "COM misfits on the DLL decoder under auto",
         "dll", 4, Clocking((1, "ps"), period=16000))
def _(st: Stream) -> None:
    com_misfits(st)
    st.pad_idle()


# ── generation ───────────────────────────────────────────────────────────────

def make(fx: Fixture) -> dict[str, str]:
    st = Stream(fx.lanes, scrambled=fx.scrambled)
    fx.build(st)
    st.finish()
    edges = L.build_edges(st)
    tr = L.write_vcd(edges, fx.lanes, fx.clk, fx.names)
    bound = ["pclk", "data", "datak"] + [s for s in ("rxvalid", "txelecidle", "rxstatus")
                                         if getattr(fx.names, s)]
    for e in edges:
        if e.kind == "gate":
            sig = "rxvalid" if e.rxvalid == 0 else "txelecidle"
            assert sig in bound, f"{fx.name}: gating by unbound {sig}"
    # The host model must see exactly the words the encoder laid out.
    seen = L.host_edges(tr, bound, fx.clk.sample_point)
    assert len(seen) == len(edges), (fx.name, len(seen), len(edges))
    for k, (t, vals) in enumerate(seen):
        e = edges[k]
        assert t == e.t, (fx.name, k, t, e.t)
        want = {"data": L._bits(e.data, 8 * fx.lanes), "datak": L._bits(e.datak, fx.lanes),
                "rxvalid": L._bits(e.rxvalid, 1), "txelecidle": L._bits(e.txelecidle, 1),
                "rxstatus": L._bits(e.rxstatus, 3)}
        for s in bound[1:]:
            assert vals.get(s) == want[s], (fx.name, k, s, vals.get(s), want[s])
    txs = L.expected(st, edges, fx.decoder, fx.params, rxstatus_bound="rxstatus" in bound)
    paths = {s: tr.paths[s] for s in bound}
    fpt = L.fs_per_tick(*fx.clk.timescale)
    return {f"{fx.name}.vcd": tr.text,
            f"{fx.name}.bindings.json": L.bindings_json(fx.decoder, paths, fx.params),
            f"{fx.name}.expected.json": L.canonical(txs, fpt)}


def generate(out_dir: str, quiet: bool = False) -> dict[str, str]:
    os.makedirs(out_dir, exist_ok=True)
    names = set()
    files: dict[str, str] = {}
    for fx in FIXTURES:
        assert fx.name not in names, fx.name
        names.add(fx.name)
        try:
            made = make(fx)
        except AssertionError as e:
            raise SystemExit(f"{fx.name}: construction check failed: {e!r}") from e
        for fname, text in made.items():
            files[fname] = text
            with open(os.path.join(out_dir, fname), "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
    return files


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="regenerate into a temp dir; fail if anything would change")
    ap.add_argument("--list", action="store_true", help="list the fixtures and what each proves")
    ap.add_argument("--out", default=GENERATED, help="output directory (default: fixtures/generated)")
    args = ap.parse_args()
    print(f"protocol self-check: {len(L.self_check())} known answers OK")
    if args.list:
        for fx in FIXTURES:
            print(f"{fx.name:42s} {fx.decoder:22s} {fx.proves}")
        return 0
    if args.check:
        with tempfile.TemporaryDirectory() as tmp:
            files = generate(tmp)
            bad = []
            for fname, text in sorted(files.items()):
                p = os.path.join(GENERATED, fname)
                try:
                    with open(p, encoding="utf-8", newline="") as f:
                        if f.read() != text:
                            bad.append(f"changed: {fname}")
                except FileNotFoundError:
                    bad.append(f"missing: {fname}")
            if os.path.isdir(GENERATED):
                for fname in sorted(os.listdir(GENERATED)):
                    if fname not in files:
                        bad.append(f"stale (not generated any more): {fname}")
        import capture_check  # noqa: E402  (captured fixture: expected vs the reference decoder)
        bad += capture_check.check()
        if bad:
            print("fixtures are out of date:\n  " + "\n  ".join(bad))
            return 1
        print(f"--check: {len(files)} generated files and the captured fixtures are up to date")
        return 0
    files = generate(args.out)
    print(f"wrote {len(files)} files ({len(FIXTURES)} fixtures) to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
