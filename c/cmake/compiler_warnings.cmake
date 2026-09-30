# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

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
            # External (SYSTEM) headers, e.g. azure-sdk-for-c, are not analyzed.
            target_compile_options(${target} PRIVATE /analyze /analyze:external-)
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
