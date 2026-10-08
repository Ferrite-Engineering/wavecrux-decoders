# Testing standard

A decoder that draws the wrong transaction is worse than one that draws
nothing: the engineer trusts it and debugs the wrong thing. WaveCrux's own
test suite found, in October 2026, that its 1-Wire plugin golden had been
written from the buggy output it was meant to catch, so the plugin interface
shipped with transaction times wrong by the timescale factor for every
third-party plugin. This standard exists so that never happens here.

Every `ferrite.*` decoder meets all of it. Contributed decoders meet the
[minimum bar](../../CONTRIBUTING.md#the-minimum-bar).

## 1. The layers

| Layer | What it proves | Where |
|---|---|---|
| **Unit** | Each protocol primitive (CRC, 8b/10b, scrambler, field extraction, framing) against known answers taken from the specification or an independent implementation, never from our own output. | `decoders/<plugin>/tests/unit_*.c` |
| **Golden, generated** | The whole plugin, loaded through the host emulator, decodes deterministic fixtures to the expected transactions. | `decoders/<plugin>/fixtures/generated/` |
| **Golden, captured** | The same, on traces from real RTL (the target project's own simulation), with hand-verified anchor transactions. | `decoders/<plugin>/fixtures/captured/` |
| **Lifecycle** | The ABI contract: create/feed/flush/destroy ordering, `NEED_MORE_SLOTS` retries, string lifetime across calls, hostile `config_json`, wrong `bit_width`, X/Z values. | `common/tests/`, shared harness |
| **Fuzz** | No input, however malformed, crashes, leaks, hangs or reads out of bounds. | `decoders/<plugin>/fuzz/` |
| **Sanitized** | All of the above, again, under AddressSanitizer + UndefinedBehaviorSanitizer + LeakSanitizer. | CI |
| **Cross-platform** | Byte-identical golden output on Linux, Windows and macOS. | CI |

## 2. Where expected values come from

This is the rule that matters most:

> **An expected value must come from somewhere other than the code under test.**

Acceptable sources, best first:

1. A worked example or table in the protocol specification.
2. An independent implementation: the fixture generator (written separately,
   in Python, from the specification), a different open-source decoder, or
   the target RTL's own monitor or log output.
3. A value computed by hand and written in the test with the derivation in a
   comment.

Never acceptable: running the decoder, copying what it printed into the
expected file, and calling that a test. A regenerated golden is reviewed
line by line against the source above, and the commit message says what
changed and why.

The generator and the decoder must not share code. The generator encodes
(packet to symbols); the decoder decodes (symbols to packet). Agreement
between two independent directions is the evidence.

## 3. Generated fixtures

- Produced by `decoders/<plugin>/tools/generate_fixtures.py`: standard library only,
  deterministic (no clock, no unseeded randomness), re-runnable in seconds,
  and CI fails if re-running it changes a committed file.
- Each `.vcd` has a sibling `.expected.json`: the exact transaction list the
  host emulator must print.
- **Timescale is never 1 fs** in at least one fixture per decoder, and at
  least one fixture uses each of 1 ps, 1 ns and a non-unit factor (`10ps`).
  A 1 fs timescale hides time-conversion bugs, because the factor is 1.
- **Edge sampling is exercised both ways**: data changing on the same
  timestamp as the clock edge (zero-delay RTL), and data changing between
  edges (testbench with delays).
- Every error path the decoder can report has a fixture that triggers it.

## 4. Captured fixtures

- From the project the decoder serves, built from its own testbench with an
  open simulator (Icarus Verilog, Verilator). The exact commands go in
  `PROVENANCE.md` with the upstream commit SHA and licence.
- Licence allow-list: Apache-2.0, MIT, BSD-2/3, ISC, Zlib, CC0, public domain.
  Nothing else is committed.
- Trimmed to the interesting window; each fixture is at most 5 MB.
- `PROVENANCE.md` lists at least three **anchor transactions** checked by hand
  against the specification, with the timestamp of each.

## 5. The host emulator

`tools/wcxhost` loads a plugin exactly as WaveCrux does: it reads a VCD,
builds the union of change timestamps across the bound signals, packs values
two bits per signal bit in manifest order (unbound optional signals omitted),
converts ticks to femtoseconds on the way in and back on the way out, honours
`NEED_MORE_SLOTS`, and prints the transactions as JSON. Its own tests pin its
behaviour to WaveCrux's loader
(`lib/services/decoders/ffi/ffi_decoder_loader_io.dart` in the open core);
when the loader changes, the emulator changes in the same week.

## 6. Coverage

Measured with `llvm-cov` over unit + golden + lifecycle tests (not fuzzing):

- **Line coverage ≥ 95 %, branch coverage ≥ 90 %** for decoder sources.
- Every uncovered line is either unreachable defensive code (marked
  `/* defensive: <why> */`) or a gap with a tracking issue.
- Coverage is a floor that finds untested code, not a goal. A test that
  executes a line without asserting what it did does not count in review.

## 7. Fuzzing

- A libFuzzer target per decoder drives `create` with fuzzed `config_json`
  and `feed` with a fuzzed sample stream (fuzzed widths, timestamps that go
  backwards, X/Z everywhere), then `flush` and `destroy`.
- Seed corpus: the generated fixtures, converted to sample streams.
- CI runs each target for 60 seconds per pull request under ASan + UBSan with
  `-rss_limit_mb=512 -timeout=5`; the weekly job runs 30 minutes.
- Any crash becomes a regression input under `fuzz/regressions/`, replayed by
  the ordinary test run forever after.

## 8. Before a pull request

```
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --build --preset dev --target format-check tidy cppcheck
```

All green, no skips. A skipped test is a failing test that has not admitted
it yet.
