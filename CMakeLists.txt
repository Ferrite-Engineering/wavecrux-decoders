# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
#
# WaveCrux decoder plugins. See cmake/README.md for how a decoder plugs in,
# and docs/standards/ for the rules every target here is built to.

cmake_minimum_required(VERSION 3.21)

# Let CMake de-duplicate static libraries on link lines (Apple's ld warns
# about repeats, and warnings are errors in spirit here).
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()

# Fuzzing and coverage need LLVM's clang; Apple's clang ships neither
# libFuzzer nor an llvm-cov that matches it. When neither the cache nor CC
# names a compiler, default to Homebrew's LLVM on macOS. Override with
# -DCMAKE_C_COMPILER=... or CC=....
if((WCX_FUZZ OR WCX_COVERAGE) AND APPLE AND NOT CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    foreach(_wcx_llvm_cc IN ITEMS /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang)
        if(EXISTS "${_wcx_llvm_cc}")
            set(CMAKE_C_COMPILER "${_wcx_llvm_cc}" CACHE FILEPATH "C compiler")
            break()
        endif()
    endforeach()
endif()

# Platform floors from the operations standard §1, matching WaveCrux: macOS
# 12.0, and on Windows the static CRT, so a plugin needs no runtime installer.
# Both must be set before project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(wavecrux_decoders
    VERSION 0.1.0
    DESCRIPTION "WaveCrux protocol-decoder plugins"
    LANGUAGES C)

set(WCX_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${WCX_SOURCE_DIR}/cmake")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

# nm (or dumpbin on Windows) reads a plugin's export table for the
# <decoder>.exports tests.
set(WCX_NM "${CMAKE_NM}")
if(NOT WCX_NM)
    find_program(WCX_NM NAMES llvm-nm nm)
endif()
# No `set(WCX_DUMPBIN "")` beforehand: a defined normal variable stops
# find_program from searching, which left the Windows export check without
# dumpbin. The developer command prompt puts dumpbin on PATH; the linker's
# directory is a fallback.
if(MSVC)
    get_filename_component(_wcx_linker_dir "${CMAKE_LINKER}" DIRECTORY)
    find_program(WCX_DUMPBIN NAMES dumpbin HINTS "${_wcx_linker_dir}")
    if(NOT WCX_DUMPBIN)
        message(WARNING "dumpbin not found; the <decoder>.exports tests will fail")
    endif()
else()
    set(WCX_DUMPBIN "")
endif()

include(WcxHardening)
include(WcxDecoder)
include(WcxTooling)

wcx_apply_hardening()

# Reproducible builds (operations standard §4): no absolute source paths in
# the binaries, so the same commit and toolchain give the same bytes.
if(NOT MSVC)
    include(CheckCCompilerFlag)
    check_c_compiler_flag("-ffile-prefix-map=a=b" WCX_CC_HAS_FILE_PREFIX_MAP)
    if(WCX_CC_HAS_FILE_PREFIX_MAP)
        add_compile_options("-ffile-prefix-map=${WCX_SOURCE_DIR}/=")
    endif()
endif()

enable_testing()

add_subdirectory(common)
add_subdirectory(tools/wcxhost)

# Decoders: every decoders/<name>/CMakeLists.txt calls wcx_add_decoder().
file(GLOB _wcx_decoder_dirs LIST_DIRECTORIES true "${WCX_SOURCE_DIR}/decoders/*")
list(SORT _wcx_decoder_dirs)
foreach(_wcx_dir IN LISTS _wcx_decoder_dirs)
    if(EXISTS "${_wcx_dir}/CMakeLists.txt")
        add_subdirectory("${_wcx_dir}")
    endif()
endforeach()

# The ABI header is vendored byte for byte; this test fails when a local edit
# changed it. Drift from upstream is tools/check_abi_header.py's job (needs
# the network, so it is a separate target).
add_test(NAME abi.header.vendored
         COMMAND "${Python3_EXECUTABLE}" "${WCX_SOURCE_DIR}/tools/check_abi_header.py" --offline)
