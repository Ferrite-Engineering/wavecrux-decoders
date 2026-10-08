# Hand-written smoke fixtures

Small VCDs that wire the whole plugin through the host emulator (`wcxhost
--sort --check` and `--lifecycle`, test names `pcie.golden.smoke.*` and
`pcie.lifecycle.smoke.*`). They are **not** the decoders' golden fixtures;
those come from the independent generator under `../../fixtures/`. Every
expected transaction here is derived by hand from `SPEC.md` and the known
answers of `docs/protocol-notes.md`, never from running the decoder (testing
standard §2, source 3). The PIPE and DLL variants of a fixture share one
trace; only the bindings differ.

Times: `startTime = floor(start_fs / fs_per_tick)`, `endTime = ceil(end_fs /
fs_per_tick)`. With `N` lanes and period `P`, lane `j` of the edge at `t`
starts at `t + floor(j P / N)` and lasts `floor(P / N)` (SPEC.md §4.2); the
first edge has `P = 0`. All zero-delay traces launch the word for edge `k` at
edge `k - 1`, so the pre-edge sample holds it.

## w16_basic_1ns (1 ns per tick, 16-bit, zero delay, defaults)

`pclk` rises at 4, 12, 20, … ns (edge `k` at `8k - 4`); two lanes of 4 ns.
Symbols: idle word at edge 1; TS1 ×4 (COM PAD PAD 0x18 0x02 0x00 then ten
0x4A, 8 words each, edges 2-33); SKP OS (COM + 3 SKP, edges 34-35); two idle
words (36-37); InitFC1-P `5C 40 08 03 F0 35 BC FD` (edges 38-41, CRC from
protocol-notes §6); Ack seq 0 `5C 00 00 00 00 B3 62 FD` (42-45); the MRd32 of
protocol-notes §7, seq 0, LCRC `9A E8 F8 C2` (46-55); idle (56-57).

`scrambling` is `auto`: both pipelines buffer until the InitFC1-P closes with
a valid CRC-16 in the `off` pipeline (the descrambled bytes do not verify), so
`off` wins at that packet (§5.5). The buffered transactions come out, then the
notice spanning the packet, then the rest directly.

| PIPE transaction | start | end | derivation |
|---|---|---|---|
| `TS1 ×4 link=PAD lane=PAD n_fts=24 gen1` | 12 | 268 | edge 2 lane 0 → edge 33 lane 1 end (260 + 4 + 4) |
| `SKP` | 268 | 284 | edge 34 lane 0 → edge 35 lane 1 end |
| `DLLP frame (6 symbols)` | 300 | 332 | edge 38 → edge 41 lane 1 end |
| `Scrambling: off (detected)` | 300 | 332 | spans the winning packet |
| `DLLP frame (6 symbols)` | 332 | 364 | the Ack |
| `TLP frame seq=0 (18 symbols)` | 364 | 444 | edge 46 → edge 55 lane 1 end |

DLL: `InitFC1-P VC0 H=32 D=1008` (crc `0x35BC`), the notice, `Ack seq=0` (crc
`0xB362`), `TLP seq=0 MRd32 len=1` (bytes 12, lcrc `0x9AE8F8C2`, replay
false), same spans. FC fields: b1 = 0x08 → HdrFC = (0x08 & 0x3F) << 2 = 32;
b2 = 0x03, b3 = 0xF0 → DataFC = 0x3F0 = 1008.

## w8_scrambled_10ps (10 ps per tick, 8-bit, zero delay, defaults)

Edge `k` at `400k - 200` ticks. Symbols from edge 2: COM, SKP SKP SKP (not
advancing the LFSR), SDP (takes key k0 = FF), then InitFC1-P bytes XORed with
k1..k6 = `17 C0 14 B2 E7 02` (protocol-notes §3): `57 C8 17 42 D2 BE`, END,
then two scrambled idles `72 6E` (k8, k9; END took k7).

The `on` pipeline closes a valid InitFC1-P at the END (edge 13), the `off`
pipeline a CRC mismatch, so `on` wins.

| PIPE | start | end |
|---|---|---|
| `SKP` | 600 | 2200 (edge 2 → edge 5 end) |
| `DLLP frame (6 symbols)` | 2200 | 5400 (edge 6 → edge 13 end) |
| `Scrambling: on (detected)` | 2200 | 5400 |

DLL: `InitFC1-P VC0 H=32 D=1008` and the notice, same span.

## w64_lanes_100ps (100 ps per tick, 64-bit, data changes between edges, `off`, `at_edge`, PIPE `show_idle`)

Edges at 50, 150, 250, … ticks (period 100 = 10 ns, 12.5 ticks per lane,
exact in fs). Lane `j` starts at `t + 12.5 j` fs-exact: 0, 12.5, 25, 37.5, …;
the host floors starts and ceils ends, so lane 3 starts at `t + 37` and lane 2
ends at `t + 38`. Words (lane 0 first): edge 1 idle; edge 2 `00 00 00 SDP 40
08 03 F0`; edge 3 `35 BC END 00 00 00 00 STP`; edge 4 `00 00 00 00 00 01 01
00`; edge 5 `00 0F 00 00 10 00 9A E8`; edge 6 `F8 C2 END 00 00 00 00 00`;
edge 7 idle.

| PIPE | start | end | derivation |
|---|---|---|---|
| `Idle ×11` | 50 | 188 | 8 idles at edge 1 (zero length) + lanes 0-2 of edge 2; ends at 150 + 37.5 |
| `DLLP frame (6 symbols)` | 187 | 288 | edge 2 lane 3 (187.5) → edge 3 lane 2 end (287.5) |
| `Idle ×4` | 287 | 338 | edge 3 lanes 3-6 |
| `TLP frame seq=0 (18 symbols)` | 337 | 588 | edge 3 lane 7 (337.5) → edge 6 lane 2 end (587.5) |
| `Idle ×13` | 587 | 750 | edge 6 lanes 3-7 + edge 7; emitted at flush |

DLL: `InitFC1-P VC0 H=32 D=1008` 187-288 and `TLP seq=0 MRd32 len=1`
337-588.

## w32_errors_1ps (1 ps per tick, 32-bit, zero delay, `off`, rxvalid and rxstatus bound)

Edge `k` at `16000k - 8000`; four lanes of 4000. Words: edge 2 `STP 00 00
00`; edge 3 x data with rxvalid 0 (gated); edge 4 x data, rxvalid 1, rxstatus
100; edge 5 idle, rxstatus 100; edge 6 `END 00 00 00`; edge 7 `9C(K) SDP 00
00`; edge 8 `00 00 SKP(K) END`; edge 9 `12 34 00 00`; edge 10 idle.

| PIPE | start | end | rule |
|---|---|---|---|
| `Packet truncated by rxvalid` (tlp, 3 symbols) | 24000 | 40000 | §4.3: STP at edge 2 lane 0 to the last body symbol |
| `RxStatus: 8b/10b decode error` (`0b100`, error) | 56000 | 72000 | §4.6: ungated edge, changed from 0 |
| `X/Z on data` (edges 1) | 56000 | 72000 | §4.4: the gated x edge does not count |
| `END without start` | 88000 | 92000 | §6 |
| `Invalid K symbol 0x9C` | 104000 | 108000 | §6 |
| `Invalid K symbol in packet 0x1C` | 128000 | 132000 | §10.2: the DLLP opened by SDP is dropped |
| `END without start` | 132000 | 136000 | the END after the dropped packet |
| `Data outside a packet` (2, first `0x12`) | 136000 | 144000 | §6 |

DLL: the same without the RxStatus line (§2: the DLL decoder ignores
`rxstatus`). Every `error` sentence is the fixed text of §10.1.
