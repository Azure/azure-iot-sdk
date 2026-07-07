# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Authentication / certificate-management samples. See README.md for the full
# scenario matrix and the environment variables each sample expects.

set(_auth_common ${CMAKE_CURRENT_SOURCE_DIR}/../common)

# BYO-provider template: self-contained, no MQTT/crypto deps, does not connect.
# Always built so the vtable contract stays compiling.
add_executable(az_iot_sample_auth_custom_provider_template
    custom_provider_template/main.c)
target_link_libraries(az_iot_sample_auth_custom_provider_template PRIVATE az_iot_core)
az_iot_apply_warnings(az_iot_sample_auth_custom_provider_template)

# The DPS-CSR and hub-renewal samples need the Paho adapter to connect and the
# OpenSSL "managed" provider for real CSR generation + persistence.
if(AZ_IOT_WITH_PAHO AND TARGET az_iot_certificate_provider_managed)
    add_executable(az_iot_sample_auth_dps_csr_managed
        dps_csr_managed/main.c ${_auth_common}/sample_utils.c)
    target_include_directories(az_iot_sample_auth_dps_csr_managed PRIVATE ${_auth_common})
    target_link_libraries(az_iot_sample_auth_dps_csr_managed
        PRIVATE az_iot_core az_iot_adapter_paho az_iot_certificate_provider_managed)
    az_iot_apply_warnings(az_iot_sample_auth_dps_csr_managed)

    add_executable(az_iot_sample_auth_hub_renew
        hub_renew/main.c ${_auth_common}/sample_utils.c)
    target_include_directories(az_iot_sample_auth_hub_renew PRIVATE ${_auth_common})
    target_link_libraries(az_iot_sample_auth_hub_renew
        PRIVATE az_iot_core az_iot_adapter_paho az_iot_certificate_provider_managed)
    az_iot_apply_warnings(az_iot_sample_auth_hub_renew)
endif()

# The complete app-owned custom provider: real PKCS#10 CSR issuance from
# samples/common using platform-native crypto - CNG on Windows, OpenSSL
# elsewhere. Needs the Paho adapter; on non-Windows it needs OpenSSL 3.0+.
if(AZ_IOT_WITH_PAHO)
    set(_csr_backend_src "")
    set(_csr_backend_libs "")
    if(WIN32)
        set(_csr_backend_src ${_auth_common}/sample_csr_cng.c)
        set(_csr_backend_libs crypt32 ncrypt)
    else()
        find_package(OpenSSL 3.0 QUIET)
        if(OPENSSL_FOUND)
            set(_csr_backend_src ${_auth_common}/sample_csr_openssl.c)
            set(_csr_backend_libs OpenSSL::Crypto)
        endif()
    endif()

    if(_csr_backend_src)
        add_executable(az_iot_sample_auth_custom_certificate_provider
            custom_certificate_provider/main.c
            ${_auth_common}/sample_cert_provider.c
            ${_csr_backend_src}
            ${_auth_common}/sample_utils.c)
        target_include_directories(az_iot_sample_auth_custom_certificate_provider PRIVATE ${_auth_common})
        target_link_libraries(az_iot_sample_auth_custom_certificate_provider
            PRIVATE az_iot_core az_iot_adapter_paho ${_csr_backend_libs})
        az_iot_apply_warnings(az_iot_sample_auth_custom_certificate_provider)
    endif()
endif()
