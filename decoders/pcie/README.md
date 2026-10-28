# PCIe decoders (PIPE, serial line, Data Link Layer)

WaveCrux protocol decoders for PCI Express Gen1/Gen2 (2.5 and 5 GT/s,
8b/10b), shipped in one plugin library (`wcx_pcie`): two that read a PIPE
interface, and two that read the serial line itself
([below](#serial-line-decoders)).

| Decoder | Ids | Shows |
|---|---|---|
| **PCIe PIPE** | `ferrite.pcie_pipe_w8`, `_w16`, `_w32`, `_w64` | Ordered sets (TS1/TS2 with link, lane, N_FTS, rate and training control; SKP; FTS; EIOS; EIEOS), logical idle, TLP and DLLP frames, RxStatus events, symbol and framing errors |
| **PCIe serial line** | `ferrite.pcie_serial` (symbols), `ferrite.pcie_serial_dll` (Data Link Layer) | The same two views from the 1-bit TXP or RXP of a SerDes model, plus symbol lock, 8b/10b code and disparity errors, realignment and electrical idle |
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
From WaveCrux 1.2, auto-bind also takes the other names these decoders
declare: `datak` matches `data_k` and any name ending in `_data_k`, `_datak`
or `_k` (`phy_tx_data_k`), and `rxvalid` matches `rx_active` and `rx_valid`
and names ending in `_rx_valid`, `_rxvalid` or `_rx_active`. Where an ending
matches on two buses, the one `data` bound to wins. Older WaveCrux releases
ignore these names; bind such signals by hand there.

### openCologne-PCIE

The co-simulation (`5.sim/tb.sv`) drives a 64-bit PIPE. Bind the 64-bit
decoders to the pcieVHost model's ports in `tb.bfm_pcie` or `tb.dut.bfm_pcie`:
`pclk`, `TxData`/`TxDataK` (one instance) and `RxData`/`RxDataK` (another), or
the top-level `downdata`/`downdatak` and `updata`/`updatak` with `tb.pclk`.
The DLL's internal `phy_tx_data`/`phy_tx_data_k` stream (64-bit, unscrambled,
no ordered sets) decodes with the Data Link Layer decoder too: bind `datak`
to `phy_tx_data_k` (auto-bind finds it from WaveCrux 1.2; bind it by hand
before that) and set
`scrambling` to `off` (or leave `auto`, which locks to `off` at the first
packet). The receive side of that stream has no K signal and cannot be
framed; use the transmit side.

Simulations run unscrambled with 8b/10b disabled, exactly the symbol stream
these decoders expect; a hardware capture is scrambled, which `auto` detects.

### openPCIE

The simulation PIPE is 16 bits wide. `make run USRSIMOPTS="--define DUMP_VCD"`
in `5.sim` dumps exactly the pins the decoders need, at the pcieVHost
endpoint: bind the 16-bit decoders to `tb.dut.u_pcie.u_phy_top.u_pcie_ep`
(`pclk`, `TxData`, `TxDataK` for one direction, `RxData`, `RxDataK` for the
other), and `tb.dut.u_pcie.u_phy_top.rx_active` to `rxvalid` (auto-bind finds
it from WaveCrux 1.2; bind it by hand before that). The
ready-made sessions `5.sim/tb.TLP-DLL-PIPE.wavecrux` and
`5.sim/tb.PIPE-DLL.wavecrux` open that file with the decoders bound.

### Dump only the pins you bind

Dumping a whole PIPE PHY scope also records its fast internal clocks and
shift registers. In openPCIE that made `tb.vcd` four times larger (about
40 MB instead of 15 MB) for the same decode. Dump the bound signals by name
instead, keeping the scope so auto-bind still finds them, for example:

```verilog
$dumpvars(0, tb.dut.phy.pclk);
$dumpvars(0, tb.dut.phy.RxData);
$dumpvars(0, tb.dut.phy.RxDataK);
```

## Parameters

| Name | Values | Default | Decoder | Meaning |
|---|---|---|---|---|
| `scrambling` | `auto` (Detect), `on` (On, descramble), `off` | `auto` | both | `auto` runs a descrambled and a plain decode side by side and locks to the one that first closes a packet with a correct CRC (or to `off` when a TS1/TS2 carries Disable Scrambling); it emits a `Scrambling: …` notice saying which. Until the lock nothing is shown; if no packet ever verifies it locks to `off` after 4096 buffered transactions or at the end of the trace |
| `sample_point` | `before_edge`, `at_edge` | `before_edge` | both | `before_edge` reads the values a flip-flop captures in a zero-delay RTL dump (data changing on the same timestamp as the clock); `at_edge` reads the values present at the edge |
| `coalesce` | bool | `true` | both | PIPE: one transaction per run of identical TS1, TS2, FTS or EIEOS ordered sets, with a `count` (`TS1 ×1024 …`). DLL: one per run of identical DLLPs (`UpdateFC-P ×5 …`, `Ack ×3 seq=12`) and per run of identical InitFC1 or InitFC2 P/NP/Cpl triplets (`InitFC1 ×4 VC0 …`) |
| `show_idle` | bool | `false` | PIPE | One transaction per run of logical idle symbols |
| `max_skew` | 0 to 64 | `8` | both | Multi-lane links: how many symbols apart the lanes' copies of one ordered set may be and still line up |
| `lane_order` | `auto`, `physical` | `auto` | both | Multi-lane links: `auto` takes the logical lane order from the TS1/TS2 Lane#, so a reversed link decodes |

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
  wire CRC and, on a mismatch, `CRC mismatch` and the expected value. With
  `coalesce` on (the default), repeated DLLPs are one transaction with a
  `count`: `UpdateFC-P ×5 VC0 H=10 D=200`, and flow-control initialisation,
  which repeats InitFC1 and then InitFC2 for P, NP and Cpl, shows as
  `InitFC1 ×4 VC0 P H=32 D=1008, NP H=32 D=1, Cpl H=0 D=0`. SKP ordered
  sets and idle between them do not split a run; anything else the decoder
  reports, training ordered sets and gating do (`SPEC.md` §8.3).
- **TLPs** (DLL): `TLP seq=5 MWr32 len=1`, `Nullified TLP seq=5 MWr32`, with
  `LCRC mismatch`, `length mismatch`, `(replay)` and `sequence error` suffixes.
- **Errors**: invalid or unexpected K symbols, truncated packets (by STP, SDP,
  COM, `rxvalid`, `txelecidle` or the end of the trace), END/EDB without a
  start, data outside a packet, unknown ordered sets, X/Z on the data bus,
  PHY-reported RxStatus errors.

A Gen1 DLLP lasts only a few nanoseconds, so its box is often too narrow for
its label. DLLPs and frames also carry a `short_label` (`SPEC.md` §9.1):
`Ack 12`, `UFC-NP`, `IFC1-P`, `PM L1`, `UFC-P ×5`, `IFC1 ×4`, `DLLP`, `TLP`.
From WaveCrux 1.2 the lane draws it when the label does not fit and shows
the full label when you rest the mouse on the box; older releases list it as
an extra field. Zoomed out, coalescing keeps a run of DLLPs to one box.

## Multi-lane links

Bind lanes 1 to 15 next to lane 0: `data1`/`datak1`, `data2`/`datak2`, …,
and optionally `rxvalid<l>`, `txelecidle<l>` and `rxstatus<l>`, all of lane
0's widths. Bind lanes 0 to B-1, with B = 1, 2, 4, 8, 12 or 16; anything
else is reported and nothing is decoded. Auto-bind matches `TxData1`,
`TxDataK1` (and `RxData1`, …) the way it matches lane 0.

The decoders deskew the lanes on their ordered sets (up to `max_skew`
symbols), take the link width from the TS2s at the end of training (or from
the first SKP in a trace that starts in L0), un-stripe the packets and check
the start lane and PAD fill. A link that trains narrower than its binding
is fine: the lanes outside it are ignored, except that their TS1/TS2 are
still watched, so a later retrain to the full width widens the link. The
contract is
[`SPEC.md`](SPEC.md) §12. On top of the x1 output:

- `Link width x2 (4 bound)` and `Lane order 3,2,1,0` (PIPE).
- `Lanes aligned x4 (offsets 0,1,0,2)` at each lock (PIPE), and
  `Lanes not aligned (lane 2)` when the lanes stop lining up.
- Per-lane fields: `TS1 ×4 link=0 lanes=0,1,2,3 n_fts=24 gen1`,
  `SKP ×3,3,2,3`, and `Lane 1 differs: TS2, lane 0 TS1` (PIPE).
- `STP on lane 2` and `PAD outside a packet (lane 5)`.
- `Packet data before lane alignment`: packets before the first lock, not
  decoded (normal for a trace that starts in L0); after a loss, the same as
  an error.
- RxStatus names its lane: `RxStatus: SKP added (lane 2)`.

## Serial line decoders

`ferrite.pcie_serial` and `ferrite.pcie_serial_dll` decode one direction of
an x1 link from its serial line in simulation, where a SerDes model drives
TXP/RXP bit by bit. They recover the bits from the transition times, find
symbol boundaries on COM, decode 8b/10b, and then show exactly what the PIPE
decoder (`pcie_serial`) or the Data Link Layer decoder (`pcie_serial_dll`)
shows. The contract is [`SPEC.md`](SPEC.md) §11.

| Signal | Width | Meaning |
|---|---|---|
| `line` | 1 | TXP or RXP of one lane. X or Z is read as electrical idle |

| Parameter | Values | Default | Meaning |
|---|---|---|---|
| `rate` | `auto` (Detect), `gen1` (2.5 GT/s), `gen2` (5.0 GT/s) | `auto` | `auto` decides per burst: a run of bits shorter than 300 ps means Gen2; otherwise Gen1 after 40 runs. A speed change goes through electrical idle, so each burst is decided again |
| `invert` | bool | `false` | Invert every bit, for an inverted pair or the N side bound |
| `scrambling` | as above | `auto` | as above |
| `coalesce` | as above | `true` | as above, for both |
| `show_idle` | as above | `false` | `pcie_serial` only |

On top of the PIPE and DLL output they add:

- `Symbol lock (Gen1, 2.5 GT/s)` where a burst first aligns on COM
  (`pcie_serial` only).
- `8b/10b code error` (the code group, in wire order; the symbol is replaced
  by EDB, as a PIPE PHY does) and `Disparity error D21.5` (decoded anyway).
- `Symbol lock moved by 3 bits` when a COM arrives off the symbol boundary,
  for example after a bit slip.
- `Electrical idle` for each idle period after the line has been active
  (`pcie_serial` only), and `Packet truncated by electrical idle`.

**Timescale.** A Gen2 bit is 200 ps. Dump the line with a timescale of
100 ps or finer (1 ps or 10 ps is usual for SerDes models); at 1 ns the bits
cannot be placed. A run of one level longer than 8 ns is taken as electrical
idle, not data. The level after the last transition in the dump is never
decoded (its length is unknown), so a symbol right at the end of a dump that
stops on a held level is lost. With a lot of jitter on a Gen1 line, set
`rate` to `gen1`: `auto` reads a one-bit run shorter than 300 ps as Gen2.

## Limits

- The serial line decoders are x1 only. The PIPE and DLL decoders take up
  to x16 (no x32).
- A link decided at the end of training changes at a later TS2 run: it
  shrinks when fewer lanes carry Link# and Lane# (Recovery), and widens
  when more do (a link-down and retrain to a wider width). A link taken from
  the first SKP widens at a later SKP that more lanes carry.
- Skew beyond `max_skew` is not detected as such: on back-to-back TS1/TS2 a
  lane that late can pair with the next ordered set and lose alignment at
  the first row. Raise `max_skew` for slow simulation models.
- Gen1/Gen2 (8b/10b) only; no 128b/130b, no Gen3 equalisation TS fields.
- The DLL decoder's header decode is deliberately light (name, Length, TC,
  TD, EP). It does not check ECRC and does not interpret addresses, requester
  ids or completion status; a TLP-level decoder is the place for that.
- Scaled flow control (Gen4+) and the newer DLLP types (MR_Init,
  Data_Link_Feature, NOP) are reported as reserved.

## Checking a decode without WaveCrux

The release archive carries `tools/wcxhost` (`tools/wcxhost.exe` on
Windows), which runs these decoders over a VCD from the command line, for a
check in CI. With a bindings file naming the signals as in [Signals](#signals)
(here for openPCIE's 16-bit PIPE, transmit side):

```json
{
  "decoder": "ferrite.pcie_dll_w16",
  "signal_bindings": {
    "pclk": "tb.dut.u_pcie.u_phy_top.u_pcie_ep.pclk",
    "data": "tb.dut.u_pcie.u_phy_top.u_pcie_ep.TxData",
    "datak": "tb.dut.u_pcie.u_phy_top.u_pcie_ep.TxDataK"
  }
}
```

run, from the folder you unpacked the archive into (macOS shown; the library
is `libwcx_pcie.so` on Linux and `wcx_pcie.dll` on Windows):

```
tools/wcxhost --plugin ./libwcx_pcie.dylib --vcd tb.vcd --bindings tx.bindings.json --sort
```

Add `--check expected.json` to compare with a list you have checked by hand;
the exit status is 1 at the first difference. `tools/README.md` in the
archive has the rest.

## Building and testing

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev -R pcie
```

`tests/unit_*.c` hold the unit tests (CRCs, scrambler, 8b/10b, framing,
both front ends, the serial line front end, scrambling detection, the
plugin's ABI behaviour and multi-lane links), `tests/vcd/`
hand-derived smoke fixtures, `fixtures/` the independently generated golden
fixtures, and `fuzz/` the libFuzzer target and its seed corpus.
