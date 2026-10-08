# Changelog

All notable changes to the PCIe decoders (`wcx_pcie`). The format is
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

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
