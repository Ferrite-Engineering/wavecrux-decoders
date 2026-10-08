// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "wcx/config.h"

#include <stdlib.h>
#include <string.h>

#include "wcx/str.h"

// Longest excerpt of an offending value quoted back in an error message.
#define WCX_CONFIG_EXCERPT 40u

void wcx_config_free(wcx_config *cfg) {
    if (cfg == NULL) {
        return;
    }
    wcx_jdoc_free(&cfg->doc);
    free(cfg->text);
    memset(cfg, 0, sizeof *cfg);
    cfg->decoder_id = WCX_JSON_NONE;
    cfg->bindings = WCX_JSON_NONE;
    cfg->parameters = WCX_JSON_NONE;
    cfg->options = WCX_JSON_NONE;
}

// Checks that `key` is absent or of `type`; stores its token in *slot.
static bool top_level(wcx_config *cfg, const char *key, wcx_jtype type, const char *what,
                      size_t *slot, wcx_error *err) {
    const size_t tok = wcx_jdoc_member(&cfg->doc, 0, key);
    *slot = WCX_JSON_NONE;
    if (tok == WCX_JSON_NONE || cfg->doc.toks[tok].type == WCX_JT_NULL) {
        return true;
    }
    if (cfg->doc.toks[tok].type != type) {
        wcx_error_set(err, "configuration \"%s\" must be %s", key, what);
        return false;
    }
    *slot = tok;
    return true;
}

static bool check_bindings(const wcx_config *cfg, wcx_error *err) {
    if (cfg->bindings == WCX_JSON_NONE) {
        return true;
    }
    const wcx_jdoc *d = &cfg->doc;
    size_t key = cfg->bindings + 1;
    for (uint32_t i = 0; i < d->toks[cfg->bindings].size; i++) {
        const size_t value = key + 1;
        const wcx_jtype t = d->toks[value].type;
        if (t != WCX_JT_STRING && t != WCX_JT_NULL) {
            wcx_error_set(err, "configuration \"signal_bindings\" values must be strings");
            return false;
        }
        key = d->toks[value].next;
    }
    return true;
}

bool wcx_config_parse(wcx_config *cfg, const char *config_json, wcx_error *err) {
    if (cfg == NULL) {
        return false;
    }
    memset(cfg, 0, sizeof *cfg);
    wcx_config_free(cfg);
    if (config_json == NULL) {
        wcx_error_set(err, "no configuration was passed to create");
        return false;
    }
    const size_t len = wcx_str_len(config_json, (size_t)WCX_CONFIG_MAX_BYTES + 1u);
    if (len > WCX_CONFIG_MAX_BYTES) {
        wcx_error_set(err, "configuration is larger than %lu bytes",
                      (unsigned long)WCX_CONFIG_MAX_BYTES);
        return false;
    }
    cfg->text = malloc(len + 1u);
    if (cfg->text == NULL) {
        wcx_error_set(err, "out of memory reading the configuration");
        return false;
    }
    memcpy(cfg->text, config_json, len + 1u);
    wcx_error inner = {{0}};
    if (!wcx_jdoc_parse(&cfg->doc, cfg->text, len, WCX_CONFIG_MAX_TOKENS, &inner)) {
        wcx_error_set(err, "configuration is not valid JSON (%s)", inner.msg);
        wcx_config_free(cfg);
        return false;
    }
    if (cfg->doc.toks[0].type != WCX_JT_OBJECT) {
        wcx_error_set(err, "configuration must be a JSON object");
        wcx_config_free(cfg);
        return false;
    }
    if (!top_level(cfg, "decoder_id", WCX_JT_STRING, "a string", &cfg->decoder_id, err) ||
        !top_level(cfg, "signal_bindings", WCX_JT_OBJECT, "an object", &cfg->bindings, err) ||
        !top_level(cfg, "parameters", WCX_JT_OBJECT, "an object", &cfg->parameters, err) ||
        !top_level(cfg, "options", WCX_JT_OBJECT, "an object", &cfg->options, err) ||
        !check_bindings(cfg, err)) {
        wcx_config_free(cfg);
        return false;
    }
    return true;
}

bool wcx_config_has_decoder_id(const wcx_config *cfg) {
    return cfg != NULL && cfg->text != NULL && cfg->decoder_id != WCX_JSON_NONE;
}

bool wcx_config_decoder_id_is(const wcx_config *cfg, const char *id) {
    return wcx_config_has_decoder_id(cfg) && wcx_jdoc_string_equals(&cfg->doc, cfg->decoder_id, id);
}

bool wcx_config_is_bound(const wcx_config *cfg, const char *signal) {
    if (cfg == NULL || cfg->text == NULL || signal == NULL) {
        return false;
    }
    const size_t tok = wcx_jdoc_member(&cfg->doc, cfg->bindings, signal);
    return tok != WCX_JSON_NONE && cfg->doc.toks[tok].type == WCX_JT_STRING &&
           cfg->doc.toks[tok].end > cfg->doc.toks[tok].start;
}

// The token holding parameter `name`, or WCX_JSON_NONE when absent or null.
static size_t param_token(const wcx_config *cfg, const char *name) {
    if (cfg == NULL || cfg->text == NULL || name == NULL) {
        return WCX_JSON_NONE;
    }
    size_t tok = wcx_jdoc_member(&cfg->doc, cfg->parameters, name);
    if (tok == WCX_JSON_NONE) {
        tok = wcx_jdoc_member(&cfg->doc, cfg->options, name);
    }
    if (tok != WCX_JSON_NONE && cfg->doc.toks[tok].type == WCX_JT_NULL) {
        tok = WCX_JSON_NONE;
    }
    return tok;
}

// A short, UTF-8-clean rendering of a value for an error message.
static void describe(const wcx_config *cfg, size_t tok, char *out, size_t cap) {
    const wcx_jtype t = cfg->doc.toks[tok].type;
    if (t == WCX_JT_OBJECT) {
        WCX_IGNORE(wcx_str_copy(out, cap, "an object"));
        return;
    }
    if (t == WCX_JT_ARRAY) {
        WCX_IGNORE(wcx_str_copy(out, cap, "an array"));
        return;
    }
    size_t len = 0;
    const char *raw = wcx_jdoc_raw(&cfg->doc, tok, &len);
    const bool cut = len > WCX_CONFIG_EXCERPT;
    const size_t keep = wcx_utf8_boundary(raw, cut ? WCX_CONFIG_EXCERPT : len);
    const char *quote = t == WCX_JT_STRING ? "\"" : "";
    WCX_IGNORE(
        wcx_str_format(out, cap, "%s%.*s%s%s", quote, (int)keep, raw, cut ? "..." : "", quote));
}

// Parses an integer written as text the way Dart's int.tryParse does:
// optional sign, then decimal digits or 0x/0X and hex digits.
static bool parse_int_text(const char *s, size_t n, int64_t *out) {
    size_t i = 0;
    bool negative = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        i++;
    }
    unsigned base = 10u;
    if (n - i > 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        base = 16u;
        i += 2;
    }
    if (i >= n) {
        return false;
    }
    uint64_t v = 0;
    for (; i < n; i++) {
        const char c = s[i];
        unsigned digit = 16u;
        if (c >= '0' && c <= '9') {
            digit = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (unsigned)(c - 'a') + 10u;
        } else if (c >= 'A' && c <= 'F') {
            digit = (unsigned)(c - 'A') + 10u;
        }
        if (digit >= base || v > (UINT64_MAX - digit) / base) {
            return false;
        }
        v = v * base + digit;
    }
    if (negative) {
        if (v > (uint64_t)INT64_MAX + 1u) {
            return false;
        }
        *out = v == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)v;
        return true;
    }
    if (v > (uint64_t)INT64_MAX) {
        return false;
    }
    *out = (int64_t)v;
    return true;
}

static bool int_from_token(const wcx_config *cfg, size_t tok, int64_t *out) {
    const wcx_jtype t = cfg->doc.toks[tok].type;
    if (t == WCX_JT_NUMBER) {
        return wcx_jdoc_int64(&cfg->doc, tok, out);
    }
    if (t == WCX_JT_STRING) {
        char text[32] = {0};
        size_t len = 0;
        return wcx_jdoc_string(&cfg->doc, tok, text, sizeof text, &len) && len == strlen(text) &&
               parse_int_text(text, len, out);
    }
    return false;
}

bool wcx_config_int(const wcx_config *cfg, const char *name, int64_t def, int64_t min, int64_t max,
                    int64_t *out, wcx_error *err) {
    if (out == NULL || min > max || def < min || def > max) {
        wcx_error_set(err, "parameter \"%s\": decoder passed an invalid range or default",
                      name != NULL ? name : "");
        return false;
    }
    const size_t tok = param_token(cfg, name);
    if (tok == WCX_JSON_NONE) {
        *out = def;
        return true;
    }
    int64_t v = 0;
    if (!int_from_token(cfg, tok, &v) || v < min || v > max) {
        char got[64] = {0};
        describe(cfg, tok, got, sizeof got);
        wcx_error_set(err, "parameter \"%s\" must be an integer from %lld to %lld, got %s", name,
                      (long long)min, (long long)max, got);
        return false;
    }
    *out = v;
    return true;
}

static bool ascii_ieq(const char *a, size_t n, const char *b) {
    if (strlen(b) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != b[i]) {
            return false;
        }
    }
    return true;
}

bool wcx_config_bool(const wcx_config *cfg, const char *name, bool def, bool *out, wcx_error *err) {
    if (out == NULL) {
        return false;
    }
    const size_t tok = param_token(cfg, name);
    if (tok == WCX_JSON_NONE) {
        *out = def;
        return true;
    }
    const wcx_jtype t = cfg->doc.toks[tok].type;
    if (t == WCX_JT_TRUE || t == WCX_JT_FALSE) {
        *out = t == WCX_JT_TRUE;
        return true;
    }
    if (t == WCX_JT_STRING) {
        char text[8] = {0};
        size_t len = 0;
        if (wcx_jdoc_string(&cfg->doc, tok, text, sizeof text, &len)) {
            if (ascii_ieq(text, len, "true") || ascii_ieq(text, len, "1")) {
                *out = true;
                return true;
            }
            if (ascii_ieq(text, len, "false") || ascii_ieq(text, len, "0")) {
                *out = false;
                return true;
            }
        }
    }
    char got[64] = {0};
    describe(cfg, tok, got, sizeof got);
    wcx_error_set(err, "parameter \"%s\" must be true or false, got %s", name, got);
    return false;
}

bool wcx_config_string(const wcx_config *cfg, const char *name, const char *def, char *buf,
                       size_t cap, wcx_error *err) {
    if (buf == NULL || cap == 0) {
        return false;
    }
    const size_t tok = param_token(cfg, name);
    if (tok == WCX_JSON_NONE) {
        if (!wcx_str_copy(buf, cap, def != NULL ? def : "")) {
            wcx_error_set(err, "parameter \"%s\": default does not fit", name);
            return false;
        }
        return true;
    }
    size_t len = 0;
    if (cfg->doc.toks[tok].type != WCX_JT_STRING) {
        char got[64] = {0};
        describe(cfg, tok, got, sizeof got);
        wcx_error_set(err, "parameter \"%s\" must be a string, got %s", name, got);
        return false;
    }
    if (!wcx_jdoc_string(&cfg->doc, tok, buf, cap, &len)) {
        wcx_error_set(err, "parameter \"%s\" must be at most %lu bytes long", name,
                      (unsigned long)(cap - 1u));
        return false;
    }
    if (len != strlen(buf)) {
        buf[0] = '\0';
        wcx_error_set(err, "parameter \"%s\" must not contain NUL characters", name);
        return false;
    }
    return true;
}

bool wcx_config_enum(const wcx_config *cfg, const char *name, const char *const *values,
                     size_t count, size_t def_index, size_t *out_index, wcx_error *err) {
    if (values == NULL || out_index == NULL || def_index >= count) {
        wcx_error_set(err, "parameter \"%s\": decoder passed an invalid value list", name);
        return false;
    }
    const size_t tok = param_token(cfg, name);
    if (tok == WCX_JSON_NONE) {
        *out_index = def_index;
        return true;
    }
    if (cfg->doc.toks[tok].type == WCX_JT_STRING) {
        for (size_t i = 0; i < count; i++) {
            if (wcx_jdoc_string_equals(&cfg->doc, tok, values[i])) {
                *out_index = i;
                return true;
            }
        }
    }
    char got[64] = {0};
    describe(cfg, tok, got, sizeof got);
    char allowed[160] = {0};
    size_t used = 0;
    for (size_t i = 0; i < count && used + 1 < sizeof allowed; i++) {
        const bool fits = wcx_str_format(allowed + used, sizeof allowed - used, "%s\"%s\"",
                                         i == 0 ? "" : ", ", values[i]);
        used += strlen(allowed + used);
        if (!fits) {
            break;
        }
    }
    wcx_error_set(err, "parameter \"%s\" must be one of %s, got %s", name, allowed, got);
    return false;
}
