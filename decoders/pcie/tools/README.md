# PCIe fixture tools

The independent side of the PCIe decoders' golden tests
([testing standard §2-§4](../../../docs/standards/testing-standard.md)). Written
from [`../SPEC.md`](../SPEC.md) and
[`../docs/protocol-notes.md`](../docs/protocol-notes.md) only; nothing here reads
or shares code with `../src/`.

| File | Does |
|---|---|
| `generate_fixtures.py` | The fixture catalogue. Writes `../fixtures/generated/`; `--check` regenerates into a temporary directory and fails on any difference (and on stale files, and on captured expected files that drift from the reference decoder); `--list` prints what each fixture proves. |
| `pcie_fixture_lib.py` | The encoder: bit-serial CRC-16, LCRC-32 and scrambler; units to PIPE symbols; x1 lane packing (byte lane j = j-th symbol); the VCD writer; a model of the WaveCrux host; and the SPEC rules that turn the encoded units into expected transactions. |
| `pcie_ref_decoder.py` | A small reference decoder in the decoding direction, written separately from the SPEC with different CRC implementations (reflected CRC-16, zlib). It makes the captured fixtures' expected files and cross-checks the generator. |
| `capture_check.py` | `check()`: captured expected files equal the reference decoder's output. Run directly, it cross-checks the reference decoder against every generated fixture. |
| `make_captured.py` | Rebuilds `../fixtures/captured/` from a public openCologne-PCIE checkout ([PROVENANCE](../fixtures/captured/PROVENANCE.md)). |

```
python3 decoders/pcie/tools/generate_fixtures.py          # regenerate (about 2 s)
python3 decoders/pcie/tools/generate_fixtures.py --check  # what CI runs
python3 decoders/pcie/tools/capture_check.py              # reference-decoder cross-check
```

Python 3.10+, standard library only, deterministic (payload bytes come from a
seeded LCG; the VCDs carry no `$date`).

## How an expected file is made

1. **Self-check first.** Every run starts by reproducing every known answer
   in protocol-notes (scrambler keys after COM, the ten DLLP CRCs, the LCRC
   vectors including the nullified form, both CRC residues, bit-serial LCRC
   against zlib over 200 inputs). Any failure stops the run.
2. **Encode.** A fixture is a list of units (`Stream` methods: `ts`, `skp`,
   `fts`, `eios`, `eieos`, `idle`, `dllp`, `tlp`, and deliberate errors such
   as `truncated`, `k_in_packet`, `stray`, `gate`, `xz`). Each unit records
   its symbol range and its intent (an `Ack seq=12`, an `MWr32` with
   Length 1). `finish()` scrambles D symbols outside TS1/TS2 when the
   stream is scrambled, and checks that units cover every symbol once.
3. **Lay out and write.** Symbols are packed N per edge; gated and X/Z edges
   are inserted at word boundaries. The VCD writer emits each edge's word
   either on the previous rising edge (zero-delay RTL), `delay` ticks after
   it (testbench with delays), or on the edge itself (`at_edge`).
4. **Prove the layout.** `host_edges()` models the host (one sample per
   change time of a bound signal, unchanged signals read as zeros, the last
   `#time` is not fed) and SPEC §4.1 (rising edge between consecutive
   samples; values from the previous sample for `before_edge`). The run
   asserts that the model captures exactly the planned word at exactly the
   planned time on every edge.
5. **Expect.** Transactions come from the units' intents and the SPEC's
   rules (labels, field order, coalescing, idle runs, sequence tracking,
   auto-scrambling lock, flush order). Times follow SPEC §4.2 over all rising edges.
   Ticks are `floor(start / fs_per_tick)` and `ceil(end / fs_per_tick)`,
   written in the canonical `wcxhost` form, sorted by start, end, label,
   isError, fields.

The scrambler view is used only for assertions: for every packet the
generator computes whether its CRC holds in the `off` and the `on` pipeline,
so it knows which pipeline auto-detection must lock to and that the loser
cannot lock first.

## SPEC readings the expectations depend on

SPEC.md §10 settles the questions the first comparison raised: the exact
`error` sentences and their position (last field, one joined sentence for a
TLP with several problems), K symbols inside and outside packets
(`k_in_packet` drops the packet, `invalid_k` inside a packet, `unexpected_k`,
`dllp_edb`), ordered-set scrambler bypass (EIEOS symbol 15 included), `×N`
only when N > 1, the `Scrambling: off (training sets)` notice, times across
gated and X/Z edges, and the flush order. The generator follows §10; the
fixtures under "cases settled by SPEC §10" in `generate_fixtures.py` exercise
each of them.

Readings still taken by the generator where §10 is silent:

1. **`dllp_edb` in the PIPE decoder.** §10.2 is in the shared framing rules,
   so the PIPE decoder also reports `DLLP ended by EDB` (and no
   `DLLP frame`) for a DLLP ended by EDB.
2. **A TLP cannot have LCRC and sequence problems together**: §8.2 tracks
   sequence numbers only for good-LCRC TLPs. The multi-problem fixtures use
   LCRC + length and length + sequence (and length + replay).
3. **Unknown ordered sets.** A TS-shaped set with symbol 6 neither 0x4A nor
   0x45 spans COM through symbol 15. A set broken by a misfit D symbol spans
   COM through the misfit; fixtures use only 0x00 misfits in unscrambled
   streams, so whether the misfit is re-processed or descrambled never shows.
4. **The Disable Scrambling notice** spans the first TS ordered set that
   carried the bit (16 symbols), not the coalesced run it belongs to.
5. **Still avoided:** a reserved DLLP with a CRC mismatch (two sentences,
   join not specified for DLLPs), an incomplete ordered set at end of trace
   (§10.6 gives no `symbol` for it), invalid K and gating changes on
   scrambled streams, rxstatus changing on a gated edge, two gating signals
   on one edge.

## Adding a fixture

Add a builder function decorated with `@fixture(...)` in
`generate_fixtures.py`, with a one-line comment saying what it proves, run
the generator, and review the new `.expected.json` line by line against the
SPEC before committing (testing standard §2). Then run `capture_check.py`;
a disagreement with the reference decoder means one of the two has a bug.
