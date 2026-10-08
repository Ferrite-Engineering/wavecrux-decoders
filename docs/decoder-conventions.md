# Decoder conventions

The choices every decoder in this repository makes the same way, so that a
WaveCrux user who has used one of them can use the next without reading its
manual.

## Identity

- **Decoder id**: `<namespace>.<protocol>`, lowercase, words joined with `_`.
  `ferrite.*` is ours; a contributor picks a namespace for their organisation
  (`chilichips.pcie_tlp`). The id is permanent: saved WaveCrux sessions refer
  to it, so changing it is a major version.
- **Display name**: the protocol as an engineer would say it, no vendor prefix
  ("PCIe PIPE", not "Ferrite PCIe PIPE decoder"). WaveCrux labels the decoder
  row with this name.
- **Plugin name and description** (`wavecrux_decoder_plugin_name` /
  `_description`): the plugin name is what Settings shows on the plugin card;
  the description names the source repository and the licence.

## Signals

- **Name each input after the bare RTL signal it binds to**, lowercase, in the
  form the target projects actually use (`pclk`, `tx_data`, `tx_datak`).
  WaveCrux's auto-bind matches a decoder signal as the *suffix* of a waveform
  signal under a shared prefix and scope, so `pipe_tx_data` and
  `u_phy.tx_data` both bind `tx_data`, and a user with two links picks the
  prefix (`up_`, `dn_`) from a drop-down. Do not prefix decoder signal names.
- **Declare a bit width** for every signal. A width-parameterised bus (8 or
  16 bits) declares the widest width; the decoder reads the low bits it needs
  according to a parameter.
- **Make a signal optional only if the decoder works without it**, and state
  in the manifest description what happens when it is unbound (unbound
  `rx_valid` means "always valid").
- **Clock edges**: decoders sample on the clock's rising edge by default,
  using the values from **just before** the edge (`common/` edge helper). A
  zero-delay RTL dump changes data on the same timestamp as the clock, and the
  value a flip-flop captures is the pre-edge one. Offer a `sample_point`
  parameter (`before_edge` / `at_edge`) only if a real trace needs the other.
- **X and Z** in a sampled value never decode as data. A beat with an unknown
  bit becomes an error transaction naming the signal, once per run of
  unknown beats, not once per beat.

## Parameters

- `kind` is one of `bool`, `int`, `enum`, `string`; every parameter has a
  `default` that is right for the common case, so a user who changes nothing
  gets a useful decode.
- Enum values are short lowercase identifiers (`gen1`, `auto`), with
  `enum_labels` as an object keyed by value for the text the user sees.
- Parameter names are permanent like signal names.

## Transactions

- **One transaction per protocol unit** a user reasons about: an ordered set,
  a DLLP, a TLP. Not one per symbol.
- **Times**: `start_fs` is the first sampled edge of the unit, `end_fs` the
  last. Times are femtoseconds; WaveCrux converts to the file's units (from
  1.0.1).
- **Labels** are short enough to read in a narrow box and lead with the type:
  `TS1 L=0 N_FTS=255`, `Ack seq=12`, `UpdateFC-P H=8 D=64`. Numbers in hex
  carry `0x`; sequence numbers and credit counts are decimal.
- **`fields_json`** holds everything, including what the label leaves out.
  Keys are `snake_case`; the first key is always `type`. Numeric protocol
  fields are emitted as JSON numbers; values a user compares against a
  specification table (raw symbols, CRCs) are hex strings (`"0x5C"`), so they
  read the way the spec prints them.
- **Errors** are transactions with `is_error` set, a label that says what is
  wrong (`DLLP CRC mismatch`), and `fields_json` with `error` (a sentence),
  `expected` and `actual` where they apply. A malformed unit is reported once
  and decoding resynchronises at the next valid boundary; it never stops for
  the rest of the trace.
- **Rates are bounded**: a stuck or misbound signal must not produce one error
  per clock. Collapse a run of identical errors into the first occurrence plus
  a summary transaction with `occurrences` (WaveCrux's built-in decoders do
  the same).

## Files in a decoder directory

```
decoders/<name>/
  README.md         what it decodes, signals, parameters, WaveCrux floor
  CHANGELOG.md      Keep a Changelog
  VERSION           semver, one line
  CMakeLists.txt    one wcx_add_decoder(...) call
  src/              decoder sources
  tests/            unit tests (unit_*.c)
  tools/            generate_fixtures.py
  fixtures/
    generated/      <case>.vcd, <case>.bindings.json, <case>.expected.json
    captured/       the same, plus PROVENANCE.md
  fuzz/             fuzz_<name>.c, regressions/
```
