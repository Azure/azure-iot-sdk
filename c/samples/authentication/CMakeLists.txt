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

# The DPS-CSR and hub-renewal samples use the OpenSSL "managed" provider for
# real CSR generation + persistence.
if(TARGET az_iot_certificate_provider_managed)
    az_iot_add_adapter_sample(az_iot_sample_auth_dps_csr_managed
        SOURCES dps_csr_managed/main.c ${_auth_common}/sample_utils.c
        LIBS az_iot_mqttv3 az_iot_certificate_provider_managed)
    az_iot_add_adapter_sample(az_iot_sample_auth_dps_persist_assignment
        SOURCES dps_persist_assignment/main.c ${_auth_common}/sample_utils.c
        LIBS az_iot_core az_iot_certificate_provider_managed)
    az_iot_add_adapter_sample(az_iot_sample_auth_hub_renew
        SOURCES hub_renew/main.c ${_auth_common}/sample_utils.c
        LIBS az_iot_core az_iot_certificate_provider_managed)
endif()

# The complete app-owned custom provider: real PKCS#10 CSR issuance from
# samples/common. Needs OpenSSL 3.0+ on every platform.
find_package(OpenSSL 3.0 QUIET)
if(OPENSSL_FOUND)
    az_iot_add_adapter_sample(az_iot_sample_auth_custom_certificate_provider
        SOURCES custom_certificate_provider/main.c
                ${_auth_common}/sample_cert_provider.c
                ${_auth_common}/sample_csr_openssl.c
                ${_auth_common}/sample_utils.c
        LIBS az_iot_core OpenSSL::Crypto)
endif()

# D8 non-extractable key custody, key-reference route. Needs OpenSSL 3.0+ at RUN
# time (the provider that owns the token) -- a deployment property, not a build
# one, so the Paho build is always made and reports a clear error when the
# provider is absent. az_mqtt honours key references only over OpenSSL TLS.
# Serves either hub generation, so it links both mqttv3 and mqttv5.
set(_hsm_pkcs11_adapters paho)
if(TARGET az_iot_adapter_az_mqtt)
    get_target_property(_az_mqtt_defs az_iot_adapter_az_mqtt COMPILE_DEFINITIONS)
    if("AZ_IOT_AZ_MQTT_OPENSSL" IN_LIST _az_mqtt_defs)
        list(APPEND _hsm_pkcs11_adapters az_mqtt)
    endif()
    unset(_az_mqtt_defs)
endif()
az_iot_add_adapter_sample(az_iot_sample_auth_hsm_pkcs11 ADAPTERS ${_hsm_pkcs11_adapters}
    SOURCES hsm_pkcs11/main.c ${_auth_common}/sample_utils.c
    LIBS az_iot_mqttv3 az_iot_mqttv5)
unset(_hsm_pkcs11_adapters)

# The sign() hook sample brings its own stand-in adapter, so it needs neither
# Paho nor OpenSSL and is always built -- which also keeps the hook's signature
# compiling everywhere.
add_executable(az_iot_sample_auth_hsm_sign_callback
    hsm_sign_callback/main.c)
target_link_libraries(az_iot_sample_auth_hsm_sign_callback PRIVATE az_iot_core)
az_iot_apply_warnings(az_iot_sample_auth_hsm_sign_callback)

# SAS from symmetric keys: a crypto backend (OpenSSL or mbedTLS) for HMAC-SHA256.
if(AZ_IOT_SAMPLE_CRYPTO)
    az_iot_add_adapter_sample(az_iot_sample_auth_dps_sas_key CRYPTO
        SOURCES dps_sas_key/main.c ${_auth_common}/sample_utils.c
        LIBS az_iot_mqttv3 az_iot_mqttv5)
endif()

# The issued-certificate variant uses the OpenSSL crypto backend and the managed provider.
if(TARGET az_iot_crypto_openssl AND TARGET az_iot_certificate_provider_managed)
    az_iot_add_adapter_sample(az_iot_sample_auth_dps_sas_key_issued_cert
        SOURCES dps_sas_key_issued_cert/main.c ${_auth_common}/sample_utils.c
        LIBS az_iot_mqttv3 az_iot_mqttv5 az_iot_crypto_openssl
             az_iot_certificate_provider_managed)
endif()

# Application-supplied SAS tokens: the sample signs its stand-in tokens with
# OpenSSL directly, so the SDK needs no crypto backend.
if(TARGET OpenSSL::Crypto)
    az_iot_add_adapter_sample(az_iot_sample_auth_user_provided_sas_token
        SOURCES user_provided_sas_token/main.c ${_auth_common}/sample_utils.c
        LIBS az_iot_mqttv3 az_iot_mqttv5 OpenSSL::Crypto)
endif()

# DPS TPM attestation over HTTPS (libcurl), then the hub with SAS tokens signed
# by a TPM simulated in software (OpenSSL).
if(TARGET az_iot_crypto_openssl)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        az_iot_add_adapter_sample(az_iot_sample_auth_dps_tpm
            SOURCES dps_tpm/main.c
                    ${_auth_common}/sample_tpm_sim.c
                    ${_auth_common}/sample_utils.c
            LIBS az_iot_mqttv3 az_iot_crypto_openssl OpenSSL::Crypto CURL::libcurl)
    else()
        message(STATUS "az_iot_sample_auth_dps_tpm: libcurl not found; not built.")
    endif()
endif()
