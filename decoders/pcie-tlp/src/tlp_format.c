// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Chili.CHIPS
//
// TLP label and fields (SPEC.md §5). Field layout per PCIe Base Spec
// §2.2.1 (common header), §2.2.7 (memory, I/O, configuration requests),
// §2.2.8 (messages) and §2.2.9 (completions).

#include "tlp_format.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wcx/arena.h"
#include "wcx/json_writer.h"

typedef struct tlp_ctx {
    wcx_arena *a;
    wcx_json *j;
    const uint32_t *dw;
    const tlp_match *m;
    uint32_t n;
    uint32_t fmt;
    uint32_t type;
    uint32_t len;
    uint32_t hdr;
    uint32_t data; // first payload DW, 0 when there is none
    bool has_data;
    bool full;
    bool failed; // an arena allocation failed
    const char *name;
    const char *label;
    const char *error; // UR / CA completion
} tlp_ctx;

static uint32_t bswap32(uint32_t v) {
    return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
}

// Length in DWs: 0 encodes 1024 (§2.2.2).
static uint32_t length_n(uint32_t len) {
    return len != 0u ? len : 1024u;
}

// An arena string, or "" (and the context marked failed) when the arena ran
// out, so that no NULL ever reaches a %s.
static const char *ok(tlp_ctx *c, const char *s) {
    if (s == NULL) {
        c->failed = true;
        return "";
    }
    return s;
}

tlp_kind tlp_kind_of(uint32_t dw0) {
    const uint32_t fmt = (dw0 >> 29) & 0x7u;
    const uint32_t type = (dw0 >> 24) & 0x1Fu;
    const bool four = (fmt & 0x1u) != 0u;
    const bool data = (fmt & 0x2u) != 0u;
    if ((fmt & 0x4u) != 0u) {
        return TLP_KIND_OTHER; // a TLP prefix (§2.2.1)
    }
    if ((type == 0x04u || type == 0x05u) && !four) {
        return TLP_KIND_CFG;
    }
    if (type == 0x00u || (type == 0x01u && !data) || (type == 0x02u && !four)) {
        return TLP_KIND_MEM; // no MWrLk; I/O has a 3 DW header
    }
    if ((type == 0x0Au || type == 0x0Bu) && !four) {
        return TLP_KIND_CPL;
    }
    if ((type & 0x18u) == 0x10u && four) {
        return TLP_KIND_MSG;
    }
    return TLP_KIND_OTHER;
}

uint32_t tlp_header_dw(uint32_t dw0) {
    const uint32_t fmt = (dw0 >> 29) & 0x7u;
    if ((fmt & 0x4u) != 0u) {
        return 1u; // a prefix is one DW
    }
    return (fmt & 0x1u) != 0u ? 4u : 3u;
}

uint32_t tlp_expected_dw(uint32_t dw0) {
    const uint32_t fmt = (dw0 >> 29) & 0x7u;
    if ((fmt & 0x4u) != 0u) {
        return 0u;
    }
    const uint32_t data = (fmt & 0x2u) != 0u ? length_n(dw0 & 0x3FFu) : 0u;
    const uint32_t ecrc = (dw0 >> 15) & 0x1u; // TD: a TLP Digest follows (§2.2.3)
    return tlp_header_dw(dw0) + data + ecrc;
}

uint32_t tlp_byte_count(uint32_t dw1) {
    const uint32_t bc = dw1 & 0xFFFu;
    return bc != 0u ? bc : 4096u;
}

uint32_t tlp_tag(uint32_t dw0, uint32_t tag8) {
    const uint32_t t9 = (dw0 >> 23) & 0x1u;
    const uint32_t t8 = (dw0 >> 19) & 0x1u;
    return (t9 << 9) | (t8 << 8) | (tag8 & 0xFFu);
}

const char *tlp_reg_name(wcx_arena *a, uint32_t reg) {
    static const char *const k_names[] = {
        "ID",   "Command", "Class/Rev", "Hdr/Lat/CLS", "BAR0",   "BAR1",   "BAR2", "BAR3",
        "BAR4", "BAR5",    "CardBus",   "Subsystem",   "ExpROM", "CapPtr", NULL,   "Int",
    };
    const uint32_t idx = reg / 4u;
    if ((reg & 3u) == 0u && idx < sizeof k_names / sizeof k_names[0] && k_names[idx] != NULL) {
        return k_names[idx];
    }
    return wcx_arena_printf(a, "reg 0x%03X", reg);
}

const char *tlp_status_name(uint32_t status) {
    static const char *const k_status[] = {"SC", "UR", "CRS", "rsvd", "CA", "rsvd", "rsvd", "rsvd"};
    return k_status[status & 7u];
}

static const char *bdf(tlp_ctx *c, uint32_t id) {
    return ok(
        c, wcx_arena_printf(c->a, "%02X:%02X.%u", (id >> 8) & 0xFFu, (id >> 3) & 0x1Fu, id & 7u));
}

static const char *tlp_name(tlp_ctx *c) {
    const uint32_t type = c->type;
    switch (tlp_kind_of(c->dw[0])) {
        case TLP_KIND_CFG:
            return ok(c, wcx_arena_printf(c->a, "Cfg%s%u", c->has_data ? "Wr" : "Rd", type & 1u));
        case TLP_KIND_MEM: {
            const bool io = type == 0x02u;
            const char *size = c->hdr == 4u ? "64" : "32";
            return ok(c,
                      wcx_arena_printf(c->a, "%s%s%s%s", io ? "IO" : "M", c->has_data ? "Wr" : "Rd",
                                       type == 0x01u ? "Lk" : "", io ? "" : size));
        }
        case TLP_KIND_CPL:
            if (type == 0x0Bu) {
                return c->has_data ? "CplDLk" : "CplLk";
            }
            return c->has_data ? "CplD" : "Cpl";
        case TLP_KIND_MSG:
            return c->has_data ? "MsgD" : "Msg";
        case TLP_KIND_OTHER:
            break;
    }
    return "TLP";
}

// ── configuration requests (§5.1) ───────────────────────────────────────────

static void fmt_cfg(tlp_ctx *c) {
    const uint32_t dw1 = c->dw[1];
    const uint32_t dw2 = c->dw[2];
    const uint32_t tag = tlp_tag(c->dw[0], dw1 >> 8);
    const uint32_t bus = dw2 >> 24;
    const uint32_t dev = (dw2 >> 19) & 0x1Fu;
    const uint32_t fn = (dw2 >> 16) & 0x7u;
    const uint32_t reg = dw2 & 0xFFCu;
    const bool type1 = (c->type & 1u) != 0u;
    const char *rname = ok(c, tlp_reg_name(c->a, reg));
    const uint32_t value = bswap32(c->data);

    if (c->full) {
        c->label = c->has_data ? wcx_arena_printf(c->a, "%s %02X:%02X.%u %s = 0x%08X  tag %02X",
                                                  c->name, bus, dev, fn, rname, value, tag)
                               : wcx_arena_printf(c->a, "%s %02X:%02X.%u %s  tag %02X", c->name,
                                                  bus, dev, fn, rname, tag);
    } else {
        const bool show_bdf = bus != 1u || dev != 0u || fn != 0u || type1;
        const char *where =
            show_bdf ? ok(c, wcx_arena_printf(c->a, " %02X:%02X.%u", bus, dev, fn)) : "";
        c->label = c->has_data
                       ? wcx_arena_printf(c->a, "CfgWr%s%s %s = 0x%08X", type1 ? "1" : "", where,
                                          rname, value)
                       : wcx_arena_printf(c->a, "CfgRd%s%s %s", type1 ? "1" : "", where, rname);
    }
    wcx_json_add_u64(c->j, "bus", bus);
    wcx_json_add_u64(c->j, "device", dev);
    wcx_json_add_u64(c->j, "function", fn);
    wcx_json_add_hex(c->j, "register", reg, 3);
    wcx_json_add_str(c->j, "register_name", rname);
    wcx_json_add_str(c->j, "requester", bdf(c, dw1 >> 16));
    wcx_json_add_u64(c->j, "tag", tag);
    wcx_json_add_hex(c->j, "first_be", dw1 & 0xFu, 1);
    if (c->has_data) {
        wcx_json_add_hex(c->j, "value", value, 8);
        wcx_json_add_hex(c->j, "data", c->data, 8);
    }
}

// ── memory and I/O requests (§5.2) ──────────────────────────────────────────

static void fmt_mem(tlp_ctx *c) {
    const uint32_t dw1 = c->dw[1];
    const uint32_t tag = tlp_tag(c->dw[0], dw1 >> 8);
    const bool four = c->hdr == 4u;
    const uint32_t high = four ? c->dw[2] : 0u;
    const uint32_t low = (four ? c->dw[3] : c->dw[2]) & ~3u;
    const uint32_t n = length_n(c->len);
    const char *addr = ok(c, high != 0u ? wcx_arena_printf(c->a, "0x%08X_%08X", high, low)
                                        : wcx_arena_printf(c->a, "0x%08X", low));

    if (c->has_data) {
        const char *more = "";
        if (n > 1u) {
            more = c->full ? ok(c, wcx_arena_printf(c->a, " +%u DW", n - 1u)) : " ...";
        }
        c->label = wcx_arena_printf(c->a, "%s %s = 0x%08X%s", c->name, addr, c->data, more);
    } else if (c->full) {
        c->label = wcx_arena_printf(c->a, "%s %s  %u DW  tag %02X", c->name, addr, n, tag);
    } else {
        c->label = n != 1u ? wcx_arena_printf(c->a, "%s %s x%u DW", c->name, addr, n)
                           : wcx_arena_printf(c->a, "%s %s", c->name, addr);
    }
    wcx_json_add_hex(c->j, "address", four ? (((uint64_t)high << 32) | low) : low, four ? 16 : 8);
    wcx_json_add_u64(c->j, "length_dw", n);
    wcx_json_add_str(c->j, "requester", bdf(c, dw1 >> 16));
    wcx_json_add_u64(c->j, "tag", tag);
    wcx_json_add_hex(c->j, "first_be", dw1 & 0xFu, 1);
    wcx_json_add_hex(c->j, "last_be", (dw1 >> 4) & 0xFu, 1);
    if (c->has_data) {
        wcx_json_add_hex(c->j, "data", c->data, 8);
    }
}

// ── completions (§5.3) ──────────────────────────────────────────────────────

// `<what>` and `<c>` of §5.3, or NULL for no match.
static const char *cpl_what(tlp_ctx *c, bool compact) {
    const tlp_match *m = c->m;
    switch (m->kind) {
        case TLP_MATCH_CFG_READ:
            return ok(c, tlp_reg_name(c->a, m->reg));
        case TLP_MATCH_CFG_WRITE: {
            const char *reg = ok(c, tlp_reg_name(c->a, m->reg));
            return compact ? reg : ok(c, wcx_arena_printf(c->a, "for write %s", reg));
        }
        case TLP_MATCH_MEM:
            return ok(c, wcx_arena_printf(c->a, compact ? "0x%08X" : "MRd 0x%08X", m->addr));
        case TLP_MATCH_IO_READ:
            return ok(c, wcx_arena_printf(c->a, compact ? "0x%08X" : "IORd 0x%08X", m->addr));
        case TLP_MATCH_IO_WRITE:
            return ok(c, wcx_arena_printf(c->a, compact ? "0x%08X" : "IOWr 0x%08X", m->addr));
        case TLP_MATCH_NONE:
            break;
    }
    return NULL;
}

static const char *cpl_label(tlp_ctx *c, const char *status, uint32_t shown, uint32_t tag) {
    const char *what = cpl_what(c, !c->full);
    const char *sp = what != NULL ? " " : "";
    const char *w = what != NULL ? what : "";
    if (c->full) {
        return c->has_data
                   ? wcx_arena_printf(c->a, "%s %s%s%s = 0x%08X  tag %02X", c->name, status, sp, w,
                                      shown, tag)
                   : wcx_arena_printf(c->a, "%s %s%s%s  tag %02X", c->name, status, sp, w, tag);
    }
    const bool sc = status[0] == 'S' && status[1] == 'C';
    const char *st = sc ? "" : ok(c, wcx_arena_printf(c->a, " %s", status));
    return c->has_data ? wcx_arena_printf(c->a, "CplD%s%s%s = 0x%08X", st, sp, w, shown)
                       : wcx_arena_printf(c->a, "Cpl%s%s%s", st, sp, w);
}

static void fmt_cpl(tlp_ctx *c) {
    const uint32_t dw1 = c->dw[1];
    const uint32_t dw2 = c->dw[2];
    const uint32_t status = (dw1 >> 13) & 0x7u;
    const uint32_t tag = tlp_tag(c->dw[0], dw2 >> 8);
    const bool cfg = c->m->kind == TLP_MATCH_CFG_READ || c->m->kind == TLP_MATCH_CFG_WRITE;
    const uint32_t shown = cfg ? bswap32(c->data) : c->data;
    const char *sname = tlp_status_name(status);

    c->label = cpl_label(c, sname, shown, tag);
    wcx_json_add_str(c->j, "status", sname);
    wcx_json_add_str(c->j, "completer", bdf(c, dw1 >> 16));
    wcx_json_add_str(c->j, "requester", bdf(c, dw2 >> 16));
    wcx_json_add_u64(c->j, "tag", tag);
    wcx_json_add_u64(c->j, "byte_count", tlp_byte_count(dw1));
    wcx_json_add_hex(c->j, "lower_address", dw2 & 0x7Fu, 2);
    if (c->has_data) {
        wcx_json_add_hex(c->j, "data", c->data, 8);
        if (cfg) {
            wcx_json_add_hex(c->j, "value", shown, 8);
        }
    }
    if (c->m->kind != TLP_MATCH_NONE) {
        const char *answers = cpl_what(c, false);
        wcx_json_add_str(c->j, "answers", answers != NULL ? answers : "");
    }
    if (status == 1u) {
        c->error = "Unsupported Request";
    } else if (status == 4u) {
        c->error = "Completer Abort";
    }
}

// ── messages and the rest (§5.4, §5.5) ──────────────────────────────────────

static void fmt_msg(tlp_ctx *c) {
    const uint32_t code = c->dw[1] & 0xFFu;
    const uint32_t route = c->type & 0x7u;
    c->label = wcx_arena_printf(c->a, "%s code 0x%02X  route %u", c->name, code, route);
    wcx_json_add_hex(c->j, "message_code", code, 2);
    wcx_json_add_u64(c->j, "routing", route);
}

static void fmt_other(tlp_ctx *c) {
    c->label = wcx_arena_printf(c->a, "TLP Fmt %u Type 0x%02X", c->fmt, c->type);
}

// ── the whole TLP ───────────────────────────────────────────────────────────

static void add_common(tlp_ctx *c) {
    wcx_json_add_str(c->j, "type", c->name);
    wcx_json_add_u64(c->j, "fmt", c->fmt);
    wcx_json_add_hex(c->j, "type_code", c->type, 2);
    wcx_json_add_u64(c->j, "length", c->len);
    wcx_json_add_u64(c->j, "dw_count", c->n);
    const char *header =
        c->hdr == 4u
            ? wcx_arena_printf(c->a, "%08X %08X %08X %08X", c->dw[0], c->dw[1], c->dw[2], c->dw[3])
        : c->hdr == 3u ? wcx_arena_printf(c->a, "%08X %08X %08X", c->dw[0], c->dw[1], c->dw[2])
                       : wcx_arena_printf(c->a, "%08X", c->dw[0]); // a prefix
    wcx_json_add_str(c->j, "header", ok(c, header));
}

// The errors of §5.6, after the per-kind fields.
static void add_errors(tlp_ctx *c, tlp_text *out) {
    const uint32_t exp = tlp_expected_dw(c->dw[0]);
    if (exp != 0u && exp != c->n) {
        c->label = wcx_arena_printf(c->a, "%s  [%u DW, expected %u]", c->label, c->n, exp);
        wcx_json_add_str(c->j, "error", "length does not match the Length field");
        wcx_json_add_u64(c->j, "expected", exp);
        wcx_json_add_u64(c->j, "actual", c->n);
        out->is_error = true;
    } else if (c->error != NULL) {
        wcx_json_add_str(c->j, "error", c->error);
        out->is_error = true;
    }
}

void tlp_format(wcx_arena *a, const uint32_t *dw, uint32_t n, const tlp_match *m, bool full,
                tlp_text *out) {
    static const tlp_match k_none = {TLP_MATCH_NONE, 0u, 0u};
    wcx_json j;
    tlp_ctx c = {0};
    c.a = a;
    c.j = &j;
    c.dw = dw;
    c.m = m != NULL ? m : &k_none;
    c.n = n;
    c.fmt = (dw[0] >> 29) & 0x7u;
    c.type = (dw[0] >> 24) & 0x1Fu;
    c.len = dw[0] & 0x3FFu;
    c.hdr = tlp_header_dw(dw[0]);
    c.has_data = (c.fmt & 0x6u) == 0x2u; // a prefix carries no payload
    c.data = c.has_data && n > c.hdr ? dw[c.hdr] : 0u;
    c.full = full;
    c.name = tlp_name(&c);
    out->is_error = false;
    out->label = NULL;
    out->fields = NULL;

    wcx_json_begin(&j, a);
    add_common(&c);
    switch (tlp_kind_of(dw[0])) {
        case TLP_KIND_CFG:
            fmt_cfg(&c);
            break;
        case TLP_KIND_MEM:
            fmt_mem(&c);
            break;
        case TLP_KIND_CPL:
            fmt_cpl(&c);
            break;
        case TLP_KIND_MSG:
            fmt_msg(&c);
            break;
        case TLP_KIND_OTHER:
            fmt_other(&c);
            break;
    }
    if (c.label == NULL || c.failed) {
        return;
    }
    add_errors(&c, out);
    if (c.label == NULL) {
        out->is_error = false;
        return;
    }
    out->label = c.label;
    out->fields = wcx_json_end(&j);
}
