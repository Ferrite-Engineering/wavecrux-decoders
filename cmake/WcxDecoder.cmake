# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
#
# wcx_add_decoder(): everything a decoder needs from the build, in one call.
# See cmake/README.md for the fixture conventions it relies on.
#
#   wcx_add_decoder(
#       NAME         <short>             # e.g. pcie_pipe -> libwcx_pcie_pipe.so
#       ID           <decoder id>        # default --decoder for golden tests
#       SOURCES      <c files...>        # the plugin, incl. its entry points
#       [UNIT_TESTS  <c files...>]       # one executable + ctest each
#       [FIXTURES_DIR <dir>]             # holds generated/ and captured/
#       [FUZZ_TARGET <c file>]           # defines LLVMFuzzerTestOneInput
#       [INCLUDE_DIRS <dirs...>])        # private include dirs for the sources
#
# Targets created (all prefixed with the short name):
#   wcx_<short>_obj    OBJECT library with the decoder sources (unit tests,
#                      the fuzz target and the replay runner link it)
#   wcx_<short>        SHARED plugin: libwcx_<short>.so / .dylib, wcx_<short>.dll
#   <short>_<test>     one executable per UNIT_TESTS entry
#   fuzz_<short>       libFuzzer executable (WCX_FUZZ=ON only)
#   <short>_fuzz_replay  runs the fuzz target over fuzz/corpus and
#                      fuzz/regressions in every ordinary build
#
# ctest names:
#   <short>.exports                   exported symbols == the ABI entry points
#   <short>.unit.<test>               each unit test
#   <short>.golden.<kind>.<fixture>   wcxhost --check against .expected.json
#   <short>.lifecycle.<kind>.<fixture> wcxhost --lifecycle on that fixture
#   <short>.fuzz.replay               corpus + regressions through the target
#   <short>.fuzz.smoke                (WCX_FUZZ) a short libFuzzer run

include_guard(GLOBAL)

set(WCX_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/plugins")

# wcx_test_environment(<test...>)
#
# Sanitizer runtime options for every test: stop at the first report, and
# make UBSan print where.
function(wcx_test_environment)
    cmake_parse_arguments(ENV_ARG "FUZZ" "" "" ${ARGN})
    set(env "")
    if(WCX_SANITIZE OR WCX_FUZZ)
        set(asan "halt_on_error=1:abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1")
        if(ENV_ARG_FUZZ)
            # ASan's default 256 MB free-memory quarantine alone takes a
            # libFuzzer run near -rss_limit_mb=512 (measured: 455 MB peak
            # in 30 s, 77 MB with a 16 MB quarantine).
            string(APPEND asan ":quarantine_size_mb=64")
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            string(APPEND asan ":detect_leaks=1")
        endif()
        list(APPEND env "ASAN_OPTIONS=${asan}"
                        "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
    endif()
    if(WCX_COVERAGE)
        list(APPEND env "LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/profiles/%p-%m.profraw")
    endif()
    if(env)
        set_tests_properties(${ENV_ARG_UNPARSED_ARGUMENTS} PROPERTIES ENVIRONMENT "${env}")
    endif()
endfunction()

# wcx_test_environment([FUZZ] <test>...)
#   (above) sets sanitizer/coverage environment for tests.
#
# wcx_add_unit_test(<test-name> <exe-name> <source> <libs...>)
#   One executable from <source>, linked to <libs>, registered as <test-name>.
function(wcx_add_unit_test test_name exe_name source)
    add_executable(${exe_name} "${source}")
    target_link_libraries(${exe_name} PRIVATE ${ARGN})
    # MSVC /analyze (PREfast) covers code we ship: plugins and common/. Test
    # executables build with /W4 /WX but not /analyze, which reports C6001 on
    # realloc-grown test logs that ASan, UBSan and clang-tidy show are fine.
    if(MSVC)
        target_compile_options(${exe_name} PRIVATE /analyze-)
    endif()
    add_test(NAME ${test_name} COMMAND ${exe_name})
    wcx_test_environment(${test_name})
endfunction()

function(wcx_add_decoder)
    cmake_parse_arguments(ARG "" "NAME;ID;FIXTURES_DIR;FUZZ_TARGET"
                          "SOURCES;UNIT_TESTS;INCLUDE_DIRS" ${ARGN})
    if(NOT ARG_NAME OR NOT ARG_ID OR NOT ARG_SOURCES)
        message(FATAL_ERROR "wcx_add_decoder: NAME, ID and SOURCES are required")
    endif()
    if(NOT ARG_NAME MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "wcx_add_decoder: NAME '${ARG_NAME}' must be lower_snake_case")
    endif()
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "wcx_add_decoder: unknown arguments ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    set(short ${ARG_NAME})
    set(obj wcx_${short}_obj)
    set(lib wcx_${short})

    # ── the plugin ──────────────────────────────────────────────────────
    add_library(${obj} OBJECT ${ARG_SOURCES})
    target_link_libraries(${obj} PUBLIC wcx_common)
    if(ARG_INCLUDE_DIRS)
        target_include_directories(${obj} PUBLIC ${ARG_INCLUDE_DIRS})
    endif()

    set(_wcx_def_source "")
    if(MSVC)
        # MSVC cannot add __declspec(dllexport) to functions the vendored ABI
        # header declares without it, so the entry points are exported by a
        # module-definition file instead (see WCX_EXPORT in export.h).
        set(_wcx_def_source "${CMAKE_CURRENT_BINARY_DIR}/${lib}.def")
        file(WRITE "${_wcx_def_source}"
            "EXPORTS\n"
            "    wavecrux_decoder_abi_version\n"
            "    wavecrux_decoder_register\n"
            "    wavecrux_decoder_plugin_name\n"
            "    wavecrux_decoder_plugin_description\n")
    endif()
    add_library(${lib} SHARED $<TARGET_OBJECTS:${obj}> ${_wcx_def_source})
    target_link_libraries(${lib} PRIVATE wcx_common)
    set_target_properties(${lib} PROPERTIES
        OUTPUT_NAME wcx_${short}
        LIBRARY_OUTPUT_DIRECTORY "${WCX_PLUGIN_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${WCX_PLUGIN_OUTPUT_DIR}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    if(WIN32)
        set_target_properties(${lib} PROPERTIES PREFIX "")
    else()
        set_target_properties(${lib} PROPERTIES PREFIX "lib")
    endif()
    wcx_split_debug_info(${lib})

    # ── exported symbols ────────────────────────────────────────────────
    add_test(NAME ${short}.exports
             COMMAND "${Python3_EXECUTABLE}" "${WCX_SOURCE_DIR}/tools/check_exports.py"
                     --nm "${WCX_NM}" --dumpbin "${WCX_DUMPBIN}"
                     "$<TARGET_FILE:${lib}>")

    # ── unit tests ──────────────────────────────────────────────────────
    foreach(src IN LISTS ARG_UNIT_TESTS)
        get_filename_component(stem "${src}" NAME_WE)
        wcx_add_unit_test(${short}.unit.${stem} ${short}_${stem} "${src}" ${obj} wcx_common)
    endforeach()

    # ── golden and lifecycle tests through wcxhost ─────────────────────
    if(ARG_FIXTURES_DIR)
        get_filename_component(fixtures "${ARG_FIXTURES_DIR}" ABSOLUTE)
        foreach(kind IN ITEMS generated captured)
            file(GLOB vcds CONFIGURE_DEPENDS "${fixtures}/${kind}/*.vcd")
            list(SORT vcds)
            foreach(vcd IN LISTS vcds)
                wcx_add_fixture_tests(${short} ${lib} ${ARG_ID} ${kind} "${vcd}")
            endforeach()
        endforeach()
    endif()

    # ── fuzzing ─────────────────────────────────────────────────────────
    if(ARG_FUZZ_TARGET)
        get_filename_component(fuzz_src "${ARG_FUZZ_TARGET}" ABSOLUTE)
        get_filename_component(fuzz_dir "${fuzz_src}" DIRECTORY)
        file(GLOB replay_inputs CONFIGURE_DEPENDS
             "${fuzz_dir}/corpus/*" "${fuzz_dir}/regressions/*")
        list(SORT replay_inputs)

        add_executable(${short}_fuzz_replay "${fuzz_src}")
        target_link_libraries(${short}_fuzz_replay PRIVATE ${obj} wcx_common wcx_fuzz_replay_main)
        add_test(NAME ${short}.fuzz.replay COMMAND ${short}_fuzz_replay ${replay_inputs})
        wcx_test_environment(${short}.fuzz.replay)

        if(WCX_FUZZ)
            add_executable(fuzz_${short} "${fuzz_src}")
            target_link_libraries(fuzz_${short} PRIVATE ${obj} wcx_common wcx_fuzz_driver)
            target_link_options(fuzz_${short} PRIVATE -fsanitize=fuzzer)
            # libFuzzer writes new inputs into its FIRST corpus directory, so
            # that one is in the build tree; the checked-in corpus and
            # regressions are read-only seeds after it.
            set(work_corpus "${CMAKE_CURRENT_BINARY_DIR}/fuzz_${short}_corpus")
            file(MAKE_DIRECTORY "${work_corpus}")
            set(corpus_dirs "${work_corpus}")
            foreach(d IN ITEMS corpus regressions)
                if(IS_DIRECTORY "${fuzz_dir}/${d}")
                    list(APPEND corpus_dirs "${fuzz_dir}/${d}")
                endif()
            endforeach()
            add_test(NAME ${short}.fuzz.smoke
                     COMMAND fuzz_${short} -runs=5000 -rss_limit_mb=512 -timeout=5
                             "-artifact_prefix=${CMAKE_CURRENT_BINARY_DIR}/" ${corpus_dirs})
            wcx_test_environment(FUZZ ${short}.fuzz.smoke)
        endif()
    endif()
endfunction()

# wcx_add_fixture_tests(<short> <lib target> <decoder id> <kind> <vcd>)
#
# One golden and one lifecycle test for a fixture. The fixture's siblings
# <stem>.bindings.json and <stem>.expected.json must exist. The golden test
# compares in canonical order (wcxhost --sort): an expected file lists the
# transactions, not the order a decoder emits overlapping ones in. The
# lifecycle test compares the plugin against itself and keeps emission order.
function(wcx_add_fixture_tests short lib id kind vcd)
    get_filename_component(dir "${vcd}" DIRECTORY)
    get_filename_component(stem "${vcd}" NAME_WLE)
    set(bindings "${dir}/${stem}.bindings.json")
    set(expected "${dir}/${stem}.expected.json")
    foreach(f IN ITEMS "${bindings}" "${expected}")
        if(NOT EXISTS "${f}")
            message(FATAL_ERROR "wcx: fixture ${vcd} has no sibling ${f} (see cmake/README.md)")
        endif()
    endforeach()
    set(common_args --plugin "$<TARGET_FILE:${lib}>" --decoder "${id}"
                    --vcd "${vcd}" --bindings "${bindings}")
    add_test(NAME ${short}.golden.${kind}.${stem}
             COMMAND wcxhost ${common_args} --sort --check "${expected}")
    add_test(NAME ${short}.lifecycle.${kind}.${stem}
             COMMAND wcxhost ${common_args} --lifecycle)
    wcx_test_environment(${short}.golden.${kind}.${stem} ${short}.lifecycle.${kind}.${stem})
endfunction()
