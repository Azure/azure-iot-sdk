# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# OpenSSL crypto adapter for the ADU client (Phase 2).
#
# Provides az_iot_adu_crypto_openssl_hooks() — the verify_rs256 + SHA-256
# primitives the ADU core needs. Requires OpenSSL 3.0+. ADU core itself links no
# crypto; this adapter is the only place OpenSSL is pulled into the ADU path.

find_package(OpenSSL 3.0 QUIET)
if(NOT OPENSSL_FOUND)
    message(STATUS "OpenSSL 3.0+ NOT found - ADU OpenSSL crypto adapter disabled")
    return()
endif()

message(STATUS "OpenSSL found (${OPENSSL_VERSION}) - building ADU OpenSSL crypto adapter")

add_library(az_iot_adu_crypto_openssl STATIC
    az_iot_adu_crypto_openssl.c
)
add_library(azure::iot::adu_crypto_openssl ALIAS az_iot_adu_crypto_openssl)

target_include_directories(az_iot_adu_crypto_openssl
    PUBLIC
        ${CMAKE_SOURCE_DIR}/inc
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(az_iot_adu_crypto_openssl
    PUBLIC
        az_iot_core
    PRIVATE
        OpenSSL::Crypto
)
az_iot_apply_warnings(az_iot_adu_crypto_openssl)
