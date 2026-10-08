# Operations standard

How decoders in this repository are built, versioned, licensed, released and
supported. The coding and testing standards say how to write a decoder; this
one says how to ship it so that a user can trust what they download.

## 1. Platforms

**Every `ferrite.*` decoder ships for all three desktop platforms, or it does
not ship.** WaveCrux runs on all three, and a decoder that works on one is a
support problem on the other two.

| Platform | Artifact | Built on | Floor |
|---|---|---|---|
| macOS | universal `.dylib` (arm64 + x86_64) | `macos-latest`, Apple clang | macOS 12.0, matching WaveCrux (`-mmacosx-version-min=12.0`) |
| Linux | x86-64 `.so` | `ubuntu-22.04`, GCC 12 | glibc 2.35, below WaveCrux's own 2.38 floor, so every system that runs WaveCrux runs the plugin. Measured on every release build. |
| Windows | x64 `.dll` | `windows-latest`, MSVC | Windows 10, statically linked CRT (`/MT`), so no runtime installer is needed |

Linux arm64 and Windows arm64 follow WaveCrux: when WaveCrux ships for a
platform, this repository adds it to the matrix in the same release.

Mobile and web are out of scope: WaveCrux does not load native plugins there,
so a decoder needed in those places belongs in WaveCrux itself, as a built-in
decoder.

**Never `ubuntu-latest`.** That label moved to 24.04 once already and silently
raised the glibc floor of every WaveCrux binary for a year. The Linux job is
pinned, and the release job measures the floor (`tools/check_glibc.py`) and
fails above 2.35.

## 2. Versioning

Each plugin is versioned on its own, with [Semantic Versioning
2.0](https://semver.org/). The version lives in `decoders/<plugin>/VERSION` and is
compiled into the library.

What counts as **breaking** (major) for a decoder is decided by what a user's
saved WaveCrux session depends on:

- the decoder `id`;
- the name of any signal or parameter, or the meaning of a parameter value;
- removing a signal, parameter or enum value;
- changing a field name in `fields_json`, or the meaning of an existing field.

**Minor:** a new optional signal, a new parameter with a default that keeps
the old behaviour, a new field, a new error check. **Patch:** a fix that makes
output match the specification.

Before 1.0.0, minor versions may break, as SemVer allows, and the changelog
says so in bold.

**The plugin ABI** is versioned by WaveCrux, not by us. Each decoder declares
the ABI major it is built against (1) and the oldest WaveCrux it supports in
`decoders/<plugin>/README.md` and the catalog. The current floor is **WaveCrux
1.0.1**: 1.0.0 drew plugin transactions at the wrong time on any file not
timestamped in femtoseconds.

**Changelog:** `decoders/<plugin>/CHANGELOG.md` in [Keep a
Changelog](https://keepachangelog.com/en/1.1.0/) form. Every user-visible
change has a line, written for the person using the decoder, not the person
who wrote it.

## 3. Licensing

- **Every decoder is Apache-2.0**, so that a decoder that proves widely
  useful can later be promoted into WaveCrux's open core, which is also
  Apache-2.0. The only other licences in
  the repository are those of captured test waveforms, which keep their
  upstream permissive licence and are never compiled into a plugin. GPL
  decoders belong in `wavecrux-sigrok-bridge`.
<!-- REUSE-IgnoreStart -->
- **Every file carries an SPDX header** (`SPDX-License-Identifier:
  Apache-2.0`) and a copyright line naming its author, or an entry in
  `REUSE.toml` when the format cannot carry one. The repository follows the
  [REUSE specification](https://reuse.software/); CI runs `reuse lint`.
<!-- REUSE-IgnoreEnd -->
- **Vendored third-party code** lives under `third_party/<name>/` with its
  licence file and a `README.md` giving the source URL and the exact commit.
  Allowed licences: Apache-2.0, MIT, BSD-2-Clause, BSD-3-Clause, ISC, Zlib,
  CC0-1.0, Unlicense. Every one is listed in `NOTICE`, and `NOTICE` ships
  inside every release archive.
- **Contributions require the EDACrux CLA** ([`CLA.md`](../../CLA.md)), the
  same text as every EDACrux open-core repository. This repository is
  designated part of the Project for that agreement.
- **Fixtures** follow the captured-fixture licence allow-list in the testing
  standard, and every captured fixture has a `PROVENANCE.md` entry.

## 4. Releases

A release is one **plugin** (one library, which may register several decoders, as `decoders/pcie/` does) at one version on all three platforms. Plugin metadata lives in `decoders/<plugin>/plugin.json`.

1. **Bump** `decoders/<plugin>/VERSION`, move the `Unreleased` changelog section
   under the new version, open a pull request. CI must be green on all three
   platforms.
2. **Tag** the merge commit `<plugin>-v<semver>` (for example
   `pcie-v0.1.0`). Tags are the only trigger for a release, and the tag
   must match `VERSION`.
3. **CI builds and publishes**: the release workflow builds the decoder on all
   three platforms from the tag, runs the full test suite (including
   sanitizers) on each, signs the macOS and Windows libraries, packages each
   platform as `wcx-<plugin>-<version>-<platform>.zip` (library, `LICENSE`,
   `NOTICE`, plugin `README.md`), writes `SHA256SUMS` (LF line endings), and
   creates the GitHub Release with the changelog section as its notes. If any
   platform could not be signed, the release is created as a **draft** and a
   maintainer finishes it by hand (`tools/release.py` documents both steps).
4. **The catalog entry** (`catalog.json`, schema below) is written by
   `tools/release.py finalize` and attached to the release.

**Nothing is built on a developer's machine for release.** A library a user
downloads was compiled, tested and signed by this repository's CI from the
tagged commit.

**Signing:**

| Platform | How | Why |
|---|---|---|
| macOS | Developer ID Application (team `7957R7M965`), hardened runtime, timestamped; notarized | WaveCrux's notarized macOS build enforces library validation, so today it loads only plugins signed by Ferrite Engineering's team. A downloaded library is also quarantined, and Gatekeeper refuses a quarantined library that is not notarized. |
| Windows | Authenticode through Azure Trusted Signing, the same account that signs WaveCrux | SmartScreen and enterprise application control |
| Linux | Unsigned; integrity through `SHA256SUMS` and build provenance | No platform mechanism a loader checks |

**Provenance:** every release archive carries a GitHub build-provenance
attestation (`actions/attest-build-provenance`), so anyone can verify with
`gh attestation verify` that a library came from this repository's CI at a
given commit (SLSA Build Level 2).

**Reproducibility:** release builds pass `-ffile-prefix-map` and
`SOURCE_DATE_EPOCH`, so the same commit and toolchain produce the same bytes
on Linux. Byte-for-byte reproducibility on macOS and Windows is a goal, not a
gate.

## 5. The catalog

The catalog is the central reference for every plugin this repository, or a
repository it lists, publishes. One entry per plugin:

```json
{
  "schema": 1,
  "plugins": [
    {
      "plugin": "pcie",
      "name": "PCIe decoders (PIPE, Data Link Layer)",
      "version": "0.1.0",
      "decoders": [{"id": "ferrite.pcie_pipe_w16", "name": "PCIe PIPE (16-bit)"}],
      "abi_major": 1,
      "min_wavecrux": "1.0.1",
      "license": "Apache-2.0",
      "maintainer": "Ferrite Engineering",
      "source": "https://github.com/Ferrite-Engineering/wavecrux-decoders/tree/pcie-v0.1.0/decoders/pcie",
      "downloads": {
        "macos_universal": {"url": "…", "sha256": "…"},
        "linux_x64": {"url": "…", "sha256": "…"},
        "windows_x64": {"url": "…", "sha256": "…"}
      }
    }
  ]
}
```

A decoder hosted elsewhere may be listed when it meets the same rules:
Apache-2.0-compatible licence, CI-built signed binaries for all three
platforms, a golden test suite, and a named maintainer. The catalog CI checks
that every listed URL resolves and every checksum matches, weekly.

## 6. Repository operations

- **Changes land through pull requests** once the repository is public. Each
  description says what changed, why, how it was verified, and which
  documentation changed with it.
- **CODEOWNERS** gives each decoder directory an owner; an outside maintainer
  owns their own decoder. Changes to `common/`, `cmake/`, `include/`, the
  standards or the workflows need a Ferrite reviewer.
- **CI cost control**, from WaveCrux experience: every job has
  `timeout-minutes`; Linux runs on every push and pull request; Windows runs
  on every pull request (decoders are small and the Windows-only bug classes
  are real); macOS runs on pull requests that touch C sources, on tags and
  weekly.
- **Security reports** go to support@ferriteengineering.com with `Security`
  in the subject, per
  [`SECURITY.md`](../../SECURITY.md). A memory-safety bug is a security bug.
  We fix the latest release of each decoder; there are no long-term branches.
- **Deprecation**: a decoder is removed from the catalog only after one minor
  release that marks it deprecated in the catalog and its changelog.
