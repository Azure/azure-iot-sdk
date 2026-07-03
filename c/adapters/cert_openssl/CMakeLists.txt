# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# OpenSSL "managed" certificate provider (D5).
#
# Provides az_iot_certificate_provider_managed_init() — a ready-to-use
# az_iot_certificate_provider_t for CSR-based enrollment: owns an operational
# key, produces PKCS#10 CSRs, and persists the issued cert chain. Requires
# OpenSSL 3.0+; the core library links no crypto, so this optional adapter is
# the only place OpenSSL is pulled into the certificate path.

find_package(OpenSSL 3.0 QUIET)
if(NOT OPENSSL_FOUND)
    message(STATUS "OpenSSL 3.0+ NOT found - managed certificate provider disabled")
    return()
endif()

message(STATUS "OpenSSL found (${OPENSSL_VERSION}) - building managed certificate provider")

add_library(az_iot_certificate_provider_managed STATIC
    az_iot_certificate_provider_managed.c
)
add_library(azure::iot::certificate_provider_managed ALIAS az_iot_certificate_provider_managed)

target_include_directories(az_iot_certificate_provider_managed
    PUBLIC
        ${CMAKE_SOURCE_DIR}/inc
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(az_iot_certificate_provider_managed
    PUBLIC
        az_iot_core
    PRIVATE
        OpenSSL::Crypto
)
az_iot_apply_warnings(az_iot_certificate_provider_managed)
