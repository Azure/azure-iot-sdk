# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# az_mqtt MQTT adapter (AZ_IOT_WITH_AZ_MQTT). Builds az_mqtt from deps/az_mqtt with the SDK's
# azure-sdk-for-c, then the adapter: one library with the MQTT 3.1.1 and MQTT 5 clients.

# Compile-time limits: defaults in az_iot_az_mqtt_config.h. Each option below is passed to the
# compiler only when set, so a value from AZ_IOT_AZ_MQTT_CONFIG_FILE or the footprint applies.
# Buffer sizes and requests in flight are run-time values (az_iot_mqtt_limits).
foreach(_v BUFFER_SIZE SEND_BUFFER_SIZE RECEIVE_BUFFER_SIZE INFLIGHT_MAX)
    if(DEFINED AZ_IOT_AZ_MQTT_${_v} AND NOT AZ_IOT_AZ_MQTT_${_v} STREQUAL "")
        message(FATAL_ERROR "AZ_IOT_AZ_MQTT_${_v} was removed: set the MQTT client's sizes in "
            "az_iot_connection_client_options::mqtt_limits, and unset it (-UAZ_IOT_AZ_MQTT_${_v})")
    endif()
    unset(AZ_IOT_AZ_MQTT_${_v} CACHE) # The empty entry an earlier configure left.
endforeach()
unset(_v)
set(AZ_IOT_AZ_MQTT_FOOTPRINT "" CACHE STRING
    "az_mqtt adapter: limit defaults, DEFAULT or CONSTRAINED (empty: DEFAULT)")
set_property(CACHE AZ_IOT_AZ_MQTT_FOOTPRINT PROPERTY STRINGS "" DEFAULT CONSTRAINED)
set(AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX "" CACHE STRING
    "az_mqtt adapter: MQTT 5 user properties on CONNECT (empty: footprint's)")
set(AZ_IOT_AZ_MQTT_CONFIG_FILE "" CACHE STRING
    "az_mqtt adapter: header with limit definitions, as written in a quoted #include (empty: none)")

if(WIN32)
    set(_az_mqtt_tls schannel)
elseif(AZ_IOT_AZ_MQTT_TLS STREQUAL "openssl" OR AZ_IOT_AZ_MQTT_TLS STREQUAL "mbedtls")
    set(_az_mqtt_tls ${AZ_IOT_AZ_MQTT_TLS})
else()
    message(FATAL_ERROR "AZ_IOT_AZ_MQTT_TLS must be openssl or mbedtls (got '${AZ_IOT_AZ_MQTT_TLS}')")
endif()

# az_mqtt: static (as azure-sdk-for-c), position-independent in a shared build, no samples or
# tests, warnings as errors as for our code. Its cmake_minimum_required predates CMP0091, which
# would leave it on another MSVC runtime than the SDK.
if(WIN32)
    set(AZ_MQTT_TLS_BACKEND auto CACHE STRING "" FORCE) # Always Schannel.
else()
    set(AZ_MQTT_TLS_BACKEND ${_az_mqtt_tls} CACHE STRING "" FORCE)
endif()
set(AZ_MQTT_ENABLE_MQTTV3 ON CACHE BOOL "" FORCE)
set(AZ_MQTT_ENABLE_MQTTV5 ON CACHE BOOL "" FORCE)
set(AZ_MQTT_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(AZ_MQTT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AZ_MQTT_BUILD_FUZZERS OFF CACHE BOOL "" FORCE) # Its sources are not vendored.
set(AZ_MQTT_WARNINGS_AS_ERRORS ${AZ_IOT_WARNINGS_AS_ERRORS} CACHE BOOL "" FORCE)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(_az_iot_shared "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
set(_az_iot_pic "${CMAKE_POSITION_INDEPENDENT_CODE}")
if(_az_iot_shared)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
# deps/az_mqtt reuses azure-sdk-for-c when target az_core exists, else builds its
# own. An installed azure-sdk-for-c is az::core: alias it, in this directory only.
if(NOT TARGET az_core)
    add_library(az_core ALIAS az::core)
endif()
add_subdirectory(${PROJECT_SOURCE_DIR}/deps/az_mqtt ${CMAKE_CURRENT_BINARY_DIR}/az_mqtt)
set(BUILD_SHARED_LIBS "${_az_iot_shared}")
set(CMAKE_POSITION_INDEPENDENT_CODE "${_az_iot_pic}")
unset(_az_iot_shared)
unset(_az_iot_pic)

add_library(az_iot_adapter_az_mqtt STATIC
    az_iot_mqtt_az_mqtt.c
    az_iot_mqtt_az_mqtt_v3.c
    az_iot_mqtt_az_mqtt_v5.c
    az_iot_mqtt_az_mqtt_adapter.c
)
add_library(azure::iot::adapter_az_mqtt ALIAS az_iot_adapter_az_mqtt)

target_include_directories(az_iot_adapter_az_mqtt
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/core # internal/crypto.h: az_iot_crypto__wipe()
)
target_link_libraries(az_iot_adapter_az_mqtt
    PUBLIC
        az_iot_core
    PRIVATE
        az_mqttv3
        az_mqttv5
)
string(TOUPPER "${AZ_IOT_AZ_MQTT_FOOTPRINT}" _az_mqtt_footprint)
if(_az_mqtt_footprint MATCHES "^(DEFAULT|CONSTRAINED)$")
    target_compile_definitions(az_iot_adapter_az_mqtt PRIVATE
        AZ_IOT_AZ_MQTT_FOOTPRINT=AZ_IOT_AZ_MQTT_FOOTPRINT_${_az_mqtt_footprint})
elseif(NOT _az_mqtt_footprint STREQUAL "")
    message(FATAL_ERROR
        "AZ_IOT_AZ_MQTT_FOOTPRINT must be DEFAULT or CONSTRAINED (got '${AZ_IOT_AZ_MQTT_FOOTPRINT}')")
endif()
unset(_az_mqtt_footprint)
if(NOT AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX STREQUAL "")
    target_compile_definitions(az_iot_adapter_az_mqtt PRIVATE
        AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX=${AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX})
endif()
if(NOT AZ_IOT_AZ_MQTT_CONFIG_FILE STREQUAL "")
    target_compile_definitions(az_iot_adapter_az_mqtt PRIVATE
        AZ_IOT_AZ_MQTT_CONFIG_FILE="${AZ_IOT_AZ_MQTT_CONFIG_FILE}")
endif()
if(_az_mqtt_tls STREQUAL "openssl")
    # Key references (client_key_uri) load an OpenSSL 3 provider.
    find_package(OpenSSL 3.0 REQUIRED)
    target_compile_definitions(az_iot_adapter_az_mqtt PRIVATE AZ_IOT_AZ_MQTT_OPENSSL)
    target_link_libraries(az_iot_adapter_az_mqtt PRIVATE OpenSSL::Crypto)
elseif(_az_mqtt_tls STREQUAL "mbedtls")
    # PSA key ids (client_key_psa_id) need the mbedTLS backend.
    target_compile_definitions(az_iot_adapter_az_mqtt PRIVATE AZ_IOT_AZ_MQTT_MBEDTLS)
endif()
az_iot_apply_warnings(az_iot_adapter_az_mqtt)
az_iot_apply_coverage(az_iot_adapter_az_mqtt)
