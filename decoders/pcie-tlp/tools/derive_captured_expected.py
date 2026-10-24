#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Chili.CHIPS
#
# Clean-room derivation of the expected output for a captured fixture, from
# SPEC.md and the raw VCD only. Independent of the decoder: it imports and
# reads nothing from it. It implements only what the captured traces need
# and stops with an error on anything else, rather than guessing.

"""Derive a captured fixture's expected output from SPEC.md and its VCD."""

import argparse
import itertools
import json
import sys
from pathlib import Path

CAPTURED = Path(__file__).resolve().parent.parent / "fixtures" / "captured"
REG_NAMES = {
    0x00: "ID",
    0x04: "Command",
    0x08: "Class/Rev",
    0x0C: "Hdr/Lat/CLS",
    **{0x10 + 4 * i: f"BAR{i}" for i in range(6)},
    0x28: "CardBus",
    0x2C: "Subsystem",
    0x30: "ExpROM",
    0x34: "CapPtr",
    0x3C: "Int",
}
STATUS = {0: "SC", 1: "UR", 2: "CRS", 4: "CA"}


class Unsupported(Exception):
    pass


# --- VCD: values just before each rising clk edge (SPEC §1) -----------------


def sample_edges(vcd_path, clk, names):
    """Yield (time, {name: value}) with pre-edge values at each 0->1 of clk."""
    ids, wanted = {}, set(names) | {clk}
    toks = iter(Path(vcd_path).read_text().split())
    scope = []
    for t in toks:
        if t == "$scope":
            next(toks)
            scope.append(next(toks))
        elif t == "$upscope":
            scope.pop()
        elif t == "$var":
            _, _, code, ref = next(toks), next(toks), next(toks), next(toks)
            full = ".".join(scope + [ref])
            if full in wanted:
                ids.setdefault(code, []).append(full)
        elif t == "$enddefinitions":
            next(toks)
            break
        if t in ("$scope", "$var", "$timescale", "$version", "$date", "$comment"):
            while t != "$end":
                t = next(toks)
    missing = wanted - {n for v in ids.values() for n in v}
    if missing:
        raise Unsupported(f"signals not in VCD: {sorted(missing)}")

    cur = {n: "x" for n in wanted}
    time, block = None, []

    def flush():
        clk_changes = [v for n, v in block if n == clk]
        edge = None
        if len(clk_changes) > 1:
            raise Unsupported(f"several clk changes at #{time}")
        if clk_changes and cur[clk] == "0" and clk_changes[0] == "1":
            edge = (time, dict(cur))
        for n, v in block:
            cur[n] = v
        return edge

    for t in toks:
        if t[0] == "#":
            if time is not None:
                e = flush()
                if e:
                    yield e
            time, block = int(t[1:]), []
        elif t[0] == "$":
            continue  # $dumpvars / $end etc.: their values are just changes
        elif t[0] in "bB":
            code = next(toks)
            for n in ids.get(code, []):
                block.append((n, t[1:].lower()))
        elif t[0] in "rR":
            raise Unsupported("real values in VCD")
        else:
            for n in ids.get(t[1:], []):
                block.append((n, t[0].lower()))
    if time is not None:
        e = flush()
        if e:
            yield e


def bit(v):
    if v not in ("0", "1"):
        raise Unsupported(f"X/Z beat ({v!r}) not implemented")
    return v == "1"


def word(v):
    if any(c not in "01" for c in v):
        raise Unsupported(f"X/Z beat ({v!r}) not implemented")
    return int(v, 2)


# --- TLP decoding (SPEC §5) ---------------------------------------------------


def bdf(i):
    return f"{i >> 8 & 0xFF:02X}:{i >> 3 & 0x1F:02X}.{i & 7}"


def swap(d):
    return int.from_bytes(d.to_bytes(4, "big"), "little")


def kind_of(dw0):
    fmt, typ = dw0 >> 29, dw0 >> 24 & 0x1F
    if fmt & 4:
        return "prefix"
    if typ in (4, 5) and fmt in (0, 2):
        return "cfg"
    if typ == 0 or (typ == 1 and fmt in (0, 1)):
        return "mem"
    if typ in (0x0A, 0x0B) and fmt in (0, 2):
        return "cpl"
    return "other"  # I/O, messages, §5.5: not needed by these traces


def header_len(dw0):
    return 1 if dw0 >> 31 else (4 if dw0 >> 29 & 1 else 3)


def tag_of(dws, low_dw):
    return (dws[0] >> 23 & 1) << 9 | (dws[0] >> 19 & 1) << 8 | (dws[low_dw] >> 8 & 0xFF)


def request_entry(dws):
    """(requester, tag, info) for a request the pairing remembers, else None."""
    k, fmt = kind_of(dws[0]), dws[0] >> 29
    if k == "cfg":
        reg = dws[2] & 0xFFC
        return dws[1] >> 16, tag_of(dws, 1), ("cfg", fmt == 2, reg)
    if k == "mem" and fmt in (0, 1):
        low = (dws[3] if fmt & 1 else dws[2]) & ~3
        return dws[1] >> 16, tag_of(dws, 1), ("mrd", low)
    return None


class Pairing:
    def __init__(self):
        self.by_tag = {}  # tag -> list of [requester, info], oldest first

    def remember(self, entry):
        req, tag, info = entry
        lst = self.by_tag.setdefault(tag, [])
        for e in lst:
            if e[0] == req:
                e[1] = info
                return
        if len(lst) == 4:
            lst.pop(0)
        lst.append([req, info])

    def match(self, req, tag, keep):
        lst = self.by_tag.get(tag, [])
        for e in lst:
            if e[0] == req:
                if not keep:
                    lst.remove(e)
                return e[1]
        return None


def decode(dws, pairing, full):
    """Return (label, is_error, fields) for one complete main-stream TLP."""
    dw0 = dws[0]
    fmt, typ, length = dw0 >> 29, dw0 >> 24 & 0x1F, dw0 & 0x3FF
    k, hl = kind_of(dw0), header_len(dw0)
    if k in ("prefix", "other"):
        raise Unsupported(f"TLP kind Fmt {fmt:03b} Type 0x{typ:02X} not implemented")
    if len(dws) < hl:
        raise Unsupported("TLP shorter than its header: not implemented")
    n = length or 1024
    has_data = fmt in (2, 3)
    data = dws[hl] if has_data and len(dws) > hl else None
    if has_data and data is None:
        raise Unsupported("data TLP without a data DW: not implemented")
    f = {
        "type": None,
        "fmt": fmt,
        "type_code": f"0x{typ:02X}",
        "length": length,
        "dw_count": len(dws),
        "header": " ".join(f"{d:08X}" for d in dws[:hl]),
    }
    err = []
    if k == "cfg":
        name = ("CfgWr" if has_data else "CfgRd") + str(typ & 1)
        tag, req = tag_of(dws, 1), dws[1] >> 16
        bus, dev, fn = dws[2] >> 24, dws[2] >> 19 & 0x1F, dws[2] >> 16 & 7
        reg = dws[2] & 0xFFC
        rname = REG_NAMES.get(reg, f"reg 0x{reg:03X}")
        tgt = f"{bus:02X}:{dev:02X}.{fn}"
        val = f" = 0x{swap(data):08X}" if has_data else ""
        if full:
            label = f"{name} {tgt} {rname}{val}" + f"  tag {tag:02X}"
        else:
            one = "1" if typ & 1 else ""
            shown_bdf = "" if (tgt == "01:00.0" and not typ & 1) else " " + tgt
            label = f"{name[:5]}{one}{shown_bdf} {rname}{val}"
        f.update(bus=bus, device=dev, function=fn, register=f"0x{reg:03X}")
        f.update(register_name=rname, requester=bdf(req), tag=tag)
        f["first_be"] = f"0x{dws[1] & 0xF:X}"
        if has_data:
            f.update(value=f"0x{swap(data):08X}", data=f"0x{data:08X}")
    elif k == "mem":
        name = "MWr" if has_data else "MRd"
        name += ("Lk" if typ == 1 else "") + ("64" if hl == 4 else "32")
        tag, req = tag_of(dws, 1), dws[1] >> 16
        high, low = (dws[2], dws[3] & ~3) if hl == 4 else (0, dws[2] & ~3)
        a = f"0x{high:08X}_{low:08X}" if high else f"0x{low:08X}"
        if has_data:
            label = f"{name} {a} = 0x{data:08X}"
            if n > 1:
                label += f" +{n - 1} DW" if full else " ..."
        elif full:
            label = f"{name} {a}  {n} DW  tag {tag:02X}"
        else:
            label = f"{name} {a}" + (f" x{n} DW" if n != 1 else "")
        f["address"] = f"0x{high:08X}{low:08X}" if hl == 4 else f"0x{low:08X}"
        f.update(length_dw=n, requester=bdf(req), tag=tag)
        f["first_be"] = f"0x{dws[1] & 0xF:X}"
        f["last_be"] = f"0x{dws[1] >> 4 & 0xF:X}"
        if has_data:
            f["data"] = f"0x{data:08X}"
    else:  # completion
        name = ("CplD" if has_data else "Cpl") + ("Lk" if typ == 0x0B else "")
        st = STATUS.get(dws[1] >> 13 & 7, "rsvd")
        bc = (dws[1] & 0xFFF) or 4096
        req, tag, la = dws[2] >> 16, tag_of(dws, 2), dws[2] & 0x7F
        keep = has_data and bc + (la & 3) > 4 * n
        m = pairing.match(req, tag, keep)
        what = c = None
        if m and m[0] == "cfg":
            c = REG_NAMES.get(m[2], f"reg 0x{m[2]:03X}")
            what = f"for write {c}" if m[1] else c
        elif m:
            what = c = f"MRd 0x{m[1]:08X}"
            c = f"0x{m[1]:08X}"
        shown = (swap(data) if m and m[0] == "cfg" else data) if has_data else None
        sval = f" = 0x{shown:08X}" if has_data else ""
        if full:
            label = f"{name} {st}" + (f" {what}" if what else "")
            label += f"{sval}  tag {tag:02X}"
        else:
            label = "CplD" if has_data else "Cpl"
            label += (f" {st}" if st != "SC" else "") + (f" {c}" if c else "")
            label += sval
        f.update(status=st, completer=bdf(dws[1] >> 16), requester=bdf(req))
        f.update(tag=tag, byte_count=bc, lower_address=f"0x{la:02X}")
        if has_data:
            f["data"] = f"0x{data:08X}"
            if m and m[0] == "cfg":
                f["value"] = f"0x{shown:08X}"
        if m:
            f["answers"] = what
        if st in ("UR", "CA"):
            err.append("Unsupported Request" if st == "UR" else "Completer Abort")
    f["type"] = name
    expected = hl + (n if has_data else 0) + (dw0 >> 15 & 1)
    extra = {}
    if expected != len(dws):
        err.append("length does not match the Length field")
        extra = {"expected": expected, "actual": len(dws)}
        label += f"  [{len(dws)} DW, expected {expected}]"
    if err:
        f["error"] = "; ".join(err)
        f.update(extra)
    return label, bool(err), f


# --- Stream assembly, pairing, span (SPEC §1, §3, §5.3) ----------------------


def derive(stem):
    b = json.loads((CAPTURED / f"{stem}.bindings.json").read_text())
    sb, params = b["signal_bindings"], b.get("parameters", {})
    full = params.get("labels", "compact") == "full"
    span = params.get("span", "packet")
    if not full and params.get("labels", "compact") != "compact":
        raise Unsupported(f"labels={params['labels']}")
    if span not in ("packet", "until_next"):
        raise Unsupported(f"span={span}")
    main = ["tlp_valid", "tlp_data", "tlp_sop", "tlp_eop"]
    peer = ["peer_valid", "peer_data", "peer_sop", "peer_eop"]
    has_peer = all(p in sb for p in peer)
    names = [sb[s] for s in main + (peer if has_peer else [])]
    pairing, out = Pairing(), []
    cur_main, cur_peer = None, None  # (start, [dws]) / [dws]
    last_edge = 0

    for t, v in sample_edges(CAPTURED / f"{stem}.vcd", sb["clk"], names):
        last_edge = t
        if has_peer and bit(v[sb["peer_valid"]]):
            d = word(v[sb["peer_data"]])
            sop, eop = bit(v[sb["peer_sop"]]), bit(v[sb["peer_eop"]])
            if sop:
                cur_peer = [d]
            elif cur_peer is not None:
                cur_peer.append(d)
            if eop and cur_peer is not None:
                if len(cur_peer) < header_len(cur_peer[0]):
                    raise Unsupported(f"peer TLP shorter than its header at #{t}")
                e = request_entry(cur_peer)
                if e:
                    pairing.remember(e)
                cur_peer = None
        if not bit(v[sb["tlp_valid"]]):
            continue
        d = word(v[sb["tlp_data"]])
        sop, eop = bit(v[sb["tlp_sop"]]), bit(v[sb["tlp_eop"]])
        if sop and cur_main is not None:
            raise Unsupported(f"SOP before EOP at #{t}: not implemented")
        if not sop and cur_main is None:
            raise Unsupported(f"data beat without SOP at #{t}: not implemented")
        if sop:
            cur_main = (t, [])
        cur_main[1].append(d)
        if eop:
            start, dws = cur_main
            cur_main = None
            label, is_err, fields = decode(dws, pairing, full)
            e = request_entry(dws)
            if e:
                pairing.remember(e)
            out.append([start, t, label, is_err, fields])
    if cur_main is not None:
        raise Unsupported("missing EOP at end of capture: not implemented")
    if span == "until_next":
        for a, nxt in itertools.pairwise(out):
            a[1] = max(a[1], nxt[0])
        if out:
            out[-1][1] = max(out[-1][1], last_edge)
    return out


def serialize(txs):
    lines = []
    for start, end, label, is_err, fields in txs:
        obj = {
            "startTime": start,
            "endTime": end,
            "label": label,
            "isError": is_err,
            "fields": {k: str(v) for k, v in fields.items()},
        }
        lines.append(json.dumps(obj, separators=(",", ":")))
    return "[\n" + ",\n".join(lines) + "\n]\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("stem", help="fixture stem in fixtures/captured")
    ap.add_argument("--out", help="output file (default: stdout)")
    a = ap.parse_args()
    try:
        txs = derive(a.stem)
    except Unsupported as e:
        sys.exit(f"derive_captured_expected: unsupported: {e}")
    except FileNotFoundError as e:
        sys.exit(f"derive_captured_expected: no such fixture: {e.filename}")
    text = serialize(txs)
    if a.out:
        out = Path(a.out).resolve()
        if CAPTURED.resolve() in out.parents:
            sys.exit("derive_captured_expected: will not write into fixtures/captured")
        out.write_text(text, newline="\n")
    else:
        sys.stdout.write(text)
    print(f"{a.stem}: {len(txs)} transactions", file=sys.stderr)


if __name__ == "__main__":
    main()
