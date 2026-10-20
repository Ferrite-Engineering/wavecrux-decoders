# Changelog

All notable changes to the PCIe decoders (`wcx_pcie`). The format is
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added

- Each release archive now carries `tools/wcxhost` (`tools/wcxhost.exe` on
  Windows), the host emulator, signed like the library: it runs these
  decoders over a VCD without WaveCrux, for a check in CI. See
  `tools/README.md` in the archive.

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
