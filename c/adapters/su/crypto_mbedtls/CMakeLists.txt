# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# mbedTLS (PSA Crypto) adapter for the software updates client, host build.
#
# Provides az_iot_su_crypto_mbedtls_hooks(). Built only against a maintained mbedTLS
# branch: 3.6 LTS or 4.1+. ESP-IDF builds compile the source directly (samples/su/esp32).
# Point CMAKE_PREFIX_PATH (or MbedTLS_DIR) at an mbedTLS install that ships its CMake
# package; eng/install-mbedtls.sh produces one.

find_package(MbedTLS CONFIG QUIET)
if(NOT MbedTLS_FOUND)
    message(STATUS "mbedTLS NOT found - software updates mbedTLS crypto adapter disabled")
    return()
endif()

if(NOT (MbedTLS_VERSION VERSION_GREATER_EQUAL 4.1
        OR (MbedTLS_VERSION VERSION_GREATER_EQUAL 3.6 AND MbedTLS_VERSION VERSION_LESS 3.7)))
    message(STATUS "mbedTLS ${MbedTLS_VERSION} is not a maintained branch (3.6 LTS or 4.1+) - "
                   "software updates mbedTLS crypto adapter disabled")
    return()
endif()

# 4.x moved the crypto library into TF-PSA-Crypto; 3.6 still ships it as mbedcrypto.
if(TARGET MbedTLS::tfpsacrypto)
    set(_az_iot_su_mbedtls_crypto MbedTLS::tfpsacrypto)
else()
    set(_az_iot_su_mbedtls_crypto MbedTLS::mbedcrypto)
endif()

message(STATUS "mbedTLS found (${MbedTLS_VERSION}) - building software updates mbedTLS crypto adapter")

add_library(az_iot_su_crypto_mbedtls STATIC
    az_iot_su_crypto_mbedtls.c
)
add_library(azure::iot::su_crypto_mbedtls ALIAS az_iot_su_crypto_mbedtls)

target_include_directories(az_iot_su_crypto_mbedtls
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(az_iot_su_crypto_mbedtls
    PUBLIC
        az_iot_core
    PRIVATE
        ${_az_iot_su_mbedtls_crypto}
)
az_iot_apply_warnings(az_iot_su_crypto_mbedtls)
az_iot_apply_coverage(az_iot_su_crypto_mbedtls)
unset(_az_iot_su_mbedtls_crypto)
