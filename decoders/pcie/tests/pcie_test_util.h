// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ferrite Engineering LLC
//
// Helpers shared by the PCIe unit tests: a compact symbol notation for the
// pipeline, an event log, a buffered sink for the front ends, and a driver
// that talks to the plugin through its ABI entry points exactly as a host
// does (packed samples, NEED_MORE_SLOTS retries, flush, destroy).

#ifndef PCIE_TEST_UTIL_H
#define PCIE_TEST_UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pcie_fe_common.h" // PCIE_TIMES for "×N" labels
#include "pcie_pipeline.h"
#include "pcie_sink.h"
#include "wavecrux_decoder.h"
#include "wcx/arena.h"
#include "wcx/str.h"
#include "wcx/test.h"

// ── symbols ────────────────────────────────────────────────────────────────

typedef struct tsym {
    uint8_t byte;
    bool k;
} tsym;

#define KS(b) {(uint8_t)(b), true}
#define DS(b) {(uint8_t)(b), false}
#define IDLE  DS(0x00)

// Feeds syms[0..n): symbol i spans [t0 + i*dur, t0 + (i+1)*dur).
static inline void feed_syms(pcie_pipeline *p, const tsym *syms, size_t n, uint64_t t0,
                             uint64_t dur) {
    for (size_t i = 0; i < n; i++) {
        pcie_sym s;
        s.byte = syms[i].byte;
        s.k = syms[i].k;
        s.start_fs = t0 + (uint64_t)i * dur;
        s.end_fs = s.start_fs + dur;
        pcie_pipeline_symbol(p, &s);
    }
}

#define FEED(p, arr, t0, dur) feed_syms((p), (arr), sizeof(arr) / sizeof(arr)[0], (t0), (dur))

// ── event log ──────────────────────────────────────────────────────────────

#define EVLOG_CAP 128

typedef struct evlog {
    pcie_event ev[EVLOG_CAP];
    size_t n;
    size_t dropped;
} evlog;

static inline void evlog_fn(void *ctx, const pcie_event *ev) {
    evlog *l = ctx;
    if (l->n < EVLOG_CAP) {
        l->ev[l->n++] = *ev;
    } else {
        l->dropped++;
    }
}

// ── buffered sink for front-end tests ──────────────────────────────────────

typedef struct test_sink {
    pcie_txbuf buf;
    wcx_arena arena;
    pcie_sink sink;
} test_sink;

static inline bool test_sink_init(test_sink *s, size_t cap) {
    memset(s, 0, sizeof *s);
    if (!pcie_txbuf_init(&s->buf, cap) || !wcx_arena_init(&s->arena, 4096, (size_t)4u << 20u)) {
        return false;
    }
    s->sink.d = NULL;
    s->sink.buf = &s->buf;
    s->sink.buf_arena = &s->arena;
    return true;
}

static inline void test_sink_free(test_sink *s) {
    pcie_txbuf_free(&s->buf);
    wcx_arena_free(&s->arena);
}

// ── the plugin through its ABI ─────────────────────────────────────────────

static inline WcDecoderDef abi_find(wcx_test *t, const char *id) {
    WcDecoderDef defs[8];
    memset(defs, 0, sizeof defs);
    size_t n = 8;
    WCX_CHECK_EQ_I64(wavecrux_decoder_register(defs, &n), WC_DECODER_OK);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(defs[i].id, id) == 0) {
            return defs[i];
        }
    }
    WCX_FAIL("decoder %s not registered", id);
    WcDecoderDef none;
    memset(&none, 0, sizeof none);
    return none;
}

// Which optional signals are bound; the layout is pclk, data, datak, then
// the bound optional ones in manifest order (SPEC.md §2).
typedef struct abi_cfg {
    unsigned lanes;
    bool rxvalid;
    bool txelecidle;
    bool rxstatus;
} abi_cfg;

typedef struct abi_word {
    uint64_t data;
    uint64_t datak;
    uint64_t data_x;  // X/Z mask over data bits
    uint64_t datak_x; // X/Z mask over datak bits
    bool rxvalid;
    bool rxvalid_x;
    bool txelecidle;
    bool txelecidle_x;
    unsigned rxstatus;
    bool rxstatus_x;
} abi_word;

static inline abi_word abi_word_from(const tsym *syms, unsigned lanes) {
    abi_word w;
    memset(&w, 0, sizeof w);
    w.rxvalid = true;
    for (unsigned j = 0; j < lanes; j++) {
        w.data |= (uint64_t)syms[j].byte << (8u * j);
        if (syms[j].k) {
            w.datak |= UINT64_C(1) << j;
        }
    }
    return w;
}

static inline uint32_t abi_width(const abi_cfg *c) {
    return 1u + 9u * c->lanes + (c->rxvalid ? 1u : 0u) + (c->txelecidle ? 1u : 0u) +
           (c->rxstatus ? 3u : 0u);
}

// Two buffer bits per signal bit: level, then unknown.
static inline void abi_put(uint8_t *buf, uint32_t *bit, uint64_t value, uint64_t unknown,
                           unsigned width) {
    for (unsigned i = 0; i < width; i++) {
        const uint32_t pos = (*bit + i) * 2u;
        if (((value >> i) & 1u) != 0) {
            buf[pos / 8u] = (uint8_t)(buf[pos / 8u] | (1u << (pos % 8u)));
        }
        if (((unknown >> i) & 1u) != 0) {
            buf[pos / 8u] = (uint8_t)(buf[pos / 8u] | (2u << (pos % 8u)));
        }
    }
    *bit += width;
}

static inline WcSample abi_pack(uint8_t buf[64], const abi_cfg *c, uint64_t ts, bool pclk,
                                const abi_word *w) {
    memset(buf, 0, 64);
    uint32_t bit = 0;
    abi_put(buf, &bit, pclk ? 1u : 0u, 0, 1);
    abi_put(buf, &bit, w->data, w->data_x, 8u * c->lanes);
    abi_put(buf, &bit, w->datak, w->datak_x, c->lanes);
    if (c->rxvalid) {
        abi_put(buf, &bit, w->rxvalid ? 1u : 0u, w->rxvalid_x ? 1u : 0u, 1);
    }
    if (c->txelecidle) {
        abi_put(buf, &bit, w->txelecidle ? 1u : 0u, w->txelecidle_x ? 1u : 0u, 1);
    }
    if (c->rxstatus) {
        abi_put(buf, &bit, w->rxstatus, w->rxstatus_x ? 7u : 0u, 3);
    }
    WcSample s;
    memset(&s, 0, sizeof s);
    s.timestamp_fs = ts;
    s.bits_ptr = buf;
    s.bit_width = bit;
    return s;
}

// Transactions copied out of the plugin (its strings live only until the
// next call).
typedef struct txrec {
    uint64_t start;
    uint64_t end;
    char *label;
    char *fields;
    bool is_error;
} txrec;

typedef struct txlog {
    txrec *items;
    size_t n;
    size_t cap;
} txlog;

static inline char *dup_str(const char *s) {
    const size_t n = strlen(s) + 1u;
    char *out = malloc(n);
    if (out != NULL) {
        memcpy(out, s, n);
    }
    return out;
}

static inline void txlog_add(txlog *l, const WcTransaction *tx, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (l->n == l->cap) {
            const size_t cap = l->cap == 0 ? 64u : l->cap * 2u;
            txrec *grown = realloc(l->items, cap * sizeof *grown);
            if (grown == NULL) {
                return;
            }
            // Zero the new tail: every slot is initialised, which MSVC's
            // /analyze (C6001) needs to see before txlog_free reads it.
            memset(grown + l->cap, 0, (cap - l->cap) * sizeof *grown);
            l->items = grown;
            l->cap = cap;
        }
        txrec *r = &l->items[l->n++];
        r->start = tx[i].start_fs;
        r->end = tx[i].end_fs;
        r->label = dup_str(tx[i].label != NULL ? tx[i].label : "(null)");
        r->fields = dup_str(tx[i].fields_json != NULL ? tx[i].fields_json : "(null)");
        r->is_error = tx[i].is_error != 0;
    }
}

static inline void txlog_free(txlog *l) {
    for (size_t i = 0; i < l->n; i++) {
        free(l->items[i].label);
        free(l->items[i].fields);
    }
    free(l->items);
    memset(l, 0, sizeof *l);
}

// One feed (sample != NULL) or flush with the host's NEED_MORE_SLOTS retry.
static inline int32_t abi_call(const WcDecoderDef *def, WcDecoderHandle h, const WcSample *s,
                               txlog *l) {
    size_t slots = 16;
    for (int attempt = 0; attempt < 4; attempt++) {
        WcTransaction *out = calloc(slots, sizeof *out);
        if (out == NULL) {
            return WC_DECODER_ERR;
        }
        size_t count = slots;
        const int32_t rc = s != NULL ? def->feed(h, s, out, &count) : def->flush(h, out, &count);
        if (rc == WC_DECODER_OK) {
            txlog_add(l, out, count);
        }
        free(out);
        if (rc != WC_DECODER_NEED_MORE_SLOTS) {
            return rc;
        }
        slots = count;
    }
    return WC_DECODER_ERR;
}

// A zero-delay word stream: the word given to abi_edge() is the one the
// flip-flops capture at that rising edge (it is placed in the sample before
// the edge, as a zero-delay RTL dump does). Edges are `period` apart,
// starting at `t`.
typedef struct abi_drv {
    WcDecoderDef def;
    WcDecoderHandle h;
    abi_cfg cfg;
    uint64_t t;
    uint64_t period;
    txlog log;
    uint8_t buf[64];
} abi_drv;

static inline bool abi_open(abi_drv *d, wcx_test *t, const char *id, const abi_cfg *cfg,
                            const char *params_json, uint64_t first_edge_fs, uint64_t period_fs) {
    memset(d, 0, sizeof *d);
    d->def = abi_find(t, id);
    d->cfg = *cfg;
    d->t = first_edge_fs;
    d->period = period_fs;
    char cfg_json[512] = {0};
    WCX_IGNORE(wcx_str_format(cfg_json, sizeof cfg_json,
                              "{\"decoder_id\":\"%s\",\"signal_bindings\":{\"pclk\":\"0\","
                              "\"data\":\"1\",\"datak\":\"2\"%s%s%s},\"parameters\":%s}",
                              id, cfg->rxvalid ? ",\"rxvalid\":\"3\"" : "",
                              cfg->txelecidle ? ",\"txelecidle\":\"4\"" : "",
                              cfg->rxstatus ? ",\"rxstatus\":\"5\"" : "",
                              params_json != NULL ? params_json : "{}"));
    if (d->def.create == NULL) {
        return false;
    }
    d->h = d->def.create(cfg_json);
    return d->h != NULL;
}

// Feeds the pre-edge sample (pclk low, the word) and the edge (pclk high).
static inline void abi_edge(abi_drv *d, const abi_word *w) {
    WcSample s = abi_pack(d->buf, &d->cfg, d->t - d->period / 2u, false, w);
    (void)abi_call(&d->def, d->h, &s, &d->log);
    s = abi_pack(d->buf, &d->cfg, d->t, true, w);
    (void)abi_call(&d->def, d->h, &s, &d->log);
    d->t += d->period;
}

// Feeds a symbol stream word by word, padding the last word with idle.
static inline void abi_stream(abi_drv *d, const tsym *syms, size_t n) {
    for (size_t i = 0; i < n; i += d->cfg.lanes) {
        tsym word[8];
        for (unsigned j = 0; j < d->cfg.lanes; j++) {
            word[j] = i + j < n ? syms[i + j] : (tsym)IDLE;
        }
        const abi_word w = abi_word_from(word, d->cfg.lanes);
        abi_edge(d, &w);
    }
}

#define ABI_STREAM(d, arr) abi_stream((d), (arr), sizeof(arr) / sizeof(arr)[0])

static inline void abi_flush(abi_drv *d) {
    (void)abi_call(&d->def, d->h, NULL, &d->log);
}

static inline void abi_close(abi_drv *d) {
    if (d->h != NULL) {
        d->def.destroy(d->h);
        d->h = NULL;
    }
    txlog_free(&d->log);
}

// Index of the first logged transaction whose label starts with `prefix`,
// or SIZE_MAX.
static inline size_t txlog_find(const txlog *l, const char *prefix) {
    for (size_t i = 0; i < l->n; i++) {
        if (strncmp(l->items[i].label, prefix, strlen(prefix)) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

#endif // PCIE_TEST_UTIL_H
