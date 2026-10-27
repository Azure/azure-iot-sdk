# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Paho-C MQTT adapter for azure-iot-sdk (Phase 1.2).
#
# Pulls eclipse/paho.mqtt.c via FetchContent (no submodules, no system package
# requirement), or with AZ_IOT_USE_INSTALLED_PAHO uses an installed
# eclipse-paho-mqtt-c CMake package. Builds a static lib only (no DLL). TLS is
# enabled when OpenSSL is available (required for Azure IoT connections on port 8883).
#
# Registers two factories:
#   - v3.1.1, supported roles: { DPS, HUB_MQTT_V3 }
#   - v5,     supported roles: { HUB_MQTT_V5 }
# Each factory creates a fresh Paho client object per session (no sharing).

if(AZ_IOT_USE_INSTALLED_PAHO)
    # Upstream's package config does not load OpenSSL, which its SSL targets link.
    find_package(OpenSSL QUIET)
    # A target a parent project already imported is visible here and cannot be promoted.
    set(_paho_inherited "")
    foreach(_pt IN ITEMS paho-mqtt3as-static paho-mqtt3as paho-mqtt3a-static paho-mqtt3a)
        if(TARGET eclipse-paho-mqtt-c::${_pt})
            list(APPEND _paho_inherited eclipse-paho-mqtt-c::${_pt})
        endif()
    endforeach()
    find_package(eclipse-paho-mqtt-c 1.3.14 CONFIG REQUIRED)
    # The asynchronous API; static if available, SSL-capable if OpenSSL was found.
    set(_paho_candidates paho-mqtt3a-static paho-mqtt3a)
    if(OPENSSL_FOUND)
        list(PREPEND _paho_candidates paho-mqtt3as-static paho-mqtt3as)
    endif()
    set(_paho_target "")
    foreach(_pt IN LISTS _paho_candidates)
        if(TARGET eclipse-paho-mqtt-c::${_pt})
            set(_paho_target eclipse-paho-mqtt-c::${_pt})
            break()
        endif()
    endforeach()
    if(NOT _paho_target)
        message(FATAL_ERROR "eclipse-paho-mqtt-c (${eclipse-paho-mqtt-c_DIR}) has none of: ${_paho_candidates}"
            " (the SSL targets need OpenSSL, found: ${OPENSSL_FOUND}).")
    endif()
    if(_paho_target MATCHES "3as")
        message(STATUS "Using installed Paho ${eclipse-paho-mqtt-c_VERSION} (${_paho_target}), OpenSSL ${OPENSSL_VERSION} - enabling Paho TLS")
        set(_PAHO_SSL ON)
    else()
        message(STATUS "Using installed Paho ${eclipse-paho-mqtt-c_VERSION} (${_paho_target}) without SSL - Paho TLS disabled (tcp:// only)")
        set(_PAHO_SSL OFF)
    endif()
    get_target_property(_paho_inc ${_paho_target} INTERFACE_INCLUDE_DIRECTORIES)
    # Visible to az_iot_install.cmake and az_iot_pkgconfig.cmake at the top level.
    get_target_property(_paho_global ${_paho_target} IMPORTED_GLOBAL)
    if(NOT _paho_global AND NOT _paho_target IN_LIST _paho_inherited)
        set_property(TARGET ${_paho_target} PROPERTY IMPORTED_GLOBAL TRUE)
    endif()
    unset(_paho_global)
    unset(_paho_inherited)
    # For the package config file (az_iot_install.cmake).
    set(AZ_IOT_PAHO_VERSION "${eclipse-paho-mqtt-c_VERSION}" PARENT_SCOPE)
else()
    include(FetchContent)

    # Keep the commit for this tag in c/cgmanifest.json.
    set(PAHO_C_TAG  "v1.3.16" CACHE STRING "Eclipse Paho MQTT C git tag")
    set(PAHO_C_REPO "https://github.com/eclipse/paho.mqtt.c.git" CACHE STRING "Eclipse Paho MQTT C git repository")
    # The commit patches/ is made against (v1.3.16). Any other commit is built
    # unpatched, with a warning below (docs/dependencies.md).
    set(_paho_patched_commit "4a939ddb01eea581a32fd6f0adcfee51b91d2601")
    find_package(Git REQUIRED)

    # Paho before 1.3.14 declares cmake_minimum_required <3.5 (PAHO_C_TAG may select one),
    # which CMake >= 4.0 refuses outright. Override the floor so the upstream
    # scripts run unmodified.
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "" FORCE)

    # Suppress deprecation warnings from Paho's old cmake_minimum_required().
    set(_sdk_save_warn_deprecated ${CMAKE_WARN_DEPRECATED})
    set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)

    # Paho's cmake_minimum_required resets CMP0091 to OLD, so it ignores the
    # MSVC_RUNTIME_LIBRARY property and uses CMAKE_C_FLAGS_* directly. Replace
    # /MD with /MT so Paho links the static CRT (matching azure-sdk-for-c).
    if(MSVC)
        foreach(_cfg IN ITEMS DEBUG RELEASE MINSIZEREL RELWITHDEBINFO)
            string(REGEX REPLACE "/MD" "/MT" CMAKE_C_FLAGS_${_cfg} "${CMAKE_C_FLAGS_${_cfg}}")
        endforeach()
    endif()

    # Attempt to locate OpenSSL. If found, enable Paho TLS support.
    find_package(OpenSSL QUIET)
    if(OPENSSL_FOUND)
        message(STATUS "OpenSSL found (${OPENSSL_VERSION}) - enabling Paho TLS")
        set(_PAHO_SSL ON)
    else()
        message(STATUS "OpenSSL NOT found - Paho TLS disabled (tcp:// only)")
        set(_PAHO_SSL OFF)
    endif()

    # Tell Paho's CMake what to build before MakeAvailable.
    set(PAHO_BUILD_STATIC        TRUE  CACHE BOOL "" FORCE)
    set(PAHO_BUILD_SHARED        FALSE CACHE BOOL "" FORCE)
    set(PAHO_BUILD_DOCUMENTATION FALSE CACHE BOOL "" FORCE)
    set(PAHO_BUILD_SAMPLES       FALSE CACHE BOOL "" FORCE)
    set(PAHO_ENABLE_TESTING      FALSE CACHE BOOL "" FORCE)
    set(PAHO_HIGH_PERFORMANCE    TRUE  CACHE BOOL "" FORCE)
    set(PAHO_WITH_SSL            ${_PAHO_SSL} CACHE BOOL "" FORCE)
    set(PAHO_WITH_LIBUUID        FALSE CACHE BOOL "" FORCE)

    # Paho v1.3.x calls project() without a VERSION, which trips the CMP0048
    # developer warning under newer CMake. Default the policy to NEW for the fetched
    # subproject so it stays quiet (the empty PROJECT_VERSION vars are harmless here).
    set(CMAKE_POLICY_DEFAULT_CMP0048 NEW)

    # One source directory per tag: the Git update step of a shared one would carry
    # the patches (local changes) over to another PAHO_C_TAG.
    string(MAKE_C_IDENTIFIER "${PAHO_C_TAG}" _paho_tag_dir)
    FetchContent_Declare(
        paho_mqtt_c
        GIT_REPOSITORY ${PAHO_C_REPO}
        GIT_TAG        ${PAHO_C_TAG}
        GIT_SHALLOW    TRUE
        SOURCE_DIR     ${FETCHCONTENT_BASE_DIR}/paho_mqtt_c-${_paho_tag_dir}-src
        PATCH_COMMAND  ${CMAKE_COMMAND}
            -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
            -DPATCH_DIR=${CMAKE_CURRENT_SOURCE_DIR}/patches
            -DPATCHED_COMMIT=${_paho_patched_commit}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/patch_paho.cmake
    )
    FetchContent_MakeAvailable(paho_mqtt_c)

    # The patch step is skipped for another commit and for FETCHCONTENT_SOURCE_DIR_PAHO_MQTT_C,
    # so check the source actually built.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${paho_mqtt_c_SOURCE_DIR}"
        OUTPUT_VARIABLE _paho_head
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _paho_rc
        ERROR_QUIET)
    set(_paho_patched OFF)
    if(_paho_rc EQUAL 0 AND _paho_head STREQUAL _paho_patched_commit)
        set(_paho_patched ON)
        file(GLOB _paho_patches "${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch")
        foreach(_paho_patch IN LISTS _paho_patches)
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_paho_patch}"
                WORKING_DIRECTORY "${paho_mqtt_c_SOURCE_DIR}"
                RESULT_VARIABLE _paho_rc
                OUTPUT_QUIET ERROR_QUIET)
            if(NOT _paho_rc EQUAL 0)
                set(_paho_patched OFF)
            endif()
        endforeach()
    endif()
    if(NOT _paho_patched)
        message(WARNING "Paho at ${paho_mqtt_c_SOURCE_DIR} is not v1.3.16 with "
            "adapters/paho/patches applied, so it keeps the data races those patches fix "
            "(eclipse-paho/paho.mqtt.c#1709, #1714). See docs/dependencies.md.")
    endif()

    # Paho v1.3.x warnings are not fatal and the known ones are silenced, so strict
    # CMAKE_C_FLAGS (-Wall -Wextra -Werror) do not break it. Not -w: that would also
    # disable -Werror=format-security from az_iot_hardening.cmake.
    if(NOT MSVC)
        foreach(_pt IN ITEMS
            paho-mqtt3a-static paho-mqtt3as-static
            paho-mqtt3c-static paho-mqtt3cs-static
            common_obj_static common_ssl_obj_static)
            if(TARGET ${_pt})
                target_compile_options(${_pt} PRIVATE
                    -Wno-error
                    -Wno-overflow -Wno-sign-compare -Wno-unused-parameter -Wno-pedantic
                    -Wno-unused-but-set-variable -Wno-unused-function -Wno-deprecated-declarations
                    -Wno-misleading-indentation)
            endif()
        endforeach()
    endif()

    # Paho < v1.3.15 is not C23-clean (MQTTPacket.h: `typedef unsigned int bool;`).
    # Pin its sources to C99 so a CMAKE_C_STANDARD override applies to first-party
    # code only, as azure-sdk-for-c already does for itself.
    foreach(_pt IN ITEMS
        paho-mqtt3a-static paho-mqtt3as-static
        paho-mqtt3c-static paho-mqtt3cs-static
        common_obj_static common_ssl_obj_static)
        if(TARGET ${_pt})
            set_target_properties(${_pt} PROPERTIES C_STANDARD 99)
        endif()
    endforeach()

    # Restore deprecation warning setting.
    set(CMAKE_WARN_DEPRECATED ${_sdk_save_warn_deprecated} CACHE BOOL "" FORCE)
    unset(_sdk_save_warn_deprecated)

    # Paho's static target for the async API. When SSL is enabled, use the *s*
    # (SSL-capable) variant; otherwise use the plain async target.
    if(_PAHO_SSL)
        set(_paho_target paho-mqtt3as-static)
    else()
        set(_paho_target paho-mqtt3a-static)
    endif()
    if(NOT TARGET ${_paho_target})
        message(FATAL_ERROR "Expected Paho target '${_paho_target}' was not created.")
    endif()

    # Treat Paho headers as SYSTEM so our -Werror -Wstrict-prototypes etc. do not
    # flag legacy upstream declarations. Same trick we use for azure-sdk-for-c.
    get_target_property(_paho_inc ${_paho_target} INTERFACE_INCLUDE_DIRECTORIES)
    if(_paho_inc)
        set_target_properties(${_paho_target} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "")
        target_include_directories(${_paho_target} SYSTEM INTERFACE ${_paho_inc})
    endif()

    # Align MSVC runtime so we don't trip LNK4098.
    if(MSVC)
        set_property(TARGET ${_paho_target} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

add_library(az_iot_adapter_paho STATIC
    az_iot_mqtt_paho.c
    az_iot_paho_key_custody.c
)
add_library(azure::iot::adapter_paho ALIAS az_iot_adapter_paho)

target_include_directories(az_iot_adapter_paho
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
)
target_link_libraries(az_iot_adapter_paho
    PUBLIC
        az_iot_core
    PRIVATE
        ${_paho_target}
)
if(_PAHO_SSL)
    target_compile_definitions(az_iot_adapter_paho PRIVATE AZ_IOT_PAHO_SSL=1)
    target_link_libraries(az_iot_adapter_paho PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# Non-extractable key custody (D8): resolving a "pkcs11:"/"tpm2:" key reference
# through an OpenSSL ENGINE or provider needs the OSSL_STORE/OSSL_ENCODER APIs,
# which are OpenSSL 3.0+. Older OpenSSL (or no OpenSSL) still builds -- the
# adapter then refuses a key reference at connect time with
# AZ_IOT_ERR_NOT_SUPPORTED rather than silently connecting without a key.
if(AZ_IOT_PAHO_KEY_CUSTODY AND _PAHO_SSL AND OPENSSL_VERSION VERSION_GREATER_EQUAL 3.0)
    message(STATUS "Paho adapter: non-extractable key custody enabled (OpenSSL ${OPENSSL_VERSION})")
    target_compile_definitions(az_iot_adapter_paho PRIVATE AZ_IOT_PAHO_KEY_CUSTODY=1)
elseif(AZ_IOT_PAHO_KEY_CUSTODY)
    message(STATUS "Paho adapter: non-extractable key custody disabled (needs OpenSSL 3.0+)")
endif()
az_iot_apply_warnings(az_iot_adapter_paho)
az_iot_apply_coverage(az_iot_adapter_paho)

if(WIN32)
    # Paho on Windows needs ws2_32 + crypt32 even without SSL.
    target_link_libraries(az_iot_adapter_paho PRIVATE ws2_32 crypt32)
endif()
if(UNIX)
    target_link_libraries(az_iot_adapter_paho PRIVATE pthread)
endif()

# Published to the parent scope for the connect-cleanup unit test, which
# compiles az_iot_mqtt_paho.c against a mock MQTTAsync: it needs Paho's headers
# for the types, and must not link the real library.
set(AZ_IOT_PAHO_INCLUDE_DIRS "${_paho_inc}" PARENT_SCOPE)
