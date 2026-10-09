# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# az_mqtt MQTT adapter (AZ_IOT_WITH_AZ_MQTT). Builds az_mqtt from deps/az_mqtt with the SDK's
# azure-sdk-for-c, then the adapter: one library with the MQTT 3.1.1 and MQTT 5 factories.

# Per-client sizes: defaults in az_iot_az_mqtt_config.h. Each option below is passed to the
# compiler only when set, so a value from AZ_IOT_AZ_MQTT_CONFIG_FILE or the footprint applies.
if(DEFINED AZ_IOT_AZ_MQTT_BUFFER_SIZE)
    message(FATAL_ERROR "AZ_IOT_AZ_MQTT_BUFFER_SIZE was replaced by AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE "
        "and AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE: unset it (-UAZ_IOT_AZ_MQTT_BUFFER_SIZE)")
endif()
set(AZ_IOT_AZ_MQTT_FOOTPRINT "" CACHE STRING
    "az_mqtt adapter: size defaults, DEFAULT or CONSTRAINED (empty: DEFAULT)")
set_property(CACHE AZ_IOT_AZ_MQTT_FOOTPRINT PROPERTY STRINGS "" DEFAULT CONSTRAINED)
set(AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE "" CACHE STRING
    "az_mqtt adapter: largest outgoing packet (empty: footprint's)")
set(AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE "" CACHE STRING
    "az_mqtt adapter: largest incoming packet; MQTT 5: also the Maximum Packet Size advertised (empty: footprint's)")
set(AZ_IOT_AZ_MQTT_INFLIGHT_MAX "" CACHE STRING
    "az_mqtt adapter: requests in flight at once, 1-32767; MQTT 5: also the Receive Maximum advertised (empty: footprint's)")
set(AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX "" CACHE STRING
    "az_mqtt adapter: MQTT 5 user properties per packet (empty: footprint's)")
set(AZ_IOT_AZ_MQTT_STATIC_CLIENTS "" CACHE STRING
    "az_mqtt adapter: clients of each MQTT version in static storage, no heap; 0 or empty: allocated when created")
set(AZ_IOT_AZ_MQTT_TRANSPORT_SIZE "" CACHE STRING
    "az_mqtt adapter, static clients: bytes for the transport (empty: 200 KiB on Windows, 8 KiB elsewhere)")
set(AZ_IOT_AZ_MQTT_CONNECT_STRINGS_SIZE "" CACHE STRING
    "az_mqtt adapter, static clients: bytes for a connect's strings (empty: 8 KiB)")
set(AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE "" CACHE STRING
    "az_mqtt adapter: store of unacknowledged QoS 1/2 PUBLISH; 0: no session outliving the connection (empty: send size + 18)")
set(AZ_IOT_AZ_MQTT_CONFIG_FILE "" CACHE STRING
    "az_mqtt adapter: header with size definitions, as written in a quoted #include (empty: none)")

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
add_subdirectory(${PROJECT_SOURCE_DIR}/deps/az_mqtt ${CMAKE_CURRENT_BINARY_DIR}/az_mqtt)
set(BUILD_SHARED_LIBS "${_az_iot_shared}")
set(CMAKE_POSITION_INDEPENDENT_CODE "${_az_iot_pic}")
unset(_az_iot_shared)
unset(_az_iot_pic)

add_library(az_iot_adapter_az_mqtt STATIC
    az_iot_mqtt_az_mqtt.c
    az_iot_mqtt_az_mqtt_v3.c
    az_iot_mqtt_az_mqtt_v5.c
)
add_library(azure::iot::adapter_az_mqtt ALIAS az_iot_adapter_az_mqtt)

target_include_directories(az_iot_adapter_az_mqtt
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
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
foreach(_v SEND_BUFFER_SIZE RECEIVE_BUFFER_SIZE INFLIGHT_MAX USER_PROPERTIES_MAX STATIC_CLIENTS
        TRANSPORT_SIZE CONNECT_STRINGS_SIZE MESSAGE_STORAGE_SIZE)
    if(NOT AZ_IOT_AZ_MQTT_${_v} STREQUAL "")
        target_compile_definitions(az_iot_adapter_az_mqtt PRIVATE
            AZ_IOT_AZ_MQTT_${_v}=${AZ_IOT_AZ_MQTT_${_v}})
    endif()
endforeach()
unset(_v)
if(NOT AZ_IOT_AZ_MQTT_CONFIG_FILE STREQUAL "")
    target_compile_definitions(az_iot_adapter_az_mqtt PRIVATE
        AZ_IOT_AZ_MQTT_CONFIG_FILE="${AZ_IOT_AZ_MQTT_CONFIG_FILE}")
endif()
if(_az_mqtt_tls STREQUAL "openssl")
    # Key references (client_key_uri) load an OpenSSL 3 provider.
    find_package(OpenSSL 3.0 REQUIRED)
    target_compile_definitions(az_iot_adapter_az_mqtt PRIVATE AZ_IOT_AZ_MQTT_OPENSSL)
    target_link_libraries(az_iot_adapter_az_mqtt PRIVATE OpenSSL::Crypto)
endif()
az_iot_apply_warnings(az_iot_adapter_az_mqtt)
az_iot_apply_coverage(az_iot_adapter_az_mqtt)
