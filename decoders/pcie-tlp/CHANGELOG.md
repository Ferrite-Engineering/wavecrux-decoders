# Changelog

All notable changes to the PCIe TLP decoder (`wcx_pcie_tlp`). The format is
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.0] - unreleased

First release here. The decoder comes from openPCIE
(`5.sim/tools/wavecrux-pcie-tlp`), now Apache-2.0 and built on `common/`.

### Changed (from the openPCIE plugin)

- Output follows `SPEC.md` and the decoder conventions: `type` first and the
  TLP name only (`MRd32`, `CplD`); numbers as JSON numbers; requester and
  completer as `BB:DD.F`; the first payload DW as `data` (was `data0`,
  `payload_dw`, `as_register`).
- Values are sampled just before the clock edge, so zero-delay dumps decode
  on the right clock. A TLP ends at its EOP edge, not one period later.
- Runs of X/Z beats and of beats without SOP give one error with
  `occurrences`, not one per beat.
- Errors carry `type` `error`, an `error` sentence and `expected` /
  `actual` where they apply.
- Completions pair with a request by requester ID and tag (was tag only),
  and a split completion keeps the request for the parts that follow.
- A completion to an I/O request says `IORd` / `IOWr`.

### Fixed

- `header` lists all four DWs of a 4 DW header (it showed three).
- A sample of the wrong width is rejected (it was read past its end).
