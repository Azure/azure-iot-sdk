# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Warning level for fetched dependencies (azure-sdk-for-c, Paho) built into SDK binaries.
# MSVC: /W3. Without it they build at Visual Studio's default /W1, as CMP0092 drops CMake's
# default /W3 and azure-sdk-for-c sets warning flags only for its own unit tests. BinSkim
# (BA2007) rejects binaries containing modules built below /W3. Warnings stay non-fatal.
function(az_iot_apply_dependency_warnings)
    if(MSVC)
        foreach(_target IN LISTS ARGN)
            if(TARGET ${_target})
                target_compile_options(${_target} PRIVATE /W3)
            endif()
        endforeach()
    endif()
endfunction()

# Apply strict-but-portable C warning flags to a target.
function(az_iot_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            # C4061: an enumerator is not handled by an explicit case label, even
            # when a default: exists. Off by default at every level, including
            # /W4. This is the MSVC counterpart of -Wswitch-enum below; see the
            # note there for why the weaker on-by-default check is not enough.
            /w14061
        )
        if(AZ_IOT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        if(AZ_IOT_ENABLE_MSVC_ANALYZE)
            # External (SYSTEM) headers, e.g. azure-sdk-for-c, are not analyzed. Stack warning
            # (C6262) at 64 KiB, not 16: SDK clients embed fixed buffers and samples keep them
            # on main()'s stack.
            target_compile_options(${target} PRIVATE
                /analyze /analyze:external- /analyze:stacksize65536)
        endif()
        az_iot_apply_hardening(${target})
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wstrict-prototypes
            -Wmissing-prototypes
            # -Wall already brings -Wswitch, but that only fires on a switch with
            # NO default: label. Every *_to_string() helper has one, so -Wswitch
            # is structurally unable to notice a new enumerator that nobody wrote
            # an arm for -- it just starts returning "unknown" at runtime.
            # -Wswitch-enum fires on a missing case even when a default: is
            # present, which is the shape we actually ship. A switch that
            # deliberately handles a subset must now say so with explicit case
            # labels rather than leaning on default:.
            -Wswitch-enum
            -Werror=implicit-function-declaration
            -Werror=incompatible-pointer-types
        )
        if(AZ_IOT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
