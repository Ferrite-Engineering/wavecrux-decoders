# Changelog

All notable changes to the PCIe decoders (`wcx_pcie`). The format is
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Auto-bind names for common PIPE signal spellings (`SPEC.md` §2): `datak`
  also binds to `data_k` and names ending in `_data_k`, `_datak` or `_k`
  (`phy_tx_data_k`), and `rxvalid` to `rx_active`, `rx_valid` and names
  ending in `_rx_valid`, `_rxvalid` or `_rx_active`. Needs WaveCrux 1.2 or
  later; older releases ignore them. Requested in #4.
- Short labels for narrow boxes (`SPEC.md` §9.1): every DLLP and every
  PIPE `DLLP frame` and `TLP frame` carries a `short_label` field (`Ack 12`,
  `UFC-NP`, `IFC1-P`, `PM L1`, `Rsvd`, `DLLP`, `TLP`), right after `type`.
  WaveCrux 1.2 draws it when the label does not fit and shows the full label
  on hover; older releases list it as a field. Requested in #5.
- The vendored ABI header is 1.3 (WaveCrux 1.2). Plugins still load in
  WaveCrux 1.0.1 and later.
- DLLP coalescing (`SPEC.md` §8.3): the Data Link Layer decoders
  (`ferrite.pcie_dll_*` and `ferrite.pcie_serial_dll`) gain the `coalesce`
  parameter, on by default as in the PIPE decoder. A run of identical DLLPs
  (repeated UpdateFC, Ack or PM DLLPs) is one transaction with a `count`
  (`UpdateFC-P ×5 VC0 H=10 D=200`, short label `UFC-P ×5`), and a run of
  identical InitFC1 or InitFC2 P/NP/Cpl triplets, in any order on the wire,
  is one too (`InitFC1 ×4 VC0 P H=32 D=1008, NP H=32 D=1, Cpl H=0 D=0`,
  short label `IFC1 ×4`). SKP ordered sets and idle do not split a run;
  any other transaction of the decoder, a training ordered set or a break
  does. With `coalesce` off the output is as before. Requested in #5.

### Changed

- With the default `coalesce=true`, the Data Link Layer decoders now show
  repeated DLLPs and InitFC triplets as one transaction each (see Added). A
  DLLP that may begin an InitFC triplet is held until the next DLLP shows
  whether it does, so in `auto` scrambling the `Scrambling: … (detected)`
  notice can come before the DLLP that locked it.

### Fixed

- A link width decided at the end of training now grows at a later TS2 run
  (`SPEC.md` §12.4, §12.5). Lanes outside the link were no longer watched,
  so after a link-down and retrain from x2 to x4 the decoder stayed at x2
  and the packets that followed decoded as truncated. Lanes outside the link
  now still join TS1/TS2 rounds (and nothing else), and the TS2 run that
  ends the retrain widens the link. Reported in #20.

## [0.3.0] - 2026-10-28

### Added

- Multi-lane links, x2 to x16 (`SPEC.md` §12), for the PIPE and Data Link
  Layer decoders, on the existing ids: optional `data1`/`datak1` …
  `data15`/`datak15` and per-lane `rxvalid`, `txelecidle`, `rxstatus`.
  Lane deskew on the ordered sets (`max_skew`, default 8 symbols), the link
  width from the TS2s at the end of training or, in a trace that starts in
  L0, from SKP (widened by a later SKP on more lanes; never once a TS1/TS2
  was seen, so a SKP during Polling does not decide it), lane
  reversal from Lane# (`lane_order`), per-lane descrambling, un-striping,
  start-lane and PAD-fill checks. New transactions: `link_width`,
  `lane_order`, `lanes_aligned`, `lane_skew`, `lane_mismatch`,
  `framing_lane`, `pad_misplaced`, `unaligned`; TS1/TS2 carry `links` and
  `lanes`, SKP OS per-lane counts, RxStatus its lane. A SKP right after the
  last TS2 does not hide the end of the TS2 run (up to 16 SKP OSs; at 16
  the run ends), SKP add/remove does not
  emit `lanes_aligned`, and a stuck lane gives `pad_misplaced` and
  `unknown_os` runs with `occurrences`, not one error per row. With one lane
  bound the output is unchanged. Requested in #8.
- Each release archive now carries `tools/wcxhost` (`tools/wcxhost.exe` on
  Windows), the host emulator, signed like the library: it runs these
  decoders over a VCD without WaveCrux, for a check in CI. See
  `tools/README.md` in the archive.
- Fixtures for both directions of one x1 link in one VCD
  (`serial_link_x1_gen2_*`, `serial_dll_link_x1_gen2_*`): training at
  2.5 GT/s, a speed change to 5.0 GT/s, Acks, a Nak and its replay, and L0s,
  each line through both serial decoders. Test data only; the decoders are
  unchanged.

## [0.2.0] - 2026-10-15

### Added

- PCIe serial line decoders (`ferrite.pcie_serial`, `ferrite.pcie_serial_dll`,
  `SPEC.md` §11): decode an x1 Gen1/Gen2 link from the 1-bit serial line of
  a SerDes model. Bit recovery from transition times, per-burst rate
  detection (`rate` = `auto`, `gen1`, `gen2`), COM alignment with
  realignment, 8b/10b decoding with code and disparity errors, electrical
  idle (X/Z or a constant level for more than 8 ns), `invert`, and then the
  PIPE or Data Link Layer decoder's output on the recovered symbols.

## [0.1.0] - 2026-10-08

### Added

- PCIe PIPE decoder (`ferrite.pcie_pipe_w8`, `_w16`, `_w32`, `_w64`): TS1/TS2,
  SKP, FTS, EIOS and EIEOS ordered sets with coalescing of repeated sets,
  logical idle, TLP and DLLP framing, RxStatus annotations, and the symbol
  and framing errors of `SPEC.md` §6.
- PCIe Data Link Layer decoder (`ferrite.pcie_dll_w8`, `_w16`, `_w32`,
  `_w64`): every Gen1/Gen2 DLLP type with CRC-16 checking, TLPs with LCRC
  checking, nullified TLPs, the light header decode (Fmt/Type name, Length,
  TC, TD, EP) with the Length check, and sequence-number tracking with
  replay and wrap detection.
- Scrambling handling: `off`, `on` (Gen1/Gen2 LFSR descrambler with the
  ordered-set bypass) and `auto`, which runs both and locks on the first
  packet with a correct CRC or on the Disable Scrambling training-set bit.
- `rxvalid` and `txelecidle` gating, X/Z run reporting, `before_edge` /
  `at_edge` sample point.
- One plugin library registering all eight decoders, for WaveCrux 1.0.1 and
  later.
