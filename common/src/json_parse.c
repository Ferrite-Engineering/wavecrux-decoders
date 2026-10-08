// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// RFC 8259 tokeniser. One pass, an explicit stack instead of recursion
// (C coding standard §4), and every loop advances `pos` through a buffer of
// known length, so the work is O(len) whatever the input.

#include "wcx/json_parse.h"

#include <stdlib.h>
#include <string.h>

#include "wcx/size.h"
#include "wcx/str.h"

typedef enum parse_state {
    ST_VALUE,          // a value (top level, after ':' or after ',' in an array)
    ST_VALUE_OR_CLOSE, // just after '['
    ST_KEY,            // after ',' in an object
    ST_KEY_OR_CLOSE,   // just after '{'
    ST_COLON,          // after a key
    ST_COMMA_OR_CLOSE, // after a value inside a container
    ST_DONE            // top-level value complete; only whitespace may follow
} parse_state;

typedef struct parser {
    const char *src;
    size_t len;
    size_t pos;
    wcx_jtok *toks; // NULL: count only
    size_t cap;
    size_t count;
    size_t stack[WCX_JSON_MAX_DEPTH];
    wcx_jtype stack_type[WCX_JSON_MAX_DEPTH];
    size_t depth;
    parse_state st;
    wcx_error *err;
} parser;

static bool fail(parser *p, const char *what) {
    wcx_error_set(p->err, "invalid JSON at byte %lu: %s", (unsigned long)p->pos, what);
    return false;
}

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Appends a token; `in_parent` counts it as a child of the enclosing
// container (array elements and object keys; object values are not
// counted, so an object's size is its member count).
static bool add_token(parser *p, wcx_jtype type, size_t start, size_t end, bool in_parent,
                      size_t *idx) {
    if (p->count >= p->cap) {
        return fail(p, "document has too many values");
    }
    *idx = p->count;
    if (p->toks != NULL) {
        wcx_jtok *t = &p->toks[p->count];
        t->type = type;
        t->start = (uint32_t)start;
        t->end = (uint32_t)end;
        t->size = 0;
        t->next = (uint32_t)(p->count + 1);
        if (in_parent && p->depth > 0) {
            p->toks[p->stack[p->depth - 1]].size++;
        }
    }
    p->count++;
    return true;
}

static void after_value(parser *p) {
    p->st = p->depth == 0 ? ST_DONE : ST_COMMA_OR_CLOSE;
}

// On entry src[pos] == '"'. On success pos is just past the closing quote
// and *end is the index of that quote.
static bool scan_string(parser *p, size_t *end) {
    size_t i = p->pos + 1;
    while (i < p->len) {
        const unsigned char c = (unsigned char)p->src[i];
        if (c == '"') {
            *end = i;
            p->pos = i + 1;
            return true;
        }
        if (c < 0x20u) {
            p->pos = i;
            return fail(p, "control character in string");
        }
        if (c == '\\') {
            if (i + 1 >= p->len) {
                break;
            }
            const char e = p->src[i + 1];
            if (e == 'u') {
                for (size_t k = 2; k < 6; k++) {
                    if (i + k >= p->len || hex_value(p->src[i + k]) < 0) {
                        p->pos = i;
                        return fail(p, "\\u escape needs four hex digits");
                    }
                }
                i += 6;
            } else if (strchr("\"\\/bfnrt", e) != NULL && e != '\0') {
                i += 2;
            } else {
                p->pos = i;
                return fail(p, "unknown escape in string");
            }
            continue;
        }
        const size_t n = wcx_utf8_seq_len((const unsigned char *)p->src + i, p->len - i);
        if (n == 0) {
            p->pos = i;
            return fail(p, "string is not valid UTF-8");
        }
        i += n;
    }
    p->pos = p->len;
    return fail(p, "unterminated string");
}

static size_t skip_digits(const parser *p, size_t i) {
    while (i < p->len && is_digit(p->src[i])) {
        i++;
    }
    return i;
}

// -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
static bool scan_number(parser *p, size_t *end) {
    size_t i = p->pos;
    if (p->src[i] == '-') {
        i++;
    }
    if (i >= p->len || !is_digit(p->src[i])) {
        return fail(p, "number needs a digit");
    }
    i = (p->src[i] == '0') ? i + 1 : skip_digits(p, i);
    if (i < p->len && p->src[i] == '.') {
        const size_t frac = skip_digits(p, i + 1);
        if (frac == i + 1) {
            return fail(p, "number needs a digit after '.'");
        }
        i = frac;
    }
    if (i < p->len && (p->src[i] == 'e' || p->src[i] == 'E')) {
        i++;
        if (i < p->len && (p->src[i] == '+' || p->src[i] == '-')) {
            i++;
        }
        const size_t exp = skip_digits(p, i);
        if (exp == i) {
            return fail(p, "number needs a digit in the exponent");
        }
        i = exp;
    }
    *end = i;
    p->pos = i;
    return true;
}

static bool scan_literal(parser *p, const char *word, size_t *end) {
    const size_t n = strlen(word);
    if (p->len - p->pos < n || memcmp(p->src + p->pos, word, n) != 0) {
        return fail(p, "unexpected character");
    }
    *end = p->pos + n;
    p->pos += n;
    return true;
}

static bool open_container(parser *p, wcx_jtype type, bool in_parent) {
    if (p->depth >= WCX_JSON_MAX_DEPTH) {
        return fail(p, "nesting is deeper than 64 levels");
    }
    size_t idx = 0;
    if (!add_token(p, type, p->pos, p->pos + 1, in_parent, &idx)) {
        return false;
    }
    p->stack[p->depth] = idx;
    p->stack_type[p->depth] = type;
    p->depth++;
    p->pos++;
    p->st = type == WCX_JT_OBJECT ? ST_KEY_OR_CLOSE : ST_VALUE_OR_CLOSE;
    return true;
}

static bool close_container(parser *p, char c) {
    const wcx_jtype want = c == '}' ? WCX_JT_OBJECT : WCX_JT_ARRAY;
    if (p->depth == 0 || p->stack_type[p->depth - 1] != want) {
        return fail(p, "mismatched bracket");
    }
    p->depth--;
    if (p->toks != NULL) {
        wcx_jtok *t = &p->toks[p->stack[p->depth]];
        t->end = (uint32_t)(p->pos + 1);
        t->next = (uint32_t)p->count;
    }
    p->pos++;
    after_value(p);
    return true;
}

static bool parse_scalar(parser *p, bool in_parent) {
    const size_t start = p->pos;
    size_t end = 0;
    size_t idx = 0;
    const char c = p->src[p->pos];
    if (c == '"') {
        return scan_string(p, &end) && add_token(p, WCX_JT_STRING, start + 1, end, in_parent, &idx);
    }
    if (c == '-' || is_digit(c)) {
        return scan_number(p, &end) && add_token(p, WCX_JT_NUMBER, start, end, in_parent, &idx);
    }
    if (c == 't') {
        return scan_literal(p, "true", &end) &&
               add_token(p, WCX_JT_TRUE, start, end, in_parent, &idx);
    }
    if (c == 'f') {
        return scan_literal(p, "false", &end) &&
               add_token(p, WCX_JT_FALSE, start, end, in_parent, &idx);
    }
    if (c == 'n') {
        return scan_literal(p, "null", &end) &&
               add_token(p, WCX_JT_NULL, start, end, in_parent, &idx);
    }
    return fail(p, "expected a value");
}

static bool parse_value(parser *p) {
    const bool in_parent = p->depth > 0 && p->stack_type[p->depth - 1] == WCX_JT_ARRAY;
    const char c = p->src[p->pos];
    if (c == '{') {
        return open_container(p, WCX_JT_OBJECT, in_parent);
    }
    if (c == '[') {
        return open_container(p, WCX_JT_ARRAY, in_parent);
    }
    if (!parse_scalar(p, in_parent)) {
        return false;
    }
    after_value(p);
    return true;
}

static bool parse_key(parser *p) {
    if (p->src[p->pos] != '"') {
        return fail(p, "expected a string key");
    }
    const size_t start = p->pos;
    size_t end = 0;
    size_t idx = 0;
    if (!scan_string(p, &end) || !add_token(p, WCX_JT_STRING, start + 1, end, true, &idx)) {
        return false;
    }
    p->st = ST_COLON;
    return true;
}

static bool step(parser *p) {
    const char c = p->src[p->pos];
    switch (p->st) {
        case ST_VALUE:
            return parse_value(p);
        case ST_VALUE_OR_CLOSE:
            return c == ']' ? close_container(p, c) : parse_value(p);
        case ST_KEY:
            return parse_key(p);
        case ST_KEY_OR_CLOSE:
            return c == '}' ? close_container(p, c) : parse_key(p);
        case ST_COLON:
            if (c != ':') {
                return fail(p, "expected ':'");
            }
            p->pos++;
            p->st = ST_VALUE;
            return true;
        case ST_COMMA_OR_CLOSE:
            if (c == ',') {
                p->pos++;
                p->st = p->stack_type[p->depth - 1] == WCX_JT_OBJECT ? ST_KEY : ST_VALUE;
                return true;
            }
            if (c == '}' || c == ']') {
                return close_container(p, c);
            }
            return fail(p, "expected ',' or a closing bracket");
        case ST_DONE:
            return fail(p, "unexpected characters after the document");
        default:
            return fail(p, "internal parser state");
    }
}

bool wcx_json_tokenize(const char *src, size_t len, wcx_jtok *toks, size_t cap, size_t *count,
                       wcx_error *err) {
    parser p;
    memset(&p, 0, sizeof p);
    p.src = src;
    p.len = len;
    p.toks = toks;
    p.cap = cap;
    p.st = ST_VALUE;
    p.err = err;
    if (count != NULL) {
        *count = 0;
    }
    if (src == NULL || count == NULL || len >= UINT32_MAX) {
        return fail(&p, "no document, or larger than 4 GiB");
    }
    // Each iteration consumes at least one byte or fails, so this runs at
    // most len times.
    while (p.pos < p.len) {
        if (is_ws(p.src[p.pos])) {
            p.pos++;
            continue;
        }
        if (!step(&p)) {
            return false;
        }
    }
    if (p.st != ST_DONE) {
        return fail(&p, "unexpected end of document");
    }
    *count = p.count;
    return true;
}

bool wcx_jdoc_parse(wcx_jdoc *doc, const char *src, size_t len, size_t max_tokens, wcx_error *err) {
    if (doc == NULL) {
        return false;
    }
    memset(doc, 0, sizeof *doc);
    size_t count = 0;
    if (!wcx_json_tokenize(src, len, NULL, max_tokens, &count, err)) {
        return false;
    }
    size_t bytes = 0;
    // A valid document has at least one value, so count >= 1 here.
    if (count == 0 || !wcx_size_mul(count, sizeof(wcx_jtok), &bytes)) {
        wcx_error_set(err, "JSON document is too large");
        return false;
    }
    doc->toks = calloc(1, bytes);
    if (doc->toks == NULL) {
        wcx_error_set(err, "out of memory parsing JSON");
        return false;
    }
    if (!wcx_json_tokenize(src, len, doc->toks, count, &doc->count, err)) {
        wcx_jdoc_free(doc);
        return false;
    }
    doc->src = src;
    doc->len = len;
    return true;
}

void wcx_jdoc_free(wcx_jdoc *doc) {
    if (doc == NULL) {
        return;
    }
    free(doc->toks);
    memset(doc, 0, sizeof *doc);
}

wcx_jtype wcx_jdoc_type(const wcx_jdoc *doc, size_t tok) {
    if (doc == NULL || tok >= doc->count) {
        return WCX_JT_NONE;
    }
    return doc->toks[tok].type;
}

// ── string decoding ─────────────────────────────────────────────────────────

static unsigned read_hex4(const char *s) {
    unsigned v = 0;
    for (int k = 0; k < 4; k++) {
        v = (v << 4u) | (unsigned)hex_value(s[k]);
    }
    return v;
}

static size_t encode_utf8(unsigned cp, unsigned char out[4]) {
    if (cp < 0x80u) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (unsigned char)(0xC0u | (cp >> 6u));
        out[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (unsigned char)(0xE0u | (cp >> 12u));
        out[1] = (unsigned char)(0x80u | ((cp >> 6u) & 0x3Fu));
        out[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (unsigned char)(0xF0u | (cp >> 18u));
    out[1] = (unsigned char)(0x80u | ((cp >> 12u) & 0x3Fu));
    out[2] = (unsigned char)(0x80u | ((cp >> 6u) & 0x3Fu));
    out[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
    return 4;
}

// Decodes the next unit of a validated string body at s[*i] (< end) into
// out, returning its byte count and advancing *i.
static size_t decode_next(const char *s, size_t *i, size_t end, unsigned char out[4]) {
    if (s[*i] != '\\') {
        out[0] = (unsigned char)s[*i];
        (*i)++;
        return 1;
    }
    const char e = s[*i + 1];
    if (e != 'u') {
        static const char from[] = "\"\\/bfnrt";
        static const char to[] = "\"\\/\b\f\n\r\t";
        const char *at = strchr(from, e);
        out[0] = (unsigned char)to[at - from];
        *i += 2;
        return 1;
    }
    unsigned cp = read_hex4(s + *i + 2);
    *i += 6;
    if (cp >= 0xD800u && cp <= 0xDBFFu) {
        if (*i + 6 <= end && s[*i] == '\\' && s[*i + 1] == 'u') {
            const unsigned lo = read_hex4(s + *i + 2);
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                *i += 6;
                return encode_utf8(0x10000u + ((cp - 0xD800u) << 10u) + (lo - 0xDC00u), out);
            }
        }
        cp = 0xFFFDu;
    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
        cp = 0xFFFDu;
    }
    return encode_utf8(cp, out);
}

static bool is_string(const wcx_jdoc *doc, size_t tok) {
    return doc != NULL && tok < doc->count && doc->toks[tok].type == WCX_JT_STRING;
}

bool wcx_jdoc_string_equals(const wcx_jdoc *doc, size_t tok, const char *s) {
    if (!is_string(doc, tok) || s == NULL) {
        return false;
    }
    const wcx_jtok *t = &doc->toks[tok];
    const size_t want = strlen(s);
    size_t i = t->start;
    size_t matched = 0;
    while (i < t->end) {
        unsigned char unit[4] = {0};
        const size_t n = decode_next(doc->src, &i, t->end, unit);
        if (matched + n > want || memcmp(s + matched, unit, n) != 0) {
            return false;
        }
        matched += n;
    }
    return matched == want;
}

size_t wcx_jdoc_string_len(const wcx_jdoc *doc, size_t tok) {
    if (!is_string(doc, tok)) {
        return SIZE_MAX;
    }
    const wcx_jtok *t = &doc->toks[tok];
    size_t i = t->start;
    size_t total = 0;
    while (i < t->end) {
        unsigned char unit[4] = {0};
        total += decode_next(doc->src, &i, t->end, unit);
    }
    return total;
}

bool wcx_jdoc_string(const wcx_jdoc *doc, size_t tok, char *buf, size_t cap, size_t *out_len) {
    if (!is_string(doc, tok) || buf == NULL || cap == 0) {
        return false;
    }
    const wcx_jtok *t = &doc->toks[tok];
    size_t i = t->start;
    size_t w = 0;
    while (i < t->end) {
        unsigned char unit[4] = {0};
        const size_t n = decode_next(doc->src, &i, t->end, unit);
        if (w + n >= cap) {
            buf[0] = '\0';
            return false;
        }
        memcpy(buf + w, unit, n);
        w += n;
    }
    buf[w] = '\0';
    if (out_len != NULL) {
        *out_len = w;
    }
    return true;
}

// ── numbers ─────────────────────────────────────────────────────────────────

bool wcx_jdoc_is_integer(const wcx_jdoc *doc, size_t tok) {
    if (doc == NULL || tok >= doc->count || doc->toks[tok].type != WCX_JT_NUMBER) {
        return false;
    }
    const wcx_jtok *t = &doc->toks[tok];
    for (size_t i = t->start; i < t->end; i++) {
        const char c = doc->src[i];
        if (c == '.' || c == 'e' || c == 'E') {
            return false;
        }
    }
    return true;
}

// Magnitude of an integer literal; false when it exceeds UINT64_MAX.
static bool integer_magnitude(const wcx_jdoc *doc, size_t tok, bool *negative, uint64_t *mag) {
    if (!wcx_jdoc_is_integer(doc, tok)) {
        return false;
    }
    const wcx_jtok *t = &doc->toks[tok];
    size_t i = t->start;
    *negative = doc->src[i] == '-';
    if (*negative) {
        i++;
    }
    uint64_t v = 0;
    for (; i < t->end; i++) {
        const uint64_t digit = (uint64_t)(doc->src[i] - '0');
        if (v > (UINT64_MAX - digit) / 10u) {
            return false;
        }
        v = v * 10u + digit;
    }
    *mag = v;
    return true;
}

bool wcx_jdoc_int64(const wcx_jdoc *doc, size_t tok, int64_t *out) {
    bool negative = false;
    uint64_t mag = 0;
    if (out == NULL || !integer_magnitude(doc, tok, &negative, &mag)) {
        return false;
    }
    if (negative) {
        if (mag > (uint64_t)INT64_MAX + 1u) {
            return false;
        }
        *out = mag == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)mag;
        return true;
    }
    if (mag > (uint64_t)INT64_MAX) {
        return false;
    }
    *out = (int64_t)mag;
    return true;
}

bool wcx_jdoc_uint64(const wcx_jdoc *doc, size_t tok, uint64_t *out) {
    bool negative = false;
    uint64_t mag = 0;
    if (out == NULL || !integer_magnitude(doc, tok, &negative, &mag)) {
        return false;
    }
    if (negative && mag != 0) {
        return false;
    }
    *out = mag;
    return true;
}

const char *wcx_jdoc_raw(const wcx_jdoc *doc, size_t tok, size_t *len) {
    if (doc == NULL || tok >= doc->count) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    const wcx_jtok *t = &doc->toks[tok];
    if (len != NULL) {
        *len = t->end - t->start;
    }
    return doc->src + t->start;
}

size_t wcx_jdoc_member(const wcx_jdoc *doc, size_t obj, const char *key) {
    if (doc == NULL || obj >= doc->count || doc->toks[obj].type != WCX_JT_OBJECT) {
        return WCX_JSON_NONE;
    }
    size_t found = WCX_JSON_NONE;
    size_t k = obj + 1;
    for (uint32_t i = 0; i < doc->toks[obj].size; i++) {
        const size_t value = k + 1;
        if (wcx_jdoc_string_equals(doc, k, key)) {
            found = value;
        }
        k = doc->toks[value].next;
    }
    return found;
}
