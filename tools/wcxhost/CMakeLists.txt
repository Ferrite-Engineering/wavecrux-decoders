# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
#
# wcxhost: the host emulator (testing standard §5). wcxhost_core holds
# everything but main() so the tests can drive it directly.

add_library(wcxhost_core STATIC
    src/dart.c
    src/input.c
    src/lifecycle.c
    src/output.c
    src/plugin.c
    src/platform.c
    src/sb.c
    src/session.c
    src/vcd.c)
target_include_directories(wcxhost_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(wcxhost_core PUBLIC wcx_common wcx_fuzz_driver ${CMAKE_DL_LIBS})

add_executable(wcxhost src/main.c)
target_link_libraries(wcxhost PRIVATE wcxhost_core)

# MSVC's /analyze (PREfast) is for code we ship: plugins and common/. wcxhost
# is test tooling, and PREfast reports C6001/C6386 on its realloc-grown
# arrays that its own ASan/UBSan runs and clang-tidy show are false. It still
# builds with /W4 /WX.
if(MSVC)
    target_compile_options(wcxhost_core PRIVATE /analyze-)
    target_compile_options(wcxhost PRIVATE /analyze-)
endif()

# ── tests ───────────────────────────────────────────────────────────────────
set(_wcxh_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests")
foreach(t IN ITEMS test_vcd test_dart_parity test_output)
    wcx_add_unit_test(wcxhost.${t} wcxhost_${t} "${_wcxh_tests}/${t}.c" wcxhost_core)
    if(MSVC)
        target_compile_options(wcxhost_${t} PRIVATE /analyze-)
    endif()
endforeach()

# End to end against the test plugin: a deliberately wrong expected file must
# fail with a readable first difference, and bad usage must exit 2.
set(_wcxh_echo "${WCX_SOURCE_DIR}/common/tests/plugins/fixtures/generated")
add_test(NAME wcxhost.check.reports_first_difference
         COMMAND wcxhost --plugin "$<TARGET_FILE:wcx_test_echo>"
                 --vcd "${_wcxh_echo}/echo_zero_delay_1ns.vcd"
                 --bindings "${_wcxh_echo}/echo_zero_delay_1ns.bindings.json"
                 --check "${_wcxh_tests}/wrong.expected.json")
set_tests_properties(wcxhost.check.reports_first_difference PROPERTIES
    PASS_REGULAR_EXPRESSION "differs from .*wrong.expected.json at transaction 1.*expected: .*D=0xFF")
add_test(NAME wcxhost.usage.exit_2 COMMAND wcxhost --plugin)
set_tests_properties(wcxhost.usage.exit_2 PROPERTIES WILL_FAIL TRUE)
add_test(NAME wcxhost.list COMMAND wcxhost --plugin "$<TARGET_FILE:wcx_test_echo>" --list)
set_tests_properties(wcxhost.list PROPERTIES
    PASS_REGULAR_EXPRESSION "wcx\\.test\\.echo  Test echo.*wcx\\.test\\.count  Test count")
add_test(NAME wcxhost.param_override
         COMMAND wcxhost --plugin "$<TARGET_FILE:wcx_test_echo>"
                 --vcd "${_wcxh_echo}/echo_zero_delay_1ns.vcd"
                 --bindings "${_wcxh_echo}/echo_zero_delay_1ns.bindings.json"
                 --param prefix=Q --param emit_summary=false --slots 1)
set_tests_properties(wcxhost.param_override PROPERTIES
    PASS_REGULAR_EXPRESSION "\"label\":\"Q=0x5A\"")
wcx_test_environment(wcxhost.check.reports_first_difference wcxhost.list wcxhost.param_override)
