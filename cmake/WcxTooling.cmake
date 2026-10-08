# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
#
# Developer and CI targets that check the tree rather than build it:
#
#   format-check   clang-format --dry-run --Werror over the repository's C files
#   format         clang-format -i over the same files
#   tidy           clang-tidy (.clang-tidy, warnings as errors) over every C
#                  file in compile_commands.json
#   cppcheck       cppcheck with coding standard §8's settings
#   cppcheck-misra the MISRA addon in advisory mode (reports, never fails)
#   check-abi-header  the vendored ABI header against upstream (network)
#   coverage-report   merge .profraw files and print llvm-cov's report
#                     (WCX_COVERAGE=ON builds)
#
# Tools are located with find_program; override with -DWCX_CLANG_FORMAT=...,
# -DWCX_CLANG_TIDY=..., -DWCX_CPPCHECK=..., -DWCX_LLVM_PROFDATA=...,
# -DWCX_LLVM_COV=....

include_guard(GLOBAL)

set(_wcx_llvm_hints
    /opt/homebrew/opt/llvm/bin
    /usr/local/opt/llvm/bin
    /usr/lib/llvm-19/bin /usr/lib/llvm-18/bin /usr/lib/llvm-17/bin
    /usr/lib/llvm-16/bin /usr/lib/llvm-15/bin)

# Prefer the LLVM next to the compiler, so coverage data and the tools that
# read it come from the same release.
get_filename_component(_wcx_cc_dir "${CMAKE_C_COMPILER}" DIRECTORY)

find_program(WCX_CLANG_FORMAT NAMES clang-format HINTS ${_wcx_llvm_hints})
find_program(WCX_CLANG_TIDY NAMES clang-tidy HINTS ${_wcx_llvm_hints})
find_program(WCX_CPPCHECK NAMES cppcheck)
find_program(WCX_LLVM_PROFDATA NAMES llvm-profdata HINTS "${_wcx_cc_dir}" ${_wcx_llvm_hints})
find_program(WCX_LLVM_COV NAMES llvm-cov HINTS "${_wcx_cc_dir}" ${_wcx_llvm_hints})

set(_wcx_tools "${WCX_SOURCE_DIR}/tools")

add_custom_target(format-check
    COMMAND "${Python3_EXECUTABLE}" "${_wcx_tools}/run_clang_format.py"
            --clang-format "${WCX_CLANG_FORMAT}" --check "${WCX_SOURCE_DIR}"
    WORKING_DIRECTORY "${WCX_SOURCE_DIR}"
    COMMENT "clang-format --dry-run --Werror"
    VERBATIM)

add_custom_target(format
    COMMAND "${Python3_EXECUTABLE}" "${_wcx_tools}/run_clang_format.py"
            --clang-format "${WCX_CLANG_FORMAT}" "${WCX_SOURCE_DIR}"
    WORKING_DIRECTORY "${WCX_SOURCE_DIR}"
    COMMENT "clang-format -i"
    VERBATIM)

add_custom_target(tidy
    COMMAND "${Python3_EXECUTABLE}" "${_wcx_tools}/run_clang_tidy.py"
            --clang-tidy "${WCX_CLANG_TIDY}" --build-dir "${CMAKE_BINARY_DIR}"
            --source-dir "${WCX_SOURCE_DIR}"
    WORKING_DIRECTORY "${WCX_SOURCE_DIR}"
    COMMENT "clang-tidy (warnings as errors)"
    VERBATIM)

add_custom_target(cppcheck
    COMMAND "${Python3_EXECUTABLE}" "${_wcx_tools}/run_cppcheck.py"
            --cppcheck "${WCX_CPPCHECK}" --build-dir "${CMAKE_BINARY_DIR}"
            --source-dir "${WCX_SOURCE_DIR}"
    WORKING_DIRECTORY "${WCX_SOURCE_DIR}"
    COMMENT "cppcheck (coding standard §8)"
    VERBATIM)

add_custom_target(cppcheck-misra
    COMMAND "${Python3_EXECUTABLE}" "${_wcx_tools}/run_cppcheck.py"
            --cppcheck "${WCX_CPPCHECK}" --build-dir "${CMAKE_BINARY_DIR}"
            --source-dir "${WCX_SOURCE_DIR}" --misra
    WORKING_DIRECTORY "${WCX_SOURCE_DIR}"
    COMMENT "cppcheck MISRA addon (advisory)"
    VERBATIM)

add_custom_target(check-abi-header
    COMMAND "${Python3_EXECUTABLE}" "${_wcx_tools}/check_abi_header.py"
    WORKING_DIRECTORY "${WCX_SOURCE_DIR}"
    COMMENT "include/wavecrux_decoder.h against upstream"
    VERBATIM)

# The report parser behind coverage-report's floor check, against a canned
# llvm-cov report (it must sum the listed files, not llvm-cov's TOTAL row).
add_test(NAME tools.coverage_report.parser
         COMMAND "${Python3_EXECUTABLE}" "${_wcx_tools}/coverage_report.py" --self-test)

if(WCX_COVERAGE)
    add_custom_target(coverage-report
        COMMAND "${Python3_EXECUTABLE}" "${_wcx_tools}/coverage_report.py"
                --build-dir "${CMAKE_BINARY_DIR}" --source-dir "${WCX_SOURCE_DIR}"
                --llvm-profdata "${WCX_LLVM_PROFDATA}" --llvm-cov "${WCX_LLVM_COV}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "llvm-cov report over the last ctest run"
        VERBATIM)
endif()
