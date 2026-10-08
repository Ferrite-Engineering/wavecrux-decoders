# Test-plugin fixtures

Hand-written VCDs for `wcx.test.echo` and `wcx.test.count`
(`../echo_plugin.c`). Every expected transaction below is derived by hand from
the trace and the plugin's documented behaviour, not from running it
(testing standard §2, source 3). Times: `startTime = floor(start_fs /
fs_per_tick)`, `endTime = ceil(end_fs / fs_per_tick)`; the echo decoder emits
`end_fs = start_fs + 1`, so `endTime` is one tick after `startTime`.

## echo_zero_delay_1ns (1 ns: 10^6 fs per tick)

Zero-delay RTL: `data` changes on the same timestamp as each rising `clk`.
Samples at 0, 5, 10, 15, 20, 25, 30 (the change-free `#35` is the trace end
and is never fed). `b1011010` is 7 digits for an 8-bit bus: zero-extended to
0x5A.

| Edge | Time | Pre-edge sample | data | Transaction |
|---|---|---|---|---|
| 1 | 5 | t=0 | 0x5A | `D=0x5A` 5..6 |
| 2 | 15 | t=10 (data set at 5) | 0xC3 | `D=0xC3` 15..16 |
| 3 | 25 | t=20 (data set at 15) | 0x0F | `D=0x0F` 25..26 |

Flush: `edges=3`, 7 samples, from the first edge (5) to the last sample (30).
The at-edge values (0xC3, 0x0F, 0x00) must NOT appear: that is the pre-edge
sampling check.

## echo_delayed_10ps (10 ps: 10^4 fs per tick)

Testbench with delays: data and `valid` change 3 ticks after each falling
edge. Samples at 0, 3, 10, 13, 20, 30, 33, 40, 50, 53, 60, 70, 80 (13).

| Edge | Time | Pre-edge sample | valid | data | Transaction |
|---|---|---|---|---|---|
| 1 | 10 | t=3 | 1 | 0xA5 | `D=0xA5` 10..11 |
| 2 | 30 | t=20 | 0 | 0x01 | none (valid low) |
| 3 | 50 | t=40 | 1 | `bz1` = zzzzzzz1 | error `data is X or Z at the rising edge` 50..51 |
| 4 | 70 | t=60 | 1 | 0xFF | `D=0xFF` 70..71 |

Flush: `edges=4`, 13 samples, 10..80. `bx` at t=0 x-extends to the full bus.

## echo_burst_1ps (1 ps: 10^3 fs per tick)

`repeat = 20` transactions on one edge: more than WaveCrux's 16 initial
slots, so the golden run goes through `NEED_MORE_SLOTS`. `sample_point =
at_edge` with zero-delay data, so the edge at 100 reports the new value 0x22
(the before-edge value would be 0x11). `prefix` is `p"\` to exercise label
escaping. `emit_summary = false`: no flush transaction. The `$timescale` has a
space (`1 ps`).

## count_aliases_100fs (100 fs per tick)

`wcx.test.count` bound through an alias (`t.u.clk_alias` shares id `!` with
`t.clk`). The header has `$date`, `$version`, `$comment` and a real variable
(its `r` changes are ignored). `#15` after `#20` goes backwards: skipped with
its `1!`. The `1!` at `#50`, the last timestamp, is outside `[0, endTime)`.
Fed samples: 0 (0), 10 (1), 20 (0), 30 (1), 40 (0): 5 samples, rising edges at
10 and 30. Flush: `samples=5 rising=2`, 0..40.
