# Build system

Three modules, included once by the top-level `CMakeLists.txt`:

| Module | Does |
|---|---|
| `WcxHardening.cmake` | The flags of [C coding standard §7](../docs/standards/c-coding-standard.md#7-compiler-and-linker-settings) for every target, the force-included `banned.h`, sanitizer / coverage / fuzz instrumentation |
| `WcxDecoder.cmake` | `wcx_add_decoder()`: plugin, unit tests, golden + lifecycle tests, export check, fuzz target |
| `WcxTooling.cmake` | `format`, `format-check`, `tidy`, `cppcheck`, `cppcheck-misra`, `check-abi-header`, `coverage-report` |

## Adding a decoder

```
decoders/pcie_pipe/
  CMakeLists.txt
  src/pipe_decoder.c          # the plugin, including WCX_PLUGIN(...)
  src/pipe_lanes.c ...        # more sources
  tests/unit_scrambler.c ...  # unit tests (wcx/test.h)
  fixtures/generated/         # from tools/generate_fixtures.py
  fixtures/captured/          # real RTL traces + PROVENANCE.md
  fuzz/fuzz_pcie_pipe.c       # two lines; see common/tests/plugins/fuzz/fuzz_echo.c
  fuzz/corpus/                # seeds (wcxhost --fuzz-seed)
  fuzz/regressions/           # every crash ever found, replayed forever
```

```cmake
wcx_add_decoder(
    NAME         pcie_pipe                   # lower_snake_case
    ID           ferrite.pcie.pipe           # default decoder for the golden tests
    SOURCES      src/pipe_decoder.c src/pipe_lanes.c
    UNIT_TESTS   tests/unit_scrambler.c tests/unit_8b10b.c
    FIXTURES_DIR fixtures
    FUZZ_TARGET  fuzz/fuzz_pcie_pipe.c)
```

The top-level `CMakeLists.txt` adds every `decoders/*/CMakeLists.txt`; nothing
else needs editing. Never set compiler flags in a decoder.

What you get (all names derive from `NAME`):

| Target / test | What |
|---|---|
| `wcx_pcie_pipe` | `libwcx_pcie_pipe.so` / `libwcx_pcie_pipe.dylib` / `wcx_pcie_pipe.dll` in `<build>/plugins/` |
| `wcx_pcie_pipe_obj` | the sources as an OBJECT library: unit tests and the fuzz target link it, so they can call internal (hidden, non-static) functions |
| `pcie_pipe.exports` | the library exports exactly `wavecrux_decoder_abi_version`, `wavecrux_decoder_register`, `wavecrux_decoder_plugin_name`, `wavecrux_decoder_plugin_description` (`tools/check_exports.py`; nm or dumpbin) |
| `pcie_pipe.unit.<file>` | one executable + test per `UNIT_TESTS` entry |
| `pcie_pipe.golden.<generated\|captured>.<stem>` | `wcxhost --sort --check` on each fixture (order-insensitive, see below) |
| `pcie_pipe.lifecycle.<generated\|captured>.<stem>` | `wcxhost --lifecycle` on each fixture |
| `pcie_pipe.fuzz.replay` | the fuzz target over `fuzz/corpus/*` and `fuzz/regressions/*`, in every build, on every compiler |
| `fuzz_pcie_pipe`, `pcie_pipe.fuzz.smoke` | the libFuzzer binary and a 5000-run smoke test (`WCX_FUZZ=ON` only) |

Fixtures, corpus and regression files are globbed with `CONFIGURE_DEPENDS`:
adding one needs no CMake edit, only a rebuild.

## Fixture conventions

Every fixture is three files with the same stem, side by side in
`fixtures/generated/` or `fixtures/captured/`:

| File | Holds |
|---|---|
| `<stem>.vcd` | the trace |
| `<stem>.bindings.json` | which VCD signal each manifest signal is bound to, and parameter values |
| `<stem>.expected.json` | the exact transactions the plugin must produce, from a source other than the plugin ([testing standard §2](../docs/standards/testing-standard.md#2-where-expected-values-come-from)) |

A `.vcd` without both siblings is a configure error. Name stems after what
they exercise and their timescale, e.g. `tlp_mwr32_zero_delay_1ns`,
`dllp_ack_nak_10ps`.

### `<stem>.bindings.json`

```json
{
  "decoder": "ferrite.pcie.pipe",
  "signal_bindings": {"pclk": "tb.dut.pclk", "rx_data": "tb.dut.rx_data[31:0]"},
  "parameters": {"lanes": 4, "rate": "gen2"}
}
```

- `decoder` (optional): the decoder id; overrides `wcx_add_decoder(ID ...)`,
  so one plugin's several decoders can share a fixtures directory.
- `signal_bindings` (required): manifest signal name to VCD path. A path is
  the scopes and the variable joined with `.`; add the bit range
  (`tb.data[7:0]`) only when two variables share a name. Every required
  signal must be bound (WaveCrux will not create a decoder otherwise); an
  unbound optional signal is simply left out, exactly as in WaveCrux.
- `parameters` (optional): values as WaveCrux stores them (JSON numbers,
  booleans, strings). Every manifest parameter not given here gets its
  manifest default, because WaveCrux's configuration dialog seeds every
  parameter that way.

### `<stem>.expected.json`

A JSON array of transactions; layout and key order are free (the WaveCrux
example's pretty-printed files work as is), but the canonical form, which
`wcxhost` prints and diffs in, is one transaction per line:

```json
[
{"startTime":5,"endTime":6,"label":"D=0x5A","isError":false,"fields":{"data":"0x5A","edge":"1"}},
{"startTime":15,"endTime":16,"label":"D=0xC3","isError":false,"fields":{"data":"0xC3","edge":"2"}}
]
```

- `startTime` / `endTime`: **ticks of the VCD's timescale**, as WaveCrux
  places the transaction: `floor(start_fs / fs_per_tick)` and
  `ceil(end_fs / fs_per_tick)`. A transaction shorter than a tick still
  covers its tick.
- `fields`: every value is a **string**, because WaveCrux turns fields_json
  into `Map<String, String>` with `toString()`: emit `"edge":1` and expect
  `"edge":"1"`; `true` becomes `"true"`, `null` becomes `""`. A fields_json
  that is not a JSON object shows as `{}` in WaveCrux (and is a lifecycle
  failure here).
- `isError` is the transaction's `is_error != 0`.
- **Order.** The golden tests compare with `--sort`: both lists are put in
  canonical order (`startTime`, then `endTime`, `label`, `isError`, `fields`)
  before the first difference is looked for. An expected file therefore lists
  *which* transactions the decoder must produce, in any order; it does not
  pin the order in which a decoder emits transactions that overlap in time
  (a coalesced run that closes after a later error, say). Two lists with the
  same transactions sort identically, and a transaction that appears twice
  must appear twice. Without `--sort`, `wcxhost` prints and compares in
  emission order; the lifecycle checks, which compare the plugin against
  itself, always use emission order.

### What the host emulator reproduces (and the traps it implies)

`tools/wcxhost` mirrors `lib/services/decoders/ffi/ffi_decoder_loader_io.dart`
(its tests cite the lines). For fixture authors that means:

- One sample per distinct time at which **any bound** signal changes, in
  `[0, endTime)`, where `endTime` is the VCD's **last** `#time`. Changes at
  that last timestamp are never fed: end every fixture with a final idle
  `#time` line after the last change.
- A sample carries every bound signal's value at that time (its last change
  at or before it); a signal that has not changed yet reads as all zeros, not
  X.
- Timestamps are `ticks × fs_per_tick` femtoseconds; no `$timescale` means
  1 ns.
- Vectors shorter than their declared width extend the VCD way: `0` for a
  leading 0/1, otherwise the leading x/z. The manifest's `bit_width`, not the
  VCD width, decides how many bits are packed (wcxhost prints a note when
  they differ).
- A `#time` that goes backwards is skipped with its changes, as WaveCrux's
  waveform reader does.
- Signal binding values in `config_json` are wellen signal references
  (numbers as strings), not paths. Decoders must only test whether a signal
  is bound.

## wcxhost

```
wcxhost --plugin <lib> [--decoder <id>] --vcd <file> --bindings <file>
        [--param name=value]... [--slots N] [--out <file>] [--sort]
        [--check <expected.json>] [--lifecycle] [--fuzz-seed <file>]
wcxhost --plugin <lib> --list
```

- Without `--check` the canonical transaction list goes to stdout (or
  `--out`). This is how to *look* at a decoder's output; it is never how an
  expected file is made.
- `--check` prints the first differing transaction, expected and actual, and
  exits 1.
- `--sort` sorts the decoded list (and the expected one) into canonical
  order before printing or comparing (see "Order" above). The golden tests
  pass it; `--lifecycle` ignores it.
- `--lifecycle` runs the ABI contract checks (slots = 1 and 0 on every call
  give identical output to a 4096-slot run; two interleaved instances;
  ~100 hostile `config_json`; six wrong `bit_width`s; destroy without flush,
  flush without feed). Run it under the `asan` preset.
- `--fuzz-seed <file>` writes the fixture as an input for the shared fuzz
  driver; put the result in `fuzz/corpus/`.
- `--param name=value` overrides a parameter (typed by its manifest kind).
- Exit status: 0 ok, 1 mismatch or failed lifecycle check, 2 usage or input
  error, 3 the plugin broke the ABI contract (NULL or ill-formed UTF-8
  strings, `NEED_MORE_SLOTS` without asking for more, ...).

## Presets and commands

`cmake --preset X` configures `build/X`; build and test presets of the same
names exist. Run from the repository root.

| Preset | Configuration |
|---|---|
| `dev` | Debug, Ninja |
| `asan` | Debug, `WCX_SANITIZE=address,undefined` |
| `release` | RelWithDebInfo, release hardening, debug info split off (`.dSYM` / `.debug`; MSVC writes `.pdb`) |
| `coverage` | clang source-based coverage; then `cmake --build --preset coverage --target coverage-report`. For one decoder's floor run `tools/coverage_report.py ... --only decoders/<d>/src --fail-under-line 95 --fail-under-branch 90`; the percentages are summed over the listed files (llvm-cov's own TOTAL row ignores the filter) |
| `fuzz` | `WCX_FUZZ=ON` (libFuzzer + ASan/UBSan), RelWithDebInfo |
| `ci-linux-gcc`, `ci-linux-clang` | release with `CMAKE_C_COMPILER` gcc / clang, `WCX_REQUIRE_ALL_FLAGS=ON` |
| `ci-asan` | `asan` with `WCX_REQUIRE_ALL_FLAGS=ON` |
| `ci-macos-universal` | release, `CMAKE_OSX_ARCHITECTURES=arm64;x86_64`, Apple clang |
| `ci-windows-msvc` | release with `cl` (run from a Visual Studio developer prompt) |

`coverage` and `fuzz` need LLVM's clang. On macOS, when neither
`-DCMAKE_C_COMPILER` nor `CC` names a compiler, the top-level CMakeLists.txt
picks Homebrew's `/opt/homebrew/opt/llvm/bin/clang`; override with
`-DCMAKE_C_COMPILER=...` (it wins over the preset).

Before a pull request (testing standard §8):

```
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --build --preset dev --target format-check tidy cppcheck
```

Fuzzing locally (CI: 60 s per pull request, `-rss_limit_mb=512 -timeout=5`):

```
cmake --preset fuzz && cmake --build --preset fuzz
ASAN_OPTIONS=quarantine_size_mb=64 build/fuzz/bin/fuzz_pcie_pipe \
    -max_total_time=60 -rss_limit_mb=512 -timeout=5 \
    /tmp/new-corpus decoders/pcie_pipe/fuzz/corpus
```

Copy any `crash-*` it writes into `fuzz/regressions/` with the fix.

## Options

| Option | Default | Effect |
|---|---|---|
| `WCX_REQUIRE_ALL_FLAGS` | OFF | configure fails if the compiler lacks any §7 flag (CI turns it on) |
| `WCX_SANITIZE` | empty | sanitizer list for `-fsanitize=` (MSVC: `address` only) |
| `WCX_COVERAGE` | OFF | `-fprofile-instr-generate -fcoverage-mapping` (clang) |
| `WCX_FUZZ` | OFF | `-fsanitize=fuzzer-no-link` everywhere, fuzz executables, implies address,undefined |
| `WCX_SPLIT_DEBUG` | OFF (`release`: ON) | strip debug info from plugins into a sibling file |

Flags are probed with `-Werror` (and release flags at `-O2`), so a flag the
compiler accepts but ignores counts as unsupported. Two documented
equivalences: Clang's `-Wcast-align` stands in for GCC's `-Wcast-align=strict`
(Clang's is always strict), and `-fstack-clash-protection` is exempt on Apple
targets, where LLVM does not implement it.
