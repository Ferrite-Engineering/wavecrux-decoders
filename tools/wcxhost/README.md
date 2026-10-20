# wcxhost

`wcxhost` is a command-line host for WaveCrux decoder plugins. It loads a
plugin library exactly as WaveCrux does, decodes a VCD through it and prints
the transactions as JSON, so a decode can be looked at, or checked against a
known-good list, without the WaveCrux GUI: in CI, for example.

## Where to get it

Every release archive carries `tools/wcxhost` (`tools/wcxhost.exe` on
Windows), built, tested and signed by the same CI run, from the same commit,
as the plugin library next to it:

| Platform | Binary | Runs on |
|---|---|---|
| macOS | universal (arm64 + x86_64), Developer ID signed, hardened runtime, notarized | macOS 12.0 or later |
| Linux | x86-64 | glibc 2.35 or later |
| Windows | x64, Authenticode signed, static CRT | Windows 10 or later |

It loads any plugin built against the WaveCrux decoder ABI, not only the one
it shipped with. On macOS it is signed with library validation turned off, so
it also loads plugins you build yourself, which WaveCrux's notarized build
does not.

To build it from source instead, build this repository
(`cmake --preset release && cmake --build --preset release`); the binary is
`build/release/bin/wcxhost`.

## Running it

From the folder you unpacked a release into (the PCIe plugin on Linux, here):

```
tools/wcxhost --plugin ./libwcx_pcie.so --decoder ferrite.pcie_pipe_w16 \
    --vcd tb.vcd --bindings tb.bindings.json --sort
```

The library is `libwcx_<plugin>.dylib` on macOS and `wcx_<plugin>.dll` on
Windows. `--list` prints the decoders a plugin registers:

```
tools/wcxhost --plugin ./libwcx_pcie.so --list
```

The bindings file says which VCD signal each decoder signal reads, and sets
parameters; anything not given gets its default, as in WaveCrux:

```json
{
  "decoder": "ferrite.pcie_pipe_w16",
  "signal_bindings": {"pclk": "tb.pclk", "data": "tb.data", "datak": "tb.datak"},
  "parameters": {"scrambling": "off"}
}
```

A signal path is its scopes and name joined with `.`. `--param name=value`
overrides one parameter on the command line.

## Checking a decode in CI

Save a list you have checked by hand, then compare every later run with it:

```
tools/wcxhost --plugin ./libwcx_pcie.so --vcd tb.vcd --bindings tb.bindings.json \
    --sort --out expected.json
tools/wcxhost --plugin ./libwcx_pcie.so --vcd tb.vcd --bindings tb.bindings.json \
    --sort --check expected.json
```

`--check` prints the first transaction that differs, expected and actual.
`--sort` compares in time order rather than the order the decoder emitted
transactions in, so the check does not depend on how a decoder orders
transactions that overlap.

Exit status: 0 success; 1 the output differs from `--check`; 2 bad usage or
input; 3 the plugin broke the ABI contract.

The complete option list (`--lifecycle`, `--slots`, `--fuzz-seed`) and the
exact file formats are in the repository's
[build system notes](https://github.com/Ferrite-Engineering/wavecrux-decoders/blob/main/cmake/README.md#wcxhost).
