# PCIe decoders (PIPE, Data Link Layer)

Two WaveCrux protocol decoders for PCI Express Gen1/Gen2 (2.5 and 5 GT/s,
8b/10b) as seen on a PIPE interface, shipped in one plugin library
(`wcx_pcie`):

| Decoder | Ids | Shows |
|---|---|---|
| **PCIe PIPE** | `ferrite.pcie_pipe_w8`, `_w16`, `_w32`, `_w64` | Ordered sets (TS1/TS2 with link, lane, N_FTS, rate and training control; SKP; FTS; EIOS; EIEOS), logical idle, TLP and DLLP frames, RxStatus events, symbol and framing errors |
| **PCIe Data Link Layer** | `ferrite.pcie_dll_w8`, `_w16`, `_w32`, `_w64` | Every Gen1/Gen2 DLLP (Ack, Nak, InitFC1/InitFC2/UpdateFC for P, NP and Cpl, PM_*, Vendor) with CRC-16 checks; TLPs with LCRC checks, nullified TLPs, the header name (MRd32, MWr64, CfgRd0, CplD, Msg, …), Length, TC, TD and EP, the Length check, and sequence-number tracking (replays, wrap at 4096, out-of-order errors) |

The four ids per decoder are the four PIPE data widths (8, 16, 32 and 64
bits, one to eight symbols per clock). WaveCrux auto-binds a signal only to a
decoder declaring its exact width, so pick the id that matches your `data`
bus. One decoder instance decodes one direction (TX or RX); add two instances
for both.

They were written for [openCologne-PCIE](https://github.com/chili-chips-ba/openCologne-PCIE)
and [openPCIE](https://github.com/chili-chips-ba/openPCIE) and follow the
output contract in [`SPEC.md`](SPEC.md); the protocol facts and known-answer
vectors are in [`docs/protocol-notes.md`](docs/protocol-notes.md).

## Requirements

**WaveCrux 1.0.1 or later.** (1.0.0 placed plugin transactions at the wrong
time on waveforms whose timescale is not 1 fs.) WaveCrux's notarized macOS
build loads only plugins signed by Ferrite Engineering; use the release
archives on macOS, or a build from source on Linux and Windows.

## Signals

Both decoders bind the same signals, named after the PIPE pins:

| Signal | Width | Required | Meaning |
|---|---|---|---|
| `pclk` | 1 | yes | PIPE parallel clock; symbols are captured on its rising edge |
| `data` | 8N | yes | PIPE `TxData` or `RxData`; byte lane 0 (bits 7:0) is the first symbol on the wire |
| `datak` | N | yes | PIPE `TxDataK` or `RxDataK`; bit j marks byte lane j as a K (control) symbol |
| `rxvalid` | 1 | no | PIPE `RxValid`; when bound, edges where it is 0 carry no symbols. Unbound: always valid |
| `txelecidle` | 1 | no | PIPE `TxElecIdle`; when bound, edges where it is 1 carry no symbols. Unbound: never idle |
| `rxstatus` | 3 | no | PIPE `RxStatus`; the PIPE decoder annotates SKP add/remove, receiver detection and PHY errors. The DLL decoder declares it so one binding set fits both, and ignores it |

WaveCrux's auto-bind matches a decoder signal as the suffix of a waveform
signal under a shared prefix, so `TxData`/`TxDataK` and `RxData`/`RxDataK`
bind as the `Tx` and `Rx` groups; the prefix drop-down picks the direction.

### openCologne-PCIE

The co-simulation (`5.sim/tb.sv`) drives a 64-bit PIPE. Bind the 64-bit
decoders to the pcieVHost model's ports in `tb.bfm_pcie` or `tb.dut.bfm_pcie`:
`pclk`, `TxData`/`TxDataK` (one instance) and `RxData`/`RxDataK` (another), or
the top-level `downdata`/`downdatak` and `updata`/`updatak` with `tb.pclk`.
The DLL's internal `phy_tx_data`/`phy_tx_data_k` stream (64-bit, unscrambled,
no ordered sets) decodes with the Data Link Layer decoder too: bind `datak`
to `phy_tx_data_k` by hand (auto-bind does not match `data_k`) and set
`scrambling` to `off` (or leave `auto`, which locks to `off` at the first
packet). The receive side of that stream has no K signal and cannot be
framed; use the transmit side.

Simulations run unscrambled with 8b/10b disabled, exactly the symbol stream
these decoders expect; a hardware capture is scrambled, which `auto` detects.

### openPCIE

The simulation PIPE is 16 bits wide. Bind the 16-bit decoders to
`tb.dut.pcie_inst.serdes_front_i.u_pcie_ep` (`pclk`, `TxData`, `TxDataK`,
`RxData`, `RxDataK`) or to the `ep_tx_data`/`ep_tx_datak` and
`ep_rx_data`/`ep_rx_datak` registers with `pipe_clk`. The stock `DUMP_VCD`
does not include the PIPE scope; add it to `$dumpvars` (for example
`$dumpvars(1, dut.pcie_inst.serdes_front_i.u_pcie_ep);`). `rx_active` can be
bound to `rxvalid` by hand.

## Parameters

| Name | Values | Default | Decoder | Meaning |
|---|---|---|---|---|
| `scrambling` | `auto` (Detect), `on` (On, descramble), `off` | `auto` | both | `auto` runs a descrambled and a plain decode side by side and locks to the one that first closes a packet with a correct CRC (or to `off` when a TS1/TS2 carries Disable Scrambling); it emits a `Scrambling: …` notice saying which. Until the lock nothing is shown; if no packet ever verifies it locks to `off` after 4096 buffered transactions or at the end of the trace |
| `sample_point` | `before_edge`, `at_edge` | `before_edge` | both | `before_edge` reads the values a flip-flop captures in a zero-delay RTL dump (data changing on the same timestamp as the clock); `at_edge` reads the values present at the edge |
| `coalesce` | bool | `true` | PIPE | One transaction per run of identical TS1, TS2, FTS or EIEOS ordered sets, with a `count` (`TS1 ×1024 …`) |
| `show_idle` | bool | `false` | PIPE | One transaction per run of logical idle symbols |

## What you see

Every transaction carries a `type` field and the decoded fields of
`SPEC.md` §7 (PIPE) or §8 (DLL); hex values are `0x`-prefixed strings as the
specification prints them. Errors are flagged, carry a one-sentence `error`
field, and never stop the decode: it resynchronises at the next symbol.

- **Ordered sets**: `TS1 ×4 link=PAD lane=PAD n_fts=24 gen1`, `SKP` (or
  `SKP ×2` when the elastic buffer changed the count), `FTS ×8`, `EIOS`,
  `EIEOS`.
- **Frames** (PIPE): `TLP frame seq=5 (24 symbols)`, `DLLP frame (6 symbols)`.
- **DLLPs** (DLL): `Ack seq=12`, `Nak seq=5`, `InitFC1-P VC0 H=32 D=1008`,
  `UpdateFC-P VC0 H=10 D=200`, `PM_Enter_L1`, `Vendor 0x123456`, with the
  wire CRC and, on a mismatch, `CRC mismatch` and the expected value.
- **TLPs** (DLL): `TLP seq=5 MWr32 len=1`, `Nullified TLP seq=5 MWr32`, with
  `LCRC mismatch`, `length mismatch`, `(replay)` and `sequence error` suffixes.
- **Errors**: invalid or unexpected K symbols, truncated packets (by STP, SDP,
  COM, `rxvalid`, `txelecidle` or the end of the trace), END/EDB without a
  start, data outside a packet, unknown ordered sets, X/Z on the data bus,
  PHY-reported RxStatus errors.

## Limits

- **x1 links only.** A multi-lane link stripes symbols across lanes and
  needs lane deskew; bound to lane 0 of a wider link the PIPE decoder still
  shows the ordered sets, but packets will appear broken.
- Gen1/Gen2 (8b/10b) only; no 128b/130b, no Gen3 equalisation TS fields.
- The DLL decoder's header decode is deliberately light (name, Length, TC,
  TD, EP). It does not check ECRC and does not interpret addresses, requester
  ids or completion status; a TLP-level decoder is the place for that.
- Scaled flow control (Gen4+) and the newer DLLP types (MR_Init,
  Data_Link_Feature, NOP) are reported as reserved.

## Building and testing

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev -R pcie
```

`tests/unit_*.c` hold the unit tests (CRCs, scrambler, framing, both front
ends, scrambling detection and the plugin's ABI behaviour), `tests/vcd/`
hand-derived smoke fixtures, `fixtures/` the independently generated golden
fixtures, and `fuzz/` the libFuzzer target and its seed corpus.
