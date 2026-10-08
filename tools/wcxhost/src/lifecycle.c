// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// --lifecycle: the ABI contract (wavecrux_decoder.h) checked against any
// plugin, using a real fixture as the sample stream (testing standard §1,
// "Lifecycle"). Run it under AddressSanitizer: most of what it provokes only
// shows as a sanitizer report.
//
//   retries     slots = 1 and slots = 0 offered on every call, a fresh bits
//               buffer per feed, and WaveCrux's own policy (16, growing) all
//               give exactly the transactions of a 4096-slot run;
//   isolation   two instances fed alternately each give that same output;
//   hostile     malformed, truncated, wrongly typed, oversized and non-UTF-8
//               config_json never crash (create may return NULL);
//   width       a sample whose bit_width disagrees with the bindings never
//               crashes and is reported (an error transaction or
//               WC_DECODER_ERR);
//   ordering    destroy without flush, flush without feed, create+destroy.

#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "sb.h"
#include "wcx/str.h"
#include "wcx/tool_io.h"

// Samples fed in the hostile and width checks: enough to get past any
// start-up state, few enough to keep hundreds of runs fast.
#define SHORT_RUN 64u

typedef struct checker {
    const wcxh_input *in;
    FILE *log;
    unsigned failures;
    unsigned checks;
    wcxh_run reference;
} checker;

static void report(checker *c, bool ok, const char *name, const char *detail) {
    c->checks++;
    if (!ok) {
        c->failures++;
    }
    wcx_tool_printf(c->log, "lifecycle: %s %s%s%s\n", ok ? "PASS" : "FAIL", name,
                    detail != NULL && detail[0] != '\0' ? ": " : "", detail != NULL ? detail : "");
}

static bool clean(const wcxh_run *r, char *why, size_t cap) {
    if (r->violations > 0) {
        WCX_IGNORE(wcx_str_format(why, cap, "%u contract violation(s), first: %s", r->violations,
                                  r->violation));
        return false;
    }
    return true;
}

// The run must be clean and give exactly the reference transactions.
static void expect_same(checker *c, const char *name, const wcxh_run *r) {
    char why[1024] = {0};
    bool ok = clean(r, why, sizeof why);
    if (ok && r->created != c->reference.created) {
        ok = false;
        WCX_IGNORE(wcx_str_copy(why, sizeof why, "create succeeded in one run and not the other"));
    }
    const size_t diff = wcxh_first_difference(&r->tx, &c->reference.tx);
    if (ok && diff != SIZE_MAX) {
        ok = false;
        char *got = diff < r->tx.count ? wcxh_tx_line(&r->tx.items[diff]) : NULL;
        char *want =
            diff < c->reference.tx.count ? wcxh_tx_line(&c->reference.tx.items[diff]) : NULL;
        WCX_IGNORE(wcx_str_format(
            why, sizeof why,
            "transaction %lu differs (%lu vs %lu in the 4096-slot run): got %.300s, "
            "want %.300s",
            (unsigned long)diff, (unsigned long)r->tx.count, (unsigned long)c->reference.tx.count,
            got != NULL ? got : "(none)", want != NULL ? want : "(none)"));
        free(got);
        free(want);
    }
    if (ok) {
        WCX_IGNORE(wcx_str_format(why, sizeof why, "%lu transactions, %lu NEED_MORE_SLOTS",
                                  (unsigned long)r->tx.count, (unsigned long)r->need_more));
    }
    report(c, ok, name, why);
}

static void check_retries(checker *c) {
    wcxh_run_opts o;
    static const struct {
        const char *name;
        size_t slots;
        bool reset;
        bool fresh;
    } variants[] = {
        {"slots=1 on every call", 1, true, true},
        {"slots=0 on every call", 0, true, false},
        {"WaveCrux slot policy (16, growing)", 16, false, false},
    };
    for (size_t i = 0; i < sizeof variants / sizeof variants[0]; i++) {
        wcxh_run_opts_default(&o);
        o.strict = true;
        o.slots = variants[i].slots;
        o.reset_slots = variants[i].reset;
        o.fresh_bits = variants[i].fresh;
        wcxh_run r;
        wcxh_run_decode(c->in, &o, &r);
        expect_same(c, variants[i].name, &r);
        wcxh_run_free(&r);
    }
    wcxh_run_opts_default(&o);
    o.strict = true;
    wcxh_run a;
    wcxh_run b;
    wcxh_run_pair(c->in, &o, &a, &b);
    expect_same(c, "two instances interleaved (first)", &a);
    expect_same(c, "two instances interleaved (second)", &b);
    wcxh_run_free(&a);
    wcxh_run_free(&b);
}

// Runs `config` briefly; only crashes (sanitizers) and violations fail.
static unsigned hostile_run(const checker *c, const char *config, bool null_config, char *why,
                            size_t cap) {
    wcxh_run_opts o;
    wcxh_run_opts_default(&o);
    o.strict = true;
    o.config = config;
    o.null_config = null_config;
    o.max_samples = SHORT_RUN;
    wcxh_run r;
    wcxh_run_decode(c->in, &o, &r);
    const bool ok = clean(&r, why, cap);
    wcxh_run_free(&r);
    return ok ? 0u : 1u;
}

static char *repeat_char(char ch, size_t n, const char *prefix, const char *suffix) {
    const size_t p = strlen(prefix);
    const size_t s = strlen(suffix);
    char *out = malloc(p + n + s + 1u);
    if (out != NULL) {
        memcpy(out, prefix, p + 1u); /* terminator is overwritten next */
        memset(out + p, ch, n);
        memcpy(out + p + n, suffix, s + 1u);
    }
    return out;
}

static unsigned hostile_fixed(const checker *c, char *why, size_t cap, unsigned *runs) {
    static const char *const configs[] = {
        "",
        "{",
        "}",
        "null",
        "[]",
        "42",
        "\"config\"",
        "{}",
        "{\"signal_bindings\":5}",
        "{\"signal_bindings\":[]}",
        "{\"signal_bindings\":{\"x\":1}}",
        "{\"signal_bindings\":null,\"parameters\":null}",
        "{\"parameters\":\"x\",\"options\":[]}",
        "{\"decoder_id\":5}",
        "\xFF\xFE\xFD",
        "{\"decoder_id\":\"\xC0\xAF\"}",
        "{\"parameters\":{\"\xFF\":1}}",
    };
    unsigned failed = 0;
    failed += hostile_run(c, NULL, true, why, cap);
    (*runs)++;
    for (size_t i = 0; i < sizeof configs / sizeof configs[0]; i++) {
        failed += hostile_run(c, configs[i], false, why, cap);
        (*runs)++;
    }
    char *deep = repeat_char('[', 100000u, "", "");
    char *big = repeat_char('a', (size_t)2u << 20u, "{\"parameters\":{\"x\":\"", "\"}}");
    if (deep != NULL && big != NULL) {
        failed += hostile_run(c, deep, false, why, cap);
        failed += hostile_run(c, big, false, why, cap);
        *runs += 2u;
    }
    free(deep);
    free(big);
    return failed;
}

static unsigned hostile_variants(const checker *c, char *why, size_t cap, unsigned *runs) {
    static const char *const values[] = {
        "\"\"",
        "\"x\"",
        "\"\\u0000\"",
        "1e999",
        "99999999999999999999999",
        "-99999999999999999999",
        "-1",
        "0",
        "1.5",
        "true",
        "null",
        "[]",
        "{}",
        "\"\xFF\xFE\"",
        "2147483648",
        "9223372036854775808",
        "-9223372036854775809",
    };
    unsigned failed = 0;
    const size_t len = strlen(c->in->config_json);
    for (size_t k = 1; k < 16; k++) { // truncations
        char *cut = malloc(len + 1u);
        if (cut == NULL) {
            break;
        }
        const size_t at = len * k / 16u;
        memcpy(cut, c->in->config_json, at);
        cut[at] = '\0';
        failed += hostile_run(c, cut, false, why, cap);
        (*runs)++;
        free(cut);
    }
    for (size_t p = 0; p < c->in->manifest.param_count; p++) { // every parameter, every type
        for (size_t v = 0; v < sizeof values / sizeof values[0]; v++) {
            const wcxh_config_variant var = {p, values[v], false, NULL};
            char *cfg = wcxh_build_config(c->in, &var);
            failed += hostile_run(c, cfg, false, why, cap);
            (*runs)++;
            free(cfg);
        }
    }
    static const wcxh_config_variant others[] = {
        {SIZE_MAX, NULL, true, NULL},
        {SIZE_MAX, NULL, false, "\"no.such.decoder\""},
        {SIZE_MAX, NULL, false, "7"},
    };
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
        char *cfg = wcxh_build_config(c->in, &others[i]);
        failed += hostile_run(c, cfg, false, why, cap);
        (*runs)++;
        free(cfg);
    }
    return failed;
}

static void check_hostile(checker *c) {
    char why[1024] = {0};
    unsigned runs = 0;
    unsigned failed = hostile_fixed(c, why, sizeof why, &runs);
    failed += hostile_variants(c, why, sizeof why, &runs);
    char detail[1200] = {0};
    if (failed == 0) {
        WCX_IGNORE(wcx_str_format(detail, sizeof detail, "%u configurations", runs));
    } else {
        WCX_IGNORE(wcx_str_format(detail, sizeof detail, "%u of %u configurations failed; %s",
                                  failed, runs, why));
    }
    report(c, failed == 0, "hostile config_json", detail);
}

static bool reported(const wcxh_run *r) {
    if (r->stop_rc == WC_DECODER_ERR) {
        return true;
    }
    for (size_t i = 0; i < r->tx.count; i++) {
        if (r->tx.items[i].is_error) {
            return true;
        }
    }
    return false;
}

static void check_widths(checker *c) {
    const uint32_t w = c->in->bits_per_sample;
    const uint32_t claims[] = {w + 1u, w > 0 ? w - 1u : 2u, 0u, w + 1000u, w * 2u + 1u, UINT32_MAX};
    unsigned failed = 0;
    char why[1024] = {0};
    for (size_t i = 0; i < sizeof claims / sizeof claims[0]; i++) {
        if (claims[i] == w) {
            continue;
        }
        wcxh_run_opts o;
        wcxh_run_opts_default(&o);
        o.strict = true;
        o.override_width = true;
        o.width = claims[i];
        o.max_samples = SHORT_RUN;
        wcxh_run r;
        wcxh_run_decode(c->in, &o, &r);
        if (!clean(&r, why, sizeof why)) {
            failed++;
        } else if (r.created && c->in->timeline_count > 0 && !reported(&r)) {
            failed++;
            WCX_IGNORE(
                wcx_str_format(why, sizeof why,
                               "bit_width %lu instead of %lu was neither an error transaction "
                               "nor WC_DECODER_ERR",
                               (unsigned long)claims[i], (unsigned long)w));
        }
        wcxh_run_free(&r);
    }
    report(c, failed == 0, "wrong bit_width", failed == 0 ? "6 widths" : why);
}

static void check_ordering(checker *c) {
    static const struct {
        const char *name;
        size_t samples;
        bool skip_flush;
    } cases[] = {
        {"destroy without flush", SIZE_MAX, true},
        {"flush without feed", 0, false},
        {"create then destroy", 0, true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        wcxh_run_opts o;
        wcxh_run_opts_default(&o);
        o.strict = true;
        o.max_samples = cases[i].samples;
        o.skip_flush = cases[i].skip_flush;
        wcxh_run r;
        wcxh_run_decode(c->in, &o, &r);
        char why[1024] = {0};
        report(c, clean(&r, why, sizeof why), cases[i].name, why);
        wcxh_run_free(&r);
    }
}

unsigned wcxh_lifecycle(const wcxh_input *in, FILE *log) {
    checker c;
    memset(&c, 0, sizeof c);
    c.in = in;
    c.log = log;
    wcxh_run_opts o;
    wcxh_run_opts_default(&o);
    o.strict = true;
    o.slots = 4096;
    wcxh_run_decode(in, &o, &c.reference);
    char why[1024] = {0};
    const bool ref_ok = clean(&c.reference, why, sizeof why);
    if (ref_ok) {
        WCX_IGNORE(wcx_str_format(why, sizeof why, "%lu transactions from %lu samples",
                                  (unsigned long)c.reference.tx.count,
                                  (unsigned long)in->timeline_count));
    }
    report(&c, ref_ok, "reference run (slots=4096)", why);
    check_retries(&c);
    check_hostile(&c);
    check_widths(&c);
    check_ordering(&c);
    wcx_tool_printf(log, "lifecycle: %u checks, %u failed\n", c.checks, c.failures);
    wcxh_run_free(&c.reference);
    return c.failures;
}
