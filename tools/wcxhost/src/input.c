// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Line references: lib/services/decoders/ffi/ffi_decoder_loader_io.dart and
// lib/features/decoders/widgets/decoder_config_dialog.dart in the WaveCrux
// open core.

#include <stdlib.h>
#include <string.h>

#include "dart.h"
#include "host.h"
#include "sb.h"
#include "wcx/str.h"
#include "wcx/tool_io.h"

#define BINDINGS_MAX_BYTES ((size_t)16u * 1024u * 1024u)
#define VCD_MAX_BYTES      ((size_t)2u << 30u)
#define NAME_CAP           512u

typedef struct bindings_file {
    char *text;
    size_t len;
    wcx_jdoc doc;
    size_t decoder;  // token or WCX_JSON_NONE
    size_t bindings; // object token
    size_t params;   // object token or WCX_JSON_NONE
} bindings_file;

static bool read_bindings(bindings_file *b, const char *path, wcx_error *err) {
    memset(b, 0, sizeof *b);
    wcx_error inner = {{0}};
    if (!wcx_tool_read_file(path, BINDINGS_MAX_BYTES, &b->text, &b->len, err)) {
        return false;
    }
    if (!wcx_jdoc_parse(&b->doc, b->text, b->len, 1000000u, &inner) ||
        b->doc.toks[0].type != WCX_JT_OBJECT) {
        wcx_error_set(err, "%s: not a JSON object (%s)", path, inner.msg);
        return false;
    }
    const wcx_jdoc *d = &b->doc;
    b->decoder = wcx_jdoc_member(d, 0, "decoder");
    b->bindings = wcx_jdoc_member(d, 0, "signal_bindings");
    b->params = wcx_jdoc_member(d, 0, "parameters");
    if ((b->decoder != WCX_JSON_NONE && d->toks[b->decoder].type != WCX_JT_STRING) ||
        b->bindings == WCX_JSON_NONE || d->toks[b->bindings].type != WCX_JT_OBJECT ||
        (b->params != WCX_JSON_NONE && d->toks[b->params].type != WCX_JT_OBJECT)) {
        wcx_error_set(err,
                      "%s must be {\"decoder\"?: string, \"signal_bindings\": {name: path}, "
                      "\"parameters\"?: {name: value}}",
                      path);
        return false;
    }
    size_t k = b->bindings + 1u;
    for (uint32_t i = 0; i < d->toks[b->bindings].size; i++) {
        if (d->toks[k + 1u].type != WCX_JT_STRING) {
            wcx_error_set(err, "%s: every signal_bindings value must be a signal path string",
                          path);
            return false;
        }
        k = d->toks[k + 1u].next;
    }
    return true;
}

static void free_bindings(bindings_file *b) {
    wcx_jdoc_free(&b->doc);
    free(b->text);
    memset(b, 0, sizeof *b);
}

static bool pick_decoder(wcxh_input *in, const wcxh_plugin *p, const wcxh_setup *s,
                         const bindings_file *b, wcx_error *err) {
    char id[NAME_CAP] = {0};
    if (b->decoder != WCX_JSON_NONE) {
        if (!wcx_jdoc_string(&b->doc, b->decoder, id, sizeof id, NULL)) {
            wcx_error_set(err, "bindings \"decoder\" is too long");
            return false;
        }
    } else if (s->decoder_id != NULL) {
        WCX_IGNORE(wcx_str_copy(id, sizeof id, s->decoder_id));
    } else if (p->count == 1) {
        WCX_IGNORE(wcx_str_copy(id, sizeof id, p->defs[0].id));
    } else {
        wcx_error_set(err, "the plugin has %lu decoders; name one with --decoder",
                      (unsigned long)p->count);
        return false;
    }
    in->def = wcxh_plugin_find(p, id, &in->def_index);
    if (in->def == NULL) {
        wcx_error_set(err, "the plugin has no decoder \"%s\" (try --list)", id);
        return false;
    }
    wcx_error inner = {{0}};
    if (!wcx_manifest_parse(&in->manifest, in->def->manifest_json, &inner)) {
        // WaveCrux rejects the decoder with this message (lines 416-427).
        wcx_error_set(err, "decoder \"%s\" has an invalid manifest: %s", id, inner.msg);
        return false;
    }
    return true;
}

// Resolves every binding to a VCD variable; collects them for the body
// parse in manifest order.
static bool resolve_bindings(wcxh_input *in, const bindings_file *b, size_t *vars, size_t *nvars,
                             wcx_error *err) {
    const wcx_jdoc *d = &b->doc;
    size_t paths[WCX_MAX_SIGNALS] = {0};
    size_t k = b->bindings + 1u;
    for (uint32_t i = 0; i < d->toks[b->bindings].size; i++, k = d->toks[k + 1u].next) {
        char name[NAME_CAP] = {0};
        WCX_IGNORE(wcx_jdoc_string(d, k, name, sizeof name, NULL));
        const size_t sig = wcx_manifest_find_signal(&in->manifest, name);
        if (sig == WCX_JSON_NONE) {
            wcx_error_set(err, "binding \"%s\" is not a signal of decoder \"%s\"", name,
                          in->def->id);
            return false;
        }
        char path[4096] = {0};
        size_t plen = 0;
        if (!wcx_jdoc_string(d, k + 1u, path, sizeof path, &plen) || plen == 0) {
            continue; // an empty binding is no binding (dialog _submit)
        }
        const size_t var = wcxh_vcd_find(&in->vcd, path, err);
        if (var == WCXH_VCD_NONE) {
            return false;
        }
        in->bound[sig] = true;
        paths[sig] = var;
    }
    *nvars = 0;
    for (size_t sig = 0; sig < in->manifest.signal_count; sig++) {
        char name[NAME_CAP] = {0};
        WCX_IGNORE(
            wcx_manifest_text(&in->manifest, in->manifest.signals[sig].name, name, sizeof name));
        if (!in->bound[sig]) {
            if (!in->manifest.signals[sig].optional) {
                // WaveCrux will not create a decoder without every required
                // binding (decoder_config_dialog.dart _allRequiredBound).
                wcx_error_set(err, "required signal \"%s\" has no binding", name);
                return false;
            }
            continue;
        }
        in->vcd_sig[sig] = *nvars;
        vars[(*nvars)++] = paths[sig];
        char ref[WCX_FMT_U64_CAP] = {0};
        (void)wcx_fmt_u64(ref, in->vcd.vars[paths[sig]].signal_ref);
        in->binding_ref[sig] = malloc(strlen(ref) + 1u);
        if (in->binding_ref[sig] == NULL) {
            wcx_error_set(err, "out of memory");
            return false;
        }
        memcpy(in->binding_ref[sig], ref, strlen(ref) + 1u);
    }
    return true;
}

// The JSON text of a --param value, typed by the parameter's kind.
static bool cli_param_json(const wcx_manifest *m, size_t idx, const char *text, wcxh_sb *out,
                           wcx_error *err) {
    const wcx_manifest_param *p = &m->params[idx];
    const size_t n = strlen(text);
    switch (p->kind) {
        case WCX_PARAM_INT: {
            wcx_jdoc d;
            int64_t v = 0;
            const bool ok = wcx_jdoc_parse(&d, text, n, 4, NULL) && wcx_jdoc_int64(&d, 0, &v);
            wcx_jdoc_free(&d);
            if (!ok) {
                wcx_error_set(err, "--param %s needs an integer", text);
                return false;
            }
            char num[WCX_FMT_I64_CAP] = {0};
            wcxh_sb_put(out, num, wcx_fmt_i64(num, v));
            return true;
        }
        case WCX_PARAM_BOOL:
            if (strcmp(text, "true") != 0 && strcmp(text, "false") != 0) {
                wcx_error_set(err, "--param %s needs true or false", text);
                return false;
            }
            wcxh_sb_puts(out, text);
            return true;
        case WCX_PARAM_ENUM:
        case WCX_PARAM_STRING:
        case WCX_PARAM_NONE:
        default:
            wcxh_dart_string(out, text, n);
            return true;
    }
}

static bool set_param(wcxh_input *in, size_t idx, wcxh_sb *value, wcx_error *err) {
    free(in->param_json[idx]);
    in->param_json[idx] = wcxh_sb_take(value);
    if (in->param_json[idx] == NULL) {
        wcx_error_set(err, "out of memory");
        return false;
    }
    return true;
}

// Dialog seeding (decoder_config_dialog.dart initState): every manifest
// parameter, with its manifest default or the type default (lines 686, 764-774),
// then the bindings file's values, then --param.
static bool build_params(wcxh_input *in, const wcxh_setup *s, const bindings_file *b,
                         wcx_error *err) {
    const wcx_manifest *m = &in->manifest;
    for (size_t i = 0; i < m->param_count; i++) {
        wcxh_sb v = {0};
        const wcx_manifest_param *p = &m->params[i];
        if (p->default_value != WCX_JSON_NONE) {
            (void)wcxh_dart_reencode(&v, &m->doc, p->default_value);
        } else {
            wcxh_sb_puts(&v, p->kind == WCX_PARAM_BOOL  ? "false"
                             : p->kind == WCX_PARAM_INT ? "0"
                                                        : "\"\"");
        }
        if (!set_param(in, i, &v, err)) {
            return false;
        }
    }
    const wcx_jdoc *d = &b->doc;
    size_t k = b->params + 1u;
    const uint32_t nb = b->params != WCX_JSON_NONE ? d->toks[b->params].size : 0u;
    for (uint32_t i = 0; i < nb; i++, k = d->toks[k + 1u].next) {
        char name[NAME_CAP] = {0};
        WCX_IGNORE(wcx_jdoc_string(d, k, name, sizeof name, NULL));
        const size_t idx = wcx_manifest_find_param(m, name);
        if (idx == WCX_JSON_NONE) {
            wcx_error_set(err, "bindings parameter \"%s\" is not a parameter of \"%s\"", name,
                          in->def->id);
            return false;
        }
        wcxh_sb v = {0};
        (void)wcxh_dart_reencode(&v, d, k + 1u);
        if (!set_param(in, idx, &v, err)) {
            return false;
        }
    }
    for (size_t i = 0; i < s->param_count; i++) {
        const size_t idx = wcx_manifest_find_param(m, s->params[i].name);
        wcxh_sb v = {0};
        if (idx == WCX_JSON_NONE) {
            wcx_error_set(err, "--param %s: not a parameter of \"%s\"", s->params[i].name,
                          in->def->id);
            return false;
        }
        if (!cli_param_json(m, idx, s->params[i].value, &v, err) || !set_param(in, idx, &v, err)) {
            wcxh_sb_free(&v);
            return false;
        }
    }
    return true;
}

// _collectTimestamps (lines 1243-1266) over the provider's changesInRange,
// which is half-open: [startTime, endTime) with startTime 0 and endTime the
// waveform's last time stamp (active_decoders_provider.dart decodeAll).
static bool build_timeline(wcxh_input *in, wcx_error *err) {
    size_t total = 0;
    for (size_t i = 0; i < in->vcd.sig_count; i++) {
        total += in->vcd.sigs[i].count;
    }
    in->timeline = calloc(total + 1u, sizeof *in->timeline);
    if (in->timeline == NULL) {
        wcx_error_set(err, "out of memory");
        return false;
    }
    size_t n = 0;
    for (size_t i = 0; i < in->vcd.sig_count; i++) {
        const wcxh_vcd_signal *s = &in->vcd.sigs[i];
        for (size_t c = 0; c < s->count; c++) {
            if (s->changes[c].time < in->vcd.end_time) {
                in->timeline[n++] = s->changes[c].time;
            }
        }
    }
    in->timeline_count = wcxh_sort_unique(in->timeline, n, 0);
    return true;
}

static bool load_vcd(wcxh_input *in, const wcxh_setup *s, wcx_error *err) {
    if (!wcx_tool_read_file(s->vcd_path, VCD_MAX_BYTES, &in->vcd_text, &in->vcd_len, err)) {
        return false;
    }
    wcx_error inner = {{0}};
    if (!wcxh_vcd_parse_header(&in->vcd, in->vcd_text, in->vcd_len, &inner)) {
        wcx_error_set(err, "%s: %s", s->vcd_path, inner.msg);
        return false;
    }
    return true;
}

bool wcxh_input_build(wcxh_input *in, const wcxh_plugin *p, const wcxh_setup *s, wcx_error *err) {
    memset(in, 0, sizeof *in);
    bindings_file b;
    size_t vars[WCX_MAX_SIGNALS] = {0};
    size_t nvars = 0;
    bool ok = read_bindings(&b, s->bindings_path, err) && pick_decoder(in, p, s, &b, err) &&
              load_vcd(in, s, err) && resolve_bindings(in, &b, vars, &nvars, err);
    if (ok) {
        wcx_error inner = {{0}};
        ok = wcxh_vcd_parse_body(&in->vcd, vars, nvars, &inner);
        if (!ok) {
            wcx_error_set(err, "%s: %s", s->vcd_path, inner.msg);
        }
    }
    ok = ok && build_params(in, s, &b, err) && build_timeline(in, err);
    free_bindings(&b);
    if (!ok) {
        return false;
    }
    // decode() lines 963-970: the sample width is the sum of the bound
    // manifest widths.
    for (size_t sig = 0; sig < in->manifest.signal_count; sig++) {
        if (in->bound[sig]) {
            in->bits_per_sample += in->manifest.signals[sig].bit_width;
        }
    }
    in->fs_per_tick =
        wcxh_fs_per_tick(in->vcd.has_timescale, in->vcd.ts_factor, in->vcd.ts_exponent);
    in->config_json = wcxh_build_config(in, NULL);
    if (in->config_json == NULL) {
        wcx_error_set(err, "out of memory");
        return false;
    }
    return true;
}

static void put_object(wcxh_sb *sb, const wcxh_input *in, bool params,
                       const wcxh_config_variant *v) {
    wcxh_sb_putc(sb, '{');
    bool first = true;
    const wcx_manifest *m = &in->manifest;
    const size_t count = params ? m->param_count : m->signal_count;
    for (size_t i = 0; i < count; i++) {
        if (!params && (!in->bound[i] || (v != NULL && v->drop_bindings))) {
            continue;
        }
        char name[NAME_CAP] = {0};
        WCX_IGNORE(wcx_manifest_text(m, params ? m->params[i].name : m->signals[i].name, name,
                                     sizeof name));
        if (!first) {
            wcxh_sb_putc(sb, ',');
        }
        first = false;
        wcxh_dart_string(sb, name, strlen(name));
        wcxh_sb_putc(sb, ':');
        if (!params) {
            wcxh_dart_string(sb, in->binding_ref[i], strlen(in->binding_ref[i]));
        } else if (v != NULL && v->param == i) {
            wcxh_sb_puts(sb, v->param_value);
        } else {
            wcxh_sb_puts(sb, in->param_json[i]);
        }
    }
    wcxh_sb_putc(sb, '}');
}

char *wcxh_build_config(const wcxh_input *in, const wcxh_config_variant *v) {
    // _serializeConfig (lines 1134-1141): jsonEncode of
    // {decoder_id, signal_bindings, parameters, options: parameters}.
    wcxh_sb sb = {0};
    wcxh_sb_puts(&sb, "{\"decoder_id\":");
    if (v != NULL && v->decoder_id != NULL) {
        wcxh_sb_puts(&sb, v->decoder_id);
    } else {
        wcxh_dart_string(&sb, in->def->id, strlen(in->def->id));
    }
    wcxh_sb_puts(&sb, ",\"signal_bindings\":");
    put_object(&sb, in, false, v);
    wcxh_sb_puts(&sb, ",\"parameters\":");
    put_object(&sb, in, true, v);
    wcxh_sb_puts(&sb, ",\"options\":");
    put_object(&sb, in, true, v);
    wcxh_sb_putc(&sb, '}');
    return wcxh_sb_take(&sb);
}

void wcxh_input_free(wcxh_input *in) {
    if (in == NULL) {
        return;
    }
    wcx_manifest_free(&in->manifest);
    wcxh_vcd_free(&in->vcd);
    free(in->vcd_text);
    for (size_t i = 0; i < WCX_MAX_SIGNALS; i++) {
        free(in->binding_ref[i]);
    }
    for (size_t i = 0; i < WCX_MANIFEST_MAX_PARAMS; i++) {
        free(in->param_json[i]);
    }
    free(in->timeline);
    free(in->config_json);
    memset(in, 0, sizeof *in);
}
