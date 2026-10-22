# Captured fixtures: provenance

Two fixtures share one trace, recorded from a real RTL simulation:

| Stem | Decoder | Parameters |
|---|---|---|
| `openpcie_rc_down_peer_1ps` | `openpcie.pcie_tlp_down` | defaults, `tx_*` (RC to EP), peer `rx_*` |
| `openpcie_rc_up_full_peer_1ps` | `openpcie.pcie_tlp_up` | `labels=full`, `rx_*` (EP to RC), peer `tx_*` |

## Source

- Upstream: <https://github.com/chili-chips-ba/openPCIE>, commit
  `2318f2ea0a2c6630560fc3e0985ee67b08cbce96`, test bench `5.sim/tb.sv`.
- The DUT is openPCIE's root complex: the RTL in `2.rtl/0.common.opensource`
  around AMD's `PCIE_2_1` hard block, with its RISC-V firmware
  (`3.sw/RC-direct`). The link partner is the
  [pcieVHost](https://github.com/wyvernSemi/pcievhost) endpoint model, at the
  PIPE.
- The dumped signals are `tb.tlp_view` (`5.sim/models/tlp_wavecrux_view.sv`):
  the RTL's AXI-Stream in both directions, re-timed to one DW per clock by
  `5.sim/models/tlp_dw_monitor.sv`.

## Expected entries: where they come from

The 41 entries (21 down, 20 up) in the two `<stem>.expected.json` files
come from SPEC.md and the raw trace values, never from the decoder's code or
output:

1. A reviewer who did not see the decoder's code (`src/`, `tests/`,
   `fuzz/`) or run it wrote a model of SPEC.md, checked it byte for byte
   against all hand-derived `fixtures/generated/*.expected.json` (as of
   `00e58e2`), and ran
   it on the trace. That gave the committed files.
2. A second reviewer, under the same rules and without seeing those files,
   wrote `tools/derive_captured_expected.py`: it reads the VCD, decodes each
   header per SPEC.md and the PCIe Base Specification, and imports nothing
   of the decoder. Its output matches both files byte for byte:

   ```
   python3 decoders/pcie-tlp/tools/derive_captured_expected.py openpcie_rc_down_peer_1ps --out down.json
   python3 decoders/pcie-tlp/tools/derive_captured_expected.py openpcie_rc_up_full_peer_1ps --out up.json
   diff down.json decoders/pcie-tlp/fixtures/captured/openpcie_rc_down_peer_1ps.expected.json
   diff up.json decoders/pcie-tlp/fixtures/captured/openpcie_rc_up_full_peer_1ps.expected.json
   ```

3. The three anchors below were checked by hand against the beats.

The decoder was run on the fixtures only afterwards, as the test.

## Licence

BSD-3-Clause, openPCIE's `LICENSE`, by its copyright holder Chili.CHIPS,
who also wrote this decoder. Only the simulation's output is copied here:
signal values seen at our RTL, no source code of openPCIE or of the VIP. pcieVHost
and VProc (GPL-3.0-or-later) drive the link in the simulation; none of their
code or text is in the trace.

The trace is redistributed under openPCIE's notice (`LICENSE` at commit
`2318f2e`; its `REUSE.toml` gives `SPDX-FileCopyrightText: 2026
CHILI.CHIPS.LLC`, `SPDX-License-Identifier: BSD-3-Clause`; the same
holder as Chili.CHIPS in `LICENSE` and in this repository's `REUSE.toml`):

```
BSD 3-Clause License

Copyright (c) 2026, Chili.CHIPS

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Simulator: Vivado xsim, not an open simulator

The testing standard (§4) asks for Icarus Verilog or Verilator. This trace
needs Vivado xsim: openPCIE's core instantiates AMD's `PCIE_2_1` hard block,
which exists only as an encrypted simulation model inside Vivado. No open
simulator can run it. The TLP stream it produces is the real thing, with
the hard block's own completion timing, which is why we use it.

A fixture from an open simulator will follow from
[openCologne-PCIE](https://github.com/chili-chips-ba/openCologne-PCIE), which
runs on Verilator, once its TL RTL is in (`DUT=rtl`).

## Reproducing

With Vivado 2026.1 (xsim) and the xPack `riscv-none-elf-gcc` on `PATH`, on
Linux or WSL:

```
git clone https://github.com/chili-chips-ba/openPCIE
git -C openPCIE checkout 2318f2ea0a2c6630560fc3e0985ee67b08cbce96
cd openPCIE && source 4.build/set-env.sh && cd 5.sim
make run USRSIMOPTS="--define DUMP_VCD"
python3 <this repo>/decoders/pcie-tlp/tools/make_captured.py --vcd tb.vcd
```

The run must end with `FIRMWARE RESULT ... PASS (0x0000face)`: link up,
21 TLPs sent by the firmware, the payload written to the EP read back.
`make_captured.py` keeps only `tb.tlp_view`, trims it to the TLP traffic
(#154567000 to #385319000, 8 clocks before the first and after the last
change of a `*_valid`), drops the
`$date` section and writes the same trace for both fixtures (about 500 KB
each).

## What the trace contains

Timescale 1 ps, 62.5 MHz clock (16000 ps). The firmware enumerates the EP
and then moves data:

- 21 requests, RC to EP, requester `10EE`, tags `00` to `14`: CfgRd0 and
  CfgWr0 to `01:00.0` (ID, Command, BAR0 to BAR5 sized and assigned), then
  one MWr32 and one MRd32 to BAR0 at `0x80000000`.
- 20 completions, EP to RC, one per non-posted request: CplD for each read,
  Cpl for each configuration write. The first one, before the bus number is
  set, carries completer `0000`.
- Payload DWs are in TLP byte order, byte 0 in bits 31:24. A configuration
  value is shown byte-swapped (SPEC §5.1): the BAR0 sizing read `0x08F0FFFF`
  is `BAR0 = 0xFFFFF008`. Memory data is shown as on the bus.
- xsim changes data and flags on the clock's own timestamp (zero-delay
  RTL), so each beat is the value from before that edge.
- No X/Z reaches a sampled edge; no framing errors.

## Anchor transactions, checked by hand

Times are VCD ticks (1 ps), one clock = 16000. A TLP spans from the edge of
its first beat to the edge of its last (EOP).

1. **CfgRd ID, 154711000-154743000** (down). Beats `04000001 10EE000F
   01000000`. DW0: Fmt 000, Type 00100 = CfgRd0, Length 1. DW1: requester
   `10EE`, tag `00`, first BE `F`. DW2: bus 01, device 0, function 0,
   register 0x000 = `ID`. The only device is 01:00.0, so compact labels
   omit the BDF. `tlp_sop` rises at 154695000, on the previous edge, so the
   first beat is taken at 154711000; the third beat, EOP, is at 154711000 +
   2 x 16000.
2. **CplD BAR0, 190327000-190375000** (up, full). Beats `4A000001 01000004
   10EE0300 08F0FFFF`. DW0: Fmt 010, Type 01010 = CplD, Length 1. DW1:
   completer `0100` = 01:00.0, status 000 = SC, Byte Count 4. DW2: requester
   `10EE`, tag `03`, lower address 0. Tag 03 from `10EE` is the CfgRd BAR0
   at 189303000 (DW2 `01000010`, register 0x010), so the label names BAR0
   and shows the data byte-swapped: `0xFFFFF008`, a 4 KB prefetchable
   32-bit memory BAR. Label `CplD SC BAR0 = 0xFFFFF008  tag 03`.
3. **MWr32, MRd32 and its CplD, 380231000-385255000.** MWr32 `40000001
   10EE130F 80000000 00000006`: Fmt 010, Type 0, Length 1, address
   0x80000000, data `0x00000006`, posted (no completion; tag 13 is never
   answered). MRd32 `00000001 10EE140F 80000000` at 384183000, tag 14.
   CplD `4A000001 01000004 10EE1400 00000006` at 385207000: requester
   `10EE`, tag 14, so it answers the MRd32, and memory data is not swapped:
   `CplD SC MRd 0x80000000 = 0x00000006  tag 14`. The value written is the
   value read back, as the firmware checks.
