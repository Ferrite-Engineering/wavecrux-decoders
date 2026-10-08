# C coding standard

This is the standard Ferrite Engineering holds its own decoders to. A decoder
plugin is native code that runs inside someone else's process, on data we did
not produce, at full user rights. A crash takes WaveCrux down with it; a
memory-safety bug is a security bug in every machine that loads the plugin.
So the bar is higher than for ordinary application C, not lower, and "it is
only a plugin" is never a reason to relax it.

The rules draw on the work that has proven itself on code that must not fail:
[SEI CERT C](https://wiki.sei.cmu.edu/confluence/display/c/SEI+CERT+C+Coding+Standard),
MISRA C:2012 (a subset, where it buys safety rather than ceremony), NASA/JPL's
[Power of Ten](https://spinroot.com/gerard/pdf/P10.pdf), the
[OpenSSF Compiler Options Hardening Guide](https://best.openssf.org/Compiler-Hardening-Guides/Compiler-Options-Hardening-Guide-for-C-and-C++.html),
and git's and Microsoft SDL's banned-function lists. Where this document says
**must**, CI enforces it or review rejects the change. Where it says
**should**, a reviewer may accept a written reason.

Contributed (non-`ferrite.*`) decoders are held to the
[minimum bar in CONTRIBUTING.md](../../CONTRIBUTING.md#the-minimum-bar). This
document is the example we set; we hope contributors adopt as much of it as
they can.

## 1. Language

- **C17**, compiled as `-std=c17` (GCC, Clang) and `/std:c17` (MSVC). No
  compiler extensions in decoder code; `common/` may wrap one behind a macro
  when a platform needs it (symbol export is the only current case).
- **C++ only with a written reason** in the decoder's README (an existing,
  well-tested C++ library worth reusing is a good reason; preference is not).
  C++ code is C++17, builds with `-fno-exceptions -fno-rtti`, and nothing but
  `extern "C"` functions with C types crosses the ABI boundary. An exception
  must never propagate into the host.
- **Rust is welcome for contributed decoders** through the same C ABI; this
  standard covers C and C++.
- No VLAs (`-Werror=vla`), no `alloca`, no `setjmp`/`longjmp`, no `goto`
  except the single-exit cleanup pattern (§5), no recursion (§4).

## 2. The ABI boundary

The host contract is [`include/wavecrux_decoder.h`](../../include/wavecrux_decoder.h),
vendored unchanged from the WaveCrux open core; CI fails if it drifts.

- **Export exactly the ABI entry points**, through `WCX_EXPORT` from
  `common/include/wcx/export.h`. Everything else is `static` or hidden
  (`-fvisibility=hidden`); a decoder library's exported symbol table is
  checked in CI.
- **Treat every input from the host as hostile.** `config_json` is parsed with
  the bounded parser in `common/` and every field is type- and range-checked.
  `WcSample.bit_width` is checked against the width the decoder computed from
  its bindings before a single bit is read; a mismatch emits one error
  transaction and the instance stops decoding rather than reading past the
  buffer.
- **Never trust a pointer you did not allocate** beyond the call it arrived
  in: `bits_ptr` is valid only during `feed`.
- **Outputs obey the header's lifetime rule.** Labels and `fields_json` live
  in a per-handle string arena that is reset at the start of each `feed` and
  `flush`, never sooner.
- **`WC_DECODER_NEED_MORE_SLOTS` is a normal path, not an error.** Decoders
  queue transactions internally and drain them into the host's buffer; a
  retry must emit the same transactions, in order, exactly once.
- **Nothing reaches stdout or stderr.** The host owns both (WaveCrux's CLIs
  parse stdout). Diagnostics are error transactions.
- **Never abort the host.** No `abort`, `exit`, `assert` in release builds,
  `__builtin_trap` or signals. An internal invariant violation turns the
  instance into an error state that emits one error transaction and then
  ignores further input. `WCX_ASSERT` exists for debug and sanitizer builds,
  where crashing loudly is the point.

## 3. Memory

The rules that prevent the classic C failures, each enforced by a tool as well
as by review:

| Failure | Rule | Enforced by |
|---|---|---|
| Leak | Every allocation is owned by exactly one handle and freed in `destroy`. Nothing is allocated at library load. | LeakSanitizer in every CI test run; fuzzing |
| Use after free / double free | Free once, in `destroy`, then never touch the handle. Pointers into the arena are invalid after the next `feed`. | AddressSanitizer; the host emulator's lifecycle tests |
| Buffer overflow | No unbounded write. Every array write goes through a length that was checked against the allocation in the same function, or through a `common/` helper that does. | ASan; `-fstrict-flex-arrays=3`, `_FORTIFY_SOURCE=3`; clang-tidy; fuzzing |
| Integer overflow in size math | Size computations use `wcx_size_mul` / `wcx_size_add`, which fail rather than wrap. | UBSan (`integer`), review |
| Uninitialised read | Every struct is zero-initialised at creation (`calloc` or `= {0}`); every local is initialised at declaration. | `-Wuninitialized`, MemorySanitizer in the nightly job, clang-tidy |
| Unbounded growth | Every queue and buffer has a documented ceiling derived from the protocol. Hitting it emits an error transaction and drops, never grows without bound. | Fuzzing with a memory limit (`-rss_limit_mb`) |

- **Allocation happens in `create`** for everything whose size is known from
  the configuration. Growth during `feed` is allowed only for buffers with a
  protocol-derived ceiling, through `wcx_buf_reserve`, which checks it.
- **No allocation per sample.** `feed` runs millions of times per trace.
- **Use `common/` for strings.** `wcx_json_*` escapes everything it writes;
  hand-building JSON with `snprintf` is how a stray quote in a signal name
  becomes a malformed object the host rejects.

## 4. Control flow (Power of Ten, adapted)

- **No recursion.** Stack depth must be provable.
- **Every loop has a static bound** the reader can see: a constant, a length
  checked on entry, or the size of a buffer. `while (1)` exists only in the
  test harness.
- **Functions stay short**: aim for one screen (about 60 lines). A protocol
  state machine is a `switch` over an `enum` whose cases call named functions.
- **Every `switch` over an enum handles every enumerator** (`-Wswitch-enum`)
  and has a `default` that routes to the error state.
- **Check every return value** that can fail; mark our own fallible functions
  `WCX_NODISCARD`.
- **No global mutable state.** The host may run different handles on
  different threads at once, so all state lives in the handle. `static const`
  tables are fine and encouraged.
- **Data-dependent work is bounded per call.** A single `feed` does O(bound)
  work; a malicious trace cannot make one call spin.

## 5. Style

`clang-format` (the repo's `.clang-format`, based on LLVM style, 4-space
indent, 100 columns) is the style; CI checks it. Beyond formatting:

- **Names**: `snake_case` for functions and variables; `wcx_` prefix for
  `common/`; a short decoder prefix (`pipe_`, `dll_`) for decoder-internal
  symbols; `UPPER_SNAKE` for macros and enumerators; types end in `_t` only
  when they are typedefs of scalars.
- **One cleanup path**: a function that acquires several resources releases
  them through a single `cleanup:` label. That is the only allowed `goto`.
- **Fixed-width types** (`uint8_t`, `uint32_t`, `uint64_t`) for anything that
  is a protocol field or crosses the ABI; `size_t` for sizes and indices;
  `bool` from `<stdbool.h>`.
- **No implicit narrowing**: `-Wconversion -Wsign-conversion` are errors. Cast
  explicitly, after a range check, with the reason in a comment if it is not
  obvious.
- **`const` by default** for pointers to data the function does not modify.
- **Comments say why**, cite the spec section for protocol rules
  (`/* PCIe Base 2.1 §3.5.2.1: Ack carries AckNak_Seq_Num */`), and carry no
  dates or ticket numbers that rot.
<!-- REUSE-IgnoreStart -->
- **Every source file** starts with `// SPDX-License-Identifier: Apache-2.0`
  and `// Copyright <year> <holder>`.
<!-- REUSE-IgnoreEnd -->

## 6. Banned functions

`common/include/wcx/banned.h` is included by every translation unit (CMake
force-includes it) and poisons, on GCC and Clang, functions whose safe use
depends on the caller getting a length right somewhere else:

`strcpy`, `strcat`, `strncpy`, `strncat`, `sprintf`, `vsprintf`, `gets`,
`strtok`, `atoi`, `atol`, `atof`, `asctime`, `ctime`, `gmtime`, `localtime`,
`tmpnam`, `rand`, `printf`, `puts`, `fprintf`, `system`.

Use `snprintf` into a checked buffer (via `wcx_str_*`), `memcpy` with a
checked length, `strtol`/`strtoull` with `errno` and end-pointer checks. MSVC
builds get the same list through `/we4996` plus the SDL checks.

## 7. Compiler and linker settings

Set once in `cmake/WcxHardening.cmake`; a decoder never sets its own flags.
These follow the OpenSSF hardening guide, adjusted for a library that the host
loads with `dlopen` (so no `-z nodlopen`, and `-fPIC`).

**GCC and Clang, every build:**

```
-std=c17 -fPIC -fvisibility=hidden
-Wall -Wextra -Wpedantic -Werror
-Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Wcast-align=strict
-Wformat=2 -Werror=format-security -Wimplicit-fallthrough -Wswitch-enum
-Wnull-dereference -Wdouble-promotion -Wstrict-prototypes -Wmissing-prototypes
-Wvla -Werror=vla -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion
-fstrict-flex-arrays=3 -fno-strict-aliasing -ftrivial-auto-var-init=zero
```

**Release builds add:** `-O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong
-fstack-clash-protection`, `-fcf-protection=full` on x86-64,
`-mbranch-protection=standard` on arm64, and on Linux
`-Wl,-z,relro,-z,now,-z,noexecstack -Wl,--as-needed -Wl,--no-undefined`.

**MSVC:** `/std:c17 /W4 /WX /sdl /guard:cf /Qspectre /analyze
/analyze:external-` with `/DYNAMICBASE /HIGHENTROPYVA /CETCOMPAT /guard:cf` at
link.

CMake probes each flag (`check_c_compiler_flag`, with `-Werror` so an
ignored flag does not pass as supported) and applies those the compiler
supports, so a contributor's older toolchain still builds. CI does not get
that leniency: its compilers support every flag above (`-fstrict-flex-arrays=3`
needs GCC 13 or Clang 16, so the Ubuntu 22.04 jobs install GCC 13 and Clang 18
while keeping the 22.04 glibc floor), and with `WCX_REQUIRE_ALL_FLAGS=ON` the
configure step fails if any is missing.

Two measured exceptions, both in `cmake/WcxHardening.cmake`:

- `-fstack-clash-protection` is not implemented for Mach-O, so Apple targets
  are exempt. Linux still requires it.
- Clang has no `-Wcast-align=strict`; it gets `-Wcast-align`.

A warning a compiler adds tomorrow is a build failure tomorrow. That is
intended; fix the code, do not lower the flag.

## 8. Static analysis

Every pull request runs, and must be clean under:

- **clang-tidy** with the repo's `.clang-tidy`: `bugprone-*`, `cert-*`,
  `clang-analyzer-*` (including `security.*` and `unix.Malloc`),
  `misc-*`, `performance-*`, `portability-*`, `readability-*` (minus the
  purely cosmetic checks listed in the file), warnings as errors.
- **cppcheck** with `--enable=warning,style,performance,portability
  --inconclusive --error-exitcode=1` and the bundled MISRA addon in
  advisory mode for `ferrite.*` decoders.
- **MSVC `/analyze`** (PREfast) on everything that ships (plugins and `common/`), which catches a different class of
  buffer-length mistakes from the other two.

A suppression is a one-line comment naming the check and the reason, next to
the line it suppresses. A file-wide or directory-wide suppression is not
accepted.

## 9. Determinism

A decoder is a pure function of its configuration and the sample stream:
the same input produces byte-identical output on every platform, compiler and
run. So: no clocks, no randomness, no locale-dependent formatting (format
numbers with the `wcx_` helpers, which never call into the locale), no
dependence on `char` signedness, no floating point in protocol logic. The
golden tests compare output across all three platforms.

## 10. What review looks for that tools do not

- Does every protocol rule cite the spec section it implements?
- Is every error a user can see a sentence that names the field and the
  expected value, not a code?
- Is every ceiling (queue length, packet length, buffer size) derived from the
  protocol and written next to the constant?
- Would a reader who knows the protocol but not this code find the state
  machine in under a minute?
