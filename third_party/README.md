# third_party/

Vendored third-party code goes here, one directory per project, each with its
`LICENSE` and a `README.md` stating the source URL, the pinned commit or
release, and the licence. Licences allowed: Apache-2.0, MIT, BSD-2/3, ISC,
Zlib, CC0, public domain (the testing standard's allow-list).

Nothing is vendored today. jsmn was evaluated for the configuration parser and
not adopted; `common/README.md` ("Why a parser of our own") gives the reasons.
The WaveCrux ABI header is not third-party code: it lives in `include/` and is
checked against upstream by `tools/check_abi_header.py`.

Third-party sources are excluded from `format-check`, `tidy` and `cppcheck`,
but are still compiled with the repository's flags; code that cannot be is
not vendored.
