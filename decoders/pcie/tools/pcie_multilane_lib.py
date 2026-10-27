# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Multi-lane (x2 to x16) side of the PCIe PIPE / DLL golden fixtures.

ENCODER direction, like pcie_fixture_lib.py, and written from SPEC.md §12
(plus §4-§10) and protocol-notes.md §8 only. Nothing here reads, or is
derived from, the C decoders.

- `Link` is a transmitter for a link of up to 16 lanes: a packet layer (an
  x1 `Stream` of data units, in the order the pipeline must see them) striped
  round-robin over the logical lanes, a copy of every ordered set on every
  lane (with per-lane Link#/Lane#, SKP counts, N_FTS ...), per-lane
  scrambling (each lane its own LFSR, reset by its own COM, not advanced by
  SKP), per-lane skew, dropped or substituted symbols, lane gating and X/Z,
  and lanes that die or stay outside the link.
- `Layout` lays each lane out on its own PIPE bus (N symbols per pclk edge,
  gaps on whole edges), and cuts each lane into the items of SPEC §12.3. The
  items are taken from what the transmitter put on the lane, never parsed
  from the bus.
- `LaneModel` applies SPEC §12.4-§12.13 (rounds, the link, alignment, rows,
  breaks, unaligned runs) to those items.
- `build_view` turns what the lane layer feeds the pipeline back into an x1
  unit list (from the transmitter's own units); the PIPE / DLL expectation
  rules of pcie_fixture_lib then apply, with §12.9-§12.10 on top.
- `write_vcd` writes per-lane buses `data`, `datak`, `data1`, `datak1`, ...
"""

from __future__ import annotations

from dataclasses import dataclass, field
from types import SimpleNamespace

import pcie_fixture_lib as L
from pcie_fixture_lib import (COM, EDB, EIE, END, FTS, IDL, PAD, SDP, SKP, STP, TS1_ID, TS2_ID,
                              Lfsr, Stream, Sym, Tx, Unit, h2)

LINK_WIDTHS = (1, 2, 4, 8, 12, 16)

# SPEC §12.12 error sentences.
ML_ERROR_TEXT = {
    "lane_skew": "The lane did not line up with the other lanes of the link.",
    "lane_mismatch": "The lanes of one link sent different ordered sets.",
    "framing_lane": "A packet starts on lane 0 (x2, x4) or on a lane that is a multiple of 4 (x8 and wider).",
    "pad_misplaced": "PAD outside a packet is valid only as fill after END or EDB.",
    "unaligned": "Packet data after lane alignment was lost is not decoded.",
}

TS2_SKP_LOOKAHEAD = 16               # §12.5 rule 1: SKP OSs looked past after a TS2

TYPE_NAME = {"ts1": "TS1", "ts2": "TS2", "skp": "SKP", "fts": "FTS", "eios": "EIOS", "eieos": "EIEOS",
             "unknown": "unknown"}


# ── 1. Ordered sets and lane symbols ─────────────────────────────────────────

@dataclass(frozen=True)
class Os:
    """One lane's copy of an ordered set (protocol-notes §2)."""
    kind: str                 # ts1 ts2 skp fts eios eieos unknown
    link: int | None = None   # TS: Link#, None = PAD
    lane: int | None = None   # TS: Lane#, None = PAD
    n_fts: int = 24
    rate: int = 0x02
    ctrl: int = 0x00
    skp: int = 3              # SKP OS: SKP symbols (1-5)
    raw: tuple = ()           # unknown: COM, then symbols ending with the misfit (SPEC §12.3)

    def symbols(self) -> list[tuple[int, bool]]:
        if self.kind == "unknown":
            assert self.raw and self.raw[0] == (COM, True)
            return list(self.raw)
        if self.kind in ("ts1", "ts2"):
            ident = TS1_ID if self.kind == "ts1" else TS2_ID
            body = [(PAD, True) if v is None else (v, False) for v in (self.link, self.lane)]
            body += [(self.n_fts, False), (self.rate, False), (self.ctrl, False)] + [(ident, False)] * 10
            return [(COM, True)] + body
        if self.kind == "skp":
            assert 1 <= self.skp <= 5
            return [(COM, True)] + [(SKP, True)] * self.skp
        if self.kind == "fts":
            return [(COM, True)] + [(FTS, True)] * 3
        if self.kind == "eios":
            return [(COM, True)] + [(IDL, True)] * 3
        if self.kind == "eieos":
            return [(COM, True)] + [(EIE, True)] * 14 + [(TS1_ID, False)]
        raise ValueError(self.kind)

    def key(self) -> tuple:
        """Symbols 1-15 of a TS (what coalescing compares, SPEC §7)."""
        return tuple(self.symbols()[1:])


@dataclass(eq=False)
class LSym:
    """One symbol a transmitter puts on one lane."""
    byte: int                 # plain value
    k: bool
    origin: tuple             # ("row", logical index) | ("os", uid) | ("fill",) | ("out",) | ("sub",)
    os_sym: bool = False      # ordered-set symbol: sent unscrambled (§10.3)
    lost: bool = False        # sent, but a lane gap replaced it (never received)
    wire: int = -1
    key: int = -1             # receiver key (-1 for COM and SKP)
    before_com: bool = True   # receiver has not seen a COM on this lane yet
    index: int = -1           # SPEC §12.2 symbol index


@dataclass(eq=False)
class GapE:
    """Gap edges on one lane: inserted (common break) or replacing the
    lane's next `edges * N` symbols (`replace`)."""
    kind: str                 # rxvalid, txelecidle, unknown
    edges: int
    bus: list | None = None   # per edge (data, datak); None = default junk / X
    replace: bool = False


@dataclass(eq=False)
class RxMark:
    code: int


@dataclass(eq=False)
class DeadMark:
    kind: str


@dataclass
class LaneEdge:
    kind: str                 # data, gap
    syms: list = field(default_factory=list)
    gap: str = ""
    bus: object = None
    rxstatus: int = 0


# ── 2. The transmitter ───────────────────────────────────────────────────────

class Link:
    """A transmitter for B bound lanes, N symbols per pclk edge.

    `self.log` is the packet layer: an x1 Stream holding the data units in
    the order the receiver's pipeline must see them (no ordered sets: those
    are per lane). `order` is the transmitter's striping, logical lane ->
    physical lane; lanes outside it send logical idle in data rows."""

    def __init__(self, bound: int, n: int, scrambled: bool = False, skew: list[int] | None = None) -> None:
        self.B, self.n, self.scrambled = bound, n, scrambled
        self.log = Stream(1)
        self.lanes: list[list] = [[] for _ in range(bound)]
        self.os_inst: dict[int, tuple[int, Os]] = {}
        self.order: list[int] = list(range(bound))
        self.dead: set[int] = set()
        self.flushed = 0
        self.gap_reqs: list[tuple[int, int, GapE]] = []
        for p, s in enumerate(skew or []):
            for _ in range(s):                       # SPEC §12.2: lane p delayed by s symbols
                self.lanes[p].append(LSym(0x00, False, ("fill",)))

    # rows
    @property
    def w(self) -> int:
        return len(self.order)

    def row_pos(self) -> int:
        return (len(self.log.syms) - self.flushed) % self.w

    def flush(self) -> None:
        """Stripe the pending packet-layer symbols: symbol i of a row goes to
        logical lane i (protocol-notes §8)."""
        syms = self.log.syms
        pend = len(syms) - self.flushed
        if not pend:
            return
        w = self.w
        assert pend % w == 0, f"{pend} symbols do not fill whole x{w} rows"
        for r in range(pend // w):
            for pos in range(w):
                li = self.flushed + r * w + pos
                s = syms[li]
                self.lanes[self.order[pos]].append(LSym(s.byte, s.k, ("row", li)))
            for q in range(self.B):
                if q not in self.order and q not in self.dead:
                    self.lanes[q].append(LSym(0x00, False, ("out",)))
        self.flushed = len(syms)

    def set_link(self, order: list[int]) -> None:
        self.flush()
        assert len(order) in LINK_WIDTHS
        self.order = list(order)

    def at(self, pos: int) -> None:
        """Logical idle up to row position `pos`."""
        n = (pos - self.row_pos()) % self.w
        if n:
            self.log.idle(n)

    def row_end(self) -> None:
        self.at(0)

    def idle_rows(self, rows: int) -> None:
        self.log.idle(rows * self.w)

    def fill(self) -> None:
        """PAD fill after END/EDB to the end of the row (protocol-notes §8)."""
        n = (-self.row_pos()) % self.w
        if not n:
            return
        last = self.log.syms[-1]
        assert last.k and last.byte in (END, EDB)
        a = len(self.log.syms)
        for _ in range(n):
            self.log._put(PAD, True)
        self.log._unit("pad_fill", a)

    def pad_misplaced(self) -> None:
        a = self.log._put(PAD, True)
        self.log._unit("pad_misplaced", a)

    # ordered sets
    def os(self, per: list) -> None:
        """One ordered set on every live lane: per[p] is an Os, or a list of
        (byte, k) sent instead as plain symbols (a corrupted ordered set)."""
        self.flush()
        for p in range(self.B):
            if p in self.dead:
                continue
            v = per[p]
            if isinstance(v, Os):
                uid = len(self.os_inst)
                self.os_inst[uid] = (p, v)
                for b, k in v.symbols():
                    self.lanes[p].append(LSym(b, k, ("os", uid), os_sym=True))
            else:
                assert isinstance(v, list)
                for b, k in v:
                    self.lanes[p].append(LSym(b, k, ("sub",)))

    def _lane_val(self, v: object, p: int, what: str) -> object:
        if isinstance(v, (list, tuple)):
            return v[p]
        if v == "order":                              # Lane# = logical lane
            return self.order.index(p) if p in self.order else None
        if v == "link" or (what == "link" and isinstance(v, int)):
            num = 0 if v == "link" else v
            return num if p in self.order else None
        return v

    def ts(self, kind: str = "ts1", link: object = None, lane: object = None, count: int = 1,
           n_fts: int = 24, rate: int = 0x02, ctrl: int = 0x00, per: dict | None = None) -> None:
        for _ in range(count):
            vs = []
            for p in range(self.B):
                o = dict(kind=kind, link=self._lane_val(link, p, "link"), lane=self._lane_val(lane, p, "lane"),
                         n_fts=n_fts, rate=rate, ctrl=ctrl)
                if per and p in per:
                    o.update(per[p])
                vs.append(Os(**o) if "sub" not in o else o["sub"])
            self.os(vs)

    def skp(self, counts: object = 3, per: dict | None = None) -> None:
        vs = []
        for p in range(self.B):
            c = counts[p] if isinstance(counts, (list, tuple)) else counts
            v = per[p] if per and p in per else Os("skp", skp=c)
            vs.append(v)
        self.os(vs)

    def simple_os(self, kind: str, count: int = 1) -> None:
        for _ in range(count):
            self.os([Os(kind) for _ in range(self.B)])

    # lane events
    def brk(self, kind: str, edges: int, bus: list | None = None) -> None:
        """A common break: every live lane gated (or X/Z) on the same edges."""
        self.flush()
        for p in range(self.B):
            if p not in self.dead:
                assert self.syms_on(p) % self.n == 0, f"break on lane {p} is not at an edge boundary"
                self.lanes[p].append(GapE(kind, edges, bus))

    def lane_gap(self, p: int, kind: str, edges: int, after_rows: int = 0, bus: list | None = None) -> None:
        """Lane p alone is gated (or X/Z) for `edges` edges starting `after_rows`
        rows from here; the gap replaces the symbols it covers."""
        self.flush()
        self.gap_reqs.append((p, len(self.lanes[p]) + after_rows, GapE(kind, edges, bus, replace=True)))

    def lane_pause(self, p: int, kind: str, edges: int, bus: list | None = None) -> None:
        """Lane p alone is gated (or X/Z) for `edges` edges here; nothing is
        lost, so the lane falls `edges * N` symbols further behind."""
        self.flush()
        assert self.syms_on(p) % self.n == 0, f"pause on lane {p} is not at an edge boundary"
        self.lanes[p].append(GapE(kind, edges, bus))

    def lane_drop(self, p: int, count: int) -> None:
        """Lane p loses its last `count` symbols (logical idle) before what
        comes next: it runs `count` symbols ahead from here on."""
        self.flush()
        for _ in range(count):
            s = self.lanes[p].pop()
            assert isinstance(s, LSym) and self._droppable(s), s

    def rxstatus(self, p: int, code: int) -> None:
        self.flush()
        self.lanes[p].append(RxMark(code))

    def die(self, p: int, kind: str) -> None:
        """Lane p is gated (or X) from here to the end of the trace."""
        self.flush()
        self.lanes[p].append(DeadMark(kind))
        self.dead.add(p)

    def syms_on(self, p: int) -> int:
        return sum(1 for e in self.lanes[p] if isinstance(e, LSym))

    def _droppable(self, s: LSym) -> bool:
        if s.origin[0] in ("fill", "out"):
            return True
        if s.origin[0] == "row":
            u = self.unit_of[s.origin[1]] if hasattr(self, "unit_of") else self._unit_at(s.origin[1])
            return u.kind == "idle"
        return False

    def _unit_at(self, li: int) -> Unit:
        for u in self.log.units:
            if u.a <= li <= u.b:
                return u
        raise KeyError(li)

    # finish: scramble per lane, lay out on edges
    def finish(self) -> "Layout":
        self.flush()
        self.unit_of: dict[int, Unit] = {}
        for u in self.log.units:
            for i in range(u.a, u.b + 1):
                self.unit_of[i] = u
        assert sorted(self.unit_of) == list(range(len(self.log.syms))), "packet-layer units must cover every symbol"
        for p, at, g in sorted(self.gap_reqs, key=lambda r: (r[0], -r[1])):
            self.lanes[p].insert(at, g)
        for p in range(self.B):
            # A replacing gap swallows the next edges*N symbols of its lane.
            lost = 0
            for e in self.lanes[p]:
                if isinstance(e, GapE) and e.replace:
                    lost = e.edges * self.n
                elif isinstance(e, LSym) and lost:
                    assert not e.os_sym, "a lane gap may not cut an ordered set"
                    e.lost = True
                    lost -= 1
            assert lost == 0
            self._scramble(p)
        return Layout(self)

    def _scramble(self, p: int) -> None:
        """protocol-notes §3 per lane (§8: one LFSR per lane). The transmitter
        LFSR runs over every symbol it sent; the receiver's over what arrived."""
        syms = [e for e in self.lanes[p] if isinstance(e, LSym)]
        lf = Lfsr()
        seen = False
        for s in syms:
            if s.k and s.byte == COM:
                lf.reset()
                seen = True
                key = -1
            elif s.k and s.byte == SKP:
                key = -1
            else:
                key = lf.key()
            scr = self.scrambled and not s.k and not s.os_sym and seen
            s.wire = s.byte ^ key if scr else s.byte
        lf = Lfsr()
        seen = False
        for s in syms:
            if s.lost:
                continue
            if s.k and s.byte == COM:
                lf.reset()
                seen = True
                s.key = -1
                s.before_com = False
            elif s.k and s.byte == SKP:
                s.key = -1
                s.before_com = not seen
            else:
                s.key = lf.key()
                s.before_com = not seen


class Layout:
    """Each lane on its own PIPE bus: N symbols per edge, gaps on whole
    edges; all lanes cut to the same edge count."""

    def __init__(self, lk: Link) -> None:
        self.lk = lk
        self.n, self.B = lk.n, lk.B
        n = self.n
        per: list[list[LaneEdge]] = []
        leftovers: list[list[LSym]] = []
        dead_kind: dict[int, str] = {}
        for p in range(self.B):
            out: list[LaneEdge] = []
            buf: list[LSym] = []
            rx = 0
            for e in lk.lanes[p]:
                if isinstance(e, RxMark):
                    assert not buf, f"rxstatus change on lane {p} is not at an edge boundary"
                    rx = e.code
                elif isinstance(e, DeadMark):
                    assert not buf, f"lane {p} dies mid-edge"
                    dead_kind[p] = e.kind
                    break
                elif isinstance(e, GapE):
                    assert not buf, f"gap on lane {p} is not at an edge boundary"
                    for t in range(e.edges):
                        out.append(LaneEdge("gap", gap=e.kind, bus=e.bus[t] if e.bus else None, rxstatus=rx))
                elif not e.lost:
                    buf.append(e)
                    if len(buf) == n:
                        out.append(LaneEdge("data", syms=buf, rxstatus=rx))
                        buf = []
            per.append(out)
            leftovers.append(buf)
        live = [p for p in range(self.B) if p not in dead_kind]
        count = min(len(per[p]) for p in live)
        for p in range(self.B):
            if p in dead_kind:
                assert len(per[p]) <= count
                rx = per[p][-1].rxstatus if per[p] else 0
                while len(per[p]) < count:
                    per[p].append(LaneEdge("gap", gap=dead_kind[p], rxstatus=rx))
                continue
            # SPEC §12.13: the trace ends; the tail cut here must be logical idle.
            for e in per[p][count:]:
                assert e.kind == "data" and all(lk._droppable(s) for s in e.syms), (p, e)
            assert all(lk._droppable(s) for s in leftovers[p]), p
            per[p] = per[p][:count]
        self.count = count
        self.edges = per
        self.dead_kind = dead_kind

    def items(self) -> list[list["Item"]]:
        """SPEC §12.3, from what the transmitter put on each lane."""
        n = self.n
        out: list[list[Item]] = []
        for p in range(self.B):
            its: list[Item] = []
            cur: Item | None = None

            def close() -> None:
                nonlocal cur
                if cur is not None:
                    want = len(cur.os.symbols())
                    assert len(cur.syms) == want and cur.b - cur.a + 1 == want, \
                        f"lane {p}: ordered set at {cur.a} cut short (avoided, §12.3)"
                    cur = None

            for k, e in enumerate(self.edges[p]):
                if e.kind == "gap":
                    close()
                    if its and its[-1].kind == "gap" and its[-1].gap == e.gap and its[-1].e2 == k - 1:
                        its[-1].e2 = k
                        its[-1].b = k * n + n - 1
                    else:
                        its.append(Item("gap", p, k * n, k * n + n - 1, gap=e.gap, e1=k, e2=k))
                    continue
                for j, s in enumerate(e.syms):
                    i = k * n + j
                    s.index = i
                    if s.os_sym:
                        uid = s.origin[1]
                        if cur is not None and cur.uid == uid:
                            cur.syms.append(s)
                            cur.b = i
                        else:
                            close()
                            cur = Item("os", p, i, i, os=self.lk.os_inst[uid][1], syms=[s], uid=uid)
                            its.append(cur)
                    else:
                        close()
                        assert not (s.k and s.byte == COM), "a COM outside an ordered set (avoided)"
                        prev = its[-1] if its else None
                        if s.k and s.byte == SKP and prev is not None and prev.kind == "os":
                            assert not (prev.os.kind == "skp" and prev.os.skp < 5), \
                                "a SKP right after a SKP OS would extend it (avoided)"
                        its.append(Item("row", p, i, i, syms=[s]))
            close()
            out.append(its)
        return out


# ── 3. Times ─────────────────────────────────────────────────────────────────

def edge_times(clk: L.Clocking, count: int) -> tuple[list[int], list[int], list[int]]:
    """(tick times, fs times, P_k in fs) of the rising edges, as L.write_vcd
    places them."""
    periods = clk.periods(count)
    t0 = clk.first_edge if clk.first_edge is not None else periods[0]
    times = [t0]
    for k in range(1, count):
        times.append(times[-1] + periods[k - 1])
    fpt = L.fs_per_tick(*clk.timescale)
    tfs = [t * fpt for t in times]
    pfs = [0] + [(times[k] - times[k - 1]) * fpt for k in range(1, count)]
    return times, tfs, pfs


class LaneTiming:
    """SPEC §4.2 / §12.2: symbol index i = k*N + j has time t_k + floor(j*P_k/N)."""

    def __init__(self, n: int, tfs: list[int], pfs: list[int]) -> None:
        self.n, self.tfs, self.pfs = n, tfs, pfs

    def sym(self, i: int) -> tuple[int, int]:
        k, j = divmod(i, self.n)
        return self.tfs[k] + (j * self.pfs[k]) // self.n, self.pfs[k] // self.n

    def span(self, a: int, b: int) -> tuple[int, int]:
        s, _ = self.sym(a)
        sb, db = self.sym(b)
        return s, sb + db

    def edge_span(self, e1: int, e2: int) -> tuple[int, int]:
        return self.span(e1 * self.n, e2 * self.n + self.n - 1)


# ── 4. The lane layer (SPEC §12.3-§12.8, §12.11-§12.13) ──────────────────────

@dataclass(eq=False)
class Item:
    kind: str                 # os, row, gap
    lane: int
    a: int
    b: int
    os: Os | None = None
    syms: list = field(default_factory=list)
    gap: str = ""
    e1: int = -1
    e2: int = -1
    uid: int = -1


@dataclass
class FeedRound:
    ref: int
    item: Item
    links: str
    lanes: str
    skps: str


@dataclass
class FeedRow:
    items: list
    lanes: list
    start: int
    end: int


@dataclass
class FeedBreak:
    kind: str
    item: Item


class LaneModel:
    def __init__(self, items: list[list[Item]], bound: int, tm: LaneTiming, max_skew: int,
                 lane_order: str) -> None:
        self.items, self.B, self.tm, self.W, self.lane_order = items, bound, tm, max_skew, lane_order
        self.ptr = [0] * bound
        self.link: list[int] | None = None
        self.rule1 = False                        # decided by a TS2 run (rule 1)
        self.aligned = False
        self.order_state: tuple | None = None    # None = physical
        self.last_al: tuple | None = None
        self.skew_since = False                   # lane_skew since the link was decided or locked
        self.had_skew = False                     # lane_skew since the last lock (or the start)
        self.run: dict | None = None
        self.mm_prev: dict[int, tuple | None] = {}
        self.txs: list[tuple[Tx, bool]] = []      # (tx, both decoders)
        self.xz: list[Tx] = []
        self.feed: list = []

    # helpers
    def peek(self, p: int) -> Item | None:
        its = self.items[p]
        return its[self.ptr[p]] if self.ptr[p] < len(its) else None

    def next_os(self, p: int) -> int | None:
        its = self.items[p]
        i = self.ptr[p]
        while i < len(its) and its[i].kind != "os":
            i += 1
        return i if i < len(its) else None

    def considered(self) -> list[int]:
        """§12.4: the link's lanes once rule 1 decided it, else every lane."""
        return list(self.link) if self.rule1 else list(range(self.B))

    def openers(self) -> list[int]:
        """§12.4: lanes that open rounds: the link's once decided, else the considered lanes."""
        return list(self.link) if self.link is not None else self.considered()

    def ref_outside(self) -> int:
        return self.link[0] if self.link is not None else 0

    def span_items(self, its: list[Item]) -> tuple[int, int]:
        ss = [self.tm.span(i.a, i.b) for i in its]
        return min(s for s, _ in ss), max(e for _, e in ss)

    def emit(self, span: tuple[int, int], label: str, err: bool, fields: list, both: bool) -> None:
        self.txs.append((Tx(span[0], span[1], label, err, fields), both))

    def lane_skew(self, p: int, span: tuple[int, int]) -> None:
        self.end_run()
        self.emit(span, f"Lanes not aligned (lane {p})", True,
                  [("type", "lane_skew"), ("lane", p), ("error", ML_ERROR_TEXT["lane_skew"])], True)
        self.had_skew = True
        self.skew_since = True

    def withhold(self, it: Item) -> None:
        if self.run is None:
            self.run = dict(first=it, last=it, count=1, err=self.had_skew)
        else:
            self.run["last"] = it
            self.run["count"] += 1

    def end_run(self) -> None:
        r = self.run
        self.run = None
        if r is None or r["count"] < 1:
            return
        span = self.tm.span(r["first"].a, r["last"].b)
        if r["err"]:
            self.emit(span, "Packet data after lane alignment lost", True,
                      [("type", "unaligned"), ("symbols", r["count"]), ("error", ML_ERROR_TEXT["unaligned"])], True)
        else:
            self.emit(span, "Packet data before lane alignment", False,
                      [("type", "unaligned"), ("symbols", r["count"])], True)

    def gap_break(self, it: Item) -> None:
        self.feed.append(FeedBreak(it.gap, it))
        if it.gap == "unknown":
            s, e = self.tm.edge_span(it.e1, it.e2)
            self.xz.append(Tx(s, e, "X/Z on data", True,
                              [("type", "unknown_value"), ("edges", it.e2 - it.e1 + 1), ("lane", it.lane),
                               ("error", L.ERROR_TEXT["unknown_value"])]))

    def consume_unaligned(self, p: int, it: Item) -> None:
        if p != self.ref_outside():
            return
        if it.kind == "row":
            self.withhold(it)
        else:
            self.gap_break(it)

    def pending(self, c: int | None) -> None:
        for p in self.considered():
            while True:
                it = self.peek(p)
                if it is None or it.kind == "os" or (c is not None and it.a >= c):
                    break
                self.consume_unaligned(p, it)
                self.ptr[p] += 1

    def skip_outside(self) -> None:
        """Considered lanes outside a SKP-decided link take part in rounds
        only: their other items produce nothing."""
        if self.link is None or self.rule1:
            return
        for p in self.considered():
            if p in self.link:
                continue
            while self.peek(p) is not None and self.peek(p).kind != "os":
                self.ptr[p] += 1

    # main loop
    def run_model(self) -> None:
        while True:
            if self.aligned:
                if self.rows() == "end":
                    break
            self.skip_outside()
            cand = []
            for p in self.openers():
                oi = self.next_os(p)
                if oi is not None:
                    cand.append((self.items[p][oi].a, p))
            c = min(cand)[0] if cand else None
            if not self.aligned:
                self.pending(c)
            if c is None:
                break
            self.round(c)
        self.end_run()

    def rows(self) -> str:
        link = self.link
        while True:
            its = [self.peek(p) for p in link]
            if all(it is not None and it.kind == "os" for it in its):
                return "os"
            if any(it is None for it in its):
                # §12.13: rows some lane cannot complete are dropped; a gap of
                # the reference lane in them still breaks (and gives X/Z).
                for p in link:
                    assert self.next_os(p) is None, "ordered set after an incomplete final row (avoided)"
                while self.peek(link[0]) is not None:
                    if self.peek(link[0]).kind == "gap":
                        self.gap_break(self.peek(link[0]))
                    self.ptr[link[0]] += 1
                return "end"
            kinds = [it.kind if it.kind != "gap" else "gap:" + it.gap for it in its]
            if all(k == "row" for k in kinds):
                ss = [self.tm.span(it.a, it.b) for it in its]
                self.feed.append(FeedRow(list(its), list(link), min(s for s, _ in ss), max(e for _, e in ss)))
                for p in link:
                    self.ptr[p] += 1
                continue
            if kinds[0].startswith("gap:") and len(set(kinds)) == 1:
                # Common break (§12.6): pipeline break, no longer aligned, no error.
                self.gap_break(its[0])
                for p in link:
                    self.ptr[p] += 1
                self.aligned = False
                return "break"
            # Loss at row k (§12.6): the kind most link lanes have is the norm
            # ("os" = no k-th item); on a tie, the tied kind first in logical order.
            tally = {k: kinds.count(k) for k in kinds}
            top = max(tally.values())
            norm = next(k for k in kinds if tally[k] == top)
            named = next(i for i in range(len(link)) if kinds[i] != norm)
            if its[named].kind != "os":
                target = its[named]
            else:
                target = next(its[i] for i in range(len(link)) if kinds[i] == norm)
            self.aligned = False
            self.lane_skew(link[named], self.tm.span(target.a, target.b))
            self.feed.append(FeedBreak("unknown", None))     # a loss is an unknown break
            # The k-th items are not consumed: they are withheld like the
            # rows after them (§12.11), so the reference lane's counts.
            return "loss"

    def round(self, c: int) -> None:
        pre_link = list(self.link) if self.link is not None else None
        ref_out = self.ref_outside()
        present: dict[int, Item] = {}
        idx: dict[int, int] = {}
        openers = self.openers()
        for p in self.considered():
            oi = self.next_os(p)
            if p not in openers:
                # §12.4: a considered lane outside the link joins within W of
                # c, before or after; one more than W before c is dropped.
                while oi is not None and self.items[p][oi].a < c - self.W:
                    self.ptr[p] = oi + 1
                    oi = self.next_os(p)
            if oi is None or self.items[p][oi].a > c + self.W:
                continue
            while self.ptr[p] < oi:
                it = self.items[p][self.ptr[p]]
                if pre_link is None or p in pre_link:
                    assert not self.aligned, "items before an ordered set while aligned"
                    if p == ref_out:
                        # §12.7: the reference lane's items before its own
                        # ordered set in this round come before the round.
                        self.consume_unaligned(p, it)
                self.ptr[p] += 1
            present[p] = self.items[p][oi]
            idx[p] = oi
        link_present = [p for p in (pre_link or []) if p in present]
        refl = link_present[0] if link_present else min(present)
        ro = present[refl].os
        span = self.span_items(list(present.values()))

        # §12.9 lane mismatch (PIPE): considered lanes in the link before this round.
        for p in sorted(present):
            if pre_link is not None and p not in pre_link:
                continue
            d = self.difference(present[p].os, ro)
            if d is not None and d != self.mm_prev.get(p):
                exp, got = d
                self.emit(self.tm.span(present[p].a, present[p].b), f"Lane {p} differs: {got}, lane {refl} {exp}",
                          True, [("type", "lane_mismatch"), ("lane", p), ("expected", exp), ("got", got),
                                 ("error", ML_ERROR_TEXT["lane_mismatch"])], False)
            self.mm_prev[p] = d

        # §12.7/§12.9 the round's ordered set, with per-lane fields
        def ent(p: int, what: str) -> str:
            o = present[p].os if p in present else None
            if what in ("link", "lane"):
                if o is None or o.kind not in ("ts1", "ts2"):
                    return "-"
                v = o.link if what == "link" else o.lane
                return "PAD" if v is None else str(v)
            return str(o.skp) if o is not None and o.kind == "skp" else "-"

        fr = FeedRound(refl, present[refl], ",".join(ent(p, "link") for p in range(self.B)),
                       ",".join(ent(p, "lane") for p in range(self.B)),
                       ",".join(ent(p, "skp") for p in range(self.B)))

        # §12.5 the link
        its_r = self.items[refl]
        j = idx[refl] + 1                 # rule 1 looks past SKP ordered sets,
        skps = 0                          # at most TS2_SKP_LOOKAHEAD: then the run ends
        while (j < len(its_r) and its_r[j].kind == "os" and its_r[j].os.kind == "skp"
               and skps < TS2_SKP_LOOKAHEAD):
            j += 1
            skps += 1
        nxt = None if skps == TS2_SKP_LOOKAHEAD else (its_r[j] if j < len(its_r) else None)
        if ro.kind == "ts2" and (nxt is None or nxt.kind != "os"):
            cands = [p for p in sorted(present) if present[p].os.kind == "ts2"
                     and present[p].os.link is not None and present[p].os.lane is not None]
            if cands:
                self.decide(cands, "ts2", present, span)
        elif not self.rule1 and ro.kind == "skp":
            cands = [p for p in sorted(present) if present[p].os.kind == "skp"]
            if self.cut_width(len(cands)) > (len(self.link) if self.link is not None else 0):
                self.decide(cands, "skp", present, span)

        # §12.6 alignment
        loss = False
        if self.link is not None:
            if all(p in present for p in self.link):
                lo = min(present[p].a for p in self.link)
                offs = [present[p].a - lo for p in self.link]
                if not self.aligned:
                    self.aligned = True
                    self.end_run()
                    self.lanes_aligned(offs, span)
                    self.had_skew = False
                    self.skew_since = False
                elif (len(self.link), offs) != self.last_al:
                    self.lanes_aligned(offs, span)
                if all(present[p].os.kind == "skp" for p in self.link):
                    # SKP add/remove is silent: compare the next round with
                    # the offsets at the end of each lane's SKP OS.
                    lo_end = min(present[p].b for p in self.link)
                    self.last_al = (len(self.link), [present[p].b - lo_end for p in self.link])
            else:
                first = next(p for p in self.link if p not in present)
                if self.aligned:
                    self.aligned = False
                    loss = True
                    self.lane_skew(first, span)
                elif not self.skew_since:
                    self.lane_skew(first, span)
        if loss:
            self.feed.append(FeedBreak("unknown", None))      # before the round's ordered set
        self.feed.append(fr)
        for p in present:
            self.ptr[p] = idx[p] + 1

    @staticmethod
    def cut_width(w: int) -> int:
        return w if w in LINK_WIDTHS or w == 0 else max(x for x in LINK_WIDTHS if x < w)

    def lanes_aligned(self, offs: list[int], span: tuple[int, int]) -> None:
        w = len(self.link)
        o = ",".join(str(x) for x in offs)
        self.emit(span, f"Lanes aligned x{w} (offsets {o})", False,
                  [("type", "lanes_aligned"), ("width", w), ("offsets", o)], False)
        self.last_al = (w, offs)

    @staticmethod
    def difference(o: Os, r: Os) -> tuple[str, str] | None:
        if o.kind != r.kind:
            return TYPE_NAME[r.kind], TYPE_NAME[o.kind]
        if o.kind in ("ts1", "ts2"):
            if o.n_fts != r.n_fts:
                return f"n_fts={r.n_fts}", f"n_fts={o.n_fts}"
            if o.rate != r.rate:
                return f"rate_id={h2(r.rate)}", f"rate_id={h2(o.rate)}"
            if o.ctrl != r.ctrl:
                return f"training_control={h2(r.ctrl)}", f"training_control={h2(o.ctrl)}"
        return None

    def decide(self, cands: list[int], src: str, present: dict[int, Item], span: tuple[int, int]) -> None:
        order = list(cands)
        if src == "ts2" and self.lane_order == "auto":
            nums = [present[p].os.lane for p in cands]
            if sorted(nums) == list(range(len(cands))):
                order = sorted(cands, key=lambda p: present[p].os.lane)
        w = len(order)
        if w not in LINK_WIDTHS:
            w2 = self.cut_width(w)
            for p in order[w2:]:
                self.lane_skew(p, self.tm.span(present[p].a, present[p].b))
            order = order[:w2]
        if order != self.link:
            w = len(order)
            tail = ", from SKP" if src == "skp" else ""
            self.emit(span, f"Link width x{w} ({self.B} bound{tail})", False,
                      [("type", "link_width"), ("width", w), ("bound", self.B),
                       ("lanes", ",".join(str(p) for p in order)), ("from", src)], False)
            self.link = order
        state = None if order == sorted(order) else tuple(order)
        if state != self.order_state:
            o = ",".join(str(p) for p in order)
            self.emit(span, f"Lane order {o}", False, [("type", "lane_order"), ("order", o)], False)
            self.order_state = state
        if src == "ts2":
            self.rule1 = True
        self.skew_since = False


# ── 5. What the pipeline sees, as x1 units (SPEC §12.7, §12.10) ──────────────

class ViewTiming:
    def __init__(self) -> None:
        self.start: list[int] = []
        self.end: list[int] = []

    def span(self, a: int, b: int) -> tuple[int, int]:
        return self.start[a], self.end[b]


PACKETS = ("dllp", "tlp", "dllp_badlen", "tlp_badlen", "truncated", "dropped", "partial")


def build_view(lk: Link, feed: list, tm: LaneTiming, hand: dict | None = None):
    """The pipeline's input (§12.7) as an x1 Stream of units. Rounds become
    ordered-set units from the reference lane's copy; row symbols map back to
    the transmitter's own packet-layer units (a packet whose tail was not fed
    is truncated by what came next, §6); `hand[seg]` builds a row segment's
    units by hand where the pipeline sees something the transmitter did not
    send in that order (lane_order=physical on a reversed link)."""
    V = Stream(1)
    vt = ViewTiming()
    fed: list[LSym] = []
    meta: list[tuple] = []          # per view symbol: (row id or -1, position, physical lane, width)
    entries: list = []              # ("round", r) | ("seg", [rows]) | ("brk", b)
    for f in feed:
        if isinstance(f, FeedRow):
            if entries and entries[-1][0] == "seg":
                entries[-1][1].append(f)
            else:
                entries.append(("seg", [f]))
        elif isinstance(f, FeedRound):
            entries.append(("round", f))
        else:
            entries.append(("brk", f))
    breaks_at: dict[int, str] = {}  # view symbol count -> break kind
    seg_no = 0
    row_id = 0
    for ei, (kind, val) in enumerate(entries):
        if kind == "brk":
            breaks_at.setdefault(len(V.syms), val.kind)
            continue
        if kind == "round":
            o = val.item.os
            a = len(V.syms)
            for s in val.item.syms:
                V._put(s.byte, s.k, ts=not s.k)
                fed.append(s)
                st, d = tm.sym(s.index)
                vt.start.append(st)
                vt.end.append(st + d)
                meta.append((-1, -1, -1, 0))
            if o.kind in ("ts1", "ts2"):
                V._unit("ts", a, dict(kind=o.kind, link=o.link, lane=o.lane, n_fts=o.n_fts, rate=o.rate,
                                      ctrl=o.ctrl, key=o.key(), links=val.links, lanes=val.lanes))
            elif o.kind == "skp":
                V._unit("skp", a, dict(n=o.skp, counts=val.skps))
            elif o.kind == "unknown":
                V._unit("os_unknown", a, dict(symbol=o.raw[-1][0]))
            else:
                V._unit(o.kind, a)
            continue
        # a segment of fed rows
        rows: list[FeedRow] = val
        a0 = len(V.syms)
        flat: list[LSym] = []
        for r in rows:
            for pos, it in enumerate(r.items):
                s = it.syms[0]
                flat.append(s)
                fed.append(s)
                vt.start.append(r.start)
                vt.end.append(r.end)
                meta.append((row_id, pos, r.lanes[pos], len(r.items)))
            row_id += 1
        nxt = entries[ei + 1] if ei + 1 < len(entries) else None
        if hand and seg_no in hand:
            hand[seg_no](V)
        else:
            _default_rows(lk, V, flat, nxt)
        assert len(V.syms) == a0 + len(flat), f"segment {seg_no}: view covers {len(V.syms) - a0} of {len(flat)} symbols"
        seg_no += 1
    assert len(V.syms) == len(fed)
    for v, s in zip(V.syms, fed):
        assert (v.byte, v.k) == (s.byte, s.k), "view symbols differ from what the lane layer feeds"
        v.wire, v.key, v.before_com = s.wire, s.key, s.before_com
    covered = []
    for u in V.units:
        covered += list(range(u.a, u.b + 1))
    assert covered == list(range(len(V.syms))), "view units must cover every fed symbol once"
    # A truncation by gating must be followed by that break (§4.3); a drop by X/Z.
    for u in V.units:
        by = u.info.get("by") if u.kind == "truncated" else None
        if by in ("rxvalid", "txelecidle"):
            assert breaks_at.get(u.b + 1) == by, (u, breaks_at)
        if u.kind == "dropped":
            assert breaks_at.get(u.b + 1) == "unknown", u
    V.finished = True
    return V, vt, meta


def _default_rows(lk: Link, V: Stream, flat: list[LSym], nxt) -> None:
    origins = [s.origin for s in flat]
    assert all(o[0] == "row" for o in origins), f"fed rows carry non-packet symbols {set(o[0] for o in origins)}"
    lis = [o[1] for o in origins]
    for p, q in zip(lis, lis[1:]):
        # In order; symbols skipped (a link narrower than the transmitter's
        # striping) may only be logical idle, and so must their neighbours.
        assert q > p, "fed row symbols out of transmitter order (needs a hand view)"
        if q != p + 1:
            assert all(lk.unit_of[x].kind == "idle" for x in range(p, q + 1)), \
                "a narrower link skips packet symbols (needs a hand view)"
    i = 0
    while i < len(lis):
        u = lk.unit_of[lis[i]]
        j = i
        while j + 1 < len(lis) and lk.unit_of[lis[j + 1]] is u:
            j += 1
        if u.kind == "idle":
            a = len(V.syms)
            for _ in range(j - i + 1):
                V._put(0x00, False)
            V.units.append(Unit("idle", a, len(V.syms) - 1, dict(n=j - i + 1)))
            i = j + 1
            continue
        assert lis[i:j + 1] == list(range(lis[i], lis[j] + 1))
        x, y = lis[i], lis[j]
        a = len(V.syms)
        for li in range(x, y + 1):
            s = lk.log.syms[li]
            V._put(s.byte, s.k, ts=s.ts)
        b = len(V.syms) - 1
        if x == u.a and y == u.b:
            V.units.append(Unit(u.kind, a, b, u.info))
        elif u.kind == "idle":
            V.units.append(Unit("idle", a, b, dict(n=b - a + 1)))
        elif u.kind in ("dllp", "tlp", "dllp_badlen", "tlp_badlen") and x == u.a and j == len(lis) - 1:
            packet = "tlp" if lk.log.syms[u.a].byte == STP else "dllp"
            info = dict(packet=packet, symbols=b - a, wrong_lane=u.info.get("wrong_lane", False))
            if nxt is None:
                V.units.append(Unit("truncated", a, b, dict(info, by="end of trace")))
            elif nxt[0] == "round":
                V.units.append(Unit("truncated", a, b, dict(info, by="COM")))
            elif nxt[1].kind == "unknown":
                V.units.append(Unit("dropped", a, b, info))
            else:
                V.units.append(Unit("truncated", a, b, dict(info, by=nxt[1].kind)))
        else:
            raise AssertionError(f"unit {u.kind} at {u.a} only partly fed (needs a hand view)")
        i = j + 1


def framing_txs(V: Stream, vt: ViewTiming, meta: list[tuple]) -> list[Tx]:
    """SPEC §12.10 on the fed rows: start lane, PAD fill, misplaced PAD. The
    transmitter marks what it placed wrongly (`wrong_lane`, `pad_misplaced`);
    the rule is applied here and must agree with that intent."""
    out = []
    for u in V.units:
        rid, pos, plane, w = meta[u.a]
        s = V.syms[u.a]
        if u.kind in PACKETS and s.k and s.byte in (STP, SDP) and rid >= 0 and w >= 2:
            ok = pos == 0 if w in (2, 4) else pos % 4 == 0
            assert ok != bool(u.info.get("wrong_lane")), f"start at position {pos} of x{w}: intent differs"
            if not ok:
                name = "STP" if s.byte == STP else "SDP"
                out.append(Tx(vt.start[u.a], vt.end[u.a], f"{name} on lane {plane}", True,
                              [("type", "framing_lane"), ("symbol", h2(s.byte)), ("lane", plane),
                               ("error", ML_ERROR_TEXT["framing_lane"])]))
        if u.kind in ("pad_fill", "pad_misplaced"):
            # Fill: PADs right after an END/EDB that closed a packet in the same row.
            prev = u.a - 1
            fill = (prev >= 0 and meta[prev][0] == rid and V.syms[prev].k and V.syms[prev].byte in (END, EDB)
                    and any(x.kind in PACKETS and x.b == prev for x in V.units))
            for i in range(u.a, u.b + 1):
                assert meta[i][0] == rid and w >= 2
            assert fill == (u.kind == "pad_fill"), f"PAD at view {u.a}: fill rule differs from intent"
            if u.kind == "pad_fill":
                nx = u.b + 1
                assert nx >= len(V.syms) or meta[nx][0] != rid or not (V.syms[nx].k and V.syms[nx].byte == PAD)
            # misplaced PADs are reported as runs (error_runs)
    return out


# Units that give no transaction ending a run (SPEC §12.12): nothing, or Idle.
QUIET_PIPE = ("pad_fill", "pre_com", "dropped", "partial", "idle")
QUIET_DLL = QUIET_PIPE + ("ts", "skp", "fts", "eios", "eieos")


def error_runs(V: Stream, vt: ViewTiming, meta: list[tuple], pipe: bool) -> list[Tx]:
    """SPEC §12.12: on a multi-lane link, consecutive misplaced PADs on one
    lane, or Unknown ordered sets with one symbol, form a run: one error with
    ` (N times)` and `occurrences` when N > 1. Any other transaction of the
    decoder's pipeline but Idle ends the run; lane-level and edge-level ones
    do not."""
    out: list[Tx] = []
    run: dict | None = None

    def end() -> None:
        nonlocal run
        if run is None:
            return
        n = run["n"]
        if run["key"][0] == "pad":
            lane = run["key"][1]
            label = f"PAD outside a packet (lane {lane})"
            f = [("type", "pad_misplaced"), ("lane", lane), ("error", ML_ERROR_TEXT["pad_misplaced"])]
        else:
            label = "Unknown ordered set"
            f = [("type", "unknown_os"), ("symbol", h2(run["key"][1])), ("error", L.ERROR_TEXT["unknown_os"])]
        if n > 1:
            label += f" ({n} times)"
            f.append(("occurrences", n))
        out.append(Tx(run["s"], run["e"], label, True, f))
        run = None

    for u in V.units:
        if u.kind == "pad_misplaced":
            key, span = ("pad", meta[u.a][2]), (vt.start[u.a], vt.end[u.a])
        elif u.kind == "os_unknown":
            key, span = ("uos", u.info["symbol"]), vt.span(u.a, u.b)
        else:
            if u.kind not in (QUIET_PIPE if pipe else QUIET_DLL) or u.info.get("wrong_lane"):
                end()
            continue
        if run is not None and run["key"] == key:
            run["e"] = span[1]
            run["n"] += 1
        else:
            end()
            run = dict(key=key, s=span[0], e=span[1], n=1)
    end()
    return out


# ── 6. Pipeline engines with the §12.9 ordered-set fields ────────────────────

def ts_tx(run: dict, tm: ViewTiming) -> Tx:
    i = run["first"].info
    count = run["count"]
    links, lanes = i["links"], i["lanes"]
    vals = [x for x in links.split(",") if x != "-"]
    lpart = f"link={vals[0]}" if len(set(vals)) == 1 else f"links={links}"
    gen = "gen2" if i["rate"] & 0x04 else "gen1"
    name = "TS1" if i["kind"] == "ts1" else "TS2"
    mult = f" ×{count}" if count > 1 else ""
    c = i["ctrl"]
    fields = [("type", i["kind"]), ("count", count), ("links", links), ("lanes", lanes),
              ("n_fts", i["n_fts"]), ("rate_id", h2(i["rate"])), ("training_control", h2(c)),
              ("hot_reset", bool(c & 1)), ("disable_link", bool(c & 2)),
              ("loopback", bool(c & 4)), ("disable_scrambling", bool(c & 8)),
              ("compliance_receive", bool(c & 16))]
    s, e = tm.span(run["a"], run["b"])
    return Tx(s, e, f"{name}{mult} {lpart} lanes={lanes} n_fts={i['n_fts']} {gen}", False, fields)


def pipe_engine(V: Stream, tm: ViewTiming, coalesce: bool, show_idle: bool, runs: bool) -> list[Tx]:
    """pcie_fixture_lib.pipe_engine with SPEC §12.9 TS/SKP fields and the
    §12.10 PAD units (fill: not decoded; misplaced: reported by error_runs,
    as are Unknown ordered sets when `runs`)."""
    out: list[Tx] = []
    run: dict | None = None
    idle: dict | None = None

    def end_run() -> None:
        nonlocal run
        if run is not None:
            if run["kind"] == "ts":
                out.append(ts_tx(run, tm))
            else:
                s, e = tm.span(run["a"], run["b"])
                name = "FTS" if run["kind"] == "fts" else "EIEOS"
                cnt = run["count"]
                out.append(Tx(s, e, f"{name} ×{cnt}" if cnt > 1 else name, False,
                              [("type", run["kind"]), ("count", cnt)]))
            run = None

    def end_idle() -> None:
        nonlocal idle
        if idle is not None:
            s, e = tm.span(idle["a"], idle["b"])
            out.append(Tx(s, e, f"Idle ×{idle['n']}" if idle["n"] > 1 else "Idle", False,
                          [("type", "idle"), ("symbols", idle["n"])]))
            idle = None

    for u in V.units:
        if u.kind == "pad_fill":
            continue
        if u.kind == "idle":
            end_run()
            if show_idle:
                if idle is not None and idle["last"] + 1 == u.a:
                    idle["b"] = idle["last"] = u.b
                    idle["n"] += u.b - u.a + 1
                else:
                    end_idle()
                    idle = dict(a=u.a, b=u.b, last=u.b, n=u.b - u.a + 1)
            continue
        if u.kind == "pad_misplaced" and idle is not None:
            idle["last"] = u.b      # §12.12: a PAD in a row does not split an Idle run
            continue
        end_idle()
        if u.kind in ("ts", "fts", "eieos"):
            key = (u.kind, u.info.get("kind"), u.info.get("key"), u.info.get("links"), u.info.get("lanes"))
            if coalesce and run is not None and run["key"] == key and run["b"] + 1 == u.a:
                run["b"] = u.b
                run["count"] += 1
                continue
            end_run()
            run = dict(kind=u.kind, key=key, a=u.a, b=u.b, count=1, first=u)
            continue
        end_run()
        s, e = tm.span(u.a, u.b)
        if u.kind == "skp":
            counts = u.info["counts"]
            shown = [x for x in counts.split(",") if x != "-"]
            out.append(Tx(s, e, "SKP" if all(x == "3" for x in shown) else f"SKP ×{counts}", False,
                          [("type", "skp"), ("skp_symbols", counts)]))
        elif u.kind == "eios":
            out.append(Tx(s, e, "EIOS", False, [("type", "eios")]))
        elif u.kind in ("dllp", "dllp_badlen") and u.info["end"] == EDB:
            out.append(L.common_error_tx(Unit("dllp_edb", u.a, u.b, u.info), tm))
        elif u.kind in ("dllp", "dllp_badlen"):
            n = len(u.info["body"])
            out.append(Tx(s, e, f"DLLP frame ({n} symbols)", False,
                          [("type", "dllp_frame"), ("symbols", n), ("end", "END")]))
        elif u.kind in ("tlp", "tlp_badlen"):
            body = u.info["body"]
            n = len(body)
            assert n >= 2
            seq = ((body[0] & 0x0F) << 8) | body[1]
            endn = "END" if u.info["end"] == END else "EDB"
            tail = "" if endn == "END" else ", EDB"
            out.append(Tx(s, e, f"TLP frame seq={seq} ({n} symbols{tail})", False,
                          [("type", "tlp_frame"), ("seq", seq), ("symbols", n), ("end", endn)]))
        elif u.kind in ("pre_com", "dropped", "partial", "pad_misplaced") or (runs and u.kind == "os_unknown"):
            pass
        else:
            tx = L.common_error_tx(u, tm)
            assert tx is not None, u.kind
            out.append(tx)
    end_run()
    end_idle()
    return out


def dll_engine(V: Stream, tm: ViewTiming, runs: bool) -> list[Tx]:
    skip = ("pad_fill", "pad_misplaced") + (("os_unknown",) if runs else ())
    units = [u for u in V.units if u.kind not in skip]
    return L.dll_engine(SimpleNamespace(units=units), tm)


def rxstatus_txs(lay: Layout, tm: LaneTiming, lanes: list[int]) -> list[Tx]:
    """SPEC §12.8: RxStatus per bound lane, own previous value, own gating."""
    out = []
    for p in lanes:
        prev = 0
        for k, e in enumerate(lay.edges[p]):
            code = e.rxstatus
            gated = e.kind == "gap" and e.gap in ("rxvalid", "txelecidle")
            if not gated and code != 0 and code != prev:
                name, err = L.RXSTATUS[code]
                s, en = tm.edge_span(k, k)
                f = [("type", "rxstatus"), ("code", "0b" + format(code, "03b")), ("lane", p)]
                if err:
                    f.append(("error", L.ERROR_TEXT[f"rxstatus_{code}"]))
                out.append(Tx(s, en, f"RxStatus: {name} (lane {p})", err, f))
            prev = code
    return out


def expected(lk: Link, lay: Layout, tfs: list[int], pfs: list[int], decoder: str, params: dict,
             rx_lanes: list[int], hand: dict | None = None) -> list[Tx]:
    pipe = decoder.startswith("ferrite.pcie_pipe")
    tm = LaneTiming(lay.n, tfs, pfs)
    model = LaneModel(lay.items(), lay.B, tm, params.get("max_skew", 8), params.get("lane_order", "auto"))
    model.run_model()
    V, vt, meta = build_view(lk, model.feed, tm, hand)
    lane_txs = [t for t, both in model.txs if pipe or both]
    ftx = framing_txs(V, vt, meta)
    rx = rxstatus_txs(lay, tm, rx_lanes) if pipe else []
    coalesce = params.get("coalesce", True)
    show_idle = params.get("show_idle", False)
    runs = lay.B > 1

    def run(view: str) -> list[Tx]:
        L.check_view(V, view)
        txs = pipe_engine(V, vt, coalesce, show_idle, runs) if pipe else dll_engine(V, vt, runs)
        if runs:
            txs += error_runs(V, vt, meta, pipe)
        return txs + ftx + lane_txs + model.xz + rx

    txs = L.select_scrambling(V, vt, params.get("scrambling", "auto"), run)
    assert not any(t.label == "Scrambling: off (undetermined)" for t in txs), \
        "auto never locks: the order of lane-level transactions in the buffer is not modelled (avoided)"
    return txs


# ── 7. The VCD ───────────────────────────────────────────────────────────────

SIDEBANDS = ("rxvalid", "txelecidle", "rxstatus")
WIDTH1 = {"rxvalid": 1, "txelecidle": 1, "rxstatus": 3}


def lane_sig(base: str, p: int) -> str:
    """Decoder signal name (SPEC §12.1): lane 0 is `data`, lane l `data<l>`."""
    return base if p == 0 else f"{base}{p}"


def vcd_names(bound: int, direction: str, side: dict[str, list[int]]) -> list[tuple[str, str]]:
    """(decoder signal, VCD name) in manifest order, named like a PIPE dump:
    TxData, TxDataK, TxData1, TxDataK1, ... (RxData, RxValid1 ... for RX)."""
    pre = "Tx" if direction == "tx" else "Rx"
    pin = {"data": f"{pre}Data", "datak": f"{pre}DataK", "rxvalid": "RxValid",
           "txelecidle": "TxElecIdle", "rxstatus": "RxStatus"}
    out = [("pclk", "pclk")]
    for p in range(bound):
        for base in ("data", "datak") + SIDEBANDS:
            if base in ("data", "datak") or p in side.get(base, []):
                out.append((lane_sig(base, p), pin[base] + ("" if p == 0 else str(p))))
    return out


def _ident(i: int) -> str:
    s = ""
    i += 1
    while i:
        i, r = divmod(i - 1, 94)
        s = chr(33 + r) + s
    return s


def lane_values(lay: Layout, p: int, k: int) -> dict[str, object]:
    n = lay.n
    e = lay.edges[p][k]
    if e.kind == "data":
        data = sum(s.wire << (8 * j) for j, s in enumerate(e.syms))
        datak = sum(int(s.k) << j for j, s in enumerate(e.syms))
    elif e.gap == "unknown":
        data, datak = e.bus if e.bus else ("x" * (8 * n), "x" * n)
    else:
        if e.bus:
            data, datak = e.bus
        else:   # junk a decoder must ignore: END / SDP on every byte lane, all K
            byte = 0xFD if k % 2 else 0x5C
            data, datak = sum(byte << (8 * j) for j in range(n)), (1 << n) - 1
    return {"data": data, "datak": datak,
            "rxvalid": 0 if (e.kind == "gap" and e.gap == "rxvalid") else 1,
            "txelecidle": 1 if (e.kind == "gap" and e.gap == "txelecidle") else 0,
            "rxstatus": e.rxstatus}


def write_vcd(lay: Layout, clk: L.Clocking, names: list[tuple[str, str]], scope: tuple = ("tb", "dut")) -> L.VcdTrace:
    n = lay.n
    count = lay.count
    periods = clk.periods(count)
    times, _, _ = edge_times(clk, count)
    end_time = times[-1] + periods[-1]

    def width(sig: str) -> int:
        base = sig.rstrip("0123456789")
        return {"pclk": 1, "data": 8 * n, "datak": n}.get(base, WIDTH1.get(base, 0))

    def lane_of(sig: str) -> tuple[str, int]:
        base = sig.rstrip("0123456789")
        num = sig[len(base):]
        return base, int(num) if num else 0

    sigs = [(sig, nm, width(sig)) for sig, nm in names]
    ids = {sig: _ident(i) for i, (sig, _, _) in enumerate(sigs)}
    events: dict[int, dict[str, str]] = {}

    def put(t: int, sig: str, w: int, v: object) -> None:
        events.setdefault(t, {})[sig] = L._bits(v, w)

    init = {"pclk": 0, "data": 0, "datak": 0, "rxvalid": 1, "txelecidle": 0, "rxstatus": 0}
    for sig, _, w in sigs:
        put(0, sig, w, init[lane_of(sig)[0]])
    for k in range(count):
        put(times[k], "pclk", 1, 1)
        put(times[k] + periods[k] // 2, "pclk", 1, 0)
    for k in range(count):
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
        for sig, _, w in sigs:
            if sig == "pclk":
                continue
            base, p = lane_of(sig)
            put(t, sig, w, lane_values(lay, p, k)[base])
    tr = L.VcdTrace()
    tr.end_time = end_time
    tr.edge_times = times
    last: dict[str, str] = {}
    out = ["$version wavecrux-decoders decoders/pcie/tools/generate_fixtures.py $end\n",
           f"$timescale {clk.timescale[0]}{clk.timescale[1]} $end\n"]
    for sc in scope:
        out.append(f"$scope module {sc} $end\n")
    for sig, nm, w in sigs:
        rng = f" [{w - 1}:0]" if w > 1 else ""
        out.append(f"$var {'wire' if sig == 'pclk' else 'reg'} {w} {ids[sig]} {nm}{rng} $end\n")
        tr.paths[sig] = ".".join(scope + (nm,))
    for _ in scope:
        out.append("$upscope $end\n")
    out.append("$enddefinitions $end\n")
    for t in sorted(events):
        assert t < end_time
        lines = []
        for sig, _, w in sigs:
            if sig not in events[t]:
                continue
            v = events[t][sig]
            if last.get(sig) == v:
                continue
            last[sig] = v
            tr.changes.setdefault(sig, []).append((t, v))
            lines.append(f"{v}{ids[sig]}\n" if w == 1 else f"b{v} {ids[sig]}\n")
        if not lines:
            continue
        out.append(f"#{t}\n")
        if t == 0:
            out += ["$dumpvars\n"] + lines + ["$end\n"]
        else:
            out += lines
    out.append(f"#{end_time}\n")
    tr.text = "".join(out)
    return tr


def check_host(lay: Layout, tr: L.VcdTrace, clk: L.Clocking, names: list[tuple[str, str]], name: str) -> None:
    """The host model (L.host_edges) must capture exactly the planned values
    of every bound signal at every planned edge."""
    bound = [s for s, _ in names]
    seen = L.host_edges(tr, bound, clk.sample_point)
    assert len(seen) == lay.count, (name, len(seen), lay.count)
    n = lay.n
    for k, (t, vals) in enumerate(seen):
        assert t == tr.edge_times[k], (name, k)
        for sig in bound[1:]:
            base = sig.rstrip("0123456789")
            p = int(sig[len(base):] or 0)
            w = {"data": 8 * n, "datak": n}.get(base, WIDTH1.get(base))
            want = L._bits(lane_values(lay, p, k)[base], w)
            assert vals.get(sig) == want, (name, k, sig, vals.get(sig), want)


# ── 8. Training (protocol-notes §8; PCIe Base 2.1 §4.2.6.2-§4.2.6.3) ────────

def train(lk: Link, order: list[int] | None = None, n_fts: int = 24, rate: int = 0x02,
          polling: int = 2, cfg_link: int = 1, cfg_lane: int = 2, ts2: int = 3, per: dict | None = None) -> None:
    """Polling.Active TS1 (PAD/PAD) on every live lane, Configuration TS1
    with Link# then Lane# for the lanes of `order` (PAD elsewhere), a TS2 run;
    then the transmitter stripes over `order`."""
    order = list(range(lk.B)) if order is None else order
    lk.flush()
    lk.order = list(order)       # Lane# numbering; set_link() below checks the width
    lk.ts("ts1", None, None, count=polling, n_fts=n_fts, rate=rate, per=per)
    lk.ts("ts1", "link", None, count=cfg_link, n_fts=n_fts, rate=rate, per=per)
    lk.ts("ts1", "link", "order", count=cfg_lane, n_fts=n_fts, rate=rate, per=per)
    lk.ts("ts2", "link", "order", count=ts2, n_fts=n_fts, rate=rate, per=per)
    lk.set_link(order)
