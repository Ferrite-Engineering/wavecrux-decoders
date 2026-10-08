#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
"""Check that include/wavecrux_decoder.h is byte-identical to upstream.

The ABI header is vendored unchanged from the WaveCrux open core (C coding
standard §2) and CI fails if it drifts.

  check_abi_header.py            compare with upstream main over the network
  check_abi_header.py --offline  compare with the SHA-256 recorded below (the
                                 hash of the upstream file when it was last
                                 vendored), which catches local edits without
                                 network access; ctest runs this form
  check_abi_header.py --update   overwrite the local copy with upstream and
                                 print the new hash to record below

When upstream changes: run --update, paste the printed hash into
VENDORED_SHA256, rebuild, and review the diff like any ABI change.
"""

import argparse
import difflib
import hashlib
import pathlib
import sys
import urllib.request

UPSTREAM_URL = (
    "https://raw.githubusercontent.com/Ferrite-Engineering/wavecrux/main/include/wavecrux_decoder.h"
)
VENDORED_SHA256 = "0a8f7289f43062b2fa4c87847801d1959c3ca8e39f6238757a36bc5adb1266a5"
LOCAL = pathlib.Path(__file__).resolve().parent.parent / "include" / "wavecrux_decoder.h"


def fetch() -> bytes:
    with urllib.request.urlopen(UPSTREAM_URL, timeout=30) as response:  # noqa: S310 (fixed https URL)
        return response.read()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--offline", action="store_true")
    mode.add_argument("--update", action="store_true")
    args = parser.parse_args()

    local = LOCAL.read_bytes()
    if args.offline:
        digest = hashlib.sha256(local).hexdigest()
        if digest != VENDORED_SHA256:
            print(f"check_abi_header: {LOCAL} was edited (sha256 {digest}, vendored "
                  f"{VENDORED_SHA256}); restore it byte for byte from upstream")
            return 1
        print("check_abi_header: vendored header unchanged")
        return 0

    upstream = fetch()
    if args.update:
        LOCAL.write_bytes(upstream)
        print(f"check_abi_header: updated; record VENDORED_SHA256 = "
              f"\"{hashlib.sha256(upstream).hexdigest()}\"")
        return 0
    if upstream != local:
        print(f"check_abi_header: {LOCAL} differs from {UPSTREAM_URL}")
        diff = difflib.unified_diff(
            upstream.decode("utf-8", "replace").splitlines(),
            local.decode("utf-8", "replace").splitlines(),
            "upstream",
            "vendored",
            lineterm="",
        )
        for line in diff:
            print(line)
        return 1
    print("check_abi_header: vendored header matches upstream")
    return 0


if __name__ == "__main__":
    sys.exit(main())
