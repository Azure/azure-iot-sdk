# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Compiler and linker hardening for GCC and Clang (AZ_IOT_HARDENING, default ON).
#
# Applied with add_compile_options()/add_link_options() from the top-level CMakeLists.txt before
# any dependency is fetched, so azure-sdk-for-c, Paho, the SDK, samples and tests are all built
# hardened. It does not reach a parent project that adds this tree as a subdirectory. Each flag
# is probed first, so a toolchain without it (e.g. -fcf-protection off x86) skips it.
#
#   -fstack-protector-strong      stack canaries
#   -fstack-clash-protection      stack clash probing
#   -fcf-protection               x86 CET (IBT + shadow stack)
#   -D_FORTIFY_SOURCE=2           checked libc calls; optimized builds only (glibc warns at -O0)
#   -Werror=format-security       non-literal format strings (with -Wformat)
#   PIE, -z relro -z now          read-only relocations after start-up
#   -z noexecstack                non-executable stack
#
# eng/check-hardening.sh verifies the resulting ELF executables.

include(CheckCCompilerFlag)
include(CheckLinkerFlag)
include(CheckPIESupported)

if(NOT AZ_IOT_HARDENING OR MSVC OR NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
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
add_compile_options("$<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>")

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
