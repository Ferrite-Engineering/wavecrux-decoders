# Changelog

All notable changes to the PCIe TLP decoder (`wcx_pcie_tlp`). The format is
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed

- With `span = until_next`, the last transaction now ends at the last clock
  edge, not at its EOP: a short last TLP was a few pixels wide when zoomed
  out, with no label.

## [0.1.0] - 2026-10-21

First release here. The decoder comes from openPCIE
(`5.sim/tools/wavecrux-pcie-tlp`), now Apache-2.0 and built on `common/`.

### Changed (from the openPCIE plugin)

- Output follows `SPEC.md` and the decoder conventions: `type` first and the
  TLP name only (`MRd32`, `CplD`); numbers as JSON numbers; requester and
  completer as `BB:DD.F`; the first payload DW as `data` (was `data0`,
  `payload_dw`, `as_register`).
- Values are sampled just before the clock edge, so zero-delay dumps decode
  on the right clock. A TLP ends at its EOP edge, not one period later.
- Runs of X/Z beats, of beats without SOP, of SOP before EOP and of TLPs
  shorter than their header give one error with `occurrences`, so a stuck
  or misbound `tlp_sop` no longer gives one error per clock.
- Errors carry `type` `error`, an `error` sentence and `expected` /
  `actual` where they apply. A UR or CA completion with a length mismatch
  reports both.
- Completions pair with a request by requester ID and 10-bit tag (was the
  8-bit tag alone): two requesters, or two functions, using one tag no
  longer overwrite each other. Up to 4 requesters per tag; a fifth replaces
  the oldest. A split completion keeps the request for the
  parts that follow, counting the lower address of the first part.
- A completion to an I/O request says `IORd` / `IOWr`.
- TLP prefixes and reserved Type/Fmt combinations (a locked write; a 4 DW
  configuration, I/O or completion header; a 3 DW message header) decode as
  `TLP Fmt <f> Type <t>`, not as the TLP they resemble.

### Fixed

- `header` lists all four DWs of a 4 DW header (it showed three).
- A sample of the wrong width is rejected (it was read past its end).
- The length check counts the ECRC DW of a TLP with TD set (it flagged
  every such TLP).
- A completion's Byte Count of 0 means 4096: `byte_count` shows it, and a
  4 KB read split into completions stays paired.
- A memory write of Length 0 (1024 DW) is labelled as one (`+1023 DW`,
  `...`), not as a 1-DW write.
- No NULL string reaches a format when the string arena runs out.
