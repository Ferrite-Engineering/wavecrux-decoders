# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ferrite Engineering LLC
#
# Compiler and linker settings from docs/standards/c-coding-standard.md §7,
# set once for every C target in the tree. A decoder never sets its own
# flags.
#
# Every flag is probed with check_c_compiler_flag (or check_linker_flag) and
# applied only when the compiler supports it, so an older contributor
# toolchain still builds. With WCX_REQUIRE_ALL_FLAGS=ON (CI) configure fails
# instead, naming every listed flag the compiler lacks.
#
# Options:
#   WCX_REQUIRE_ALL_FLAGS  fail configure when a §7 flag is unsupported
#   WCX_SANITIZE           "" or a sanitizer list, e.g. "address,undefined"
#   WCX_COVERAGE           clang source-based coverage
#   WCX_FUZZ               libFuzzer instrumentation (clang only)
#   WCX_SPLIT_DEBUG        keep debug info in a separate file (see
#                          wcx_split_debug_info)

include_guard(GLOBAL)

include(CheckCCompilerFlag)
include(CheckLinkerFlag)

option(WCX_REQUIRE_ALL_FLAGS "Fail configure if any coding-standard §7 flag is unsupported" OFF)
set(WCX_SANITIZE "" CACHE STRING "Sanitizers to enable: empty, or e.g. address,undefined")
option(WCX_COVERAGE "Build with clang source-based coverage instrumentation" OFF)
option(WCX_FUZZ "Build libFuzzer targets (clang only; implies address,undefined)" OFF)
option(WCX_SPLIT_DEBUG "Strip debug info into a separate file after linking a plugin" OFF)

set(WCX_BANNED_HEADER "${CMAKE_CURRENT_LIST_DIR}/../common/include/wcx/banned.h")
get_filename_component(WCX_BANNED_HEADER "${WCX_BANNED_HEADER}" ABSOLUTE)

set(_wcx_missing_flags "")
set(_wcx_probe_context "") # extra flags while probing (-O2 for release flags)
set(_wcx_probe_werror "")  # -Werror for GCC/Clang probes (set below)

# _wcx_probe_flag(<out-list> <alternatives...>)
#
# Appends the first supported alternative to <out-list>. Alternatives exist
# only where two compilers spell the same check differently (GCC's
# -Wcast-align=strict is what Clang's -Wcast-align always does). When none is
# supported the first spelling is recorded as missing.
function(_wcx_probe_flag out_list)
    foreach(flag IN LISTS ARGN)
        string(MAKE_C_IDENTIFIER "WCX_CC_HAS${_wcx_probe_context}${flag}" var)
        # Probes run with -Werror (GCC/Clang): a flag the compiler accepts
        # but ignores ("argument unused during compilation") is not
        # supported, and CMake's own pattern matching does not always catch
        # that. Release flags are probed at -O2, where they are used: a
        # compiler may accept a flag at -O0 and ignore it once optimising.
        # GCC ignores -Wformat-security without -Wformat, hence the pairing.
        set(CMAKE_REQUIRED_FLAGS "${_wcx_probe_werror} ${_wcx_probe_context}")
        if(flag MATCHES "format-security")
            string(APPEND CMAKE_REQUIRED_FLAGS " -Wformat")
        endif()
        check_c_compiler_flag("${flag}" ${var})
        if(${var})
            set(${out_list} ${${out_list}} "${flag}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    list(GET ARGN 0 first)
    set(_wcx_missing_flags ${_wcx_missing_flags} "${first}" PARENT_SCOPE)
endfunction()

# _wcx_probe_arch_flag(<out-list> <arch> <flag>)
#
# Probes a flag that is valid for one target architecture only. In a macOS
# universal build (CMAKE_OSX_ARCHITECTURES lists several) it is applied
# through -Xarch_<arch> so the other slice never sees it.
function(_wcx_probe_arch_flag out_list arch flag)
    string(MAKE_C_IDENTIFIER "WCX_CC_HAS_${arch}${_wcx_probe_context}${flag}" var)
    set(CMAKE_OSX_ARCHITECTURES "${arch}")
    set(CMAKE_REQUIRED_FLAGS "${_wcx_probe_werror} ${_wcx_probe_context}")
    check_c_compiler_flag("${flag}" ${var})
    if(NOT ${var})
        set(_wcx_missing_flags ${_wcx_missing_flags} "${flag} (${arch})" PARENT_SCOPE)
        return()
    endif()
    list(LENGTH WCX_TARGET_ARCHS n_archs)
    if(APPLE AND n_archs GREATER 1)
        set(${out_list} ${${out_list}} "SHELL:-Xarch_${arch} ${flag}" PARENT_SCOPE)
    else()
        set(${out_list} ${${out_list}} "${flag}" PARENT_SCOPE)
    endif()
endfunction()

function(_wcx_probe_link_flag out_list flag)
    string(MAKE_C_IDENTIFIER "WCX_LD_HAS${flag}" var)
    check_linker_flag(C "${flag}" ${var})
    if(${var})
        set(${out_list} ${${out_list}} "${flag}" PARENT_SCOPE)
    else()
        set(_wcx_missing_flags ${_wcx_missing_flags} "${flag}" PARENT_SCOPE)
    endif()
endfunction()

# The architectures this configuration produces code for, normalised to
# x86_64 / arm64.
set(WCX_TARGET_ARCHS "")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(_wcx_raw_archs ${CMAKE_OSX_ARCHITECTURES})
elseif(MSVC AND CMAKE_C_COMPILER_ARCHITECTURE_ID)
    set(_wcx_raw_archs ${CMAKE_C_COMPILER_ARCHITECTURE_ID})
else()
    set(_wcx_raw_archs ${CMAKE_SYSTEM_PROCESSOR})
endif()
foreach(a IN LISTS _wcx_raw_archs)
    string(TOLOWER "${a}" a)
    if(a MATCHES "^(x86_64|amd64|x64)$")
        list(APPEND WCX_TARGET_ARCHS x86_64)
    elseif(a MATCHES "^(arm64|aarch64|arm64e)$")
        list(APPEND WCX_TARGET_ARCHS arm64)
    endif()
endforeach()

set(WCX_IS_GNU_LIKE OFF)
if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    set(WCX_IS_GNU_LIKE ON)
endif()
set(WCX_IS_CLANG OFF)
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(WCX_IS_CLANG ON)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(WCX_COMPILE_OPTIONS "")
set(WCX_RELEASE_COMPILE_OPTIONS "")
set(WCX_RELEASE_LINK_OPTIONS "")
set(WCX_LINK_OPTIONS "")

if(WCX_FUZZ AND NOT WCX_SANITIZE)
    set(WCX_SANITIZE "address,undefined")
endif()

if(WCX_IS_GNU_LIKE)
    set(_wcx_probe_werror "-Werror")
    # ── every build ─────────────────────────────────────────────────────
    foreach(flag IN ITEMS
            -std=c17 -fPIC -fvisibility=hidden
            -Wall -Wextra -Wpedantic -Werror
            -Wconversion -Wsign-conversion -Wshadow -Wcast-qual)
        _wcx_probe_flag(WCX_COMPILE_OPTIONS ${flag})
    endforeach()
    _wcx_probe_flag(WCX_COMPILE_OPTIONS -Wcast-align=strict -Wcast-align)
    foreach(flag IN ITEMS
            -Wformat=2 -Werror=format-security -Wimplicit-fallthrough -Wswitch-enum
            -Wnull-dereference -Wdouble-promotion -Wstrict-prototypes -Wmissing-prototypes
            -Wvla -Werror=vla -Werror=implicit -Werror=incompatible-pointer-types
            -Werror=int-conversion
            -fstrict-flex-arrays=3 -fno-strict-aliasing -ftrivial-auto-var-init=zero)
        _wcx_probe_flag(WCX_COMPILE_OPTIONS ${flag})
    endforeach()
    list(APPEND WCX_COMPILE_OPTIONS "SHELL:-include \"${WCX_BANNED_HEADER}\"")

    # ── release builds add ──────────────────────────────────────────────
    set(_wcx_probe_context "-O2")
    if(NOT WCX_SANITIZE)
        # The toolchain may predefine a weaker level (Ubuntu's GCC does);
        # undefine first so the redefinition is not a -Werror failure.
        _wcx_probe_flag(WCX_RELEASE_COMPILE_OPTIONS -D_FORTIFY_SOURCE=3)
        if(WCX_RELEASE_COMPILE_OPTIONS)
            list(PREPEND WCX_RELEASE_COMPILE_OPTIONS -U_FORTIFY_SOURCE)
        endif()
    endif()
    foreach(flag IN ITEMS -O2 -fstack-protector-strong)
        _wcx_probe_flag(WCX_RELEASE_COMPILE_OPTIONS ${flag})
    endforeach()
    # Stack-clash protection: LLVM implements it for ELF targets only; for
    # Mach-O, Apple clang and upstream clang accept the flag at -O0 and
    # report it unused when optimising (the Darwin kernel's stack guard is
    # the defence there). So it is required everywhere except Apple targets,
    # where it is applied only if a future toolchain implements it.
    if(APPLE)
        string(MAKE_C_IDENTIFIER "WCX_CC_HAS_O2-fstack-clash-protection" var)
        set(CMAKE_REQUIRED_FLAGS "-Werror -O2")
        check_c_compiler_flag(-fstack-clash-protection ${var})
        unset(CMAKE_REQUIRED_FLAGS)
        if(${var})
            list(APPEND WCX_RELEASE_COMPILE_OPTIONS -fstack-clash-protection)
        else()
            message(STATUS "wcx: -fstack-clash-protection is not implemented for Apple targets; "
                           "exempt there (cmake/WcxHardening.cmake)")
        endif()
    else()
        _wcx_probe_flag(WCX_RELEASE_COMPILE_OPTIONS -fstack-clash-protection)
    endif()
    foreach(arch IN LISTS WCX_TARGET_ARCHS)
        if(arch STREQUAL "x86_64")
            _wcx_probe_arch_flag(WCX_RELEASE_COMPILE_OPTIONS x86_64 -fcf-protection=full)
        elseif(arch STREQUAL "arm64")
            _wcx_probe_arch_flag(WCX_RELEASE_COMPILE_OPTIONS arm64 -mbranch-protection=standard)
        endif()
    endforeach()
    set(_wcx_probe_context "")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        _wcx_probe_link_flag(WCX_RELEASE_LINK_OPTIONS "LINKER:-z,relro,-z,now,-z,noexecstack")
        _wcx_probe_link_flag(WCX_RELEASE_LINK_OPTIONS "LINKER:--as-needed")
        # Sanitizer runtimes resolve their symbols at load time, so
        # --no-undefined only applies to uninstrumented builds.
        if(NOT WCX_SANITIZE AND NOT WCX_COVERAGE)
            _wcx_probe_link_flag(WCX_RELEASE_LINK_OPTIONS "LINKER:--no-undefined")
        endif()
    endif()
elseif(MSVC)
    # /external:anglebrackets marks <...> headers (the Windows SDK, the CRT)
    # as external; /external:W0 and /analyze:external- then keep their
    # warnings and PREfast annotations out of a /WX build. Without it,
    # winnt.h's own C28301 fails the build.
    foreach(flag IN ITEMS /std:c17 /W4 /WX /sdl /guard:cf /Qspectre
                          /external:anglebrackets /external:W0
                          /analyze /analyze:external-)
        _wcx_probe_flag(WCX_COMPILE_OPTIONS ${flag})
    endforeach()
    # /sdl makes C4996 (CRT deprecations: strcpy, sprintf, ...) an error;
    # banned.h's full list is enforced by GCC and Clang (see banned.h).
    list(APPEND WCX_COMPILE_OPTIONS /we4996 "/FI${WCX_BANNED_HEADER}")
    foreach(flag IN ITEMS /DYNAMICBASE /HIGHENTROPYVA /CETCOMPAT /guard:cf)
        _wcx_probe_link_flag(WCX_LINK_OPTIONS ${flag})
    endforeach()
else()
    message(WARNING "wcx: unrecognised C compiler ${CMAKE_C_COMPILER_ID}; no hardening flags applied")
endif()

# ── instrumentation ─────────────────────────────────────────────────────────
set(WCX_INSTRUMENT_OPTIONS "")
if(WCX_SANITIZE)
    if(MSVC)
        if(WCX_SANITIZE MATCHES "address")
            list(APPEND WCX_INSTRUMENT_OPTIONS /fsanitize=address)
        endif()
    else()
        list(APPEND WCX_INSTRUMENT_OPTIONS
            "-fsanitize=${WCX_SANITIZE}" -fno-sanitize-recover=all -fno-omit-frame-pointer)
    endif()
endif()
if(WCX_COVERAGE)
    if(NOT WCX_IS_CLANG)
        message(FATAL_ERROR "wcx: WCX_COVERAGE needs clang (source-based coverage); "
                            "configure with -DCMAKE_C_COMPILER=clang")
    endif()
    list(APPEND WCX_INSTRUMENT_OPTIONS -fprofile-instr-generate -fcoverage-mapping)
endif()
if(WCX_FUZZ)
    if(NOT WCX_IS_CLANG)
        message(FATAL_ERROR "wcx: WCX_FUZZ needs clang with libFuzzer; "
                            "configure with -DCMAKE_C_COMPILER=<llvm clang>")
    endif()
    list(APPEND WCX_INSTRUMENT_OPTIONS -fsanitize=fuzzer-no-link)
endif()

if(_wcx_missing_flags)
    list(REMOVE_DUPLICATES _wcx_missing_flags)
    string(REPLACE ";" " " _wcx_missing_text "${_wcx_missing_flags}")
    if(WCX_REQUIRE_ALL_FLAGS)
        message(FATAL_ERROR
            "wcx: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} does not support these "
            "coding-standard §7 flags: ${_wcx_missing_text}. WCX_REQUIRE_ALL_FLAGS is ON, so "
            "configure stops; use a toolchain that supports them.")
    else()
        message(STATUS "wcx: flags not supported by this compiler (skipped): ${_wcx_missing_text}")
    endif()
endif()

# wcx_apply_hardening()
#
# Applies the probed settings to every target defined after the call in this
# directory and below. Called once, from the top-level CMakeLists.txt.
macro(wcx_apply_hardening)
    add_compile_options(${WCX_COMPILE_OPTIONS} ${WCX_INSTRUMENT_OPTIONS})
    add_compile_options("$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:${WCX_RELEASE_COMPILE_OPTIONS}>")
    add_link_options(${WCX_LINK_OPTIONS})
    add_link_options("$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:${WCX_RELEASE_LINK_OPTIONS}>")
    if(WCX_INSTRUMENT_OPTIONS)
        # Link with the same instrumentation; fuzzer-no-link links nothing.
        add_link_options(${WCX_INSTRUMENT_OPTIONS})
    endif()
    # WCX_ASSERT is live in debug, sanitizer and fuzz builds (§2): there,
    # crashing loudly is the point. Release builds compile it out.
    if(WCX_SANITIZE OR WCX_FUZZ)
        add_compile_definitions(WCX_ENABLE_ASSERTS=1)
    else()
        add_compile_definitions("$<$<CONFIG:Debug>:WCX_ENABLE_ASSERTS=1>")
    endif()
endmacro()

# wcx_split_debug_info(<target>)
#
# With WCX_SPLIT_DEBUG=ON, moves a linked plugin's debug info into a
# separate file next to it: <lib>.dSYM on macOS, <lib>.debug (with a
# .gnu_debuglink) on Linux. MSVC already writes a .pdb.
function(wcx_split_debug_info target)
    if(NOT WCX_SPLIT_DEBUG)
        return()
    endif()
    if(APPLE)
        find_program(WCX_DSYMUTIL dsymutil)
        find_program(WCX_STRIP strip)
        if(WCX_DSYMUTIL AND WCX_STRIP)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${WCX_DSYMUTIL}" "$<TARGET_FILE:${target}>"
                        -o "$<TARGET_FILE:${target}>.dSYM"
                COMMAND "${WCX_STRIP}" -x "$<TARGET_FILE:${target}>"
                VERBATIM)
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_OBJCOPY)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug "$<TARGET_FILE:${target}>"
                    "$<TARGET_FILE:${target}>.debug"
            COMMAND "${CMAKE_OBJCOPY}" --strip-debug
                    "--add-gnu-debuglink=$<TARGET_FILE:${target}>.debug"
                    "$<TARGET_FILE:${target}>"
            VERBATIM)
    endif()
endfunction()
