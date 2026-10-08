# wavecrux-decoders

Open-source protocol decoder plugins for WaveCrux, in C17, built and released
for Linux, Windows and macOS. A plugin is native code running inside WaveCrux
on untrusted waveforms: the bar is higher than ordinary application C.

## Read before changing anything

- [`docs/standards/c-coding-standard.md`](docs/standards/c-coding-standard.md):
  ABI-boundary rules, memory rules, banned functions, flags, static analysis.
- [`docs/standards/testing-standard.md`](docs/standards/testing-standard.md):
  **expected values never come from the decoder's own output**; generated and
  captured fixtures; fuzzing; coverage floors.
- [`docs/standards/operations-standard.md`](docs/standards/operations-standard.md):
  platforms, versioning, licensing, signing, releases, catalog.
- [`docs/decoder-conventions.md`](docs/decoder-conventions.md): ids, signal
  names for auto-bind, labels, fields, errors.
- [`cmake/README.md`](cmake/README.md) and [`common/README.md`](common/README.md):
  how a plugin is built on the shared plumbing.

## Commands

```
cmake --preset dev        # configure (also: asan, release, coverage, fuzz, ci-*)
cmake --build --preset dev
ctest --preset dev        # unit, golden, lifecycle, exports, fuzz replay
cmake --build --preset dev --target format-check tidy cppcheck
python3 decoders/<plugin>/tools/generate_fixtures.py [--check]
```

On macOS, `fuzz` and `coverage` presets use Homebrew LLVM
(`/opt/homebrew/opt/llvm/bin/clang`); Apple clang has no libFuzzer.

## Rules that are easy to miss

- **Never read the decoder source when writing fixture expectations, and
  never edit an expected file to match the decoder.** A disagreement is a
  finding: resolve it against the plugin's `SPEC.md` and the protocol
  specification.
- **Use `common/` for config parsing, sample unpacking, edges, JSON and
  output.** The NEED_MORE_SLOTS retry, string lifetimes and pre-edge sampling
  are already right there.
- **Nothing to stdout/stderr, no abort/exit/assert in release paths, no
  global mutable state, no allocation per sample.**
- **Timescales in fixtures are never only 1 fs**; a factor of 1 hides time
  conversion bugs (it hid one in WaveCrux 1.0.0).
- **The ABI header is vendored byte for byte**; never edit
  `include/wavecrux_decoder.h`.
- Every file has an SPDX header or a `REUSE.toml` entry.

## Git

Conventional Commits scoped to the plugin (`feat(pcie): …`). Commit with an
explicit pathspec. Run `gitleaks git .` before pushing. Releases are tags
`<plugin>-v<semver>`, never local builds (see the operations standard for the
one temporary exception).
