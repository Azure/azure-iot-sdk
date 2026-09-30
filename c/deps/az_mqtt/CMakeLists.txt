# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

cmake_minimum_required(VERSION 3.14)
project(az_mqtt LANGUAGES C)

set(CMAKE_C_STANDARD 99)

# ──────────────────────── Options ─────────────────────────────

option(AZ_MQTT_ENABLE_MQTTV3 "Build az_mqttv3 (MQTT 3.1.1 client)" ON)
option(AZ_MQTT_ENABLE_MQTTV5 "Build az_mqttv5 (MQTT 5.0 client)" ON)
option(AZ_MQTT_BUILD_SAMPLES "Build sample applications" ON)
option(AZ_MQTT_BUILD_TESTS "Build tests" ON)
option(AZ_MQTT_WARNINGS_AS_ERRORS "Treat warnings in az_mqtt code (libraries, samples, tests) as errors" OFF)

# TLS backend. auto (default): OpenSSL 3.0+ if found, otherwise none.
#   openssl: require OpenSSL 3.0+.  mbedtls: require mbedTLS.  none: no TLS.
# Windows always uses Schannel.
set(AZ_MQTT_TLS_BACKEND "auto" CACHE STRING "TLS backend: auto | openssl | mbedtls | none")
set_property(CACHE AZ_MQTT_TLS_BACKEND PROPERTY STRINGS auto openssl mbedtls none)
if(NOT AZ_MQTT_TLS_BACKEND MATCHES "^(auto|openssl|mbedtls|none)$")
  message(FATAL_ERROR "AZ_MQTT_TLS_BACKEND must be auto, openssl, mbedtls or none (got '${AZ_MQTT_TLS_BACKEND}')")
endif()

if(NOT AZ_MQTT_ENABLE_MQTTV3 AND NOT AZ_MQTT_ENABLE_MQTTV5)
  message(FATAL_ERROR "Enable at least one of AZ_MQTT_ENABLE_MQTTV3 and AZ_MQTT_ENABLE_MQTTV5")
endif()

# ──────────────────────── Azure SDK for C dependency ──────────

if(WIN32)
  set(AZ_PLATFORM_IMPL "WIN32" CACHE STRING "Azure SDK platform")
else()
  set(AZ_PLATFORM_IMPL "POSIX" CACHE STRING "Azure SDK platform")
endif()

# Don't build Azure SDK tests or samples
set(UNIT_TESTING OFF CACHE BOOL "" FORCE)
set(TRANSPORT_PAHO OFF CACHE BOOL "" FORCE)

add_subdirectory(deps/azure-sdk-for-c)

# azure-sdk-for-c headers are third-party: our warning set must not apply to them.
get_target_property(_az_core_includes az_core INTERFACE_INCLUDE_DIRECTORIES)
set_property(TARGET az_core APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_az_core_includes})

# ──────────────────────── Compiler warnings ───────────────────

# az_mqtt_apply_warnings(<target>): the project's warning set, applied to our
# own targets only (never to azure-sdk-for-c or cmocka).
function(az_mqtt_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(AZ_MQTT_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
      -Wformat=2 -Wcast-qual -Wvla -Wundef -Wnull-dereference -Wimplicit-fallthrough)
    if(AZ_MQTT_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# ──────────────────────── Libraries ───────────────────────────
#
#   az_mqtt_core (az_mqtt::core)   transport, packet framing, keep-alive, session
#                                  state, wire primitives. Version-independent.
#   az_mqttv3    (az_mqtt::mqttv3) MQTT 3.1.1 client API and codec, on the core.
#   az_mqttv5    (az_mqtt::mqttv5) MQTT 5.0 client API and codec, on the core.
#
# An application links the version(s) it uses; each carries nothing of the
# other, and both can be linked together (sharing one core).

set(AZ_MQTT_CORE_SOURCES
  src/core/az_mqtt_core.c
  src/core/az_mqtt_codec_common.c
)

if(WIN32)
  list(APPEND AZ_MQTT_CORE_SOURCES src/platform/transport_win32.c)
elseif(AZ_MQTT_TLS_BACKEND STREQUAL "mbedtls")
  list(APPEND AZ_MQTT_CORE_SOURCES src/platform/transport_mbedtls.c src/platform/az_mqtt_socket_posix.c)
else()
  list(APPEND AZ_MQTT_CORE_SOURCES src/platform/transport_posix.c src/platform/az_mqtt_socket_posix.c)
endif()

add_library(az_mqtt_core ${AZ_MQTT_CORE_SOURCES})
add_library(az_mqtt::core ALIAS az_mqtt_core)

target_include_directories(az_mqtt_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
    $<INSTALL_INTERFACE:include>
)
target_include_directories(az_mqtt_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)

target_link_libraries(az_mqtt_core PUBLIC az::core)

# Wire the selected TLS backend.
if(WIN32)
  target_compile_definitions(az_mqtt_core PRIVATE AZ_MQTT_TLS_SCHANNEL)
  target_link_libraries(az_mqtt_core PRIVATE secur32 crypt32 ws2_32)
  set(AZ_MQTT_TLS_BACKEND_RESOLVED "schannel")
  message(STATUS "az_mqtt: TLS backend = Schannel")
elseif(AZ_MQTT_TLS_BACKEND STREQUAL "mbedtls")
  target_compile_definitions(az_mqtt_core PRIVATE AZ_MQTT_TLS_MBEDTLS)
  set(AZ_MQTT_TLS_BACKEND_RESOLVED "mbedtls")
  # mbedTLS 3.x/4.x install a CMake package; its MbedTLS::mbedtls target brings
  # x509 and crypto (mbedcrypto in 3.x, tfpsacrypto in 4.x).
  find_package(MbedTLS CONFIG QUIET)
  if(TARGET MbedTLS::mbedtls)
    target_link_libraries(az_mqtt_core PRIVATE MbedTLS::mbedtls)
  else()
    find_library(MBEDTLS_SSL_LIB    NAMES mbedtls    REQUIRED)
    find_library(MBEDTLS_X509_LIB   NAMES mbedx509   REQUIRED)
    find_library(MBEDTLS_CRYPTO_LIB NAMES mbedcrypto REQUIRED)
    find_path(MBEDTLS_INCLUDE_DIR NAMES mbedtls/ssl.h REQUIRED)
    target_include_directories(az_mqtt_core PRIVATE ${MBEDTLS_INCLUDE_DIR})
    # Order matters: ssl depends on x509 depends on crypto.
    target_link_libraries(az_mqtt_core PRIVATE
        ${MBEDTLS_SSL_LIB} ${MBEDTLS_X509_LIB} ${MBEDTLS_CRYPTO_LIB})
  endif()
  message(STATUS "az_mqtt: TLS backend = mbedTLS")
elseif(AZ_MQTT_TLS_BACKEND STREQUAL "openssl" OR AZ_MQTT_TLS_BACKEND STREQUAL "auto")
  if(AZ_MQTT_TLS_BACKEND STREQUAL "openssl")
    find_package(OpenSSL 3.0 REQUIRED)
  else()
    find_package(OpenSSL 3.0 QUIET)
  endif()
  if(OpenSSL_FOUND)
    target_compile_definitions(az_mqtt_core PRIVATE AZ_MQTT_TLS_OPENSSL)
    target_link_libraries(az_mqtt_core PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    set(AZ_MQTT_TLS_BACKEND_RESOLVED "openssl")
    message(STATUS "az_mqtt: TLS backend = OpenSSL")
  else()
    # TLS connects fail with AZ_MQTT_ERROR_NOT_SUPPORTED; they never fall back to plaintext.
    message(WARNING "az_mqtt: TLS support disabled (OpenSSL 3.0+ not found)")
    set(AZ_MQTT_TLS_BACKEND_RESOLVED "none")
  endif()
else()
  message(STATUS "az_mqtt: TLS support disabled (backend = none)")
  set(AZ_MQTT_TLS_BACKEND_RESOLVED "none")
endif()

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(az_mqtt_core PRIVATE Threads::Threads)
endif()

az_mqtt_apply_warnings(az_mqtt_core)

# az_mqtt_add_version(<N>): library az_mqttv<N> (alias az_mqtt::mqttv<N>).
function(az_mqtt_add_version version)
  add_library(az_mqttv${version}
    src/mqtt${version}/az_mqtt${version}_client.c
    src/mqtt${version}/az_mqtt${version}_codec.c)
  target_include_directories(az_mqttv${version} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
  target_link_libraries(az_mqttv${version} PUBLIC az_mqtt_core)
  az_mqtt_apply_warnings(az_mqttv${version})
  add_library(az_mqtt::mqttv${version} ALIAS az_mqttv${version})
endfunction()

if(AZ_MQTT_ENABLE_MQTTV3)
  az_mqtt_add_version(3)
endif()
if(AZ_MQTT_ENABLE_MQTTV5)
  az_mqtt_add_version(5)
endif()

# ──────────────────────── Samples ─────────────────────────────

if(AZ_MQTT_BUILD_SAMPLES)
  add_subdirectory(samples)
endif()

# ──────────────────────── Tests ───────────────────────────────

if(AZ_MQTT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
