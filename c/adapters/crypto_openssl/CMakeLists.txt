# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# OpenSSL crypto backend: az_iot_crypto_openssl(). Requires OpenSSL 3.0+.
# Core links no crypto; applications pass this backend to the connection client.

find_package(OpenSSL 3.0 QUIET)
if(NOT OPENSSL_FOUND)
    # Under coverage this adapter silently leaving the build would shrink the
    # denominator and RAISE the reported number, so refuse to configure instead.
    if(AZ_IOT_ENABLE_COVERAGE)
        message(FATAL_ERROR
            "OpenSSL 3.0+ not found, but AZ_IOT_ENABLE_COVERAGE is ON. Shipped "
            "adapters may not be dropped from a coverage build.")
    endif()
    message(STATUS "OpenSSL 3.0+ NOT found - OpenSSL crypto backend disabled")
    return()
endif()

message(STATUS "OpenSSL found (${OPENSSL_VERSION}) - building OpenSSL crypto backend")

add_library(az_iot_crypto_openssl STATIC
    az_iot_crypto_openssl.c
)
add_library(azure::iot::crypto_openssl ALIAS az_iot_crypto_openssl)

target_include_directories(az_iot_crypto_openssl
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(az_iot_crypto_openssl
    PUBLIC
        az_iot_core
    PRIVATE
        OpenSSL::Crypto
)
az_iot_apply_warnings(az_iot_crypto_openssl)
az_iot_apply_coverage(az_iot_crypto_openssl)
