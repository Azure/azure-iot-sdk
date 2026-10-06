# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# az_mqtt MQTT adapter (AZ_IOT_WITH_AZ_MQTT). Builds az_mqtt from deps/az_mqtt with the SDK's
# azure-sdk-for-c, then the adapter: one library with the MQTT 3.1.1 and MQTT 5 factories.

# Per-client buffer sizes (each client allocates them at create time).
set(AZ_IOT_AZ_MQTT_BUFFER_SIZE "270336" CACHE STRING
    "az_mqtt adapter: send and receive buffer size, each (the largest packet)")
set(AZ_IOT_AZ_MQTT_INFLIGHT_MAX "64" CACHE STRING
    "az_mqtt adapter: requests in flight at once (MQTT 5: also the Receive Maximum advertised)")
set(AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX "16" CACHE STRING
    "az_mqtt adapter: MQTT 5 user properties per packet")

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
target_compile_definitions(az_iot_adapter_az_mqtt PRIVATE
    AZ_IOT_AZ_MQTT_BUFFER_SIZE=${AZ_IOT_AZ_MQTT_BUFFER_SIZE}
    AZ_IOT_AZ_MQTT_INFLIGHT_MAX=${AZ_IOT_AZ_MQTT_INFLIGHT_MAX}
    AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX=${AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX}
)
if(_az_mqtt_tls STREQUAL "openssl")
    # Key references (client_key_uri) load an OpenSSL 3 provider.
    find_package(OpenSSL 3.0 REQUIRED)
    target_compile_definitions(az_iot_adapter_az_mqtt PRIVATE AZ_IOT_AZ_MQTT_OPENSSL)
    target_link_libraries(az_iot_adapter_az_mqtt PRIVATE OpenSSL::Crypto)
endif()
az_iot_apply_warnings(az_iot_adapter_az_mqtt)
az_iot_apply_coverage(az_iot_adapter_az_mqtt)
