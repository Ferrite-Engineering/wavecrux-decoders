// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// wcxhost: loads a WaveCrux decoder plugin exactly as WaveCrux does, decodes
// a VCD through it and prints the transactions as canonical JSON.
//
//   wcxhost --plugin <lib> [--decoder <id>] --vcd <file> --bindings <file>
//           [--param name=value]... [--slots N] [--out <file>] [--sort]
//           [--check <expected.json>] [--lifecycle] [--fuzz-seed <file>]
//   wcxhost --plugin <lib> --list
//
// --sort prints and compares transactions in canonical order (startTime,
// endTime, label, isError, fields) instead of emission order, so an expected
// file need not predict the order in which a decoder emits transactions that
// overlap in time.
//
// Exit status: 0 success; 1 output differs from --check, or a --lifecycle
// check failed; 2 bad usage or input; 3 the plugin broke the ABI contract.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "plugin.h"
#include "wcx/str.h"
#include "wcx/tool_io.h"

#define EXIT_MISMATCH  1
#define EXIT_USAGE     2
#define EXIT_VIOLATION 3

#define MAX_PARAMS   64
#define EXPECTED_MAX ((size_t)1u << 30u)

typedef struct options {
    const char *plugin;
    const char *check;
    const char *out;
    const char *fuzz_seed;
    bool lifecycle;
    bool list;
    bool sort;
    size_t slots;
    wcxh_setup setup;
    wcxh_param_override params[MAX_PARAMS];
} options;

static const char k_usage[] =
    "usage: wcxhost --plugin <lib> [--decoder <id>] --vcd <file> --bindings <file>\n"
    "               [--param name=value]... [--slots N] [--out <file>] [--sort]\n"
    "               [--check <expected.json>] [--lifecycle] [--fuzz-seed <file>]\n"
    "       wcxhost --plugin <lib> --list\n"
    "See cmake/README.md for the bindings and expected-file formats.\n";

static int usage(const char *why) {
    if (why != NULL) {
        wcx_tool_printf(stderr, "wcxhost: %s\n", why);
    }
    (void)fputs(k_usage, stderr);
    return EXIT_USAGE;
}

static bool parse_slots(const char *s, size_t *out) {
    char *end = NULL;
    errno = 0;
    const unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > 1000000u) {
        return false;
    }
    *out = (size_t)v;
    return true;
}

// Splits "name=value" in place.
static bool add_param(options *o, char *arg) {
    char *eq = strchr(arg, '=');
    if (eq == NULL || eq == arg || o->setup.param_count >= MAX_PARAMS) {
        return false;
    }
    *eq = '\0';
    o->params[o->setup.param_count].name = arg;
    o->params[o->setup.param_count].value = eq + 1;
    o->setup.param_count++;
    return true;
}

static int parse_args(options *o, int argc, char **argv) {
    memset(o, 0, sizeof *o);
    o->slots = 16;
    o->setup.params = o->params;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const bool has_value = i + 1 < argc;
        if (strcmp(a, "--lifecycle") == 0) {
            o->lifecycle = true;
        } else if (strcmp(a, "--list") == 0) {
            o->list = true;
        } else if (strcmp(a, "--sort") == 0) {
            o->sort = true;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            return usage(NULL);
        } else if (!has_value) {
            return usage("an option is missing its value");
        } else if (strcmp(a, "--plugin") == 0) {
            o->plugin = argv[++i];
        } else if (strcmp(a, "--decoder") == 0) {
            o->setup.decoder_id = argv[++i];
        } else if (strcmp(a, "--vcd") == 0) {
            o->setup.vcd_path = argv[++i];
        } else if (strcmp(a, "--bindings") == 0) {
            o->setup.bindings_path = argv[++i];
        } else if (strcmp(a, "--check") == 0) {
            o->check = argv[++i];
        } else if (strcmp(a, "--out") == 0) {
            o->out = argv[++i];
        } else if (strcmp(a, "--fuzz-seed") == 0) {
            o->fuzz_seed = argv[++i];
        } else if (strcmp(a, "--slots") == 0) {
            if (!parse_slots(argv[++i], &o->slots)) {
                return usage("--slots needs a number from 0 to 1000000");
            }
        } else if (strcmp(a, "--param") == 0) {
            if (!add_param(o, argv[++i])) {
                return usage("--param needs name=value");
            }
        } else {
            return usage("unknown option");
        }
    }
    if (o->plugin == NULL) {
        return usage("--plugin is required");
    }
    if (!o->list && (o->setup.vcd_path == NULL || o->setup.bindings_path == NULL)) {
        return usage("--vcd and --bindings are required");
    }
    return 0;
}

static int list_decoders(const wcxh_plugin *p) {
    wcx_tool_printf(stdout, "plugin: %s (ABI %u.%u)\n", p->name != NULL ? p->name : "(unnamed)",
                    (unsigned)WAVECRUX_DECODER_ABI_GET_MAJOR(p->abi_version),
                    (unsigned)WAVECRUX_DECODER_ABI_GET_MINOR(p->abi_version));
    for (size_t i = 0; i < p->count; i++) {
        wcx_tool_printf(stdout, "  %s  %s\n", p->defs[i].id, p->defs[i].display_name);
    }
    return 0;
}

// Things WaveCrux would do silently that a fixture author should know.
static void input_notes(const wcxh_input *in) {
    for (size_t sig = 0; sig < in->manifest.signal_count; sig++) {
        if (!in->bound[sig]) {
            continue;
        }
        const wcxh_vcd_var *var = &in->vcd.vars[in->vcd.sigs[in->vcd_sig[sig]].var];
        if (var->width != in->manifest.signals[sig].bit_width) {
            wcx_tool_printf(stderr,
                            "wcxhost: note: %s is %lu bits in the VCD but %lu in the manifest; "
                            "WaveCrux packs the manifest width (low bits)\n",
                            var->path, (unsigned long)var->width,
                            (unsigned long)in->manifest.signals[sig].bit_width);
        }
    }
    if (in->vcd.real_changes_ignored + in->vcd.string_changes_ignored > 0) {
        wcx_tool_printf(stderr, "wcxhost: note: ignored %u real and %u string value changes\n",
                        in->vcd.real_changes_ignored, in->vcd.string_changes_ignored);
    }
    if (in->vcd.backwards_times > 0) {
        wcx_tool_printf(stderr,
                        "wcxhost: note: %u time stamps went backwards; their changes were skipped "
                        "(as WaveCrux does)\n",
                        in->vcd.backwards_times);
    }
}

static void run_notes(const wcxh_run *r) {
    if (!r->created) {
        (void)fputs("wcxhost: note: create returned NULL; WaveCrux shows no transactions\n",
                    stderr);
    }
    if (r->stop_rc != WC_DECODER_OK) {
        wcx_tool_printf(stderr,
                        "wcxhost: note: %s returned %d; WaveCrux keeps the transactions before "
                        "it\n",
                        r->stop_call, (int)r->stop_rc);
    }
    if (r->warnings > 0) {
        wcx_tool_printf(stderr, "wcxhost: warning (%u): %s\n", r->warnings, r->warning);
    }
    if (r->violations > 0) {
        wcx_tool_printf(stderr, "wcxhost: ABI contract violation (%u): %s\n", r->violations,
                        r->violation);
    }
}

static int emit_output(const options *o, const wcxh_run *r) {
    char *text = wcxh_format(&r->tx);
    if (text == NULL) {
        (void)fputs("wcxhost: out of memory\n", stderr);
        return EXIT_USAGE;
    }
    int rc = 0;
    if (o->out != NULL) {
        wcx_error err = {{0}};
        if (!wcx_tool_write_file(o->out, text, strlen(text), &err)) {
            wcx_tool_printf(stderr, "wcxhost: %s\n", err.msg);
            rc = EXIT_USAGE;
        }
    } else if (o->check == NULL) {
        (void)fputs(text, stdout);
    }
    free(text);
    return rc;
}

static int check_expected(const char *path, bool sort, const wcxh_run *r) {
    char *text = NULL;
    size_t len = 0;
    wcx_error err = {{0}};
    wcxh_txlist want;
    if (!wcx_tool_read_file(path, EXPECTED_MAX, &text, &len, &err) ||
        !wcxh_parse_expected(text, len, &want, &err)) {
        wcx_tool_printf(stderr, "wcxhost: %s: %s\n", path, err.msg);
        free(text);
        return EXIT_USAGE;
    }
    free(text);
    if (sort) {
        wcxh_txlist_sort(&want); // r->tx was sorted by decode()
    }
    const size_t diff = wcxh_first_difference(&r->tx, &want);
    int rc = 0;
    if (diff != SIZE_MAX) {
        char *got = diff < r->tx.count ? wcxh_tx_line(&r->tx.items[diff]) : NULL;
        char *exp = diff < want.count ? wcxh_tx_line(&want.items[diff]) : NULL;
        wcx_tool_printf(stderr,
                        "wcxhost: output differs from %s at transaction %lu "
                        "(%lu expected, %lu decoded)\n",
                        path, (unsigned long)diff, (unsigned long)want.count,
                        (unsigned long)r->tx.count);
        (void)fputs("  expected: ", stderr);
        (void)fputs(exp != NULL ? exp : "(no more transactions)", stderr);
        (void)fputs("\n  actual:   ", stderr);
        (void)fputs(got != NULL ? got : "(no more transactions)", stderr);
        (void)fputs("\n", stderr);
        free(got);
        free(exp);
        rc = EXIT_MISMATCH;
    } else {
        wcx_tool_printf(stdout, "wcxhost: %lu transactions match %s\n", (unsigned long)want.count,
                        path);
    }
    wcxh_txlist_free(&want);
    return rc;
}

static int decode(const options *o, const wcxh_input *in) {
    if (o->fuzz_seed != NULL) {
        wcx_error err = {{0}};
        if (!wcxh_write_fuzz_seed(in, o->fuzz_seed, &err)) {
            wcx_tool_printf(stderr, "wcxhost: %s\n", err.msg);
            return EXIT_USAGE;
        }
    }
    if (o->lifecycle) {
        return wcxh_lifecycle(in, stdout) == 0 ? 0 : EXIT_MISMATCH;
    }
    wcxh_run_opts ro;
    wcxh_run_opts_default(&ro);
    ro.slots = o->slots;
    wcxh_run r;
    wcxh_run_decode(in, &ro, &r);
    run_notes(&r);
    if (o->sort) {
        wcxh_txlist_sort(&r.tx);
    }
    int rc = emit_output(o, &r);
    if (rc == 0 && o->check != NULL) {
        rc = check_expected(o->check, o->sort, &r);
    }
    if (r.violations > 0) {
        rc = EXIT_VIOLATION;
    }
    wcxh_run_free(&r);
    return rc;
}

int main(int argc, char **argv) {
    options o;
    const int prc = parse_args(&o, argc, argv);
    if (prc != 0) {
        return prc;
    }
    wcxh_plugin p;
    wcx_error err = {{0}};
    if (!wcxh_plugin_load(&p, o.plugin, &err)) {
        wcx_tool_printf(stderr, "wcxhost: %s\n", err.msg);
        wcxh_plugin_unload(&p);
        return EXIT_USAGE;
    }
    if (o.list) {
        const int rc = list_decoders(&p);
        wcxh_plugin_unload(&p);
        return rc;
    }
    wcxh_input in;
    int rc = 0;
    if (!wcxh_input_build(&in, &p, &o.setup, &err)) {
        wcx_tool_printf(stderr, "wcxhost: %s\n", err.msg);
        rc = EXIT_USAGE;
    } else {
        input_notes(&in);
        rc = decode(&o, &in);
    }
    wcxh_input_free(&in);
    wcxh_plugin_unload(&p);
    return rc;
}
