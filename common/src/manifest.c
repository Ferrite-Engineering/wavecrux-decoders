// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Line references are to lib/services/decoders/ffi/ffi_decoder_loader_io.dart
// in the WaveCrux open core.

#include "wcx/manifest.h"

#include <stdlib.h>
#include <string.h>

#include "wcx/str.h"

#define NAME_CAP 256u

void wcx_manifest_free(wcx_manifest *m) {
    if (m == NULL) {
        return;
    }
    wcx_jdoc_free(&m->doc);
    free(m->text);
    memset(m, 0, sizeof *m);
}

bool wcx_manifest_text(const wcx_manifest *m, size_t tok, char *buf, size_t cap) {
    return m != NULL && wcx_jdoc_string(&m->doc, tok, buf, cap, NULL);
}

// A member that must be absent, null or a string (the loader's
// `as String?` casts, which throw on anything else).
static bool optional_string(const wcx_jdoc *d, size_t obj, const char *key) {
    const size_t tok = wcx_jdoc_member(d, obj, key);
    return tok == WCX_JSON_NONE || d->toks[tok].type == WCX_JT_NULL ||
           d->toks[tok].type == WCX_JT_STRING;
}

static size_t present(const wcx_jdoc *d, size_t obj, const char *key) {
    const size_t tok = wcx_jdoc_member(d, obj, key);
    if (tok != WCX_JSON_NONE && d->toks[tok].type == WCX_JT_NULL) {
        return WCX_JSON_NONE;
    }
    return tok;
}

static bool nonempty_name(const wcx_jdoc *d, size_t entry, size_t *name) {
    *name = wcx_jdoc_member(d, entry, "name");
    return *name != WCX_JSON_NONE && d->toks[*name].type == WCX_JT_STRING &&
           d->toks[*name].end > d->toks[*name].start;
}

// _decodeBindings (lines 624-659).
static bool parse_signals(wcx_manifest *m, const char *key, bool optional, wcx_error *err) {
    const wcx_jdoc *d = &m->doc;
    const size_t arr = present(d, 0, key);
    if (arr == WCX_JSON_NONE) {
        return true;
    }
    if (d->toks[arr].type != WCX_JT_ARRAY) {
        wcx_error_set(err, "manifest \"%s\" must be a JSON array", key);
        return false;
    }
    size_t e = arr + 1;
    for (uint32_t i = 0; i < d->toks[arr].size; i++, e = d->toks[e].next) {
        if (d->toks[e].type != WCX_JT_OBJECT) {
            wcx_error_set(err, "each manifest \"%s\" entry must be a JSON object", key);
            return false;
        }
        wcx_manifest_signal s = {0};
        s.optional = optional;
        if (!nonempty_name(d, e, &s.name)) {
            wcx_error_set(err, "a manifest \"%s\" entry is missing \"name\"", key);
            return false;
        }
        if (!optional_string(d, e, "description")) {
            wcx_error_set(err, "a manifest signal \"description\" must be a string");
            return false;
        }
        const size_t w = present(d, e, "bit_width");
        int64_t width = 1;
        if (w != WCX_JSON_NONE && !wcx_jdoc_int64(d, w, &width)) {
            wcx_error_set(err, "a manifest signal has a non-integer \"bit_width\"");
            return false;
        }
        if (width < 0 || width > (int64_t)WCX_MAX_SIGNAL_BITS) {
            wcx_error_set(err, "a manifest signal \"bit_width\" must be 0 to %u",
                          WCX_MAX_SIGNAL_BITS);
            return false;
        }
        s.bit_width = (uint32_t)width;
        if (m->signal_count >= WCX_MAX_SIGNALS) {
            wcx_error_set(err, "manifest declares more than %u signals", WCX_MAX_SIGNALS);
            return false;
        }
        m->signals[m->signal_count++] = s;
    }
    return true;
}

// _parameterTypeOf (lines 746-762).
static wcx_param_kind kind_of(const wcx_jdoc *d, size_t tok) {
    static const struct {
        const char *name;
        wcx_param_kind kind;
    } kinds[] = {
        {"bool", WCX_PARAM_BOOL},     {"boolean", WCX_PARAM_BOOL}, {"int", WCX_PARAM_INT},
        {"integer", WCX_PARAM_INT},   {"enum", WCX_PARAM_ENUM},    {"enumeration", WCX_PARAM_ENUM},
        {"string", WCX_PARAM_STRING},
    };
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        if (wcx_jdoc_string_equals(d, tok, kinds[i].name)) {
            return kinds[i].kind;
        }
    }
    return WCX_PARAM_NONE;
}

static bool all_strings(const wcx_jdoc *d, size_t container) {
    size_t c = container + 1;
    const bool object = d->toks[container].type == WCX_JT_OBJECT;
    for (uint32_t i = 0; i < d->toks[container].size; i++) {
        const size_t value = object ? c + 1 : c;
        if (d->toks[value].type != WCX_JT_STRING) {
            return false;
        }
        c = d->toks[value].next;
    }
    return true;
}

// enum_values and enum_labels (lines 682-744).
static bool parse_enum(const wcx_jdoc *d, size_t e, wcx_manifest_param *p, wcx_error *err) {
    p->enum_values = present(d, e, "enum_values");
    if (p->enum_values != WCX_JSON_NONE &&
        (d->toks[p->enum_values].type != WCX_JT_ARRAY || !all_strings(d, p->enum_values))) {
        wcx_error_set(err, "manifest parameter \"enum_values\" must be an array of strings");
        return false;
    }
    p->enum_labels = present(d, e, "enum_labels");
    if (p->enum_labels == WCX_JSON_NONE) {
        return true;
    }
    const wcx_jtok *labels = &d->toks[p->enum_labels];
    if (labels->type == WCX_JT_OBJECT && all_strings(d, p->enum_labels)) {
        return true;
    }
    if (labels->type == WCX_JT_ARRAY && all_strings(d, p->enum_labels) &&
        p->enum_values != WCX_JSON_NONE && labels->size == d->toks[p->enum_values].size) {
        return true;
    }
    wcx_error_set(err, "manifest parameter \"enum_labels\" must be an object of strings, or an "
                       "array with one string per enum_values entry");
    return false;
}

// _decodeParameters (lines 661-698).
static bool parse_params(wcx_manifest *m, wcx_error *err) {
    const wcx_jdoc *d = &m->doc;
    const size_t arr = present(d, 0, "parameters");
    if (arr == WCX_JSON_NONE) {
        return true;
    }
    if (d->toks[arr].type != WCX_JT_ARRAY) {
        wcx_error_set(err, "manifest \"parameters\" must be a JSON array");
        return false;
    }
    size_t e = arr + 1;
    for (uint32_t i = 0; i < d->toks[arr].size; i++, e = d->toks[e].next) {
        wcx_manifest_param p = {0};
        if (d->toks[e].type != WCX_JT_OBJECT || !nonempty_name(d, e, &p.name)) {
            wcx_error_set(err, "each manifest parameter must be an object with a \"name\"");
            return false;
        }
        const size_t kind = wcx_jdoc_member(d, e, "kind");
        p.kind = kind != WCX_JSON_NONE ? kind_of(d, kind) : WCX_PARAM_NONE;
        if (p.kind == WCX_PARAM_NONE) {
            wcx_error_set(err, "a manifest parameter has a missing or unknown \"kind\"");
            return false;
        }
        if (!optional_string(d, e, "description") || !optional_string(d, e, "display_name")) {
            wcx_error_set(err, "manifest parameter \"description\" and \"display_name\" must be "
                               "strings");
            return false;
        }
        if (!parse_enum(d, e, &p, err)) {
            return false;
        }
        p.default_value = present(d, e, "default");
        if (m->param_count >= WCX_MANIFEST_MAX_PARAMS) {
            wcx_error_set(err, "manifest declares more than %u parameters",
                          WCX_MANIFEST_MAX_PARAMS);
            return false;
        }
        m->params[m->param_count++] = p;
    }
    return true;
}

bool wcx_manifest_parse(wcx_manifest *m, const char *json, wcx_error *err) {
    if (m == NULL) {
        return false;
    }
    memset(m, 0, sizeof *m);
    if (json == NULL) {
        wcx_error_set(err, "manifest_json is NULL");
        return false;
    }
    const size_t len = wcx_str_len(json, (size_t)WCX_MANIFEST_MAX_BYTES + 1u);
    if (len > WCX_MANIFEST_MAX_BYTES) {
        wcx_error_set(err, "manifest is larger than %lu bytes",
                      (unsigned long)WCX_MANIFEST_MAX_BYTES);
        return false;
    }
    m->text = malloc(len + 1u);
    if (m->text == NULL) {
        wcx_error_set(err, "out of memory reading the manifest");
        return false;
    }
    memcpy(m->text, json, len + 1u);
    wcx_error inner = {{0}};
    if (!wcx_jdoc_parse(&m->doc, m->text, len, WCX_MANIFEST_MAX_TOKENS, &inner)) {
        wcx_error_set(err, "manifest is not valid JSON (%s)", inner.msg);
        return false;
    }
    const wcx_jdoc *d = &m->doc;
    if (d->toks[0].type != WCX_JT_OBJECT) {
        wcx_error_set(err, "manifest top-level value must be a JSON object");
        return false;
    }
    if (!optional_string(d, 0, "description") || !optional_string(d, 0, "category") ||
        !optional_string(d, 0, "required_tier")) {
        wcx_error_set(err, "manifest \"description\", \"category\" and \"required_tier\" must be "
                           "strings");
        return false;
    }
    // Required bindings first, then optional (line 605: attachBindings).
    return parse_signals(m, "signals", false, err) &&
           parse_signals(m, "optional_signals", true, err) && parse_params(m, err);
}

size_t wcx_manifest_find_signal(const wcx_manifest *m, const char *name) {
    if (m == NULL || name == NULL) {
        return WCX_JSON_NONE;
    }
    for (size_t i = 0; i < m->signal_count; i++) {
        if (wcx_jdoc_string_equals(&m->doc, m->signals[i].name, name)) {
            return i;
        }
    }
    return WCX_JSON_NONE;
}

size_t wcx_manifest_find_param(const wcx_manifest *m, const char *name) {
    if (m == NULL || name == NULL) {
        return WCX_JSON_NONE;
    }
    for (size_t i = 0; i < m->param_count; i++) {
        if (wcx_jdoc_string_equals(&m->doc, m->params[i].name, name)) {
            return i;
        }
    }
    return WCX_JSON_NONE;
}

bool wcx_manifest_check_signals(const wcx_manifest *m, const wcx_signal_spec *specs, size_t n,
                                wcx_error *err) {
    if (m == NULL || (n > 0 && specs == NULL)) {
        return false;
    }
    if (m->signal_count != n) {
        wcx_error_set(err, "decoder signal table has %lu signals but its manifest declares %lu",
                      (unsigned long)n, (unsigned long)m->signal_count);
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const wcx_manifest_signal *s = &m->signals[i];
        char name[NAME_CAP] = {0};
        WCX_IGNORE(wcx_manifest_text(m, s->name, name, sizeof name));
        if (specs[i].name == NULL || !wcx_jdoc_string_equals(&m->doc, s->name, specs[i].name) ||
            specs[i].bit_width != s->bit_width || specs[i].optional != s->optional) {
            wcx_error_set(err,
                          "decoder signal table entry %lu does not match manifest signal \"%s\" "
                          "(%lu bits%s)",
                          (unsigned long)i, name, (unsigned long)s->bit_width,
                          s->optional ? ", optional" : "");
            return false;
        }
    }
    return true;
}
