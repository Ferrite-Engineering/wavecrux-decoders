# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
#
# wcx_common: the static library every decoder links (PIC, hidden symbols).
# wcx_fuzz_driver / wcx_fuzz_replay_main: the shared fuzz harness (tools only,
# never linked into a plugin).

add_library(wcx_common STATIC
    src/arena.c
    src/assert.c
    src/buf.c
    src/config.c
    src/decoder.c
    src/edge.c
    src/emit.c
    src/error.c
    src/json_parse.c
    src/json_writer.c
    src/manifest.c
    src/sample.c
    src/str.c)
target_include_directories(wcx_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${WCX_SOURCE_DIR}/include")
set_target_properties(wcx_common PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(wcx_fuzz_driver STATIC fuzz/fuzz_driver.c fuzz/tool_io.c)
target_include_directories(wcx_fuzz_driver PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/include")
target_link_libraries(wcx_fuzz_driver PUBLIC wcx_common)

add_library(wcx_fuzz_replay_main STATIC fuzz/replay_main.c)
target_link_libraries(wcx_fuzz_replay_main PUBLIC wcx_fuzz_driver)

# ── unit tests ──────────────────────────────────────────────────────────────
set(_wcx_common_tests
    test_arena
    test_banned
    test_buf
    test_config
    test_decoder
    test_edge
    test_emit
    test_json_parse
    test_json_writer
    test_manifest
    test_sample
    test_size
    test_str)
foreach(t IN LISTS _wcx_common_tests)
    wcx_add_unit_test(common.${t} common_${t} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${t}.c" wcx_common)
endforeach()

# banned.h must reject a use of a banned function. The target is never
# built by default; the test builds it and expects the build to fail.
add_executable(common_banned_rejects EXCLUDE_FROM_ALL tests/banned_rejects.c)
target_link_libraries(common_banned_rejects PRIVATE wcx_common)
add_test(NAME common.banned.rejects
         COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target common_banned_rejects
                 --config "$<CONFIG>")
# Pass only on the poison diagnostic itself (GCC/Clang) or the deprecation
# error (MSVC), not on any build failure.
set_tests_properties(common.banned.rejects PROPERTIES
    PASS_REGULAR_EXPRESSION "poisoned|C4995|C4996")

# ── the test plugin, built and tested exactly like a real decoder ──────────
wcx_add_decoder(
    NAME         test_echo
    ID           wcx.test.echo
    SOURCES      tests/plugins/echo_plugin.c
    UNIT_TESTS   tests/plugins/test_echo_lifecycle.c
    FIXTURES_DIR tests/plugins/fixtures
    FUZZ_TARGET  tests/plugins/fuzz/fuzz_echo.c)
