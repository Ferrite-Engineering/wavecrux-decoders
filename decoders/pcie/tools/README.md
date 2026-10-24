# PCIe fixture tools

The independent side of the PCIe decoders' golden tests
([testing standard §2-§4](../../../docs/standards/testing-standard.md)). Written
from [`../SPEC.md`](../SPEC.md) and
[`../docs/protocol-notes.md`](../docs/protocol-notes.md) only; nothing here reads
or shares code with `../src/`.

| File | Does |
|---|---|
| `generate_fixtures.py` | The fixture catalogue. Writes `../fixtures/generated/`; `--check` regenerates into a temporary directory and fails on any difference (and on stale files, and on captured expected files that drift from the reference decoder); `--list` prints what each fixture proves. |
| `pcie_fixture_lib.py` | The encoder: bit-serial CRC-16, LCRC-32 and scrambler; units to PIPE symbols; x1 lane packing (byte lane j = j-th symbol); the VCD writer; a model of the WaveCrux host; and the SPEC rules that turn the encoded units into expected transactions. Section 6 adds the serial line (SPEC §11): an 8b/10b encoder with running disparity, `SerialStream` (the line plan and its faults), the transmitter that lays bits out in time, a 1-bit VCD writer, and the serial expectations. |
| `pcie_ref_decoder.py` | A small reference decoder in the decoding direction, written separately from the SPEC with different CRC implementations (reflected CRC-16, zlib). It makes the captured fixtures' expected files and cross-checks the generator. |
| `capture_check.py` | `check()`: captured expected files equal the reference decoder's output. Run directly, it cross-checks the reference decoder against every generated fixture. |
| `export_stimulus.py` | Writes `../stimulus/`: `$readmemh` receive stimulus for RTL testbenches, built by the same encoder, with a manifest per file. Each file is decoded back by the reference decoder before it is written; `--check` fails on drift; `--lanes 1/2/4` writes other bus widths. See [`../stimulus/README.md`](../stimulus/README.md). |
| `make_captured.py` | Rebuilds `../fixtures/captured/` from a public openCologne-PCIE checkout ([PROVENANCE](../fixtures/captured/PROVENANCE.md)). |

```
python3 decoders/pcie/tools/generate_fixtures.py          # regenerate (about 2 s)
python3 decoders/pcie/tools/generate_fixtures.py --check  # what CI runs
python3 decoders/pcie/tools/capture_check.py              # reference-decoder cross-check
python3 decoders/pcie/tools/export_stimulus.py --check    # testbench stimulus is current
```

Python 3.10+, standard library only, deterministic (payload bytes come from a
seeded LCG; the VCDs carry no `$date`).

## How an expected file is made

1. **Self-check first.** Every run starts by reproducing every known answer
   in protocol-notes (scrambler keys after COM, the ten DLLP CRCs, the LCRC
   vectors including the nullified form, both CRC residues, bit-serial LCRC
   against zlib over 200 inputs, the seventeen 8b/10b codes of §9 at both
   disparities), and checks that no 10-bit code maps to two symbols across
   both RD tables, that every code has disparity 0 or ±2, and that the comma
   occurs only inside K28.1, K28.5 and K28.7. Any failure stops the run.
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

## Serial line fixtures (SPEC §11)

The `serial_*` fixtures bind one 1-bit signal, `{"line": "tb.dut.txp"}`, to
`ferrite.pcie_serial` or `ferrite.pcie_serial_dll`. They reuse the stream
builder and the PIPE/DLL expectation engines; only the layer underneath
changes:

1. **Encode the stream** exactly as for PIPE (1 lane; scrambled or not), with
   a line plan on top (`SerialStream`): `burst(rate, rd, prelock, jitter)`
   starts a burst whose first symbol is the lock COM, `elec_idle(...)` sends
   X, Z or a level held longer than 8 ns (optionally after a partial
   symbol), and the faults are marks on symbols: `code_error` (an EDB the
   transmitter replaces with an invalid code group, so the pipeline intent
   is EDB), `wrong_rd` (encode with the opposite RD, then continue from the
   receiver's RD or keep the transmitter's), `slip`/`garbage` (1-9 bits in
   place of a symbol right before a COM: inserted bits, or the head of a
   symbol whose tail was deleted), plus `dllp_cut`/`tlp_cut` for a code
   error inside a packet body and `os_cut` for an ordered set cut by idle.
2. **Transmit.** `transmit()` 8b/10b-encodes each symbol with running
   disparity (wire order a b c d e i f g h j), lays the bits out at the
   burst's UI (400 000 fs Gen1, 200 000 fs Gen2) and, with `jitter`, moves
   every edge by up to UI/2 - 1 tick while keeping neighbours less than UI/2
   apart so each run still rounds to its bit count. The last symbol is
   always followed by a transition or X, because the run still open at the
   end of the trace is never decoded. `write_serial_vcd()` writes the line
   (inverted when the fixture sets `invert`), optionally with an unbound
   complement and redundant same-value writes.
3. **Prove the layout.** `host_line_samples()` reads the VCD back as the host
   feeds it (one sample per written time, the final `#time` not fed) and the
   run asserts it equals the planned changes. `recover()` applies SPEC
   §11.2-§11.5 to those samples (runs, electrical idle, the per-burst
   `rate=auto` decision, `n = max(1, floor((D + UI/2) / UI))`, bit times by
   `floor(i·D/n)`) and the run asserts it yields exactly the transmitted bits
   at the intended rate (and, where a burst says `decide=`, by the intended
   §11.4 rule). `align()` asserts that the transmitted bits hold no COM
   pattern except where a COM was sent, then applies §11.6 from the COM
   positions the transmitter chose: lock, symbols on the boundary, and
   realign by k.
4. **Expect.** Line-level transactions come from that intent: lock, realign
   (spanning the COM), code errors (the receiver's misaligned group after a
   slip spans its own slot, which overlaps the COM), disparity errors (the RD
   follows §11.7, so a transmitter that keeps its own RD produces a second
   error at the next RD-dependent symbol) and electrical-idle periods. The
   pipeline transactions are the PIPE (`pcie_serial`) or DLL
   (`pcie_serial_dll`) engines' over the same units, timed by the recovered
   bit times; `auto` scrambling selection is the same code.

### Both directions of one link

`serial_link_x1_gen2_{dsp,usp}_10ps` and `serial_dll_link_x1_gen2_{dsp,usp}_10ps`
are one x1 link: a Downstream Port line (`tb.dsp.txp`) and an Upstream Port
line (`tb.usp.txp`) in the same VCD, written by `write_serial_link_vcd()`. Each
of the four fixtures binds one line to one serial decoder; the other line is in
the file and never bound, as when a user decodes each direction of a dump with
its own decoder instances. The four `.vcd` files are the same trace.

The two streams are built side by side and `in_step()` asserts, at each sync
point, that both have sent the same number of symbols at the same rates after
the same idle times, so symbol k starts at the same time on both lines. The
traffic follows the link-training and Ack/Nak sequences of the PCI Express
Base Specification (Polling, Configuration with the Downstream Port proposing
the Link number, InitFC1/InitFC2, Recovery with a speed change to 5.0 GT/s,
Acks covering several TLPs, a Nak and the replay it requests, L0s); the
decoders' expectations come from the same engines as every other serial
fixture. The comments in the builder give each packet's symbol offsets, so
the time from a TLP on one line to an Ack on the other can be read off in
symbol times.

Readings the serial expectations take where §11 is silent:

1. **`disparity_error`**: the name (`D12.3`) and `symbol` are the 8b/10b
   symbol as received, before descrambling; `code` (in both error types) is
   the ten bits after `invert`, in wire order.
2. **A misaligned group during a slip** is the boundary symbol "decoded
   normally" before the realign; the fixtures choose slips whose group is
   invalid, so it is a code error fed as EDB. A realign does not break the
   pipeline: the dropped k bits are simply never fed.
3. **A lock or realign COM** is never a disparity error, whatever its form
   (one fixture realigns on a COM sent with the opposite RD); a COM on the
   boundary is checked like any symbol (`Disparity error K28.5`).
4. **Electrical idle**: a run of exactly 8 ns is bits; a run of exactly
   300 ps does not decide Gen2. "Some run converted before it" means any bit
   run earlier in the trace, pre-lock bits included. An idle still open at
   the end spans to the last sample fed, so the fixtures end such a trace
   with X then Z. The break is gating's: a stray run ends, a SKP OS with at
   least one SKP is complete, any other ordered set is `unknown_os` with its
   last symbol.
5. **Code error inside a TLP**: cut at a valid framing length, so the DLL
   decoder reports `tlp_edb` (`bytes` = body - 6); the order of the length
   check and the EDB rule for TLPs is not specified, so it is not exercised.
   Inside a DLLP, `dllp_edb` wins over length (§10.8 item 7).
6. **Still avoided**: a coalesced run or an idle run on both sides of an
   electrical idle (asserted), a line-level error between two coalescable
   ordered sets or inside a `show_idle` idle run, a code error inside a TS,
   a slip whose misaligned group is a valid code, a trace ending in a held
   level after X, and `auto` scrambling that never locks (the order of
   line-level transactions in the undetermined buffer is not modelled; the
   generator refuses it).

## Multi-lane fixtures (SPEC §12)

The `pipe_x*`/`dll_x*` fixtures come from `pcie_multilane_lib.py`. `Link` is
the transmitter: a packet layer (an x1 `Stream`) striped over the logical
lanes, a copy of every ordered set per lane (own Link#/Lane#, SKP count,
N_FTS), one scrambler per lane, skew, dropped or corrupted symbols, lane
gaps, common breaks and dead lanes. `Layout` puts each lane on its own bus
(`TxData`, `TxDataK`, `TxData1`, ... bound as `data`, `datak`, `data1`, ...)
and cuts it into §12.3 items from what was sent. `LaneModel` applies
§12.4-§12.13; what it feeds the pipeline maps back to the transmitter's
units, and the x1 engines apply with the §12.9 lists and §12.10 framing on
top. The reference decoder is x1 only, so `capture_check.py` skips these.

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
