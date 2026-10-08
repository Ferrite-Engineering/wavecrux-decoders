# Captured fixtures: provenance

Two fixtures share one trace, recorded from a real RTL simulation:

| Stem | Decoder | Parameters |
|---|---|---|
| `opencologne_dll_tb_w64_100fs` | `ferrite.pcie_dll_w64` | defaults (`scrambling=auto`) |
| `opencologne_dll_tb_pipe_w64_100fs` | `ferrite.pcie_pipe_w64` | `scrambling=off` |

Both bind `pclk` = `tb_dll_logic.phy_clk`, `data` = `tb_dll_logic.phy_tx_data`
(64 bits) and `datak` = `tb_dll_logic.phy_tx_data_k` (8 bits).

## Source

- Upstream: <https://github.com/chili-chips-ba/openCologne-PCIE>, commit
  `ef0147959f1ecf888d756ce5133cd37c88edb936`.
- Files used, unmodified: `2.rtl/4.dll/sim/tb_dll_logic.v` (the testbench)
  and `2.rtl/4.dll/dll_src/*.v` (the Data Link Layer RTL). Nothing from the
  repository is copied here except the simulation's output trace.
- Licence: BSD-3-Clause, the repository's `LICENSE`. These files carry no
  per-file header, so the repository licence applies. Files in that
  repository marked proprietary or carrying no licence (for example under
  `5.sim/`) were not used.

The trace is output of that BSD-3-Clause testbench and RTL, redistributed
under its notice:

```
BSD 3-Clause License

Copyright (c) 2026, Chili.CHIPS*ba

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

## Reproducing

With Icarus Verilog 13.0 (`iverilog`, `vvp`) on `PATH`, from the root of
this repository:

```
git clone https://github.com/chili-chips-ba/openCologne-PCIE
git -C openCologne-PCIE checkout ef0147959f1ecf888d756ce5133cd37c88edb936
python3 decoders/pcie/tools/make_captured.py --opencologne openCologne-PCIE
```

`make_captured.py` runs, in a fresh temporary directory that has a `sim/`
subdirectory:

```
iverilog -g2012 -Wall -o tb_dll_logic.vvp -s tb_dll_logic \
    <oc>/2.rtl/4.dll/sim/tb_dll_logic.v <oc>/2.rtl/4.dll/dll_src/*.v
vvp -n tb_dll_logic.vvp -vcd
```

The testbench calls `$dumpfile("sim/dll_sim.fst")`, but `vvp -vcd` writes VCD
text whatever the name. The run must print `TEST PASSED` (the testbench's own
check: `=== SUMMARY: 0 errors ===`) or the script stops. The script then
deletes the `$date ... $end` section, the only part of the dump that changes
between runs, and writes the identical trace as both fixtures' `.vcd` (about
250 KB each, untrimmed: the whole 2.245 us run). Icarus warns about timescale
inheritance, a truncation at `virtual_channel.v:343` and an unconnected
`retransmit_seq_num`; none affects the dump.

The expected files come from `decoders/pcie/tools/pcie_ref_decoder.py`, a
small reference decoder written from `decoders/pcie/SPEC.md` independently of
the C decoders (and of the fixture encoder). `make_captured.py --expected-only`
recomputes them from the committed VCDs, and `generate_fixtures.py --check`
fails if they would change.

## What the trace contains

The testbench drives the DLL's transaction-layer side and loops `phy_tx_data`
back into `phy_rx_data` one clock later, so the DLL talks to itself. The
fixtures decode the **transmit** stream at the DLL-to-MAC byte interface:
unscrambled, no ordered sets, 8 symbols per `phy_clk` (100 MHz, timescale
100 fs, so one clock is 100000 ticks), byte lane 0 first, K flags on
`phy_tx_data_k`. Data changes on the clock edge (zero-delay RTL), so each
word is captured at the next rising edge (`sample_point=before_edge`).

- 34 DLLPs: InitFC1-Cpl/NP/P ×14 and InitFC2-P/Cpl/NP ×14 (VC0; P: H=32
  D=1008, NP: H=32 D=1, Cpl: 0/0), Ack seq 0, 1, 2, Nak seq 2, Ack seq 3,
  UpdateFC-P H=10 D=200. Every CRC verifies.
- 5 TLPs, seq 0, 1, 2, 3 and seq 3 again (the replay after the Nak; the
  testbench corrupts only the loopback copy of the first seq 3, so the
  transmit side shows the same good TLP twice). Every LCRC verifies. Their
  bodies are the testbench's arbitrary payload words, not real TLP headers,
  so the light header decode gives `Type 0x..` names and a Length field that
  does not match: each TLP is reported with ` length mismatch`, and the
  second seq 3 with ` length mismatch (replay)`. That is the SPEC's behaviour
  on this traffic, not a fault in the trace.
- Between packets the bus carries D 0x00 with K = 0 (logical idle, which
  neither decoder reports with default parameters). No X/Z reaches a sampled
  edge.

With `scrambling=auto` the DLL decoder locks to `off` at the first valid
DLLP (InitFC1-Cpl, 1150000-1250000) and reports `Scrambling: off
(detected)` there.

## Cross-checks

- The testbench's own result: `TEST PASSED`, 0 errors.
- The RTL's hard-coded InitFC CRCs (`send_DLLP.v`): 35 BC (InitFC1-P),
  B1 F6 (InitFC1-NP), D8 92 (InitFC1-Cpl), 4F C3 (InitFC2-P), CB 89
  (InitFC2-NP), A2 ED (InitFC2-Cpl). The expected file's `crc` fields carry
  exactly these, and the reference decoder's own CRC-16 (reflected, table
  free) finds each of them correct.
- The RTL computes LCRCs with parallel XOR equations; the reference decoder
  checks them with zlib's CRC-32. All five agree.

## Anchor transactions, checked by hand

Times are VCD ticks (100 fs). "Word @t" is the value written to
`phy_tx_data` at `#t`, bytes listed lane 0 first; it is captured at the
next rising edge, t + 100000.

1. **InitFC1-P, 1350000-1450000.** Word @1250000 is
   `0xFDBC35F00308405C`: lane 0 = 5C (SDP, K), then `40 08 03 F0`, CRC
   `35 BC`, lane 7 = FD (END, K); `phy_tx_data_k` = `10000001`. Byte 0 0x40 is
   InitFC1-P VC0; HdrFC = (0x08 & 0x3F) << 2 | 0x03 >> 6 = 32; DataFC =
   (0x03 & 0xF) << 8 | 0xF0 = 1008. CRC 35 BC is protocol-notes §6's vector
   for these bytes. One clock wide: start = t_k, end = t_k + P.
2. **TLP seq=0, 4750000-5150000.** Word @4650000 is `0x03040506070000FB`
   with K only on lane 0: STP, seq bytes `00 00`, then TLP bytes
   `07 06 05 04 03`. Byte 0 0x07 = Fmt 000, Type 00111, not in the SPEC
   §8.2 table, so the name is `Type 0x07`; Length = (0x05 & 3) << 8 | 0x04
   = 260 DW. Word @4950000 = `0xFD1E19D1F9201022`, K on lane 7: TLP bytes
   `22 10 20`, LCRC `F9 D1 19 1E`, END. TLP bytes: 5 + 8 + 8 + 3 = 24, no
   data payload for a 3-DW Fmt 000 header, so 24 != 12: ` length mismatch`.
   The span runs from the STP (edge 4750000, lane 0) to the END (edge
   5050000, lane 7, ending at 5050000 + 100000).
3. **UpdateFC-P VC0 H=10 D=200, 21750000-21850000.** Word @21650000 is
   `0xFDF8ECC88002805C`: SDP, `80 02 80 C8`, CRC `EC F8`, END. HdrFC =
   (0x02 & 0x3F) << 2 | 0x80 >> 6 = 10; DataFC = (0x80 & 0xF) << 8 | 0xC8 =
   200. CRC EC F8 is protocol-notes §6's UpdateFC-P vector.
4. **The replay, 18550000-18950000.** The second TLP with seq 3 follows the
   Nak (seq 2, 17150000). SPEC §8.2: after seq 3, `next` = 4, and
   (4 - 3) mod 4096 = 1 is inside 1..2048, so it is a replay: label suffix
   ` (replay)`, `replay: true`, `next` unchanged; the Ack seq 3 follows at
   19950000.
