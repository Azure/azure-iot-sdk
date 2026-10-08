# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 3.21)

# The version comes from az_iot_version.h, the single source of truth (non-CMake
# builds such as ESP-IDF compile it directly). project() takes only the numeric
# part; AZ_IOT_VERSION_FULL carries the pre-release label too.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/inc/azure/iot/az_iot_version.h" _az_iot_ver_lines
    REGEX "^#define AZ_IOT_VERSION_(STRING|MAJOR|MINOR|PATCH|PRERELEASE) ")
foreach(_l IN LISTS _az_iot_ver_lines)
    if(_l MATCHES "^#define AZ_IOT_VERSION_([A-Z]+) \"?([^\"]*)\"?$")
        set(_az_iot_ver_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endif()
endforeach()
set(_az_iot_ver_expected "${_az_iot_ver_MAJOR}.${_az_iot_ver_MINOR}.${_az_iot_ver_PATCH}")
if(DEFINED _az_iot_ver_PRERELEASE)
    string(APPEND _az_iot_ver_expected "-${_az_iot_ver_PRERELEASE}")
endif()
# SemVer 2.0.0 without build metadata; keep in sync with eng/check-version.sh.
set(_az_iot_ver_num "(0|[1-9][0-9]*)")
set(_az_iot_ver_id "(0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)")
if(NOT _az_iot_ver_STRING MATCHES
   "^${_az_iot_ver_num}\\.${_az_iot_ver_num}\\.${_az_iot_ver_num}(-${_az_iot_ver_id}(\\.${_az_iot_ver_id})*)?$")
    message(FATAL_ERROR "az_iot_version.h: AZ_IOT_VERSION_STRING \"${_az_iot_ver_STRING}\" "
        "is not a valid SemVer MAJOR.MINOR.PATCH[-PRERELEASE].")
endif()
if(NOT _az_iot_ver_STRING STREQUAL _az_iot_ver_expected)
    message(FATAL_ERROR "az_iot_version.h: AZ_IOT_VERSION_STRING \"${_az_iot_ver_STRING}\" "
        "does not match the component macros (\"${_az_iot_ver_expected}\").")
endif()
set(AZ_IOT_VERSION_FULL "${_az_iot_ver_STRING}")

project(azure-iot-sdk
    VERSION ${_az_iot_ver_MAJOR}.${_az_iot_ver_MINOR}.${_az_iot_ver_PATCH}
    DESCRIPTION "C99 client SDK for Azure IoT Hub (MQTTv3 + MQTTv5)"
    LANGUAGES C
)

# Re-configure when the version header changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/inc/azure/iot/az_iot_version.h")

# C99 is the baseline. Overridable (-DCMAKE_C_STANDARD=11|17|23) so CI can
# check forward compatibility with later standards.
if(NOT DEFINED CMAKE_C_STANDARD)
    set(CMAKE_C_STANDARD 99)
endif()
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Keep MSVC C runtime aligned across this project and FetchContent'd dependencies.
# We use /MT[d] (static CRT) to match azure-sdk-for-c and produce self-contained
# binaries with no Visual C++ Redistributable dependency.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Windows finds DLLs next to the executable: keep them together.
if(WIN32 AND BUILD_SHARED_LIBS AND NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
endif()

include(az_iot_options)
include(compiler_warnings)
include(az_iot_coverage)
# Before any dependency is fetched, so dependencies are hardened too.
include(az_iot_hardening)

# azure-sdk-for-c is a mandatory dependency.
#   az::core                 — spans, JSON, log, contexts, result codes.
#   az::iot::hub             — MQTTv3 hub MQTT topic build/parse.
#   az::iot::provisioning    — Azure DPS MQTT topic build/parse.
# All three are MQTT-stack-agnostic; we feed their topic strings into our
# adapter. Upstream is no longer published as a vcpkg port; the project's own
# README recommends FetchContent. We pin to a release tag for reproducibility.
include(FetchContent)

# azure-sdk-for-c's AzureVcpkg.cmake still uses FetchContent_Populate() which
# CMake >= 3.30 deprecates (CMP0169). Suppress the resulting dev warning until
# upstream migrates. The policy set here is overridden by azure-sdk-for-c's own
# cmake_minimum_required(), so we also suppress developer warnings temporarily.
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ON CACHE INTERNAL "")
# Keep the commit for this tag in c/cgmanifest.json.
set(AZ_SDK_C_TAG "1.5.0" CACHE STRING "azure-sdk-for-c git tag/branch")
set(AZ_SDK_C_REPO "https://github.com/Azure/azure-sdk-for-c.git" CACHE STRING "azure-sdk-for-c git repository")

# Disable the parts we never use to keep the build small.
set(AZ_NO_PRECONDITION_CHECKING OFF CACHE BOOL "" FORCE)
set(WARNINGS_AS_ERRORS         OFF CACHE BOOL "" FORCE)
set(UNIT_TESTING               OFF CACHE BOOL "" FORCE)
set(TRANSPORT_CURL             OFF CACHE BOOL "" FORCE)
set(TRANSPORT_PAHO             OFF CACHE BOOL "" FORCE)
set(BUILD_SAMPLES              OFF CACHE BOOL "" FORCE)
set(AZ_SDK_C_NO_SAMPLES        ON  CACHE BOOL "" FORCE)
if(WIN32)
    set(AZ_PLATFORM_IMPL "WIN32" CACHE STRING "" FORCE)
elseif(UNIX)
    set(AZ_PLATFORM_IMPL "POSIX" CACHE STRING "" FORCE)
endif()

FetchContent_Declare(
    azure_sdk_for_c
    GIT_REPOSITORY ${AZ_SDK_C_REPO}
    GIT_TAG        ${AZ_SDK_C_TAG}
    GIT_SHALLOW    TRUE
)
# Always static: az_core and its platform library depend on each other, which
# CMake allows only between static libraries. Position-independent in a shared
# build, so it links into our shared libraries.
set(_az_iot_shared "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
set(_az_iot_pic "${CMAKE_POSITION_INDEPENDENT_CODE}")
if(_az_iot_shared)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
FetchContent_MakeAvailable(azure_sdk_for_c)
set(BUILD_SHARED_LIBS "${_az_iot_shared}")
set(CMAKE_POSITION_INDEPENDENT_CODE "${_az_iot_pic}")
unset(_az_iot_shared)
unset(_az_iot_pic)

# Restore developer warnings for our own code.
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS OFF CACHE INTERNAL "")

# az_posix.c uses usleep()/useconds_t which require POSIX feature-test macros
# under glibc on modern Ubuntu. Upstream doesn't set them; inject here.
if(TARGET az_posix)
    target_compile_definitions(az_posix PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
endif()

# Treat azure-sdk-for-c headers as SYSTEM so our strict warnings don't flag
# upstream code (e.g. legacy "func()" declarations vs -Wstrict-prototypes).
foreach(_az_tgt IN ITEMS az_core az_iot_common az_iot_hub az_iot_provisioning az_win32 az_posix az_nohttp)
    if(TARGET ${_az_tgt})
        get_target_property(_az_inc ${_az_tgt} INTERFACE_INCLUDE_DIRECTORIES)
        if(_az_inc)
            set_target_properties(${_az_tgt} PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES ""
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_az_inc}")
            target_include_directories(${_az_tgt} SYSTEM INTERFACE ${_az_inc})
        endif()
    endif()
endforeach()

# azure-sdk-for-c uses cmake_minimum_required(3.10) which downgrades CMP0091,
# so MSVC_RUNTIME_LIBRARY is not honored for its targets. Its own
# eng/cmake/global_compile_options.cmake injects raw /MT[d] flags via
# add_compile_options(), which is fine — we now match that choice.

add_subdirectory(src)

if(AZ_IOT_WITH_PAHO)
    add_subdirectory(adapters/paho)
endif()

if(AZ_IOT_WITH_AZ_MQTT)
    add_subdirectory(adapters/az_mqtt)
endif()

if(AZ_IOT_WITH_RUST_MQTT)
    add_subdirectory(adapters/rust_mqtt)
endif()

if(AZ_IOT_WITH_CRYPTO_OPENSSL)
    add_subdirectory(adapters/crypto_openssl)
endif()

if(AZ_IOT_WITH_CRYPTO_MBEDTLS)
    add_subdirectory(adapters/crypto_mbedtls)
endif()

if(AZ_IOT_WITH_CERT_PROVIDER_MANAGED)
    add_subdirectory(adapters/cert_openssl)
endif()

if(AZ_IOT_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

if(AZ_IOT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Declared last so it sees every instrumented target.
az_iot_add_coverage_target()

include(az_iot_install)
