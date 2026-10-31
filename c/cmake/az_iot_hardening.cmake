# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Compiler and linker hardening for GCC, Clang and MSVC (AZ_IOT_ENABLE_HARDENING, default ON).
#
# Applied with add_compile_options()/add_link_options() from the top-level CMakeLists.txt before
# any dependency is fetched, so azure-sdk-for-c, Paho, the SDK, samples and tests are all built
# hardened. It does not reach a parent project that adds this tree as a subdirectory. Each flag
# is probed first, so a toolchain without it (e.g. -fcf-protection off x86) skips it.
#
#   -fstack-protector-strong      stack canaries
#   -fstack-clash-protection      stack clash probing
#   -fcf-protection               x86 CET (IBT + shadow stack)
#   -D_FORTIFY_SOURCE=2           checked libc calls; Release, RelWithDebInfo and MinSizeRel only:
#                                 it does nothing without optimization, and older glibc warns
#   -Werror=format-security       non-literal format strings (with -Wformat)
#   PIE, -z relro -z now          read-only relocations after start-up
#   -z noexecstack                non-executable stack
#
# MSVC (/GS, /DYNAMICBASE, /NXCOMPAT and /HIGHENTROPYVA are already the toolchain defaults):
#   /guard:cf                     Control Flow Guard, compiler and linker
#   /CETCOMPAT                    CET shadow stack compatible (x64 linker only)
#   /sdl                          security warnings as errors; first-party targets only (see
#                                 az_iot_apply_hardening), as fetched dependencies are not clean
#
# eng/check-hardening.sh verifies the resulting ELF or PE binaries.

include(CheckCCompilerFlag)
include(CheckLinkerFlag)
include(CheckPIESupported)

# Per-target hardening for first-party targets; called from az_iot_apply_warnings().
function(az_iot_apply_hardening target)
    if(AZ_IOT_ENABLE_HARDENING AND MSVC AND AZ_IOT_HAS_sdl)
        target_compile_options(${target} PRIVATE /sdl)
    endif()
endfunction()

if(NOT AZ_IOT_ENABLE_HARDENING)
    return()
endif()

if(MSVC)
    check_c_compiler_flag(/sdl AZ_IOT_HAS_sdl)
    check_c_compiler_flag(/guard:cf AZ_IOT_HAS_guard_cf)
    if(AZ_IOT_HAS_guard_cf)
        add_compile_options(/guard:cf)
        add_link_options(/guard:cf)
    endif()
    if(CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
        check_linker_flag(C /CETCOMPAT AZ_IOT_HAS_cetcompat)
        if(AZ_IOT_HAS_cetcompat)
            add_link_options(/CETCOMPAT)
        endif()
    endif()
    return()
endif()

if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    return()
endif()

foreach(_flag -fstack-protector-strong -fstack-clash-protection -fcf-protection)
    string(MAKE_C_IDENTIFIER "AZ_IOT_HAS${_flag}" _var)
    check_c_compiler_flag(${_flag} ${_var})
    if(${_var})
        add_compile_options(${_flag})
    endif()
endforeach()

# GCC ignores -Wformat-security without -Wformat, so the two are probed together.
check_c_compiler_flag("-Wformat -Werror=format-security" AZ_IOT_HAS_format_security)
if(AZ_IOT_HAS_format_security)
    add_compile_options(-Wformat -Werror=format-security)
endif()

# -U first: some toolchains (Ubuntu, Yocto) already define it, and a redefinition warns.
add_compile_options("$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>")

if(NOT APPLE)
    foreach(_flag -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
        string(MAKE_C_IDENTIFIER "AZ_IOT_HAS${_flag}" _var)
        check_linker_flag(C ${_flag} ${_var})
        if(${_var})
            add_link_options(${_flag})
        endif()
    endforeach()
endif()

check_pie_supported(LANGUAGES C)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

unset(_flag)
unset(_var)
