// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "vcd.h"

#include <stdlib.h>
#include <string.h>

#include "sb.h"
#include "wcx/buf.h"
#include "wcx/size.h"
#include "wcx/str.h"

#define MAX_SCOPE_DEPTH 256u
#define MAX_VARS        ((size_t)1u << 24u)
#define MAX_WIDTH       ((uint32_t)1u << 20u)
#define MAX_POOL        ((size_t)1u << 34u)
#define MAX_CHANGES     ((size_t)1u << 32u)

typedef struct tok {
    const char *p;
    size_t n;
} tok;

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static bool next_tok(const wcxh_vcd *v, size_t *pos, tok *t) {
    size_t i = *pos;
    while (i < v->len && is_space(v->text[i])) {
        i++;
    }
    if (i >= v->len) {
        *pos = i;
        return false;
    }
    const size_t start = i;
    while (i < v->len && !is_space(v->text[i])) {
        i++;
    }
    t->p = v->text + start;
    t->n = i - start;
    *pos = i;
    return true;
}

static bool tok_is(const tok *t, const char *s) {
    const size_t n = strlen(s);
    return t->n == n && memcmp(t->p, s, n) == 0;
}

static char *tok_dup(const tok *t) {
    char *s = malloc(t->n + 1u);
    if (s != NULL) {
        memcpy(s, t->p, t->n);
        s[t->n] = '\0';
    }
    return s;
}

static bool skip_to_end(const wcxh_vcd *v, size_t *pos, wcx_error *err) {
    tok t;
    while (next_tok(v, pos, &t)) {
        if (tok_is(&t, "$end")) {
            return true;
        }
    }
    wcx_error_set(err, "VCD ends inside a $ section (missing $end)");
    return false;
}

static bool parse_u64(const char *p, size_t n, uint64_t *out) {
    if (n == 0) {
        return false;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
        const uint64_t d = (uint64_t)(p[i] - '0');
        if (v > (UINT64_MAX - d) / 10u) {
            return false;
        }
        v = v * 10u + d;
    }
    *out = v;
    return true;
}

// ── header ──────────────────────────────────────────────────────────────────

static bool parse_timescale(wcxh_vcd *v, size_t *pos, wcx_error *err) {
    char text[64] = {0};
    size_t used = 0;
    tok t;
    while (next_tok(v, pos, &t) && !tok_is(&t, "$end")) {
        if (used + t.n >= sizeof text) {
            wcx_error_set(err, "VCD $timescale is malformed");
            return false;
        }
        memcpy(text + used, t.p, t.n);
        used += t.n;
    }
    size_t digits = 0;
    while (digits < used && text[digits] >= '0' && text[digits] <= '9') {
        digits++;
    }
    uint64_t factor = 0;
    static const struct {
        const char *unit;
        int exponent;
    } units[] = {{"s", 0}, {"ms", -3}, {"us", -6}, {"ns", -9}, {"ps", -12}, {"fs", -15}};
    if (!parse_u64(text, digits, &factor) || factor == 0 || factor > 1000000u) {
        wcx_error_set(err, "VCD $timescale \"%s\" needs a factor such as 1, 10 or 100", text);
        return false;
    }
    for (size_t i = 0; i < sizeof units / sizeof units[0]; i++) {
        if (strcmp(text + digits, units[i].unit) == 0) {
            v->has_timescale = true;
            v->ts_factor = (uint32_t)factor;
            v->ts_exponent = units[i].exponent;
            return true;
        }
    }
    wcx_error_set(err, "VCD $timescale \"%s\" has an unknown unit (use s, ms, us, ns, ps or fs)",
                  text);
    return false;
}

static bool add_var(wcxh_vcd *v, wcxh_vcd_var var, wcx_error *err) {
    if (!wcx_buf_reserve((void **)&v->vars, &v->var_cap, sizeof *v->vars, v->var_count + 1u,
                         MAX_VARS)) {
        wcx_error_set(err, "VCD declares too many variables");
        return false;
    }
    v->vars[v->var_count++] = var;
    return true;
}

static bool is_real_type(const tok *t) {
    return tok_is(t, "real") || tok_is(t, "realtime") || tok_is(t, "real_parameter") ||
           tok_is(t, "shortreal");
}

// $var <type> <size> <id> <reference> [<range>] $end
static bool parse_var(wcxh_vcd *v, size_t *pos, const wcxh_sb *scope, wcx_error *err) {
    tok type;
    tok size;
    tok id;
    tok ref;
    uint64_t width = 0;
    if (!next_tok(v, pos, &type) || !next_tok(v, pos, &size) || !next_tok(v, pos, &id) ||
        !next_tok(v, pos, &ref) || !parse_u64(size.p, size.n, &width) || width == 0 ||
        width > MAX_WIDTH) {
        wcx_error_set(err, "VCD $var near byte %lu is malformed", (unsigned long)*pos);
        return false;
    }
    wcxh_sb range = {0};
    const char *bracket = memchr(ref.p, '[', ref.n);
    tok name = ref;
    if (bracket != NULL) {
        name.n = (size_t)(bracket - ref.p);
        wcxh_sb_put(&range, bracket, ref.n - name.n);
    }
    tok t;
    bool closed = false;
    while (next_tok(v, pos, &t)) {
        if (tok_is(&t, "$end")) {
            closed = true;
            break;
        }
        wcxh_sb_put(&range, t.p, t.n);
    }
    wcxh_sb path = {0};
    if (scope->len > 0) {
        wcxh_sb_put(&path, scope->data, scope->len);
        wcxh_sb_putc(&path, '.');
    }
    wcxh_sb_put(&path, name.p, name.n);
    wcxh_vcd_var var = {0};
    var.path = wcxh_sb_take(&path);
    var.range = range.len > 0 ? wcxh_sb_take(&range) : NULL;
    wcxh_sb_free(&range);
    var.id = tok_dup(&id);
    var.width = (uint32_t)width;
    var.is_real = is_real_type(&type);
    if (!closed || var.path == NULL || var.id == NULL || name.n == 0) {
        free(var.path);
        free(var.range);
        free(var.id);
        wcx_error_set(err, "VCD $var near byte %lu is malformed", (unsigned long)*pos);
        return false;
    }
    if (!add_var(v, var, err)) {
        free(var.path);
        free(var.range);
        free(var.id);
        return false;
    }
    return true;
}

typedef struct id_entry {
    const char *id;
    size_t index;
} id_entry;

static int cmp_id(const void *a, const void *b) {
    const id_entry *x = a;
    const id_entry *y = b;
    const int c = strcmp(x->id, y->id);
    if (c != 0) {
        return c;
    }
    return (x->index > y->index) - (x->index < y->index);
}

// wellen numbers signals by distinct identifier code, in order of first
// declaration.
static bool assign_signal_refs(wcxh_vcd *v) {
    const size_t n = v->var_count;
    if (n == 0) {
        return true;
    }
    id_entry *e = calloc(n, sizeof *e);
    size_t *first = calloc(n, sizeof *first);
    uint32_t *rank = calloc(n, sizeof *rank);
    if (e == NULL || first == NULL || rank == NULL) {
        free(e);
        free(first);
        free(rank);
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        e[i].id = v->vars[i].id;
        e[i].index = i;
    }
    qsort(e, n, sizeof *e, cmp_id);
    size_t group = 0;
    for (size_t i = 0; i < n; i++) {
        if (i == 0 || strcmp(e[i].id, e[i - 1u].id) != 0) {
            group = e[i].index;
        }
        first[e[i].index] = group;
    }
    uint32_t next = 0;
    for (size_t i = 0; i < n; i++) {
        if (first[i] == i) {
            rank[i] = next++;
        }
        v->vars[i].signal_ref = rank[first[i]];
    }
    free(e);
    free(first);
    free(rank);
    return true;
}

static bool push_scope(wcxh_sb *scope, size_t *lens, size_t *depth, const tok *name,
                       wcx_error *err) {
    if (*depth >= MAX_SCOPE_DEPTH) {
        wcx_error_set(err, "VCD scopes nest deeper than %u", MAX_SCOPE_DEPTH);
        return false;
    }
    lens[(*depth)++] = scope->len;
    if (scope->len > 0) {
        wcxh_sb_putc(scope, '.');
    }
    wcxh_sb_put(scope, name->p, name->n);
    return !scope->failed;
}

static bool header_section(wcxh_vcd *v, const tok *t, size_t *pos, wcxh_sb *scope, size_t *lens,
                           size_t *depth, wcx_error *err) {
    if (tok_is(t, "$timescale")) {
        return parse_timescale(v, pos, err);
    }
    if (tok_is(t, "$scope")) {
        tok type;
        tok name;
        if (!next_tok(v, pos, &type) || !next_tok(v, pos, &name) || tok_is(&name, "$end")) {
            wcx_error_set(err, "VCD $scope near byte %lu has no name", (unsigned long)*pos);
            return false;
        }
        return push_scope(scope, lens, depth, &name, err) && skip_to_end(v, pos, err);
    }
    if (tok_is(t, "$upscope")) {
        if (*depth == 0) {
            wcx_error_set(err, "VCD $upscope near byte %lu without a $scope", (unsigned long)*pos);
            return false;
        }
        scope->len = lens[--(*depth)];
        if (scope->data != NULL) {
            scope->data[scope->len] = '\0';
        }
        return skip_to_end(v, pos, err);
    }
    if (tok_is(t, "$var")) {
        return parse_var(v, pos, scope, err);
    }
    if (t->n > 0 && t->p[0] == '$') {
        return skip_to_end(v, pos, err); // $date, $version, $comment, ...
    }
    wcx_error_set(err, "unexpected text in the VCD header near byte %lu", (unsigned long)*pos);
    return false;
}

bool wcxh_vcd_parse_header(wcxh_vcd *v, const char *text, size_t len, wcx_error *err) {
    memset(v, 0, sizeof *v);
    v->text = text;
    v->len = len;
    wcxh_sb scope = {0};
    size_t lens[MAX_SCOPE_DEPTH] = {0};
    size_t depth = 0;
    size_t pos = 0;
    tok t;
    bool ok = false;
    bool ended = false;
    while (next_tok(v, &pos, &t)) {
        if (tok_is(&t, "$enddefinitions")) {
            ended = skip_to_end(v, &pos, err);
            break;
        }
        if (!header_section(v, &t, &pos, &scope, lens, &depth, err)) {
            break;
        }
    }
    if (ended) {
        v->body = pos;
        ok = assign_signal_refs(v);
        if (!ok) {
            wcx_error_set(err, "out of memory reading the VCD header");
        }
    } else if (err != NULL && err->msg[0] == '\0') {
        wcx_error_set(err, "VCD has no $enddefinitions");
    }
    wcxh_sb_free(&scope);
    return ok;
}

size_t wcxh_vcd_find(const wcxh_vcd *v, const char *path, wcx_error *err) {
    size_t found = WCXH_VCD_NONE;
    unsigned matches = 0;
    const char *bracket = strchr(path, '[');
    const size_t base_len = bracket != NULL ? (size_t)(bracket - path) : strlen(path);
    for (size_t i = 0; i < v->var_count; i++) {
        const wcxh_vcd_var *var = &v->vars[i];
        if (strlen(var->path) != base_len || memcmp(var->path, path, base_len) != 0) {
            continue;
        }
        if (bracket != NULL && (var->range == NULL || strcmp(var->range, bracket) != 0)) {
            continue;
        }
        // Several declarations of the same id (aliases) are one signal.
        if (found == WCXH_VCD_NONE || strcmp(v->vars[found].id, var->id) != 0) {
            matches++;
        }
        if (found == WCXH_VCD_NONE) {
            found = i;
        }
    }
    if (found == WCXH_VCD_NONE) {
        wcx_error_set(err, "the VCD has no signal \"%s\"", path);
    } else if (matches > 1) {
        wcx_error_set(err, "\"%s\" names %u signals in the VCD; add the bit range, e.g. \"%s%s\"",
                      path, matches, path,
                      v->vars[found].range != NULL ? v->vars[found].range : "");
        return WCXH_VCD_NONE;
    }
    return found;
}

// ── body ────────────────────────────────────────────────────────────────────

static bool valid_state(char c) {
    return strchr("01xXzZhHlLuUwW-", c) != NULL && c != '\0';
}

// The full-width value wellen stores for `raw` (a scalar "1" or a vector
// "b0101"), written NUL-terminated into the pool.
static bool expand_value(wcxh_vcd *v, const tok *raw, uint32_t width, size_t *off, wcx_error *err) {
    const char *bits = raw->p;
    size_t n = raw->n;
    size_t need = 0;
    if (!wcx_size_add((size_t)width, 1u, &need) ||
        !wcx_buf_reserve((void **)&v->pool, &v->pool_cap, 1, v->pool_len + need, MAX_POOL)) {
        wcx_error_set(err, "VCD values do not fit in memory");
        return false;
    }
    char *out = v->pool + v->pool_len;
    if (width == 1) {
        // wellen: the last character; a bare "b" reads as 0.
        char c = bits[n - 1u];
        if (c == 'b' || c == 'B') {
            c = '0';
        }
        if (!valid_state(c)) {
            wcx_error_set(err, "VCD value \"%.*s\" is not 0, 1, x or z", (int)n, bits);
            return false;
        }
        out[0] = c;
    } else {
        if (n > 0 && (bits[0] == 'b' || bits[0] == 'B')) {
            bits++;
            n--;
        }
        if (n > 2 && bits[0] == '0' && bits[1] == 'b') {
            bits += 2;
            n -= 2;
        }
        for (size_t i = 0; i < n; i++) {
            if (!valid_state(bits[i])) {
                wcx_error_set(err, "VCD vector \"%.*s\" has an invalid digit", (int)raw->n, raw->p);
                return false;
            }
        }
        if (n == 0 || n > width || (n < width && strchr("01xXzZ", bits[0]) == NULL)) {
            wcx_error_set(err, "VCD value \"%.*s\" does not fit a %lu-bit signal", (int)raw->n,
                          raw->p, (unsigned long)width);
            return false;
        }
        const char pad = (bits[0] == '0' || bits[0] == '1') ? '0' : bits[0];
        const size_t padding = (size_t)width - n;
        memset(out, pad, padding);
        memcpy(out + padding, bits, n);
    }
    out[width] = '\0';
    *off = v->pool_len;
    v->pool_len += need;
    return true;
}

static bool record(wcxh_vcd_signal *s, uint64_t time, size_t off, wcx_error *err) {
    if (!wcx_buf_reserve((void **)&s->changes, &s->cap, sizeof *s->changes, s->count + 1u,
                         MAX_CHANGES)) {
        wcx_error_set(err, "VCD has too many changes");
        return false;
    }
    s->changes[s->count].time = time;
    s->changes[s->count].off = off;
    s->count++;
    return true;
}

static bool id_matches(const char *id, const char *p, size_t n) {
    return strlen(id) == n && memcmp(id, p, n) == 0;
}

typedef struct body_state {
    uint64_t time;
    bool skipping;
} body_state;

static bool apply_change(wcxh_vcd *v, const body_state *b, const tok *value, const char *id,
                         size_t id_len, wcx_error *err) {
    if (b->skipping) {
        return true;
    }
    for (size_t i = 0; i < v->sig_count; i++) {
        const wcxh_vcd_var *var = &v->vars[v->sigs[i].var];
        if (!id_matches(var->id, id, id_len)) {
            continue;
        }
        size_t off = 0;
        if (!expand_value(v, value, var->width, &off, err) ||
            !record(&v->sigs[i], b->time, off, err)) {
            return false;
        }
    }
    return true;
}

static bool apply_time(wcxh_vcd *v, body_state *b, const tok *t, wcx_error *err) {
    uint64_t time = 0;
    if (!parse_u64(t->p + 1, t->n - 1u, &time)) {
        wcx_error_set(err, "VCD time \"%.*s\" is not a number", (int)t->n, t->p);
        return false;
    }
    // wellen time_change(): equal is ignored, backwards is skipped.
    if (!v->has_time || time > v->end_time) {
        v->has_time = true;
        v->end_time = time;
        b->time = time;
        b->skipping = false;
    } else if (time < v->end_time) {
        v->backwards_times++;
        b->skipping = true;
    }
    return true;
}

static bool body_keyword(const wcxh_vcd *v, const tok *t, size_t *pos, wcx_error *err) {
    if (tok_is(t, "$dumpvars") || tok_is(t, "$dumpall") || tok_is(t, "$dumpon") ||
        tok_is(t, "$dumpoff") || tok_is(t, "$end")) {
        return true;
    }
    return skip_to_end(v, pos, err); // $comment and anything else
}

static bool body_token(wcxh_vcd *v, body_state *b, const tok *t, size_t *pos, wcx_error *err) {
    const char c = t->p[0];
    if (c == '#') {
        return apply_time(v, b, t, err);
    }
    if (c == '$') {
        return body_keyword(v, t, pos, err);
    }
    if (valid_state(c)) {
        if (t->n < 2) {
            wcx_error_set(err, "VCD value change \"%.*s\" has no identifier", (int)t->n, t->p);
            return false;
        }
        const tok value = {t->p, 1};
        return apply_change(v, b, &value, t->p + 1, t->n - 1u, err);
    }
    tok id;
    if (strchr("bBrRsS", c) == NULL || !next_tok(v, pos, &id)) {
        wcx_error_set(err, "unexpected text in the VCD value changes near byte %lu",
                      (unsigned long)*pos);
        return false;
    }
    if (c == 'b' || c == 'B') {
        return apply_change(v, b, t, id.p, id.n, err);
    }
    for (size_t i = 0; i < v->sig_count; i++) {
        if (id_matches(v->vars[v->sigs[i].var].id, id.p, id.n)) {
            wcx_error_set(err, "bound signal \"%s\" is real- or string-valued",
                          v->vars[v->sigs[i].var].path);
            return false;
        }
    }
    if (c == 'r' || c == 'R') {
        v->real_changes_ignored++;
    } else {
        v->string_changes_ignored++;
    }
    return true;
}

bool wcxh_vcd_parse_body(wcxh_vcd *v, const size_t *vars, size_t n, wcx_error *err) {
    v->sigs = calloc(n > 0 ? n : 1u, sizeof *v->sigs);
    if (v->sigs == NULL) {
        wcx_error_set(err, "out of memory reading the VCD");
        return false;
    }
    v->sig_count = n;
    for (size_t i = 0; i < n; i++) {
        if (vars[i] >= v->var_count) {
            wcx_error_set(err, "internal: bad variable index");
            return false;
        }
        v->sigs[i].var = vars[i];
    }
    body_state b = {0, false};
    size_t pos = v->body;
    tok t;
    while (next_tok(v, &pos, &t)) {
        if (!body_token(v, &b, &t, &pos, err)) {
            return false;
        }
    }
    return true;
}

const char *wcxh_vcd_value_at(const wcxh_vcd *v, size_t sig, uint64_t t, size_t *cursor) {
    if (sig >= v->sig_count) {
        return NULL;
    }
    const wcxh_vcd_signal *s = &v->sigs[sig];
    size_t c = *cursor;
    if (c > s->count || (c > 0 && s->changes[c - 1u].time > t)) {
        c = 0;
    }
    while (c < s->count && s->changes[c].time <= t) {
        c++;
    }
    *cursor = c;
    return c == 0 ? NULL : v->pool + s->changes[c - 1u].off;
}

void wcxh_vcd_free(wcxh_vcd *v) {
    if (v == NULL) {
        return;
    }
    for (size_t i = 0; i < v->var_count; i++) {
        free(v->vars[i].path);
        free(v->vars[i].range);
        free(v->vars[i].id);
    }
    free(v->vars);
    for (size_t i = 0; i < v->sig_count; i++) {
        free(v->sigs[i].changes);
    }
    free(v->sigs);
    free(v->pool);
    memset(v, 0, sizeof *v);
}
