# wavecrux-decoders

Open-source protocol decoder plugins for [WaveCrux](https://wavecrux.app),
built to one standard and released for Linux, Windows and macOS.

WaveCrux has a plugin interface that lets anyone add a protocol decoder as a
native library. This repository is where those decoders can live together:
built by one CI on all three platforms, signed, checksummed, fuzzed and
sanitizer-tested, and listed in one catalog so that a WaveCrux user can find
them and trust what they download.

## Plugins

| Plugin | Decoders | Decodes | Status |
|---|---|---|---|
| [PCIe](decoders/pcie/) | **PCIe PIPE** (`ferrite.pcie_pipe_w8/16/32/64`) | PCI Express Gen1/Gen2 at the PIPE interface: training sets (TS1, TS2), SKP, FTS, electrical idle, TLP and DLLP framing, RxStatus, symbol errors | 0.1.0, preview |
| | **PCIe Data Link Layer** (`ferrite.pcie_dll_w8/16/32/64`) | DLLPs (Ack, Nak, flow control, power management) with CRC-16 checks, TLPs with sequence tracking, replays and LCRC checks | 0.1.0, preview |

Each decoder is registered once per PIPE data width (8, 16, 32 and 64 bits),
because WaveCrux binds plugin signals at a fixed width. Both were written for
[openCologne-PCIE](https://github.com/chili-chips-ba/openCologne-PCIE) and
[openPCIE](https://github.com/chili-chips-ba/openPCIE), and bind to their PIPE
signals by name; they read scrambled or unscrambled links and detect which.

## Installing a decoder

You need **WaveCrux 1.0.1 or later**. (1.0.0 draws plugin transactions at the
wrong time on most waveforms.)

1. Download the archive for your platform from the decoder's
   [release](../../releases), check it against `SHA256SUMS`, and unpack it into
   a folder you keep for WaveCrux plugins.
2. In WaveCrux open **Settings ▸ Extensions ▸ Decoder Plugins**, accept the
   safety notice, choose **Add directory…**, pick that folder, and press
   **Reload plugins**.
3. Open a waveform and add the decoder with **Ctrl+Shift+D** (⌘⇧D on macOS).
   Auto-bind fills in signals whose names match.

## Building from source

CMake 3.25+, a C17 compiler (GCC 12+, Clang 15+, MSVC 2022), Python 3.10+ for
the fixture generators.

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

## Standards

Every decoder Ferrite Engineering writes here follows three documents, and we
publish them so contributors know exactly what "done" means:

- [C coding standard](docs/standards/c-coding-standard.md): memory safety,
  hostile-input handling, hardening flags, static analysis.
- [Testing standard](docs/standards/testing-standard.md): where expected
  values come from, fixtures, fuzzing, sanitizers, coverage.
- [Operations standard](docs/standards/operations-standard.md): platforms,
  versioning, licensing, signing, releases, the catalog.

Contributed decoders meet a shorter [minimum bar](CONTRIBUTING.md#the-minimum-bar).

## Contributing

New decoders and fixes are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md).
Report vulnerabilities privately as described in [SECURITY.md](SECURITY.md).

## Licence

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE). WaveCrux is a
trademark of Ferrite Engineering LLC.
