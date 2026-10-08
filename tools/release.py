#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Package a plugin release and write its checksums, notes and catalog entry.

Operations standard §4 and §5. Two steps, both run by the release workflow:

  release.py package  --plugin pcie --platform linux_x64 --build-dir build/ci-linux-gcc --out dist
  release.py finalize --plugin pcie --tag pcie-v0.1.0 --dist dist --repo Ferrite-Engineering/wavecrux-decoders

`package` zips one platform's library with LICENSE, NOTICE and the plugin's
README. `finalize` checks the tag against decoders/<plugin>/VERSION, writes
SHA256SUMS, extracts the version's CHANGELOG section as release notes, and
writes catalog.json from decoders/<plugin>/plugin.json and the archives.
Standard library only, so it runs unchanged on every CI runner.
"""

import argparse
import hashlib
import json
import pathlib
import re
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

PLATFORMS = {
    "linux_x64": ("lib{name}.so", "linux-x64"),
    "macos_universal": ("lib{name}.dylib", "macos-universal"),
    "windows_x64": ("{name}.dll", "windows-x64"),
}


def plugin_dir(plugin: str) -> pathlib.Path:
    path = ROOT / "decoders" / plugin
    if not (path / "plugin.json").is_file():
        sys.exit(f"error: {path}/plugin.json not found")
    return path


def read_version(plugin: str) -> str:
    version = (plugin_dir(plugin) / "VERSION").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?", version):
        sys.exit(f"error: VERSION {version!r} is not semver")
    return version


def archive_name(plugin: str, version: str, platform: str) -> str:
    return f"wcx-{plugin}-{version}-{PLATFORMS[platform][1]}.zip"


def find_library(build_dir: pathlib.Path, filename: str) -> pathlib.Path:
    # A macOS release build keeps debug symbols in <lib>.dSYM, whose DWARF
    # file has the library's own name; it is not the library to ship.
    matches = sorted(
        p for p in build_dir.rglob(filename)
        if p.is_file() and not any(part.endswith(".dSYM") for part in p.parts)
    )
    if len(matches) != 1:
        sys.exit(f"error: expected exactly one {filename} under {build_dir}, found {len(matches)}")
    return matches[0]


def cmd_package(args: argparse.Namespace) -> int:
    meta = json.loads((plugin_dir(args.plugin) / "plugin.json").read_text(encoding="utf-8"))
    version = read_version(args.plugin)
    library = find_library(args.build_dir, PLATFORMS[args.platform][0].format(name=meta["library"]))
    args.out.mkdir(parents=True, exist_ok=True)
    target = args.out / archive_name(args.plugin, version, args.platform)
    # Fixed timestamps and order, so the archive is reproducible given the
    # same library bytes.
    members = [
        (library, library.name),
        (ROOT / "LICENSE", "LICENSE"),
        (ROOT / "NOTICE", "NOTICE"),
        (plugin_dir(args.plugin) / "README.md", "README.md"),
    ]
    with zipfile.ZipFile(target, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for source, arcname in members:
            info = zipfile.ZipInfo(arcname, date_time=(2026, 1, 1, 0, 0, 0))
            info.external_attr = 0o644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, source.read_bytes())
    print(target)
    return 0


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def changelog_section(plugin: str, version: str) -> str:
    text = (plugin_dir(plugin) / "CHANGELOG.md").read_text(encoding="utf-8")
    match = re.search(
        rf"^## \[{re.escape(version)}\][^\n]*\n(.*?)(?=^## \[|\Z)", text, re.S | re.M
    )
    if not match or not match.group(1).strip():
        sys.exit(f"error: CHANGELOG.md has no section for [{version}]")
    return match.group(1).strip()


def cmd_finalize(args: argparse.Namespace) -> int:
    meta = json.loads((plugin_dir(args.plugin) / "plugin.json").read_text(encoding="utf-8"))
    version = read_version(args.plugin)
    expected_tag = f"{args.plugin}-v{version}"
    if args.tag != expected_tag:
        sys.exit(f"error: tag {args.tag} does not match VERSION (expected {expected_tag})")

    downloads = {}
    lines = []
    for platform in PLATFORMS:
        archive = args.dist / archive_name(args.plugin, version, platform)
        if not archive.is_file():
            sys.exit(f"error: {archive.name} is missing; every platform ships or none does")
        digest = sha256(archive)
        lines.append(f"{digest}  {archive.name}\n")
        downloads[platform] = {
            "url": f"https://github.com/{args.repo}/releases/download/{args.tag}/{archive.name}",
            "sha256": digest,
        }
    # LF line endings on every platform: a CRLF here once broke `shasum -c`
    # for every WaveCrux release from 0.4.0 to 0.8.0.
    (args.dist / "SHA256SUMS").write_bytes("".join(lines).encode("ascii"))

    notes = changelog_section(args.plugin, version)
    notes += (
        f"\n\nRequires WaveCrux {meta['min_wavecrux']} or later. Check downloads "
        "against `SHA256SUMS`; installation steps are in the README inside each archive.\n"
    )
    (args.dist / "RELEASE_NOTES.md").write_text(notes, encoding="utf-8")

    entry = {
        "plugin": args.plugin,
        "name": meta["name"],
        "version": version,
        "decoders": meta["decoders"],
        "abi_major": meta["abi_major"],
        "min_wavecrux": meta["min_wavecrux"],
        "license": meta["license"],
        "maintainer": meta["maintainer"],
        "source": f"https://github.com/{args.repo}/tree/{args.tag}/decoders/{args.plugin}",
        "downloads": downloads,
    }
    catalog = {"schema": 1, "plugins": [entry]}
    (args.dist / "catalog.json").write_text(json.dumps(catalog, indent=2) + "\n", encoding="utf-8")
    print(f"finalized {args.tag}: {len(downloads)} platforms")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    package = sub.add_parser("package")
    package.add_argument("--plugin", required=True)
    package.add_argument("--platform", required=True, choices=sorted(PLATFORMS))
    package.add_argument("--build-dir", required=True, type=pathlib.Path)
    package.add_argument("--out", required=True, type=pathlib.Path)
    package.set_defaults(func=cmd_package)

    finalize = sub.add_parser("finalize")
    finalize.add_argument("--plugin", required=True)
    finalize.add_argument("--tag", required=True)
    finalize.add_argument("--dist", required=True, type=pathlib.Path)
    finalize.add_argument("--repo", required=True)
    finalize.set_defaults(func=cmd_finalize)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
