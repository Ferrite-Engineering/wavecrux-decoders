# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Checks that pair the encoder with the reference decoder.

- check(): every captured fixture's expected file equals what
  pcie_ref_decoder.py makes of its VCD (used by generate_fixtures.py --check).
- cross_check(dir): runs the reference decoder over generated fixtures and
  reports where it disagrees with the encoder's expectations. Agreement is
  not required for --check (the generator is the authority for generated
  fixtures); a disagreement means one of the two independent directions has
  a bug and must be understood.

    python3 decoders/pcie/tools/capture_check.py            # cross-check generated/
"""

from __future__ import annotations

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import pcie_ref_decoder as R  # noqa: E402

FIXTURES = os.path.join(os.path.dirname(HERE), "fixtures")


def ref_output(vcd: str, bindings: str) -> str:
    with open(bindings, encoding="utf-8") as f:
        b = json.load(f)
    fpt, txs = R.decode(vcd, b)
    return R.render(fpt, txs)


def stems(d: str) -> list[str]:
    if not os.path.isdir(d):
        return []
    return sorted(f[:-4] for f in os.listdir(d) if f.endswith(".vcd"))


def check() -> list[str]:
    bad = []
    d = os.path.join(FIXTURES, "captured")
    for stem in stems(d):
        p = os.path.join(d, stem)
        got = ref_output(p + ".vcd", p + ".bindings.json")
        with open(p + ".expected.json", encoding="utf-8") as f:
            if f.read() != got:
                bad.append(f"captured/{stem}.expected.json differs from the reference decoder")
    return bad


def cross_check(d: str) -> list[str]:
    out = []
    for stem in stems(d):
        p = os.path.join(d, stem)
        got = ref_output(p + ".vcd", p + ".bindings.json").splitlines()
        with open(p + ".expected.json", encoding="utf-8") as f:
            want = f.read().splitlines()
        if got != want:
            only_w = [x for x in want if x not in got]
            only_g = [x for x in got if x not in want]
            out.append(f"{stem}:\n  encoder only: " + "\n                ".join(only_w[:6]) +
                       "\n  ref only:     " + "\n                ".join(only_g[:6]))
    return out


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else os.path.join(FIXTURES, "generated")
    diffs = cross_check(target)
    print("\n".join(diffs) if diffs else f"reference decoder agrees with every fixture in {target}")
    sys.exit(1 if diffs else 0)
