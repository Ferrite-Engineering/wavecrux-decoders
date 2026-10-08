# common/

`wcx_common`: the static library (position-independent, symbols hidden) every
decoder links. Each header in `include/wcx/` documents its own contract at the
top; this page is the map.

## Writing a decoder: the short version

Build on the decoder base (`wcx/decoder.h`). It owns everything the ABI makes
hard to get right, so a decoder is a signal table, a manifest string and four
callbacks:

```c
#include "wcx/decoder.h"
#include "wcx/edge.h"
#include "wcx/json_writer.h"

enum { SIG_PCLK, SIG_DATA, SIG_VALID, SIG_COUNT };   // manifest order
static const wcx_signal_spec k_signals[SIG_COUNT] = {
    {"pclk", 1, false}, {"data", 32, false}, {"valid", 1, true}};
static const char k_manifest[] = "{\"signals\":[...],\"optional_signals\":[...],"
                                 "\"parameters\":[...]}";

typedef struct pipe_state { wcx_edge clk; int64_t lanes; } pipe_state;

static bool pipe_init(wcx_decoder *d, void *state) {
    pipe_state *st = state;
    if (!wcx_param_int(d, "lanes", 1, 1, 16, &st->lanes)) {
        return false;                         // the base reports the message
    }
    return wcx_edge_init(&st->clk, wcx_decoder_layout(d), SIG_PCLK, WCX_SAMPLE_BEFORE_EDGE);
}

static void pipe_sample(wcx_decoder *d, void *state, const wcx_sample *s) {
    pipe_state *st = state;
    if (wcx_edge_step(&st->clk, s->timestamp_fs, s->bits) != WCX_EDGE_RISING) {
        return;
    }
    uint64_t data = 0;
    bool x = false;
    WCX_IGNORE(wcx_sample_read(s->layout, wcx_edge_data(&st->clk), SIG_DATA, &data, &x));
    wcx_json j;
    wcx_json_begin(&j, wcx_decoder_arena(d));
    wcx_json_add_hex(&j, "data", data, 8);
    (void)wcx_emit(d, s->timestamp_fs, s->timestamp_fs,
                   wcx_arena_printf(wcx_decoder_arena(d), "DATA 0x%08llX",
                                    (unsigned long long)data),
                   wcx_json_end(&j), x);
}

static void pipe_fini(wcx_decoder *d, void *state) {
    (void)d;
    wcx_edge_free(&((pipe_state *)state)->clk);
}

static WcDecoderHandle pipe_create(const char *config_json);
static const wcx_decoder_class k_pipe = {
    .id = "ferrite.pcie.pipe", .display_name = "PCIe PIPE", .manifest_json = k_manifest,
    .signals = k_signals, .signal_count = SIG_COUNT, .state_size = sizeof(pipe_state),
    .max_transactions_per_call = 4,   // derive from the protocol, cite it
    .init = pipe_init, .on_sample = pipe_sample, .fini = pipe_fini, .create = pipe_create};
static WcDecoderHandle pipe_create(const char *config_json) {
    return wcx_decoder_create(&k_pipe, config_json);
}

static const wcx_decoder_class *const k_classes[] = {&k_pipe};
WCX_PLUGIN("Ferrite PCIe decoders", "Apache-2.0, Ferrite Engineering", k_classes)
```

`common/tests/plugins/echo_plugin.c` is a complete, tested example with two
decoders, optional signals, every parameter kind and the summary-at-flush
pattern.

What the base guarantees, so the decoder does not have to:

- **The manifest and the C signal table agree** (names, widths, optional
  flags, order), checked at every `create`.
- **`config_json` is hostile-proof.** Parsed with the bounded parser, shape
  checked; parameters are read only in `init` through `wcx_param_*`, which
  check type and range and produce a user-readable message.
- **`WcSample.bit_width` is checked** before `on_sample` runs; a mismatch,
  a NULL sample or timestamps going backwards never reach the decoder.
- **`on_sample` runs exactly once per sample.** A `NEED_MORE_SLOTS` retry is
  recognised (same sample, by fingerprint) and served from the queue.
- **Strings live long enough.** The arena is reset at the start of a call
  only once the host has copied the previous batch.
- **One error, then silence.** Any failure (bad config, `wcx_decoder_fail`,
  arena or queue exhausted) emits a single error transaction whose label is
  the message, then the instance ignores input. `create` returns NULL only
  when memory runs out.
- **Ill-formed UTF-8 in a label or fields_json is repaired** (U+FFFD); the
  host would otherwise drop every transaction of the decode.
- **No global state**; handles are independent (checked by `wcxhost
  --lifecycle`, which feeds two instances interleaved).

Rules for decoder code: emit only from `on_sample` / `on_flush`; build
strings in `wcx_decoder_arena(d)` (or use static strings); never keep a
pointer to `s->bits` or to arena strings past the callback; derive
`max_transactions_per_call` and `arena_max_bytes` from the protocol and cite
the section next to the number.

## Modules

| Header | Contract in one line |
|---|---|
| `export.h` | `WCX_EXPORT` (ABI entry points only), `WCX_NODISCARD`, `WCX_PRINTF`, `WCX_IGNORE` (discard a NODISCARD result on purpose; GCC ignores `(void)`) |
| `banned.h` | Force-included everywhere; poisons the §6 functions (GCC/Clang), deprecates them (MSVC) |
| `assert.h` | `WCX_ASSERT`: aborts in debug/sanitizer/fuzz builds, compiles out in release |
| `size.h` | `wcx_size_add/mul`, `wcx_u64_add/mul`: true and the result, or false and 0 on overflow |
| `str.h` | Bounded copy/format that report truncation and never split UTF-8; locale-free `wcx_fmt_u64/i64/hex`; UTF-8 validation |
| `error.h` | `wcx_error`: one user-facing sentence, at most 255 bytes, valid UTF-8 |
| `buf.h` | `wcx_buf_reserve`: geometric growth up to a protocol-derived ceiling |
| `arena.h` | Per-handle string arena: alloc, strdup, printf, grow; a total ceiling; blocks reused across resets |
| `json_parse.h` | Bounded, non-recursive RFC 8259 parser to a flat token array; member lookup (last duplicate wins), string decoding, exact integers |
| `json_writer.h` | Flat JSON objects with full escaping (control chars as `\u00XX`, bad UTF-8 as U+FFFD), locale-free numbers, hex strings; sticky failure |
| `config.h` | The `config_json` shape; bound-signal queries; int/bool/string/enum parameters with defaults and range checks |
| `manifest.h` | A manifest parsed with WaveCrux's loader rules; signal-table cross-check |
| `sample.h` | The packed-sample layout exactly as WaveCrux builds it; reading a signal, a slice or a bit with its X/Z flag |
| `edge.h` | Clock-edge tracking that keeps the previous sample, for pre-edge (default) or at-edge data |
| `emit.h` | The `NEED_MORE_SLOTS` queue and its state machine (used by the base; usable alone) |
| `decoder.h` | The decoder base, `wcx_register`, `WCX_PLUGIN` |
| `test.h` | Unit test macros with file:line reports; ctest exit codes |

Test-tool support, never linked into a plugin: `fuzz/include/wcx/fuzz_driver.h`
(the shared libFuzzer driver and its input format) and
`fuzz/include/wcx/tool_io.h` (file I/O for tools).

## The NEED_MORE_SLOTS state machine

The ABI lets the host offer fewer slots than a call produces. The plugin then
returns `WC_DECODER_NEED_MORE_SLOTS` with the count required, the host copies
nothing, and calls again with the **same sample** and a bigger buffer. The
retry must deliver the same transactions exactly once, without decoding the
sample again, and their strings must still be valid. `wcx/emit.h` has the
diagram; the base implements it as:

```
feed(sample):
  if a batch is pending and sample is the one it came from:  drain only (retry)
  else:
    if the previous batch was delivered: clear queue, reset arena
    (a host that moved on without retrying keeps its pending batch: append)
    validate; on_sample once; drain
flush: the same, keyed on "pending flush"
```

## Why a parser of our own

The brief allowed vendoring [jsmn](https://github.com/zserge/jsmn). It was
evaluated and not used:

- Under this repository's warning flags (`-Wconversion -Wsign-conversion
  -Wswitch-enum ...`, all errors) jsmn's header produces 18 diagnostics
  (`int`/`unsigned int` mixing, implicit narrowing). Keeping it would need a
  file-wide warning suppression, which §8 does not accept, or patching the
  vendored file.
- Even in `JSMN_STRICT` mode it validates neither number grammar nor
  literals beyond their first character, nor UTF-8, and it accepts some
  malformed documents; every caller would need to re-validate, which is where
  bugs hide.
- It needs the caller to size the token array.

`json_parse.c` is about 600 lines with its accessors, rejects everything RFC 8259 rejects
(including ill-formed UTF-8 and bad escapes), counts tokens in a first pass
so allocation is exact, bounds depth at 64 without recursion, and is
exercised by every config, manifest and fuzz test. `third_party/` is
therefore empty (see its README).

## Tests

`common/tests/test_<module>.c`, one per module, include hostile JSON,
overflow, escaping, packing across byte boundaries with X/Z and the retry
contract. `common/tests/plugins/` holds the test plugin, built with
`wcx_add_decoder` exactly like a real decoder, with hand-derived fixtures
(`fixtures/README.md`), its lifecycle unit test and its fuzz target.
