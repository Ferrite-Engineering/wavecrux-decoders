# Contributing to wavecrux-decoders

This repository is the shared home for open-source protocol decoder plugins
for [WaveCrux](https://wavecrux.app): ours, and yours. A decoder here is built
by one CI on Linux, Windows and macOS, signed, checksummed, and listed in the
catalog, so a WaveCrux user can find it and trust what they download.

There are two ways to take part:

- **Add a decoder to this repository.** You own its directory (CODEOWNERS
  makes that formal), we build, sign and release it with everything else.
- **Keep your decoder in your own repository and list it in the catalog**,
  provided it meets the same bar (see the
  [operations standard](docs/standards/operations-standard.md#5-the-catalog)).

## The minimum bar

Every decoder in this repository, whoever wrote it, must:

1. **Build warning-free** with the repository's CMake on Linux, Windows and
   macOS. The hardening flags are set centrally; do not override them.
2. **Use the shared plumbing in `common/`** for configuration parsing, sample
   unpacking, JSON output and the transaction queue. It handles the parts of
   the plugin ABI that are easy to get subtly wrong.
3. **Never crash, hang or leak** on any input. Every decoder gets a fuzz
   target and runs under AddressSanitizer and UndefinedBehaviorSanitizer in
   CI; a decoder that fails either does not merge.
4. **Write nothing to stdout or stderr, and never call `abort` or `exit`.**
   The decoder runs inside WaveCrux.
5. **Have golden tests**: at least one fixture VCD with expected output whose
   values come from somewhere other than the decoder (the specification, an
   independent generator, the RTL's own monitor). See the
   [testing standard](docs/standards/testing-standard.md#2-where-expected-values-come-from).
6. **Be Apache-2.0**, with SPDX headers, and have its author sign the CLA
   (below).
7. **Have a README** that says what the decoder decodes, which signals it
   needs, what each parameter does, and which WaveCrux version it requires.

Decoders under the `ferrite.*` id namespace meet the full
[C coding standard](docs/standards/c-coding-standard.md) and
[testing standard](docs/standards/testing-standard.md). We would be glad if
yours did too, and review will point at the parts that matter most, but the
list above is what gates a merge.

## Getting started

```
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

[`cmake/README.md`](cmake/README.md) walks through adding a decoder:
`decoders/<name>/` with `src/`, `tests/`, `fixtures/{generated,captured}/`,
`fuzz/`, `VERSION`, `CHANGELOG.md` and `README.md`, registered with one
`wcx_add_decoder(...)` call. [`docs/decoder-conventions.md`](docs/decoder-conventions.md)
covers the choices every decoder makes the same way: ids, signal names that
WaveCrux's auto-bind matches, labels, fields and error transactions.

## Pull requests

One logical change per pull request. The description says what changed, why,
how you verified it (tests added, all presets green), and which documentation
changed with it. Use a [Conventional Commits](https://www.conventionalcommits.org/)
prefix scoped to the plugin: `feat(pcie): decode InitFC2`.

## Contributor License Agreement

This repository is part of the EDACrux Project for the purposes of the EDACrux
Contributor License Agreement ([`CLA.md`](CLA.md)), the same agreement every
EDACrux open-core repository uses. Your first contribution can be merged once
you have signed it; it is one-time, not per pull request.

The CLA confirms you have the right to submit your contribution and grants
Ferrite Engineering the licence to distribute it, including under commercial
licences. That is why it is a signature rather than a sign-off line. You keep
your copyright.

**To sign**, read [`CLA.md`](CLA.md) and write to
[support@ferriteengineering.com](mailto:support@ferriteengineering.com) with
`CLA` in the subject; we will send the signing instructions. If you
contribute as part of your employment, say so and name the employer. Open the
pull request whenever you like: the CLA gates the merge, not the review.

## Security

Please do not report a vulnerability in a public issue; see
[`SECURITY.md`](SECURITY.md).
