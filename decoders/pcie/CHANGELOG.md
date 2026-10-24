# Changelog

All notable changes to the PCIe decoders (`wcx_pcie`). The format is
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Multi-lane links, x2 to x16 (`SPEC.md` §12), for the PIPE and Data Link
  Layer decoders, on the existing ids: optional `data1`/`datak1` …
  `data15`/`datak15` and per-lane `rxvalid`, `txelecidle`, `rxstatus`.
  Lane deskew on the ordered sets (`max_skew`, default 8 symbols), the link
  width from the TS2s at the end of training or, in a trace that starts in
  L0, from SKP (widened by a later SKP on more lanes), lane
  reversal from Lane# (`lane_order`), per-lane descrambling, un-striping,
  start-lane and PAD-fill checks. New transactions: `link_width`,
  `lane_order`, `lanes_aligned`, `lane_skew`, `lane_mismatch`,
  `framing_lane`, `pad_misplaced`, `unaligned`; TS1/TS2 carry `links` and
  `lanes`, SKP OS per-lane counts, RxStatus its lane. With one lane bound
  the output is unchanged. Requested in #8.
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
