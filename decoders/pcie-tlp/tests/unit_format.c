// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Chili.CHIPS
//
// tlp_format.c against SPEC.md §5: the cases the golden fixtures do not
// reach. Every expected string is written from SPEC.md.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tlp_format.h"
#include "wcx/arena.h"
#include "wcx/test.h"

static tlp_text fmt(wcx_arena *a, const uint32_t *dw, uint32_t n, const tlp_match *m, bool full) {
    tlp_text out;
    tlp_format(a, dw, n, m, full, &out);
    return out;
}

static bool ends_with(const char *s, const char *tail) {
    const size_t ls = strlen(s);
    const size_t lt = strlen(tail);
    return ls >= lt && memcmp(s + ls - lt, tail, lt) == 0;
}

WCX_TEST(header_and_expected_length) {
    WCX_CHECK_EQ_U64(tlp_header_dw(0x04000001u), 3u);
    WCX_CHECK_EQ_U64(tlp_header_dw(0x20000004u), 4u);
    WCX_CHECK_EQ_U64(tlp_expected_dw(0x40000000u), 3u + 1024u); // Length 0 = 1024 DW
    WCX_CHECK_EQ_U64(tlp_expected_dw(0x60000002u), 4u + 2u);
    WCX_CHECK_EQ_U64(tlp_expected_dw(0x00000004u), 3u); // a read carries no data
    WCX_CHECK_EQ_U64(tlp_expected_dw(0x9F000000u), 0u); // a prefix is not checked
}

WCX_TEST(kinds) {
    WCX_CHECK(tlp_kind_of(0x05000000u) == TLP_KIND_CFG);
    WCX_CHECK(tlp_kind_of(0x02000000u) == TLP_KIND_MEM);
    WCX_CHECK(tlp_kind_of(0x0B000000u) == TLP_KIND_CPL);
    WCX_CHECK(tlp_kind_of(0x37000000u) == TLP_KIND_MSG);
    WCX_CHECK(tlp_kind_of(0x18000000u) == TLP_KIND_OTHER); // 1_1000 is not 1_0rrr
    WCX_CHECK(tlp_kind_of(0x03000000u) == TLP_KIND_OTHER);
}

WCX_TEST(register_and_status_names) {
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 1024, 65536));
    WCX_CHECK_STR_EQ(tlp_reg_name(&a, 0x00u), "ID");
    WCX_CHECK_STR_EQ(tlp_reg_name(&a, 0x0Cu), "Hdr/Lat/CLS");
    WCX_CHECK_STR_EQ(tlp_reg_name(&a, 0x24u), "BAR5");
    WCX_CHECK_STR_EQ(tlp_reg_name(&a, 0x38u), "reg 0x038");
    WCX_CHECK_STR_EQ(tlp_reg_name(&a, 0x3Cu), "Int");
    WCX_CHECK_STR_EQ(tlp_reg_name(&a, 0x40u), "reg 0x040");
    WCX_CHECK_STR_EQ(tlp_reg_name(&a, 0x11u), "reg 0x011");
    WCX_CHECK_STR_EQ(tlp_status_name(0u), "SC");
    WCX_CHECK_STR_EQ(tlp_status_name(2u), "CRS");
    WCX_CHECK_STR_EQ(tlp_status_name(3u), "rsvd");
    WCX_CHECK_STR_EQ(tlp_status_name(4u), "CA");
    WCX_CHECK_STR_EQ(tlp_status_name(7u), "rsvd");
    wcx_arena_free(&a);
}

WCX_TEST(compact_config_shows_bdf_unless_lone_endpoint) {
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 1024, 65536));
    const uint32_t dev1[] = {0x04000001u, 0x0000010Fu, 0x01080010u}; // 01:01.0 BAR0
    const uint32_t fn3[] = {0x04000001u, 0x0000010Fu, 0x01030010u};  // 01:00.3 BAR0
    const uint32_t t1[] = {0x05000001u, 0x0000010Fu, 0x01000000u};   // Type 1, 01:00.0 ID
    WCX_CHECK_STR_EQ(fmt(&a, dev1, 3, NULL, false).label, "CfgRd 01:01.0 BAR0");
    WCX_CHECK_STR_EQ(fmt(&a, fn3, 3, NULL, false).label, "CfgRd 01:00.3 BAR0");
    WCX_CHECK_STR_EQ(fmt(&a, t1, 3, NULL, false).label, "CfgRd1 01:00.0 ID");
    wcx_arena_free(&a);
}

WCX_TEST(completion_to_io_write) {
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 1024, 65536));
    const uint32_t cpl[] = {0x0A000000u, 0x01000000u, 0x00002600u};
    const tlp_match m = {TLP_MATCH_IO_WRITE, 0u, 0x00004000u};
    WCX_CHECK_STR_EQ(fmt(&a, cpl, 3, &m, false).label, "Cpl 0x00004000");
    const tlp_text full = fmt(&a, cpl, 3, &m, true);
    WCX_CHECK_STR_EQ(full.label, "Cpl SC IOWr 0x00004000  tag 26");
    WCX_CHECK(ends_with(full.fields, "\"answers\":\"IOWr 0x00004000\"}"));
    WCX_CHECK(!full.is_error);
    wcx_arena_free(&a);
}

WCX_TEST(locked_completion_abort_is_an_error) {
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 1024, 65536));
    const uint32_t cpl[] = {0x4B000001u, 0x01008004u, 0x00002240u, 0x00000000u};
    const tlp_text r = fmt(&a, cpl, 4, NULL, false);
    WCX_CHECK_STR_EQ(r.label, "CplD CA = 0x00000000"); // compact has no Lk
    WCX_CHECK(r.is_error);
    WCX_CHECK(ends_with(r.fields, "\"error\":\"Completer Abort\"}"));
    WCX_CHECK_STR_EQ(fmt(&a, cpl, 4, NULL, true).label, "CplDLk CA = 0x00000000  tag 22");
    wcx_arena_free(&a);
}

WCX_TEST(locked_64_bit_read_and_prefix) {
    wcx_arena a;
    WCX_REQUIRE(wcx_arena_init(&a, 1024, 65536));
    const uint32_t mrdlk[] = {0x21000001u, 0x0000000Fu, 0x00000001u, 0x00000000u};
    WCX_CHECK_STR_EQ(fmt(&a, mrdlk, 4, NULL, false).label, "MRdLk64 0x00000001_00000000");
    const uint32_t prefix[] = {0x9F000000u, 0x00000000u, 0x00000000u};
    const tlp_text r = fmt(&a, prefix, 3, NULL, false);
    WCX_CHECK_STR_EQ(r.label, "TLP Fmt 4 Type 0x1F");
    WCX_CHECK(!r.is_error);
    wcx_arena_free(&a);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, header_and_expected_length);
    WCX_RUN(&t, kinds);
    WCX_RUN(&t, register_and_status_names);
    WCX_RUN(&t, compact_config_shows_bdf_unless_lone_endpoint);
    WCX_RUN(&t, completion_to_io_write);
    WCX_RUN(&t, locked_completion_abort_is_an_error);
    WCX_RUN(&t, locked_64_bit_read_and_prefix);
    return wcx_test_finish(&t);
}
