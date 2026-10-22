// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Chili.CHIPS
//
// tlp_pair.c against SPEC.md §5.3 (pairing). Every expected value is
// written from SPEC.md.

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "tlp_format.h"
#include "tlp_pair.h"
#include "wcx/test.h"

// CfgRd0 01:00.0 BAR0 from `req`, tag 05.
static void note_cfgrd(tlp_pairs *p, uint32_t req) {
    const uint32_t dw[] = {0x04000001u, (req << 16) | 0x0000050Fu, 0x01000010u};
    tlp_pair_note(p, dw, 3);
}

// Cpl without data to `req`, tag 05.
static tlp_match take_cpl(tlp_pairs *p, uint32_t req) {
    const uint32_t dw[] = {0x0A000000u, 0x01000004u, (req << 16) | 0x00000500u};
    return tlp_pair_take(p, dw);
}

WCX_TEST(a_completion_takes_its_request_once) {
    tlp_pairs *p = calloc(1, sizeof *p);
    WCX_REQUIRE(p != NULL);
    note_cfgrd(p, 0x0000u);
    const tlp_match m = take_cpl(p, 0x0000u);
    WCX_CHECK(m.kind == TLP_MATCH_CFG_READ);
    WCX_CHECK_EQ_U64(m.reg, 0x010u);
    WCX_CHECK(take_cpl(p, 0x0000u).kind == TLP_MATCH_NONE);
    free(p);
}

WCX_TEST(two_requesters_share_a_tag) {
    tlp_pairs *p = calloc(1, sizeof *p);
    WCX_REQUIRE(p != NULL);
    note_cfgrd(p, 0x0000u);
    const uint32_t mrd[] = {0x00000001u, 0x0100050Fu, 0x90000000u}; // MRd32 from 01:00.0
    tlp_pair_note(p, mrd, 3);
    const tlp_match m1 = take_cpl(p, 0x0100u);
    WCX_CHECK(m1.kind == TLP_MATCH_MEM);
    WCX_CHECK_EQ_U64(m1.addr, 0x90000000u);
    WCX_CHECK(take_cpl(p, 0x0000u).kind == TLP_MATCH_CFG_READ);
    free(p);
}

WCX_TEST(a_fifth_requester_replaces_the_oldest) {
    tlp_pairs *p = calloc(1, sizeof *p);
    WCX_REQUIRE(p != NULL);
    for (uint32_t req = 1u; req <= 5u; req++) {
        note_cfgrd(p, req);
    }
    WCX_CHECK(take_cpl(p, 1u).kind == TLP_MATCH_NONE);
    for (uint32_t req = 2u; req <= 5u; req++) {
        WCX_CHECK(take_cpl(p, req).kind == TLP_MATCH_CFG_READ);
    }
    free(p);
}

WCX_TEST(eviction_is_oldest_first_after_a_take) {
    tlp_pairs *p = calloc(1, sizeof *p);
    WCX_REQUIRE(p != NULL);
    for (uint32_t req = 1u; req <= 5u; req++) { // 5 replaces 1
        note_cfgrd(p, req);
    }
    WCX_CHECK(take_cpl(p, 2u).kind == TLP_MATCH_CFG_READ); // frees 2's way
    note_cfgrd(p, 6u);                                     // takes it: 3, 4, 5, 6
    note_cfgrd(p, 3u);                                     // reuse: 3 keeps its place
    note_cfgrd(p, 7u);                                     // replaces 3, the oldest
    WCX_CHECK(take_cpl(p, 3u).kind == TLP_MATCH_NONE);
    for (uint32_t req = 4u; req <= 7u; req++) {
        WCX_CHECK(take_cpl(p, req).kind == TLP_MATCH_CFG_READ);
    }
    free(p);
}

WCX_TEST(split_completion_counts_the_lower_address) {
    tlp_pairs *p = calloc(1, sizeof *p);
    WCX_REQUIRE(p != NULL);
    // MRd32 0x8000003C, 2 DW, first BE 1000, last BE 0001: 2 bytes, tag 44
    const uint32_t mrd[] = {0x00000002u, 0x00004418u, 0x8000003Cu};
    tlp_pair_note(p, mrd, 3);
    const uint32_t part1[] = {0x4A000001u, 0x01000002u, 0x0000443Fu, 0u}; // BC 2, la 0x3F
    const uint32_t part2[] = {0x4A000001u, 0x01000001u, 0x00004440u, 0u}; // BC 1, la 0x40
    WCX_CHECK(tlp_pair_take(p, part1).kind == TLP_MATCH_MEM);             // 2 + 3 > 4: kept
    WCX_CHECK(tlp_pair_take(p, part2).kind == TLP_MATCH_MEM);             // 1 + 0 <= 4: removed
    WCX_CHECK(tlp_pair_take(p, part2).kind == TLP_MATCH_NONE);
    free(p);
}

WCX_TEST(byte_count_zero_keeps_a_4k_read) {
    tlp_pairs *p = calloc(1, sizeof *p);
    WCX_REQUIRE(p != NULL);
    const uint32_t mrd[] = {0x00000000u, 0x000030FFu, 0x80001000u}; // Length 0: 1024 DW
    tlp_pair_note(p, mrd, 3);
    const uint32_t first[] = {0x4A000010u, 0x01000000u, 0x00003000u, 0u}; // BC 0 = 4096, 16 DW
    WCX_CHECK(tlp_pair_take(p, first).kind == TLP_MATCH_MEM);
    WCX_CHECK(tlp_pair_take(p, first).kind == TLP_MATCH_MEM); // still there
    free(p);
}

WCX_TEST(ten_bit_tags_and_what_is_remembered) {
    tlp_pairs *p = calloc(1, sizeof *p);
    WCX_REQUIRE(p != NULL);
    const uint32_t mrd_t8[] = {0x00080001u, 0x0000310Fu, 0x80000040u}; // tag 0x131
    tlp_pair_note(p, mrd_t8, 3);
    const uint32_t cpl_031[] = {0x4A000001u, 0x01000004u, 0x00003140u, 0u};
    const uint32_t cpl_131[] = {0x4A080001u, 0x01000004u, 0x00003140u, 0u};
    WCX_CHECK(tlp_pair_take(p, cpl_031).kind == TLP_MATCH_NONE);
    WCX_CHECK(tlp_pair_take(p, cpl_131).kind == TLP_MATCH_MEM);

    const uint32_t mwr[] = {0x40000001u, 0x0000600Fu, 0x80000000u, 1u}; // posted: not kept
    tlp_pair_note(p, mwr, 4);
    const uint32_t short_rd[] = {0x04000001u, 0x0000610Fu, 0u}; // n < header: ignored
    tlp_pair_note(p, short_rd, 2);
    const uint32_t iowr[] = {0x42000001u, 0x0000620Fu, 0x00004003u, 1u};
    tlp_pair_note(p, iowr, 4);
    const uint32_t cpl_60[] = {0x0A000000u, 0x01000004u, 0x00006000u};
    const uint32_t cpl_61[] = {0x0A000000u, 0x01000004u, 0x00006100u};
    const uint32_t cpl_62[] = {0x0A000000u, 0x01000004u, 0x00006200u};
    WCX_CHECK(tlp_pair_take(p, cpl_60).kind == TLP_MATCH_NONE);
    WCX_CHECK(tlp_pair_take(p, cpl_61).kind == TLP_MATCH_NONE);
    const tlp_match io = tlp_pair_take(p, cpl_62);
    WCX_CHECK(io.kind == TLP_MATCH_IO_WRITE);
    WCX_CHECK_EQ_U64(io.addr, 0x00004000u);
    free(p);
}

int main(void) {
    wcx_test t = WCX_TEST_INIT;
    WCX_RUN(&t, a_completion_takes_its_request_once);
    WCX_RUN(&t, two_requesters_share_a_tag);
    WCX_RUN(&t, a_fifth_requester_replaces_the_oldest);
    WCX_RUN(&t, eviction_is_oldest_first_after_a_take);
    WCX_RUN(&t, split_completion_counts_the_lower_address);
    WCX_RUN(&t, byte_count_zero_keeps_a_4k_read);
    WCX_RUN(&t, ten_bit_tags_and_what_is_remembered);
    return wcx_test_finish(&t);
}
