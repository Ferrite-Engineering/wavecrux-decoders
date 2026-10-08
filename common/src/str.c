// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC

#include "wcx/str.h"

#include <stdio.h>
#include <string.h>

size_t wcx_str_len(const char *s, size_t max) {
    if (s == NULL) {
        return 0;
    }
    const void *nul = memchr(s, '\0', max);
    if (nul == NULL) {
        return max;
    }
    return (size_t)((const char *)nul - s);
}

bool wcx_str_copy(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0) {
        return false;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return false;
    }
    const size_t len = wcx_str_len(src, cap);
    if (len < cap) {
        memcpy(dst, src, len + 1);
        return true;
    }
    const size_t keep = wcx_utf8_boundary(src, cap - 1);
    memcpy(dst, src, keep);
    dst[keep] = '\0';
    return false;
}

bool wcx_str_vformat(char *dst, size_t cap, const char *fmt, va_list ap) {
    if (dst == NULL || cap == 0 || fmt == NULL) {
        return false;
    }
    const int n = vsnprintf(dst, cap, fmt, ap);
    if (n < 0) {
        dst[0] = '\0';
        return false;
    }
    if ((size_t)n >= cap) {
        const size_t keep = wcx_utf8_boundary(dst, cap - 1);
        dst[keep] = '\0';
        return false;
    }
    return true;
}

bool wcx_str_format(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const bool ok = wcx_str_vformat(dst, cap, fmt, ap);
    va_end(ap);
    return ok;
}

size_t wcx_fmt_u64(char out[WCX_FMT_U64_CAP], uint64_t value) {
    char rev[WCX_FMT_U64_CAP] = {0};
    size_t n = 0;
    do {
        rev[n] = (char)('0' + (int)(value % 10u));
        n++;
        value /= 10u;
    } while (value != 0 && n < WCX_FMT_U64_CAP - 1);
    for (size_t i = 0; i < n; i++) {
        out[i] = rev[n - 1 - i];
    }
    out[n] = '\0';
    return n;
}

size_t wcx_fmt_i64(char out[WCX_FMT_I64_CAP], int64_t value) {
    if (value >= 0) {
        return wcx_fmt_u64(out, (uint64_t)value);
    }
    // Negate in unsigned arithmetic so INT64_MIN does not overflow. The
    // magnitude has at most 19 digits, so "-" + digits + NUL fits in 21.
    const uint64_t magnitude = (uint64_t)0 - (uint64_t)value;
    char digits[WCX_FMT_U64_CAP] = {0};
    const size_t n = wcx_fmt_u64(digits, magnitude);
    out[0] = '-';
    memcpy(out + 1, digits, n + 1u);
    return n + 1u;
}

size_t wcx_fmt_hex(char out[WCX_FMT_HEX_CAP], uint64_t value, unsigned min_digits) {
    static const char digits[] = "0123456789ABCDEF";
    unsigned want = min_digits;
    if (want < 1u) {
        want = 1u;
    }
    if (want > 16u) {
        want = 16u;
    }
    unsigned needed = 1u;
    for (uint64_t v = value >> 4u; v != 0; v >>= 4u) {
        needed++;
    }
    const unsigned count = needed > want ? needed : want;
    out[0] = '0';
    out[1] = 'x';
    for (unsigned i = 0; i < count; i++) {
        const unsigned shift = 4u * (count - 1u - i);
        out[2u + i] = digits[(value >> shift) & 0xFu];
    }
    out[2u + count] = '\0';
    return 2u + (size_t)count;
}

size_t wcx_utf8_seq_len(const unsigned char *s, size_t n) {
    if (s == NULL || n == 0) {
        return 0;
    }
    const unsigned c0 = s[0];
    if (c0 < 0x80u) {
        return 1;
    }
    size_t len = 0;
    unsigned lo = 0x80u; // allowed range of the second byte (Unicode 15 table 3-7)
    unsigned hi = 0xBFu;
    if (c0 >= 0xC2u && c0 <= 0xDFu) {
        len = 2;
    } else if (c0 >= 0xE0u && c0 <= 0xEFu) {
        len = 3;
        if (c0 == 0xE0u) {
            lo = 0xA0u; // no overlong 3-byte forms
        } else if (c0 == 0xEDu) {
            hi = 0x9Fu; // no surrogates
        }
    } else if (c0 >= 0xF0u && c0 <= 0xF4u) {
        len = 4;
        if (c0 == 0xF0u) {
            lo = 0x90u; // no overlong 4-byte forms
        } else if (c0 == 0xF4u) {
            hi = 0x8Fu; // nothing above U+10FFFF
        }
    } else {
        return 0;
    }
    if (n < len || s[1] < lo || s[1] > hi) {
        return 0;
    }
    for (size_t i = 2; i < len; i++) {
        if (s[i] < 0x80u || s[i] > 0xBFu) {
            return 0;
        }
    }
    return len;
}

bool wcx_utf8_valid(const char *s, size_t n) {
    if (s == NULL) {
        return n == 0;
    }
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        const size_t len = wcx_utf8_seq_len(p + i, n - i);
        if (len == 0) {
            return false;
        }
        i += len;
    }
    return true;
}

size_t wcx_utf8_boundary(const char *s, size_t n) {
    if (s == NULL || n == 0) {
        return 0;
    }
    const unsigned char *p = (const unsigned char *)s;
    // Step back over at most three continuation bytes to the lead byte of
    // the last sequence, then keep that sequence only if it is complete.
    size_t lead = n;
    for (size_t back = 0; back < 4 && lead > 0; back++) {
        lead--;
        if ((p[lead] & 0xC0u) != 0x80u) {
            break;
        }
    }
    const unsigned c = p[lead];
    size_t need = 1;
    if (c >= 0xF0u) {
        need = 4;
    } else if (c >= 0xE0u) {
        need = 3;
    } else if (c >= 0xC0u) {
        need = 2;
    }
    return (n - lead >= need) ? n : lead;
}
