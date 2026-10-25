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
import pcie_multilane_lib as M  # noqa: E402
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


# ── serial line decoders (SPEC §11) ──────────────────────────────────────────

@dataclass
class SerialFixture:
    name: str
    proves: str
    decoder: str
    build: object
    timescale: tuple
    params: dict
    scrambled: bool
    names: L.SerialNames


def serial(name: str, proves: str, kind: str, timescale: tuple, params: dict | None = None,
           scrambled: bool = False, names: L.SerialNames | None = None):
    def wrap(fn):
        FIXTURES.append(SerialFixture(name, proves, f"ferrite.pcie_{kind}", fn, timescale, params or {},
                                      scrambled, names or L.SerialNames()))
        return fn
    return wrap


NS = 1_000_000          # fs
PRE = ("data", bytes([0x00, 0x00, 0x00]), 3)     # the tail of logical idle sent before the receiver locks
BAD = "0011111011"      # SPEC §11.8's example: K28 6b with the wrong 4b form (a comma, but not a COM)
BAD2 = "1110000011"
BAD3 = "0000011110"


def serial_training(st: L.SerialStream) -> None:
    st.ts("ts1", None, None, n_fts=24, count=4)
    st.ts("ts1", 0, None, count=2)
    st.ts("ts2", 0, 0, count=4)
    st.skp()
    st.fts(2)
    st.skp()


# Gen1 at a fixed rate: X before the line is active shows nothing; pre-lock bits are ignored; lock on the first COM (RD-); TS/SKP/FTS/packets at 400 ps per bit; the last symbol is decoded because a transition follows it; redundant value writes do not end a run.
@serial("serial_gen1_training_1ps", "Gen1 lock (RD- COM), pre-lock bits ignored, TS/SKP/FTS/EIOS and frames at UI 400 ps, redundant samples",
        "serial", (1, "ps"), params={"rate": "gen1", "scrambling": "off"},
        names=L.SerialNames(complement="txn", redundant=True))
def _(st: L.SerialStream) -> None:
    st.lead(("x", 2 * NS))
    st.burst("gen1", rd=-1, prelock=[PRE, ("bits", "01")])
    serial_training(st)
    st.idle(4)
    st.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    st.idle(2)
    st.tlp(Tlp("MRd32", 0, rng_seed=3))
    st.idle(3)
    st.skp(2)
    st.eios()


# Gen2 at a fixed rate, scrambled, scrambling=on with show_idle: lock on an RD+ COM (1100000101), EIEOS (K28.7 runs carry commas but no COM), descrambled idle runs and packets at 200 ps per bit.
@serial("serial_gen2_scrambled_10ps", "Gen2 lock on an RD+ COM, EIEOS, scrambling=on, show_idle, frames at UI 200 ps",
        "serial", (10, "ps"), params={"rate": "gen2", "scrambling": "on", "show_idle": True}, scrambled=True)
def _(st: L.SerialStream) -> None:
    st.burst("gen2", rd=1, prelock=[("data", bytes([0x00, 0x00]), 6)])
    st.eieos(2)
    st.ts("ts1", 0, 0, rate=0x06, count=2)
    st.ts("ts2", 0, 0, rate=0x06, count=2)
    st.idle(6)
    st.dllp(Dllp("ack", seq=3))
    st.idle(3)
    st.tlp(Tlp("MWr32", 0, length_dw=2, rng_seed=21))
    st.idle(4)
    st.skp()
    st.idle(2)
    st.eios()


# rate=auto: a Gen2 burst decided by a 299.9 ps run after a run of exactly 300 ps (not shorter, so not deciding); a Gen1 burst decided after 40 runs; a short Gen1 burst (< 40 runs, the first a run of exactly 8 ns, which is bits, not idle) decided when X/Z idle ends it; the open idle at the end spans to the last sample.
@serial("serial_rate_auto_100fs", "rate=auto: Gen2 on a run < 300 ps (300 ps exactly does not decide), Gen1 at 40 runs, Gen1 when the burst ends first; 8 ns run is bits",
        "serial", (100, "fs"), params={"rate": "auto", "scrambling": "off"})
def _(st: L.SerialStream) -> None:
    st.burst("gen2", rd=-1, prelock=[("run", "1", 5, 1_000_000), ("run", "0", 2, 300_000),
                                     ("run", "1", 3, 500_000), ("run", "0", 1, 299_900), ("bits", "1"), PRE], decide=1)
    st.ts("ts1", 0, 0, rate=0x86, count=2)
    st.skp()
    st.dllp(Dllp("ack", seq=1))
    st.eios()
    st.elec_idle(("x", 3 * NS))
    st.burst("gen1", rd=-1, prelock=[PRE], decide=2)
    st.ts("ts1", 0, 0, count=3)
    st.skp()
    st.idle(2)
    st.dllp(Dllp("ack", seq=2))
    st.eios()
    st.elec_idle(("~", 12 * NS))
    st.burst("gen1", rd=-1, prelock=[("run", "~", 20, 8 * NS), ("bits", "1")], decide=3)
    st.skp()
    st.eios()
    st.elec_idle(("x", 2 * NS), ("z", 1 * NS))


# rate=auto on the DLL decoder, scrambled, scrambling=auto: Gen1 burst (40 runs), X idle, Gen2 burst with EIEOS, level idle, then a short Gen1 burst still held at the end of the trace (flush converts it as Gen1): a code error there is EDB without start.
@serial("serial_dll_rate_auto_1ps", "rate=auto per burst on the DLL decoder; runs still held at flush are converted as Gen1; auto scrambling locks on",
        "serial_dll", (1, "ps"), params={"rate": "auto"}, scrambled=True)
def _(st: L.SerialStream) -> None:
    st.burst("gen1", rd=-1, prelock=[PRE], decide=2)
    st.skp()
    st.ts("ts1", 0, 0, rate=0x06, count=2)
    st.idle(3)
    st.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    st.tlp(Tlp("MRd32", 0, rng_seed=41))
    st.idle(2)
    st.eios()
    st.elec_idle(("x", 4 * NS))
    st.burst("gen2", rd=1, prelock=[("data", bytes([0x00]), 2)], decide=1)
    st.eieos(1)
    st.ts("ts1", 0, 0, rate=0x86, count=2)
    st.idle(2)
    st.dllp(Dllp("ack", seq=0))
    st.tlp(Tlp("MWr32", 1, rng_seed=42))
    st.eios()
    st.elec_idle(("~", 9 * NS))
    st.burst("gen1", rd=-1, prelock=[("bits", "1010")], decide=3)
    st.skp()
    st.unmatched_end(EDB)
    st.code_error(st.pos() - 1, BAD)


# 8b/10b faults through the symbol decoder: wrong-RD D0.0 with the transmitter resynced (one error) and not (a second error on the next D0.0), a wrong-RD COM on the boundary (Disparity error K28.5), a wrong-RD DLLP byte (DLLP still good), code errors in idle, as a DLLP's END, inside a DLLP body (EDB ends it, the rest is stray data and END without start), as a TLP's END and after SKP SKP; K28.4 is an invalid K symbol.
@serial("serial_8b10b_errors_1ps", "disparity errors (RD rule after them), code errors fed as EDB in idle/DLLP/TLP/SKP OS, K28.4 invalid",
        "serial", (1, "ps"), params={"rate": "gen1", "scrambling": "off"})
def _(st: L.SerialStream) -> None:
    st.burst("gen1", rd=-1, prelock=[PRE])
    st.skp()
    st.ts("ts1", 0, 0, count=2)
    st.idle(3)
    st.wrong_rd(st.pos())                 # resync: the receiver's RD rule leaves no second error
    st.idle(4)
    st.wrong_rd(st.pos(), resync=False)   # the transmitter keeps its RD: the next D0.0 is flagged too
    st.idle(4)
    st.wrong_rd(st.pos())                 # the COM of a SKP OS, on the boundary
    st.skp()
    st.idle(2)
    a = st.pos()
    st.dllp(Dllp("ack", seq=5))
    st.wrong_rd(a + 1, a + 6)
    st.idle(2)
    st.unmatched_end(EDB)
    st.code_error(st.pos() - 1, BAD)
    st.idle(2)
    u = st.dllp(Dllp("ack", seq=6), end=EDB)
    st.code_error(u.b, BAD2)
    st.idle(1)
    st.dllp_cut(Dllp("nak", seq=7), 2, BAD3)
    st.idle(2)
    u = st.tlp(Tlp("MWr32", 1, rng_seed=51), end=EDB)
    st.code_error(u.b, BAD)
    st.idle(2)
    st.skp(2)
    st.unmatched_end(EDB)
    st.code_error(st.pos() - 1, BAD2)
    st.idle(2)
    st.invalid_k(0x9C)
    st.idle(2)
    st.eios()


# The same faults through the DLL decoder at Gen2 on a scrambled stream (scrambling=on): wrong-RD bytes inside a DLLP and a TLP (both still decode; the label names the 8b/10b symbol on the wire), a TLP's END as a code error (tlp_edb), a TLP cut by a code error at a valid framing length (tlp_edb, then stray data and END without start), a DLLP's END as a code error (dllp_edb).
@serial("serial_dll_8b10b_errors_10ps", "DLL decoder: disparity errors on scrambled bytes, code errors ending TLPs and DLLPs (tlp_edb, dllp_edb)",
        "serial_dll", (10, "ps"), params={"rate": "gen2", "scrambling": "on"}, scrambled=True)
def _(st: L.SerialStream) -> None:
    st.burst("gen2", rd=1, prelock=[PRE])
    st.skp()
    st.ts("ts1", 0, 0, rate=0x06, count=2)
    st.idle(4)
    a = st.pos()
    st.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    st.wrong_rd(a + 1, a + 6)
    a = st.pos()
    st.tlp(Tlp("MWr32", 0, length_dw=2, rng_seed=61))
    st.wrong_rd(a + 8, a + 20, resync=False)
    st.idle(2)
    u = st.tlp(Tlp("MRd32", 1, rng_seed=62), end=EDB)
    st.code_error(u.b, BAD)
    st.idle(1)
    st.tlp_cut(Tlp("MWr32", 2, length_dw=4, rng_seed=63), 22, BAD2)
    st.idle(2)
    u = st.dllp(Dllp("ack", seq=0), end=EDB)
    st.code_error(u.b, BAD3)
    st.idle(2)
    st.dllp(Dllp("ack", seq=0))
    st.tlp(Tlp("MRd32", 1, rng_seed=64))
    st.skp()


# Bit slips while locked: 3 bits inserted before a COM (realign by 3, the misaligned symbol a code error overlapping the COM), 9 bits of an idle symbol deleted (by 1 bit), 9 bits inserted before a COM sent with the other RD (by 9; its form sets the RD, so no disparity error), 6 bits inserted (by 6), a DLLP's END cut to 2 bits (by 2; EDB ends the DLLP).
@serial("serial_slip_realign_10ps", "realign by 1, 2, 3, 6 and 9 bits; the overlapping code error; EDB ends a DLLP; a realign COM's form sets the RD",
        "serial", (10, "ps"), params={"rate": "gen1", "scrambling": "off"})
def _(st: L.SerialStream) -> None:
    st.burst("gen1", rd=-1, prelock=[PRE])
    st.skp()
    st.ts("ts1", 0, 0, count=2)
    st.idle(4)
    st.slip("111")
    st.skp()
    st.idle(3)
    st.dllp(Dllp("ack", seq=1))
    st.idle(2)
    st.slip(trunc=(0x00, False, 1))
    st.skp()
    st.idle(2)
    st.slip("111100001")
    st.wrong_rd(st.pos())
    st.skp()
    st.idle(2)
    st.slip("000011")
    st.skp()
    st.idle(2)
    u = st.dllp(Dllp("ack", seq=2), end=EDB)
    st.garbage(u.b, trunc=(END, True, 2))
    st.skp()
    st.idle(2)
    st.dllp(Dllp("ack", seq=3))
    st.idle(2)
    st.eios()


# Slips on the DLL decoder at Gen2, scrambled, scrambling=auto: realign by 5 in idle, by 7 and 8 right after a TLP, a TLP's END cut to 4 bits (tlp_edb, realign by 4); the descrambler resyncs at the COM and later packets are good.
@serial("serial_dll_slip_100fs", "DLL decoder: realign by 4, 5, 7 and 8 on a scrambled Gen2 stream; auto scrambling locks on",
        "serial_dll", (100, "fs"), params={"rate": "gen2"}, scrambled=True)
def _(st: L.SerialStream) -> None:
    st.burst("gen2", rd=-1, prelock=[PRE])
    st.skp()
    st.ts("ts1", 0, 0, rate=0x06, count=2)
    st.idle(3)
    st.dllp(Dllp("ack", seq=0))
    st.idle(2)
    st.slip("10110")
    st.skp()
    st.idle(2)
    st.tlp(Tlp("MWr32", 0, rng_seed=71))
    st.slip("1111001")
    st.skp()
    st.slip("00001111")
    st.skp()
    st.idle(2)
    u = st.tlp(Tlp("MRd32", 1, rng_seed=72), end=EDB)
    st.garbage(u.b, trunc=(END, True, 4))
    st.skp()
    st.idle(1)
    st.tlp(Tlp("MRd32", 1, rng_seed=73))
    st.dllp(Dllp("ack", seq=1))
    st.skp()


def elec_idle_body(st: L.SerialStream) -> None:
    st.burst("gen1", rd=-1, prelock=[PRE])
    st.skp()
    st.ts("ts1", 0, 0, count=2)
    st.idle(2)
    st.dllp(Dllp("ack", seq=1))
    st.idle(2)
    body = bytes([0x00, 0x09]) + Tlp("MWr32", 9, length_dw=2, rng_seed=81).bytes()
    st.truncated("tlp", body[:10], by="electrical idle")
    st.elec_idle(("x", 5 * NS))                                    # X: the TLP is truncated
    st.burst("gen1", rd=1, prelock=[("data", bytes([0x00]), 4)])   # relock on the other COM form
    st.skp()
    st.idle(2)
    st.dllp(Dllp("ack", seq=2))
    st.truncated("dllp", bytes([0x00, 0x00, 0x00]), by="electrical idle")
    st.elec_idle(("~", 9 * NS), ("x", 2 * NS), partial="10110")    # long run then X: one period; 5 bits dropped
    st.burst("gen1", rd=-1, prelock=[PRE])
    st.skp()
    st.idle(1)
    st.os_cut([(FTS, True), (FTS, True)])                         # COM FTS FTS, cut: unknown_os
    st.elec_idle(("z", 3 * NS), ("1", 10 * NS))                    # Z then a long run: one period
    st.burst("gen1", rd=-1, prelock=[("bits", "0"), PRE])
    st.skp()
    st.idle(1)
    st.skp(2)                                                     # a SKP OS with 2 SKPs is complete at idle
    st.elec_idle(("x", 1 * NS))
    st.burst("gen1", rd=-1, prelock=[PRE])
    st.skp()
    st.idle(3)
    st.dllp(Dllp("ack", seq=3))
    st.eios()


# Electrical idle on the symbol decoder: a long level first (not reported: nothing converted yet), X idle truncating a TLP, a long run then X (one period, partial symbol dropped, DLLP truncated), Z then a long run (one period, COM FTS FTS cut is unknown_os), X after a 2-SKP SKP OS (complete), X then Z at the end (open period spans to the last sample); every burst relocks, once on an RD+ COM.
@serial("serial_elec_idle_1ps", "electrical idle as X, Z and > 8 ns levels, back to back; truncation and cut ordered sets; relock; open idle at the end",
        "serial", (1, "ps"), params={"rate": "gen1", "scrambling": "off"})
def _(st: L.SerialStream) -> None:
    st.lead(("0", 12 * NS))
    elec_idle_body(st)
    st.elec_idle(("x", 2 * NS), ("z", 1 * NS))


# The same idle and truncation pattern through the DLL decoder (no electrical_idle or lock transactions), starting at X, ending with a plain transition.
@serial("serial_dll_elec_idle_10ps", "DLL decoder: packets truncated by electrical idle; no lock or idle transactions",
        "serial_dll", (10, "ps"), params={"rate": "gen1", "scrambling": "off"})
def _(st: L.SerialStream) -> None:
    st.lead(("x", 3 * NS))
    elec_idle_body(st)


# invert=true on an inverted line (RXN bound, or a swapped pair) at Gen2, scrambled, default scrambling=auto locking on at the first DLLP; an unbound TXN carries the true polarity.
@serial("serial_invert_gen2_1ps", "invert=true undoes an inverted line (Gen2, scrambled, scrambling=auto)",
        "serial", (1, "ps"), params={"rate": "gen2", "invert": True}, scrambled=True,
        names=L.SerialNames(complement="txn"))
def _(st: L.SerialStream) -> None:
    st.burst("gen2", rd=-1, prelock=[PRE])
    st.eieos(1)
    st.ts("ts1", 0, 0, rate=0x06, count=2)
    st.ts("ts2", 0, 0, rate=0x06, count=2)
    st.idle(4)
    st.dllp(Dllp("updatefc_p", hdr=8, data=64))
    st.tlp(Tlp("CplD", 0, length_dw=1, rng_seed=91))
    st.idle(2)
    st.skp()
    st.eios()


# invert=true on the DLL decoder at Gen1, unscrambled.
@serial("serial_dll_invert_10ps", "invert=true on the DLL decoder (Gen1, unscrambled)",
        "serial_dll", (10, "ps"), params={"rate": "gen1", "invert": True, "scrambling": "off"})
def _(st: L.SerialStream) -> None:
    st.burst("gen1", rd=1, prelock=[("data", bytes([0x00, 0x00]), 5)])
    st.skp()
    st.idle(2)
    for d in ALL_DLLPS[:6]:
        st.dllp(d)
    st.tlp(Tlp("MWr64", 0, length_dw=2, rng_seed=92))
    st.tlp(Tlp("CfgRd0", 1, rng_seed=93))
    st.idle(2)
    st.eios()


# Gen1 with every edge displaced by up to UI/2 - 1 fs (1 fs ticks), neighbours never more than UI/2 apart: run lengths round to whole bits (both rounding limits hit) and bit times use the floor formula; coalesce=false.
@serial("serial_jitter_gen1_1fs", "jitter up to UI/2 - 1 fs: bit counts by rounding, bit times by floor(i*D/n); coalesce=false",
        "serial", (1, "fs"), params={"rate": "gen1", "scrambling": "off", "coalesce": False})
def _(st: L.SerialStream) -> None:
    st.burst("gen1", rd=-1, prelock=[PRE], jitter=7)
    st.skp()
    st.ts("ts1", 0, 0, count=2)
    st.fts(2)
    st.idle(3)
    st.dllp(Dllp("ack", seq=4))
    st.tlp(Tlp("MWr32", 0, length_dw=1, rng_seed=94))
    st.idle(2)
    st.skp(4)
    st.eios()


# Gen2 jitter (edges moved by up to UI/2 - 100 fs) on the DLL decoder with rate=auto (every 1-bit run stays under 300 ps, every longer run at or above it), scrambled, scrambling=on.
@serial("serial_dll_jitter_gen2_100fs", "DLL decoder: Gen2 jitter under rate=auto, scrambled",
        "serial_dll", (100, "fs"), params={"rate": "auto", "scrambling": "on"}, scrambled=True)
def _(st: L.SerialStream) -> None:
    st.burst("gen2", rd=1, prelock=[PRE], jitter=11, decide=1)
    st.skp()
    st.ts("ts1", 0, 0, rate=0x06, count=2)
    st.idle(2)
    for d in ALL_DLLPS[8:14]:
        st.dllp(d)
    st.idle(1)
    st.tlp(Tlp("MRd64", 0, length_dw=2, rng_seed=95))
    st.tlp(Tlp("MWr32", 1, length_dw=3, rng_seed=96))
    st.skp()


def speed_change(st: L.SerialStream, level_idle: bool) -> None:
    st.burst("gen1", rd=-1, prelock=[PRE], decide=2)
    st.ts("ts1", 0, 0, rate=0x06, count=3)
    st.ts("ts2", 0, 0, rate=0x06, count=2)
    st.skp()
    st.idle(4)
    st.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    st.idle(2)
    st.tlp(Tlp("MWr32", 0, rng_seed=101))
    st.idle(2)
    st.ts("ts1", 0, 0, rate=0x86, count=2)       # Recovery: speed_change set
    st.ts("ts2", 0, 0, rate=0x86, count=2)
    st.eios()
    if level_idle:
        st.elec_idle(("~", 10 * NS))
    else:
        st.elec_idle(("x", 6 * NS))
    st.burst("gen2", rd=1, prelock=[("data", bytes([0x00]), 1)], decide=1)
    st.eieos(1)
    st.ts("ts1", 0, 0, rate=0x86, count=2)
    st.eieos(1)
    st.ts("ts2", 0, 0, rate=0x06, count=2)
    st.skp()
    st.idle(3)
    st.dllp(Dllp("ack", seq=0))
    st.tlp(Tlp("MRd32", 1, rng_seed=102))
    st.idle(2)
    st.dllp(Dllp("ack", seq=1))
    st.eios()


# Gen1 -> Gen2 speed change on the symbol decoder: training and packets at Gen1, EIOS, X idle, then EIEOS + TS at Gen2 and packets; rate=auto decides each burst, second lock at Gen2; scrambled, scrambling=auto locks on at Gen1 and stays.
@serial("serial_speed_change_1ps", "Gen1 -> electrical idle -> Gen2 with a second lock; rate=auto and scrambling=auto",
        "serial", (1, "ps"), params={"rate": "auto"}, scrambled=True)
def _(st: L.SerialStream) -> None:
    speed_change(st, level_idle=False)


# The same speed change through the DLL decoder, with the idle as a held level.
@serial("serial_dll_speed_change_10ps", "DLL decoder: Gen1 -> held-level idle -> Gen2; packets on both sides",
        "serial_dll", (10, "ps"), params={"rate": "auto"}, scrambled=True)
def _(st: L.SerialStream) -> None:
    speed_change(st, level_idle=True)


# The SPEC §6 common errors through the serial DLL decoder at Gen2 on a 100 ps timescale (2 ticks per bit, the coarsest that holds Gen2 bits).
@serial("serial_dll_common_errors_100ps", "SPEC §6 common errors via the serial DLL decoder; Gen2 on 100 ps ticks",
        "serial_dll", (100, "ps"), params={"rate": "gen2", "scrambling": "off"})
def _(st: L.SerialStream) -> None:
    st.burst("gen2", rd=-1, prelock=[PRE])
    common_errors(st)


# The §10.2 framing errors through the serial symbol decoder, ending with a packet still open at the end of the trace.
@serial("serial_frame_errors_10ps", "§10.2 framing errors via the serial symbol decoder; packet open at end of trace",
        "serial", (10, "ps"), params={"rate": "gen1", "scrambling": "off"})
def _(st: L.SerialStream) -> None:
    st.burst("gen1", rd=-1, prelock=[PRE])
    frame_errors(st)


# ── both directions of one link (SPEC §11, two lines in one VCD) ─────────────

@dataclass
class LinkFixture:
    """Two serial lines of one x1 link in one VCD, each decoded by both serial
    decoders: four fixtures sharing the trace. A fixture binds one line; the
    other line is in the file but never bound, as when a user decodes each
    direction of a real dump with its own decoder instances."""
    name: str            # <name>_<side>_<timescale>; serial_dll_<rest> for the DLL decoder
    proves: str
    build: object        # build(down, up)
    timescale: tuple
    params: dict
    scrambled: bool
    decoder: str = "ferrite.pcie_serial + ferrite.pcie_serial_dll"


def link(name: str, proves: str, timescale: tuple, params: dict | None = None, scrambled: bool = False):
    def wrap(fn):
        FIXTURES.append(LinkFixture(name, proves, fn, timescale, params or {}, scrambled))
        return fn
    return wrap


def in_step(down: L.SerialStream, up: L.SerialStream) -> None:
    """Both lines have sent the same number of symbols, at the same rates,
    after the same idle times: symbol k starts at the same time on both."""
    assert down.pos() == up.pos(), (down.pos(), up.pos())


# A Downstream Port and an Upstream Port bringing up an x1 link (scrambled, rate=auto,
# scrambling=auto on every decoder): Polling.Active (TS1 PAD/PAD), Polling.Configuration
# (TS2 PAD/PAD), Configuration (the Downstream Port proposes Link 0 with Lane PAD, the
# Upstream Port answers PAD/PAD then echoes it; Lane 0; TS2 0/0), idle, InitFC1/InitFC2
# for P, NP and Cpl, a TLP and its Ack at 2.5 GT/s; Recovery with speed_change set on
# both sides (TS1/TS2 Data Rate 0x86), EIOS, 6 ns electrical idle, EIEOS and TS1/TS2 at
# 5.0 GT/s; then TLPs both ways with a coalesced Ack, a Nak, a replay, an Ack 300 symbol
# times after its TLP, and the Downstream Port entering L0s (EIOS, idle, FTS x4, SKP).
# Symbol k is sent at the same time on both lines until the L0s, so the time between a
# TLP on one line and an Ack on the other is a whole number of symbol times.
@link("serial_link_x1_gen2", "both lines of an x1 link: training, speed change to Gen2, Acks, Nak, replay, L0s",
      (10, "ps"), params={"rate": "auto"}, scrambled=True)
def _(down: L.SerialStream, up: L.SerialStream) -> None:
    both = (down, up)
    for st in both:
        st.lead(("x", 2 * NS))
        st.burst("gen1", rd=-1, prelock=[PRE])
    n_fts = {id(down): 24, id(up): 40}
    for st in both:                                          # Polling.Active
        st.ts("ts1", None, None, n_fts=n_fts[id(st)], rate=0x06, count=4)
    for st in both:                                          # Polling.Configuration
        st.ts("ts2", None, None, n_fts=n_fts[id(st)], rate=0x06, count=4)
    down.ts("ts1", 0, None, n_fts=24, rate=0x06, count=4)    # Configuration.Linkwidth
    up.ts("ts1", None, None, n_fts=40, rate=0x06, count=2)
    up.ts("ts1", 0, None, n_fts=40, rate=0x06, count=2)
    in_step(down, up)
    down.ts("ts1", 0, 0, n_fts=24, rate=0x06, count=4)       # Configuration.Lanenum
    up.ts("ts1", 0, None, n_fts=40, rate=0x06, count=2)
    up.ts("ts1", 0, 0, n_fts=40, rate=0x06, count=2)
    for st in both:                                          # Configuration.Complete, Idle
        st.ts("ts2", 0, 0, n_fts=n_fts[id(st)], rate=0x06, count=4)
        st.idle(6)
        st.skp()
        st.idle(2)
        credits = {"p": (32, 256), "np": (32, 0), "cpl": (0, 0)}   # (header, data); Cpl infinite
        for phase in ("initfc1", "initfc2"):
            for fc in ("p", "np", "cpl"):
                st.dllp(Dllp(f"{phase}_{fc}", hdr=credits[fc][0], data=credits[fc][1]))
    in_step(down, up)
    # L0 at 2.5 GT/s: TLP 0 down, Ack 0 up six symbol times after its END.
    down.tlp(Tlp("MWr32", 0, length_dw=1, rng_seed=201))     # 24 symbols
    down.idle(40)
    up.idle(30)
    up.dllp(Dllp("ack", seq=0))
    up.idle(26)
    in_step(down, up)
    for st in both:                                          # Recovery, speed change requested
        st.ts("ts1", 0, 0, n_fts=n_fts[id(st)], rate=0x86, count=4)
        st.ts("ts2", 0, 0, n_fts=n_fts[id(st)], rate=0x86, count=4)
        st.eios()
        st.elec_idle(("x", 6 * NS))
        st.burst("gen2", rd=1, prelock=[("data", bytes([0x00]), 1)], decide=1)
        st.eieos(1)
        st.ts("ts1", 0, 0, n_fts=n_fts[id(st)], rate=0x06, count=4)
        st.ts("ts2", 0, 0, n_fts=n_fts[id(st)], rate=0x06, count=4)
        st.idle(8)
    in_step(down, up)
    # L0 at 5.0 GT/s. Offsets from here, in symbols:
    t3 = Tlp("MWr32", 3, length_dw=1, rng_seed=204)
    down.tlp(Tlp("MWr32", 1, length_dw=1, rng_seed=202))     # 0..23
    down.tlp(Tlp("MRd32", 2, rng_seed=203))                  # 24..43
    down.idle(20)                                            # 44..63
    down.tlp(t3)                                             # 64..87
    down.idle(24)                                            # 88..111
    down.tlp(t3)                                             # 112..135, the replay
    down.idle(12)                                            # 136..147
    down.dllp(Dllp("ack", seq=0))                            # 148..155
    down.idle(20)                                            # 156..175
    up.idle(50)                                              # 0..49
    up.dllp(Dllp("ack", seq=2))                              # 50..57, covers 1 and 2
    up.idle(38)                                              # 58..95
    up.dllp(Dllp("nak", seq=2))                              # 96..103: 3 is to be replayed
    up.idle(10)                                              # 104..113
    up.tlp(Tlp("CplD", 0, length_dw=1, rng_seed=205))        # 114..137
    up.idle(6)                                               # 138..143
    up.dllp(Dllp("ack", seq=3))                              # 144..151, the replay
    up.idle(24)                                              # 152..175
    for st in both:
        st.skp()                                             # 176..179
        st.idle(8)                                           # 180..187
    in_step(down, up)
    down.tlp(Tlp("MWr32", 4, length_dw=1, rng_seed=206))     # 0..23
    down.idle(320)                                           # 24..343
    up.idle(324)                                             # 0..323
    up.dllp(Dllp("ack", seq=4))                              # 324..331, 300 symbol times after END
    up.idle(12)                                              # 332..343
    in_step(down, up)
    down.eios()                                              # L0s
    down.elec_idle(("x", 40 * NS))
    down.burst("gen2", rd=1, prelock=[("data", bytes([0x00]), 1)], decide=1)
    down.fts(4)
    down.skp()
    down.idle(4)
    down.dllp(Dllp("updatefc_p", hdr=8, data=64))
    down.idle(4)
    up.idle(40)
    up.skp()
    up.idle(20)


def make_link(fx: LinkFixture) -> dict[str, str]:
    down = L.SerialStream(scrambled=fx.scrambled)
    up = L.SerialStream(scrambled=fx.scrambled)
    fx.build(down, up)
    fpt = L.fs_per_tick(*fx.timescale)
    sides = []
    for side, st in (("dsp", down), ("usp", up)):
        st.finish()
        sides.append((side, st, L.transmit(st, fpt), L.SerialNames(scope=("tb", side), line="txp")))
    text, paths = L.write_serial_link_vcd([(ln, nm) for _, _, ln, nm in sides], fx.timescale)
    files: dict[str, str] = {}
    suffix = f"{fx.timescale[0]}{fx.timescale[1]}"
    for (side, st, line, _), path, ident in zip(sides, paths, ("!", '"')):
        samples = L.host_line_samples(text, fpt, ident)
        dedup = [s for k, s in enumerate(samples) if k == 0 or s[1] != samples[k - 1][1]]
        assert dedup == line.changes, f"{fx.name} {side}: host samples differ from the transmitted changes"
        for decoder, prefix in (("ferrite.pcie_serial", "serial_"), ("ferrite.pcie_serial_dll", "serial_dll_")):
            stem = f"{prefix}{fx.name.removeprefix('serial_')}_{side}_{suffix}"
            txs = L.expected_serial(st, line, samples, decoder, fx.params)
            files[f"{stem}.vcd"] = text
            files[f"{stem}.bindings.json"] = L.bindings_json(decoder, {"line": path}, fx.params)
            files[f"{stem}.expected.json"] = L.canonical(txs, fpt)
    return files


# ── multi-lane links (SPEC §12) ──────────────────────────────────────────────
#
# Per-lane buses data/datak, data1/datak1, ... named like a PIPE dump
# (TxData, TxDataK, TxData1, ...). The transmitter (pcie_multilane_lib.Link)
# stripes the packet layer over the logical lanes and sends every ordered set
# on every lane; the expectations apply SPEC §12 to what it put on each lane.

@dataclass
class MlFixture:
    name: str
    proves: str
    decoder: str
    n: int                      # symbols per pclk edge (PIPE width / 8)
    bound: int                  # lanes bound
    build: object
    clk: Clocking
    direction: str = "tx"
    side: dict = field(default_factory=dict)   # sideband -> lanes dumped and bound
    params: dict = field(default_factory=dict)
    scrambled: bool = False
    skew: list | None = None
    hand: dict | None = None


def multilane(name: str, proves: str, kind: str, n: int, bound: int, clk: Clocking, direction: str = "tx",
              side: dict | None = None, params: dict | None = None, scrambled: bool = False,
              skew: list | None = None, hand: dict | None = None):
    def wrap(fn):
        FIXTURES.append(MlFixture(name, proves, f"ferrite.pcie_{kind}_w{8 * n}", n, bound, fn, clk, direction,
                                  side or {}, params or {}, scrambled, skew, hand))
        return fn
    return wrap


ALL4 = [0, 1, 2, 3]


def dl_init(lk: M.Link) -> None:
    """InitFC1 then InitFC2 for P, NP and Cpl (8 symbols each)."""
    for k, h, d in (("p", 32, 1008), ("np", 32, 1), ("cpl", 0, 0)):
        lk.log.dllp(Dllp(f"initfc1_{k}", hdr=h, data=d))
    for k, h, d in (("p", 32, 1008), ("np", 32, 1), ("cpl", 0, 0)):
        lk.log.dllp(Dllp(f"initfc2_{k}", hdr=h, data=d))


# 1. Striping, x2 to x16, after training.

# x2 after Polling -> Configuration -> TS2 run: DLLPs and TLPs striped over two lanes, a SKP between them.
@multilane("pipe_x2_stripe_w8_1ns", "x2 training (TS lists), link width and lock, DLLP/TLP frames striped over 2 lanes",
           "pipe", 1, 2, Clocking((1, "ns"), period=8), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(4)
    for k, h, d in (("p", 32, 1008), ("np", 32, 1), ("cpl", 0, 0)):
        lk.log.dllp(Dllp(f"initfc1_{k}", hdr=h, data=d))
    lk.idle_rows(2)
    lk.log.tlp(Tlp("MWr32", 0, length_dw=1, rng_seed=201))
    lk.log.tlp(Tlp("MRd32", 1, rng_seed=202))
    lk.log.dllp(Dllp("ack", seq=1))
    lk.idle_rows(2)
    lk.skp()
    lk.idle_rows(3)
    lk.log.dllp(Dllp("updatefc_p", hdr=8, data=64))
    lk.idle_rows(4)


# x4, 32-bit PIPE, data changing between edges; default auto locks off at the first DLLP; nullified TLP.
@multilane("dll_x4_stripe_w32_1ps", "x4 striping on the DLL decoder: DL init, TLPs, Ack/Nak, nullified TLP; auto locks off",
           "dll", 4, 4, Clocking((1, "ps"), period=16000, change="delay", delay=3000))
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    dl_init(lk)
    lk.log.tlp(Tlp("MWr32", 0, length_dw=1, rng_seed=211))
    lk.log.tlp(Tlp("MRd64", 1, length_dw=4, tc=2, rng_seed=212))
    lk.log.tlp(Tlp("CplD", 2, length_dw=3, rng_seed=213))
    lk.log.dllp(Dllp("ack", seq=2))
    lk.idle_rows(1)
    lk.skp()
    lk.idle_rows(2)
    lk.log.tlp(Tlp("MWr64", 3, length_dw=8, tc=7, rng_seed=214))
    lk.log.tlp(Tlp("MWr32", 4, length_dw=1, rng_seed=215), lcrc="invert", end=EDB)
    lk.log.dllp(Dllp("nak", seq=3))
    lk.idle_rows(3)


# x8: a packet ending on lane 3 is followed back to back by one starting on lane 4; PAD fill to the row end.
@multilane("pipe_x8_stripe_w16_10ps", "x8 striping: back-to-back starts on lane 4, PAD fill after END, no framing errors",
           "pipe", 2, 8, Clocking((10, "ps"), period=800), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))     # one full row
    lk.log.tlp(Tlp("MRd32", 0, rng_seed=221))             # 20 symbols: ends on lane 3
    lk.log.dllp(Dllp("ack", seq=0))                       # lanes 4-7, 0-3
    lk.fill()                                             # PAD on lanes 4-7
    lk.log.tlp(Tlp("MWr32", 1, length_dw=2, rng_seed=222))  # 28 symbols: ends on lane 3
    lk.log.tlp(Tlp("MRd32", 2, rng_seed=223))             # starts on lane 4, ends on lane 7
    lk.log.dllp(Dllp("updatefc_np", hdr=4, data=40))
    lk.log.tlp(Tlp("MRd32", 3, rng_seed=224))             # ends on lane 3
    lk.fill()
    lk.idle_rows(1)
    lk.skp()
    lk.idle_rows(1)
    lk.log.dllp(Dllp("ack", seq=3))
    lk.idle_rows(2)


# x16 on a 64-bit PIPE (8 rows per edge): starts on lanes 0, 4, 8, 12; PAD fill; data changing between edges.
@multilane("dll_x16_stripe_w64_1ns", "x16 striping on the DLL decoder: starts on lanes 4k, PAD fill, 8 rows per pclk edge",
           "dll", 8, 16, Clocking((1, "ns"), period=32, change="delay", delay=5))
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(1)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))     # lanes 0-7
    lk.log.dllp(Dllp("initfc1_np", hdr=32, data=1))       # lanes 8-15
    lk.log.tlp(Tlp("MRd32", 0, rng_seed=231))             # 20: ends on lane 3
    lk.log.dllp(Dllp("initfc1_cpl"))                      # lanes 4-11
    lk.log.dllp(Dllp("ack", seq=0))                       # lanes 12-15, 0-3
    lk.log.tlp(Tlp("MWr32", 1, length_dw=1, rng_seed=232))  # 24 from lane 4: ends on lane 11
    lk.fill()                                             # PAD on lanes 12-15
    lk.log.tlp(Tlp("MWr64", 2, length_dw=8, rng_seed=233))  # 56: ends on lane 7
    lk.log.dllp(Dllp("ack", seq=2))                       # lanes 8-15
    lk.skp()
    lk.log.dllp(Dllp("updatefc_p", hdr=10, data=200))
    lk.fill()                                             # PAD on lanes 8-15
    lk.idle_rows(2)


# 2. Skew.

# Lanes delayed by 1, 0, 8 and 4 symbols (max_skew 8): one lock with those offsets; lane 0's filler is withheld data.
@multilane("pipe_x4_skew_w16_1ps", "fixed per-lane skew up to max_skew: lock with offsets 1,0,8,4; rows pair by item, not index",
           "pipe", 2, 4, Clocking((1, "ps"), period=8000, change="delay", delay=2500), params={"scrambling": "off"},
           skew=[1, 0, 8, 4])
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.log.tlp(Tlp("MWr32", 0, length_dw=1, rng_seed=241))
    lk.idle_rows(1)
    lk.skp()
    lk.log.dllp(Dllp("ack", seq=0))
    lk.idle_rows(10)


# After lock, lane 3 runs 9 symbols ahead (> max_skew 8): loss at the row, then no round ever completes again.
@multilane("pipe_x4_skew_over_w8_10ps", "skew beyond max_skew: lane_skew at the row, no re-lock, data after it withheld as an error run",
           "pipe", 1, 4, Clocking((10, "ps"), period=400), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.idle_rows(2)
    lk.skp()
    lk.log.dllp(Dllp("ack", seq=0))
    lk.idle_rows(10)
    lk.lane_drop(3, 9)
    lk.skp()
    lk.log.dllp(Dllp("ack", seq=1))
    lk.idle_rows(22)
    lk.skp()
    lk.idle_rows(12)


# 3. SKP counts per lane.

# Lane 2's elastic buffer removes a SKP (RxStatus 010) then adds one (001): SKP x3,3,2,3 and x3,3,4,3; the COM offsets
# move and return, but SKP add/remove is silent: no new lanes_aligned.
@multilane("pipe_x4_skp_counts_w32_1ns", "per-lane SKP counts in the label; SKP add/remove emits no lanes_aligned; RxStatus per lane with (lane 2)",
           "pipe", 4, 4, Clocking((1, "ns"), period=16), direction="rx", side={"rxstatus": ALL4},
           params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(4)
    lk.log.dllp(Dllp("ack", seq=0))
    lk.log.tlp(Tlp("MWr32", 0, length_dw=1, rng_seed=251))
    lk.rxstatus(2, 2)
    lk.skp([3, 3, 2, 3])
    lk.idle_rows(5)            # lane 2 is on an edge boundary again
    lk.rxstatus(2, 0)
    lk.log.dllp(Dllp("ack", seq=1))
    lk.idle_rows(2)
    lk.rxstatus(2, 1)
    lk.skp([3, 3, 4, 3])
    lk.idle_rows(3)
    lk.rxstatus(2, 0)
    lk.log.tlp(Tlp("MRd32", 1, rng_seed=252))
    lk.idle_rows(3)
    lk.skp()
    lk.idle_rows(4)


# 4. Reversed lanes.

REV_DLLP1 = Dllp("initfc1_p", hdr=32, data=1008)
REV_DLLP2 = Dllp("ack", seq=5)


def reversed_body(lk: M.Link) -> None:
    M.train(lk, order=[3, 2, 1, 0])
    lk.idle_rows(4)
    lk.log.dllp(REV_DLLP1)
    lk.idle_rows(2)
    lk.skp()
    lk.idle_rows(2)
    lk.log.dllp(REV_DLLP2)
    lk.idle_rows(4)


def _rev_rows(V: Stream, d: Dllp, idle_before: int, idle_after: int) -> None:
    """lane_order=physical on a reversed x4: each fed row is the transmitter's
    row backwards. [SDP b0 b1 b2][b3 c0 c1 END] arrives as [b2 b1 b0 SDP]
    [END c1 c0 b3]: data, a DLLP with an empty body started on lane 3, data."""
    b = d.bytes4() + L.dllp_crc_wire(d.bytes4())
    head, tail = [b[2], b[1], b[0]], [b[5], b[4], b[3]]
    lead = 0
    while head and head[0] == 0:     # leading zeros are logical idle
        head.pop(0)
        lead += 1
    V.idle(idle_before + lead)
    if head:
        V.stray(bytes(head))
    u = V.raw_dllp_bytes(b"")
    u.info["wrong_lane"] = True
    V.stray(bytes(tail))
    V.idle(idle_after)


# Lane# 3,2,1,0 on physical lanes 0-3: lane_order=auto de-stripes by Lane#: Lane order 3,2,1,0, packets intact.
@multilane("pipe_x4_reversed_w16_10ps", "reversed Lane# in TS1/TS2: Lane order 3,2,1,0, link lanes 3,2,1,0, packets decode",
           "pipe", 2, 4, Clocking((10, "ps"), period=800, change="delay", delay=300), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    reversed_body(lk)


# The same trace with lane_order=physical: rows de-striped backwards: data, SDP on lane 3, empty DLLPs.
@multilane("pipe_x4_reversed_physical_w16_10ps", "the same reversed trace with lane_order=physical: no lane_order, rows read backwards",
           "pipe", 2, 4, Clocking((10, "ps"), period=800, change="delay", delay=300),
           params={"scrambling": "off", "lane_order": "physical"},
           hand={0: lambda V: _rev_rows(V, REV_DLLP1, 16, 8), 1: lambda V: _rev_rows(V, REV_DLLP2, 8, 16)})
def _(lk: M.Link) -> None:
    reversed_body(lk)


# A SKP right after the last TS2 of a reversed link: the TS2 run still ends there (rule 1 skips SKP),
# so the link keeps Lane order 3,2,1,0 and packets decode; not a link "from SKP" in physical order.
def ts2_then_skp_body(lk: M.Link) -> None:
    M.train(lk, order=[3, 2, 1, 0])
    lk.skp()
    lk.idle_rows(4)
    lk.log.dllp(REV_DLLP1)
    lk.idle_rows(2)
    lk.skp()
    lk.idle_rows(2)
    lk.log.dllp(REV_DLLP2)
    lk.idle_rows(4)


@multilane("pipe_x4_ts2_then_skp_w16_10ps", "SKP right after the last TS2: rule 1 looks past it, Lane order 3,2,1,0 holds, packets decode",
           "pipe", 2, 4, Clocking((10, "ps"), period=800), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    ts2_then_skp_body(lk)


# The same with max_skew 0: the round settles while the SKP is still arriving, so it waits for what follows it.
@multilane("pipe_x4_ts2_then_skp_skew0_w16_10ps", "SKP after the last TS2 at max_skew 0: the TS2 round waits past the SKP, same result",
           "pipe", 2, 4, Clocking((10, "ps"), period=800), params={"scrambling": "off", "max_skew": 0})
def _(lk: M.Link) -> None:
    ts2_then_skp_body(lk)


# Lane 2 stuck on PAD in idle rows (show_idle on): one run per stretch, not one error per row; Idle does not end it,
# the DLLP does; PAD on lane 1 is a run of its own.
@multilane("pipe_x4_pad_stuck_w16_1ns", "PAD stuck on one lane: pad_misplaced runs with (N times) and occurrences; Idle does not end a run",
           "pipe", 2, 4, Clocking((1, "ns"), period=8), params={"scrambling": "off", "show_idle": True})
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    for _ in range(12):
        lk.at(2)
        lk.pad_misplaced()
        lk.row_end()
    lk.log.dllp(Dllp("ack", seq=0))
    for _ in range(3):
        lk.at(2)
        lk.pad_misplaced()
        lk.row_end()
    lk.at(1)
    lk.pad_misplaced()
    lk.row_end()
    lk.idle_rows(2)
    lk.skp()
    lk.idle_rows(2)


# Lane 2 stuck on COM for 10 rounds: as the only lane present it is the reference, so each COM COM is an Unknown
# ordered set: one run, after one lane_skew; the next SKP re-locks and the DLLP decodes.
@multilane("dll_x4_com_stuck_w8_1ns", "COM stuck on one lane: one unknown_os run with occurrences, not one error per round",
           "dll", 1, 4, Clocking((1, "ns"), period=4), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.idle_rows(2)
    for _ in range(10):
        lk.os([M.Os("unknown", raw=((COM, True), (COM, True))) if p == 2 else [(0x00, False), (0x00, False)]
               for p in range(4)])
    lk.idle_rows(10)            # more than max_skew: the last COM COM does not pair with the SKP
    lk.skp()
    lk.idle_rows(2)
    lk.log.dllp(Dllp("ack", seq=0))
    lk.idle_rows(4)


# 5. A lane loses an ordered set; a lane gated mid-packet.

# Lane 2's SKP COM arrives as EDB (decode error): loss at that row; the other lanes are at their SKP (the norm), so
# lane_skew names lane 2 and spans its EDB; data withheld from that row until the next SKP re-locks.
@multilane("pipe_x4_os_loss_w8_1ps", "a lane misses one SKP OS: lane_skew (lane 2) at the row, error unaligned run, re-lock at the next SKP",
           "pipe", 1, 4, Clocking((1, "ps"), period=4000), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.idle_rows(2)
    lk.skp(per={2: [(EDB, True), (SKP, True), (SKP, True), (SKP, True)]})
    lk.idle_rows(3)
    lk.log.dllp(Dllp("ack", seq=0))     # withheld
    lk.idle_rows(2)
    lk.skp()
    lk.log.dllp(Dllp("updatefc_p", hdr=8, data=64))
    lk.idle_rows(3)


# RxValid1 drops for two edges inside a TLP: loss at that row, the open TLP is truncated by the next COM.
@multilane("dll_x4_lane_gated_w16_1ns", "one lane gated mid-packet: lane_skew spanning its gap, TLP truncated by COM, re-lock",
           "dll", 2, 4, Clocking((1, "ns"), period=8), direction="rx", side={"rxvalid": ALL4})
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.lane_gap(1, "rxvalid", 2, after_rows=2)
    lk.log.tlp(Tlp("MWr32", 0, length_dw=4, rng_seed=261))
    lk.idle_rows(2)
    lk.skp()
    lk.log.dllp(Dllp("ack", seq=0))
    lk.idle_rows(2)


# 6. Framing on a row.

# x8: STP on lane 2 and SDP on lane 6 (still framed), PAD in an idle row and PAD opening a row: pad_misplaced.
@multilane("pipe_x8_framing_w16_1ps", "framing_lane (STP lane 2, SDP lane 6) with the frames still decoded; pad_misplaced; PAD fill",
           "pipe", 2, 8, Clocking((1, "ps"), period=8000, change="delay", delay=1000), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    lk.log.tlp(Tlp("MRd32", 0, rng_seed=271))             # ends on lane 3
    lk.log.dllp(Dllp("ack", seq=0))                       # lane 4: allowed
    lk.fill()
    lk.at(2)
    lk.log.tlp(Tlp("MWr32", 1, length_dw=1, rng_seed=272)).info["wrong_lane"] = True   # ends on lane 1
    lk.fill()
    lk.at(6)
    lk.log.dllp(Dllp("ack", seq=1)).info["wrong_lane"] = True                          # ends on lane 5
    lk.fill()
    lk.at(5)
    lk.pad_misplaced()                                    # PAD in an idle row
    lk.row_end()
    lk.log.dllp(Dllp("updatefc_p", hdr=8, data=64))       # a full row
    lk.pad_misplaced()                                    # PAD opening the next row
    lk.row_end()
    lk.skp()
    lk.idle_rows(2)


# x4 on the DLL decoder: SDP on lane 2, STP on lane 1 (decoded, framing_lane each), PAD fill after them, a misplaced PAD.
@multilane("dll_x4_framing_w8_10ps", "framing_lane and pad_misplaced on the DLL decoder (x4); auto locks off",
           "dll", 1, 4, Clocking((10, "ps"), period=400))
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.at(2)
    lk.log.dllp(Dllp("ack", seq=0)).info["wrong_lane"] = True   # ends on lane 1
    lk.fill()
    lk.at(3)
    lk.pad_misplaced()
    lk.log.tlp(Tlp("MWr32", 0, length_dw=1, rng_seed=281))
    lk.at(1)
    lk.log.tlp(Tlp("MRd32", 1, rng_seed=282)).info["wrong_lane"] = True     # ends on lane 0
    lk.fill()
    lk.idle_rows(1)
    lk.skp()
    lk.idle_rows(2)


# 7. Scrambling.

# scrambling=on: every lane has its own LFSR (reset by its COM, SKP does not advance it); TS symbols bypass.
@multilane("dll_x4_scrambled_on_w16_1ps", "scrambling=on: per-lane descramblers, SKP not advancing them, TS bypass",
           "dll", 2, 4, Clocking((1, "ps"), period=8000, change="delay", delay=2000),
           params={"scrambling": "on"}, scrambled=True)
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(4)
    dl_init(lk)
    lk.log.tlp(Tlp("MWr32", 0, length_dw=2, rng_seed=291))
    lk.idle_rows(1)
    lk.skp()
    lk.idle_rows(3)
    lk.log.tlp(Tlp("MRd32", 1, rng_seed=292))
    lk.log.dllp(Dllp("ack", seq=1))
    lk.idle_rows(2)


# auto on a scrambled x8: locks on at the first DLLP; lane-level transactions before it come out of both buffers.
@multilane("pipe_x8_auto_on_w32_1ns", "auto locks on (per-lane descrambling) on x8; lane-level transactions before the lock kept",
           "pipe", 4, 8, Clocking((1, "ns"), period=16), scrambled=True)
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.log.tlp(Tlp("MRd32", 0, rng_seed=301))
    lk.log.dllp(Dllp("ack", seq=0))
    lk.fill()
    lk.skp()
    lk.idle_rows(1)
    lk.log.tlp(Tlp("MWr32", 1, length_dw=1, rng_seed=302))
    lk.idle_rows(2)


# 8. An x4 binding trains narrower.

# Lanes 2-3 go to electrical idle (TxElecIdle2/3) after Polling: Link width x2 (4 bound), '-' in the lists.
@multilane("pipe_x4_to_x2_elecidle_w16_1ns", "x4 bound, x2 trained: unused lanes in electrical idle, '-' entries, link x2",
           "pipe", 2, 4, Clocking((1, "ns"), period=8, change="delay", delay=3), side={"txelecidle": ALL4},
           params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    lk.ts("ts1", None, None, count=2)
    lk.die(2, "txelecidle")
    lk.die(3, "txelecidle")
    M.train(lk, order=[0, 1], polling=0)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.log.tlp(Tlp("MWr32", 0, length_dw=1, rng_seed=311))
    lk.skp()
    lk.log.dllp(Dllp("ack", seq=0))
    lk.idle_rows(2)


# Lanes 1-3 carry X for the whole trace: an x1 link on lane 0; X on lanes other than the reference makes no X/Z row.
@multilane("dll_x4_to_x1_xz_w8_1ps", "x4 bound, x1 trained, lanes 1-3 X: x1 link, packets decode, no X/Z transaction",
           "dll", 1, 4, Clocking((1, "ps"), period=4000, change="delay", delay=1000))
def _(lk: M.Link) -> None:
    for p in (1, 2, 3):
        lk.die(p, "unknown")
    M.train(lk, order=[0])
    lk.idle_rows(3)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.log.tlp(Tlp("MRd32", 0, rng_seed=321))
    lk.idle_rows(1)
    lk.skp()
    lk.log.dllp(Dllp("ack", seq=0))
    lk.idle_rows(3)


# Lanes 2-3 answer Configuration with PAD Link#/Lane#: TS2 links=0,0,PAD,PAD lanes=0,1,PAD,PAD, link x2.
@multilane("pipe_x4_to_x2_pad_w32_10ps", "x4 bound, x2 trained: lanes 2-3 PAD in TS1/TS2, then ignored (their SKPs shown as -)",
           "pipe", 4, 4, Clocking((10, "ps"), period=1600), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    M.train(lk, order=[0, 1])
    lk.idle_rows(4)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.log.tlp(Tlp("MWr32", 0, length_dw=1, rng_seed=331))
    lk.idle_rows(2)
    lk.skp()
    lk.log.dllp(Dllp("ack", seq=0))
    lk.idle_rows(4)


# 9. A lane drops out during Configuration.

# Lane 3 goes idle mid-Configuration; TS2 numbers lanes 0-2: x3 is no link width, so x2 and lane 2 is cut (lane_skew).
@multilane("pipe_x4_lane_dropout_w16_10ps", "lane 3 drops out in Configuration: TS2 on 3 lanes gives x2, lane_skew for the cut lane 2",
           "pipe", 2, 4, Clocking((10, "ps"), period=800, change="delay", delay=200), side={"txelecidle": ALL4},
           params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    lk.ts("ts1", None, None, count=2)
    lk.ts("ts1", 0, None, count=1)
    lk.die(3, "txelecidle")
    lk.order = [0, 1, 2]                 # Lane# 0-2 on the lanes left
    lk.ts("ts1", "link", "order", count=2)
    lk.ts("ts2", "link", "order", count=3)
    lk.set_link([0, 1])
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.log.tlp(Tlp("MRd32", 0, rng_seed=341))
    lk.skp()
    lk.idle_rows(3)


# 10. Link#/Lane# differing across lanes.

# Configuration assigns Link# to lanes 0-1 first, then all; Lane# 0-1 first: links=0,0,PAD,PAD and no lane_mismatch.
@multilane("pipe_x4_config_lists_w8_1ns", "Link#/Lane# that differ across lanes: links=/lanes= lists, never a lane_mismatch",
           "pipe", 1, 4, Clocking((1, "ns"), period=8), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    lk.ts("ts1", None, None, count=2)
    lk.ts("ts1", [0, 0, None, None], None, count=1)
    lk.ts("ts1", 0, None, count=1)
    lk.ts("ts1", 0, [0, 1, None, None], count=1)
    lk.ts("ts1", 0, "order", count=2)
    lk.ts("ts2", 0, [0, 1, None, None], count=1)
    lk.ts("ts2", 0, "order", count=3)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.skp()
    lk.idle_rows(2)


# 11. A trace that starts in L0.

# No TS2: the packets before the first SKP are one non-error unaligned run; the first SKP decides the link.
@multilane("pipe_x4_l0_start_w32_10ps", "trace starting in L0: 'Packet data before lane alignment', Link width x4 (4 bound, from SKP)",
           "pipe", 4, 4, Clocking((10, "ps"), period=1600, change="delay", delay=500))
def _(lk: M.Link) -> None:
    lk.idle_rows(2)
    lk.log.dllp(Dllp("ack", seq=7))
    lk.log.tlp(Tlp("MWr32", 8, length_dw=1, rng_seed=351))
    lk.idle_rows(1)
    lk.skp()
    lk.idle_rows(2)
    lk.log.dllp(Dllp("updatefc_p", hdr=8, data=64))
    lk.log.tlp(Tlp("MRd32", 9, rng_seed=352))
    lk.skp()
    lk.idle_rows(3)


# The capture starts inside lane 3's first SKP OS (its COM fell before the trace): the first SKP has 3 lanes, so a
# provisional x2 (lane 2 cut by the width rule); the next SKP, on all 4 lanes, widens the link to x4.
@multilane("pipe_x4_l0_partial_skp_w16_1ns", "L0 start inside lane 3's first SKP: provisional x2 from SKP, widened to x4 at the next SKP",
           "pipe", 2, 4, Clocking((1, "ns"), period=8), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    lk.skp(per={3: [(SKP, True)] * 3})
    lk.idle_rows(12)                     # idle only while the link is the provisional x2; > max_skew apart
    lk.skp()
    lk.idle_rows(2)
    lk.log.dllp(Dllp("updatefc_p", hdr=8, data=64))
    lk.log.tlp(Tlp("MWr32", 0, length_dw=1, rng_seed=371))
    lk.skp()
    lk.idle_rows(3)


# X on lane 2 for one edge inside a TLP: loss at that row (lane 2 named), a break of kind unknown drops the TLP silently.
@multilane("pipe_x4_loss_mid_packet_w32_10ps", "loss mid-packet: lane_skew (lane 2), the open TLP dropped by the loss's unknown break, re-lock",
           "pipe", 4, 4, Clocking((10, "ps"), period=1600, change="delay", delay=400), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(4)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.lane_gap(2, "unknown", 1, after_rows=2)
    lk.log.tlp(Tlp("MWr32", 0, length_dw=4, rng_seed=381))
    lk.idle_rows(2)
    lk.skp()
    lk.log.dllp(Dllp("ack", seq=0))
    lk.idle_rows(2)


# x12: starts on lanes 0, 4 and 8, PAD fill.
@multilane("dll_x12_stripe_w16_1ps", "x12 striping: starts on lanes 0, 4 and 8, PAD fill, no framing errors",
           "dll", 2, 12, Clocking((1, "ps"), period=8000, change="delay", delay=1500))
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(1)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))     # lanes 0-7
    lk.log.tlp(Tlp("MRd32", 0, rng_seed=391))             # from lane 8: ends on lane 3
    lk.log.dllp(Dllp("ack", seq=0))                       # lanes 4-11
    lk.log.tlp(Tlp("MWr32", 1, length_dw=1, rng_seed=392))  # two full rows
    lk.log.dllp(Dllp("updatefc_p", hdr=8, data=64))       # lanes 0-7
    lk.fill()                                             # PAD on lanes 8-11
    lk.skp()
    lk.idle_rows(2)


# max_skew=0: zero skew locks; then lane 3 runs one symbol ahead and no round pairs it again.
@multilane("pipe_x4_max_skew0_w8_1ns", "max_skew=0: lock at zero skew; one symbol of skew is a loss with no re-lock",
           "pipe", 1, 4, Clocking((1, "ns"), period=4), params={"scrambling": "off", "max_skew": 0})
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.idle_rows(2)
    lk.lane_drop(3, 1)
    lk.skp()
    lk.log.dllp(Dllp("ack", seq=0))
    lk.idle_rows(3)


# max_skew=64: lanes delayed 0, 20, 40 and 64 symbols still pair TS by TS and lock with those offsets.
@multilane("pipe_x4_max_skew64_w64_1ps", "max_skew=64: skews 0,20,40,64 lock with those offsets; rows pair by item",
           "pipe", 8, 4, Clocking((1, "ps"), period=32000), params={"scrambling": "off", "max_skew": 64},
           skew=[0, 20, 40, 64])
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.log.tlp(Tlp("MRd32", 0, rng_seed=401))
    lk.skp()
    lk.log.dllp(Dllp("ack", seq=0))
    lk.idle_rows(72)


# Trained reversed, then retrained with physical Lane#: link_width and Lane order 0,1,2,3 (back to physical).
@multilane("pipe_x4_reorder_w16_1ps", "lane_order back to physical: reversed link retrained with Lane# 0-3 emits Lane order 0,1,2,3",
           "pipe", 2, 4, Clocking((1, "ps"), period=8000), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    M.train(lk, order=[3, 2, 1, 0])
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.idle_rows(2)
    M.train(lk, order=[0, 1, 2, 3], polling=0)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("ack", seq=0))
    lk.skp()
    lk.idle_rows(2)


# Coalescing compares the round's symbols 1-15 and the lists only: lane 3's Lane# alone splits a TS1 run;
# lane 2's N_FTS (not in the lists) does not split the TS2 run, it is one lane_mismatch.
@multilane("pipe_x4_coalesce_lists_w8_10ps", "coalescing with per-lane lists: a list change splits a run, another lane's N_FTS does not",
           "pipe", 1, 4, Clocking((10, "ps"), period=400), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    lk.ts("ts1", None, None, count=2)
    lk.ts("ts1", 0, "order", count=2)
    lk.ts("ts1", 0, [0, 1, 2, None], count=2)
    lk.ts("ts2", 0, "order", count=4, per={2: dict(n_fts=40)})
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.skp()
    lk.idle_rows(2)


# Width cut: TS2 numbers lanes 0-2 and lane 3 answers PAD: x3 is no link width, so x2 and lane_skew for lane 2.
@multilane("dll_x4_width_cut_w32_1ns", "width rule on the DLL decoder: TS2 on 3 of 4 lanes gives x2 and lane_skew (lane 2)",
           "dll", 4, 4, Clocking((1, "ns"), period=16, change="delay", delay=4))
def _(lk: M.Link) -> None:
    lk.ts("ts1", None, None, count=2)
    lk.order = [0, 1, 2]                 # Lane# 0-2; lane 3 keeps PAD
    lk.ts("ts1", "link", None, count=1)
    lk.ts("ts1", "link", "order", count=2)
    lk.ts("ts2", "link", "order", count=3)
    lk.set_link([0, 1])
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.log.tlp(Tlp("MWr32", 0, length_dw=1, rng_seed=411))
    lk.skp()
    lk.idle_rows(2)


# L0 start, lanes 2-3 12 symbols late (> max_skew 8): the first SKP gives x2 from SKP; lanes 2-3's own first SKP
# opens no round and is dropped (only link lanes open rounds, §12.4). 600 idle rows later the lanes have caught up
# and a SKP on all four lanes at one index widens the link to x4; packets then decode at x4.
@multilane("pipe_x4_l0_widen_late_skp_w32_10ps", "provisional x2 from SKP; late outside SKPs open no round; a later 4-lane SKP widens to x4",
           "pipe", 4, 4, Clocking((10, "ps"), period=1600), params={"scrambling": "off"}, skew=[0, 0, 12, 12])
def _(lk: M.Link) -> None:
    lk.idle_rows(2)
    lk.skp()
    lk.idle_rows(600)                    # idle only while the link is the provisional x2
    lk.lane_drop(2, 12)
    lk.lane_drop(3, 12)
    lk.skp()
    lk.idle_rows(1)
    lk.log.dllp(Dllp("updatefc_p", hdr=8, data=64))
    lk.log.tlp(Tlp("MWr32", 0, length_dw=1, rng_seed=431))
    lk.log.dllp(Dllp("ack", seq=0))
    lk.skp()
    lk.idle_rows(3)                      # end on an edge boundary


# L0 start with lane 0 (the reference lane) the latest by 6 (max_skew 8): 5 filler symbols, then X for one edge right
# before its first SKP. Its filler and that gap come before the round opened by lanes 1-3 (§12.7); offsets 6,0,0,0.
@multilane("pipe_x4_l0_ref_late_w8_1ps", "L0 start, reference lane 6 late: its gap before its SKP reaches the pipeline before the round",
           "pipe", 1, 4, Clocking((1, "ps"), period=4000, change="delay", delay=700), skew=[5, 0, 0, 0])
def _(lk: M.Link) -> None:
    lk.idle_rows(2)
    lk.log.dllp(Dllp("ack", seq=3))
    lk.idle_rows(1)
    lk.lane_pause(0, "unknown", 1)
    lk.skp()
    lk.idle_rows(2)
    lk.log.dllp(Dllp("updatefc_p", hdr=8, data=64))
    lk.log.tlp(Tlp("MRd32", 4, rng_seed=421))
    lk.idle_rows(8)


# lane_mismatch: N_FTS on lane 2, training control on lane 1, a TS1 among TS2s on lane 1, rate id on lane 3; each once.
@multilane("pipe_x4_lane_mismatch_w16_1ps", "lane_mismatch for N_FTS, training control, type and rate id, each reported once",
           "pipe", 2, 4, Clocking((1, "ps"), period=8000), params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    lk.ts("ts1", None, None, count=3, per={2: dict(n_fts=40)})
    lk.ts("ts1", "link", None, count=1, per={1: dict(ctrl=0x01)})
    lk.ts("ts1", "link", "order", count=2)
    lk.ts("ts2", "link", "order", count=1, per={1: dict(kind="ts1")})
    lk.ts("ts2", "link", "order", count=3, per={3: dict(rate=0x06)})
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    lk.skp()
    lk.idle_rows(2)


# Common breaks: every lane's RxValid low together truncates a TLP; X on every lane drops a DLLP; both re-lock at a SKP.
@multilane("pipe_x4_common_break_w16_1ns", "common break (all lanes gated, then all X): truncated by rxvalid, X/Z (lane 0), re-lock",
           "pipe", 2, 4, Clocking((1, "ns"), period=8, change="delay", delay=2), direction="rx",
           side={"rxvalid": ALL4}, params={"scrambling": "off"})
def _(lk: M.Link) -> None:
    M.train(lk)
    lk.idle_rows(2)
    lk.log.dllp(Dllp("initfc1_p", hdr=32, data=1008))
    t = Tlp("MWr32", 0, length_dw=4, rng_seed=361).bytes()
    lk.log.truncated("tlp", (bytes([0x00, 0x00]) + t)[:15], by="rxvalid")    # STP + 15 = 4 rows = 2 edges
    lk.brk("rxvalid", 3)
    lk.idle_rows(2)
    lk.skp()
    lk.log.dllp(Dllp("ack", seq=0))
    lk.idle_rows(1)
    lk.log.dropped("dllp", bytes([0x00, 0x00, 0x00]))                          # SDP + 3 = 1 row
    lk.brk("unknown", 2)
    lk.idle_rows(2)
    lk.skp()
    lk.log.dllp(Dllp("ack", seq=1))
    lk.idle_rows(2)


def make_ml(fx: MlFixture) -> dict[str, str]:
    lk = M.Link(fx.bound, fx.n, fx.scrambled, fx.skew)
    fx.build(lk)
    lay = lk.finish()
    names = M.vcd_names(fx.bound, fx.direction, fx.side)
    for p in range(fx.bound):
        for e in lay.edges[p]:
            if e.kind == "gap" and e.gap != "unknown":
                assert p in fx.side.get(e.gap, []), f"lane {p} gated by unbound {e.gap}{p}"
            if e.rxstatus:
                assert p in fx.side.get("rxstatus", []), f"rxstatus{p} not bound"
    tr = M.write_vcd(lay, fx.clk, names)
    M.check_host(lay, tr, fx.clk, names, fx.name)
    _, tfs, pfs = M.edge_times(fx.clk, lay.count)
    txs = M.expected(lk, lay, tfs, pfs, fx.decoder, fx.params, fx.side.get("rxstatus", []), fx.hand)
    paths = {s: tr.paths[s] for s, _ in names}
    fpt = L.fs_per_tick(*fx.clk.timescale)
    return {f"{fx.name}.vcd": tr.text,
            f"{fx.name}.bindings.json": L.bindings_json(fx.decoder, paths, fx.params),
            f"{fx.name}.expected.json": L.canonical(txs, fpt)}


# ── generation ───────────────────────────────────────────────────────────────

def make_serial(fx: SerialFixture) -> dict[str, str]:
    st = L.SerialStream(scrambled=fx.scrambled)
    fx.build(st)
    st.finish()
    fpt = L.fs_per_tick(*fx.timescale)
    line = L.transmit(st, fpt)
    invert = fx.params.get("invert", False)
    text, path = L.write_serial_vcd(line, fx.timescale, invert, fx.names)
    # The host model must see exactly the changes the transmitter laid out.
    samples = L.host_line_samples(text, fpt)
    dedup = [s for k, s in enumerate(samples) if k == 0 or s[1] != samples[k - 1][1]]
    vcd_v = [(t, L._flip(v) if invert and v in "01" else v) for t, v in line.changes]
    assert dedup == vcd_v, f"{fx.name}: host samples differ from the transmitted changes"
    txs = L.expected_serial(st, line, samples, fx.decoder, fx.params)
    return {f"{fx.name}.vcd": text,
            f"{fx.name}.bindings.json": L.bindings_json(fx.decoder, {"line": path}, fx.params),
            f"{fx.name}.expected.json": L.canonical(txs, fpt)}


def make(fx: Fixture) -> dict[str, str]:
    if isinstance(fx, SerialFixture):
        return make_serial(fx)
    if isinstance(fx, LinkFixture):
        return make_link(fx)
    if isinstance(fx, MlFixture):
        return make_ml(fx)
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
