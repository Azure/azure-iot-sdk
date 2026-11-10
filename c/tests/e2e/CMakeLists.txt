# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ---------------------------------------------------------------------------
# Vendored, test-only AMQP 1.0 client (az_amqp + reference transport). Used by
# the in-process service client below to receive device telemetry (Event
# Hub-compatible endpoint) and send C2D over the IoT Hub AMQP service endpoint.
# ---------------------------------------------------------------------------
# Shared test helpers (support/) are included as "support/<name>.h" from every
# suite here, so the tests root goes on the include path once for the directory
# rather than per target.
include_directories(${PROJECT_SOURCE_DIR}/tests)

# Each suite that connects is built once per MQTT adapter in the build: <suite>
# over Paho, <suite>_az_mqtt over az_mqtt. Its sources select the adapter with
# AZ_IOT_E2E_ADAPTER_PAHO or AZ_IOT_E2E_ADAPTER_AZ_MQTT. Every name contains
# `e2e`, so the workflows' `ctest -R` filters run both.
set(AZ_IOT_E2E_ADAPTERS)
foreach(_adapter paho az_mqtt)
    if(TARGET az_iot_adapter_${_adapter})
        list(APPEND AZ_IOT_E2E_ADAPTERS ${_adapter})
    endif()
endforeach()
unset(_adapter)
# Key references (the custody suites): az_mqtt honours them only over OpenSSL TLS.
set(AZ_IOT_E2E_KEY_REF_ADAPTERS ${AZ_IOT_E2E_ADAPTERS})
if(TARGET az_iot_adapter_az_mqtt)
    get_target_property(_defs az_iot_adapter_az_mqtt COMPILE_DEFINITIONS)
    if(NOT "AZ_IOT_AZ_MQTT_OPENSSL" IN_LIST _defs)
        list(REMOVE_ITEM AZ_IOT_E2E_KEY_REF_ADAPTERS az_mqtt)
    endif()
    unset(_defs)
endif()

# az_iot_add_e2e_suite(<name> [NO_CTEST] [ADAPTERS <adapter...>] SOURCES <src...> LIBS <lib...>)
# One cmocka suite per adapter in AZ_IOT_E2E_ADAPTERS (ADAPTERS narrows it), linked to it.
function(az_iot_add_e2e_suite name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "NO_CTEST" "" "ADAPTERS;SOURCES;LIBS")
    set(adapters ${AZ_IOT_E2E_ADAPTERS})
    if("ADAPTERS" IN_LIST arg_KEYWORDS_MISSING_VALUES)
        return() # Narrowed to no adapter.
    elseif(arg_ADAPTERS)
        string(REPLACE ";" "|" re "${arg_ADAPTERS}")
        list(FILTER adapters INCLUDE REGEX "^(${re})$")
    endif()
    set(no_ctest "")
    if(arg_NO_CTEST)
        set(no_ctest NO_CTEST)
    endif()
    foreach(adapter IN LISTS adapters)
        string(TOUPPER "${adapter}" ADAPTER)
        set(t ${name})
        if(NOT adapter STREQUAL "paho")
            set(t ${name}_${adapter})
        endif()
        az_iot_add_cmocka_test(${t} ${no_ctest} ${arg_SOURCES})
        target_compile_definitions(${t} PRIVATE AZ_IOT_E2E_ADAPTER_${ADAPTER})
        target_link_libraries(${t} PRIVATE ${arg_LIBS} az_iot_adapter_${adapter})
        # Some suites use internal headers.
        target_include_directories(${t} PRIVATE ${PROJECT_SOURCE_DIR}/src/core)
    endforeach()
endfunction()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../deps/amqp
                 ${CMAKE_CURRENT_BINARY_DIR}/deps/amqp)

# Receiver credit accounting of the vendored AMQP client, over an in-memory
# peer. Needs no Azure resources; named `e2e` so the e2e job's filter runs it.
az_iot_add_cmocka_test(az_iot_tests_e2e_amqp_link
    tests/e2e_amqp_link_test.c tests/e2e_amqp_test_peer.c)
target_link_libraries(az_iot_tests_e2e_amqp_link PRIVATE az_amqp)

# A write by the harness AMQP transport to a closed loopback peer fails without
# raising SIGPIPE. POSIX only; named `e2e` so the e2e job's filter runs it.
if(NOT WIN32)
    az_iot_add_cmocka_test(az_iot_tests_e2e_amqp_transport_sigpipe
        tests/e2e_amqp_transport_sigpipe_test.c)
    target_link_libraries(az_iot_tests_e2e_amqp_transport_sigpipe PRIVATE az_amqp_transport)
    target_compile_definitions(az_iot_tests_e2e_amqp_transport_sigpipe PRIVATE
        _DEFAULT_SOURCE _POSIX_C_SOURCE=200112L)
endif()

# Notification enqueuedTimeUtc parsing, and which notifications have no watcher (the watcher
# deletes those). Pure parsing; named `e2e` so the e2e job's filter runs it.
az_iot_add_cmocka_test(az_iot_tests_e2e_notify_time
    tests/e2e_notify_time_test.c service/e2e_notify_time.c)
target_include_directories(az_iot_tests_e2e_notify_time PRIVATE service)

# Response header parsing of the e2e HTTPS client (status, Content-Length,
# Retry-After) and its retry decision. No network; named `e2e` so the e2e job's
# filter runs it.
az_iot_add_cmocka_test(az_iot_tests_e2e_http_headers tests/e2e_http_headers_test.c)
target_include_directories(az_iot_tests_e2e_http_headers PRIVATE service)
target_link_libraries(az_iot_tests_e2e_http_headers PRIVATE
    az_iot_e2e_service az_amqp az_amqp_transport)

# The telemetry watcher's capture filter and ring: messages of other devices on the
# shared events endpoint do not evict the ones a test waits for; a receiver the
# service detached is found; events replayed after a reconnect are dropped. No network; named
# `e2e` so the e2e job's filter runs it.
az_iot_add_cmocka_test(az_iot_tests_e2e_amqp_capture tests/e2e_amqp_capture_test.c)
target_include_directories(az_iot_tests_e2e_amqp_capture PRIVATE service)
target_link_libraries(az_iot_tests_e2e_amqp_capture PRIVATE
    az_iot_e2e_service az_amqp az_amqp_transport)

# The telemetry watcher against an in-memory peer playing Event Hubs: replayed
# transfers are dropped, a detached receiver reconnects with its state kept, and
# the setup deadline covers receivers still attaching. No network; named `e2e`
# so the e2e job's filter runs it.
az_iot_add_cmocka_test(az_iot_tests_e2e_amqp_telemetry
    tests/e2e_amqp_telemetry_test.c tests/e2e_amqp_test_peer.c)
target_include_directories(az_iot_tests_e2e_amqp_telemetry PRIVATE service)
target_link_libraries(az_iot_tests_e2e_amqp_telemetry PRIVATE
    az_iot_e2e_service az_amqp az_amqp_transport)

# ---------------------------------------------------------------------------
# Service-side facade (hides az_amqp behind a plain-C API used by the tests).
# ---------------------------------------------------------------------------
add_subdirectory(service)

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_esp32_qemu: the ESP32 IoT Hub sample (samples/unified/esp32) in QEMU, started by
# E2E_ESP32_QEMU_COMMAND, against the e2e hub. The service side is az_iot_e2e_service; the device
# is the firmware, so no SDK adapter is linked here.
# ---------------------------------------------------------------------------
if(AZ_IOT_BUILD_E2E_ESP32_QEMU)
    if(NOT UNIX)
        message(FATAL_ERROR "AZ_IOT_BUILD_E2E_ESP32_QEMU=ON needs a POSIX host (it starts QEMU)")
    endif()
    az_iot_add_cmocka_test(az_iot_tests_e2e_esp32_qemu tests/e2e_esp32_qemu_test.c tests/e2e_json.c)
    target_link_libraries(az_iot_tests_e2e_esp32_qemu PRIVATE az_iot_e2e_service az_iot_e2e_process)
    # DPS, TLS and MQTT run emulated: minutes, not seconds.
    set_tests_properties(az_iot_tests_e2e_esp32_qemu PROPERTIES TIMEOUT 1200)
endif()

# ---------------------------------------------------------------------------
# az_iot_tests_e2e
#
# The in-process end-to-end suite: ONE cmocka executable that plays both the
# device (shipping SDK over an MQTT adapter) and the cloud (the
# az_iot_e2e_service facade) halves of every scenario against a real Azure IoT
# Hub / DPS instance. Registered with CTest so the e2e job can run it with
# `ctest -R e2e`. The test includes ONLY the service facade's public header, so
# no AMQP type ever leaks into the test translation unit.
#
# It requires provisioning + secrets in the environment (device X.509 material
# and hub/eventhub connection strings) supplied by the e2e CI job.
# ---------------------------------------------------------------------------
az_iot_add_e2e_suite(az_iot_tests_e2e
    SOURCES tests/e2e_scenarios_test.c tests/e2e_device.c
    LIBS az_iot_e2e_service az_iot_mqttv3)

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_csr
#
# CSR-based DPS enrollment (issued operational cert). Requires a DPS enrollment
# linked to a signing CA, which only the ci-c-e2e-csr workflow provisions, so it
# is built only when AZ_IOT_BUILD_E2E_CSR is set -- and when the OpenSSL
# "managed" certificate provider is available.
#
# It used to be built everywhere and skip itself at run time. That meant the
# main e2e leg paid to compile and launch a test that could never do anything,
# and a CTest "Skipped" line was the only evidence it was not running.
# ---------------------------------------------------------------------------
if(AZ_IOT_BUILD_E2E_CSR)
    if(NOT TARGET az_iot_certificate_provider_managed)
        message(FATAL_ERROR
            "AZ_IOT_BUILD_E2E_CSR=ON requires the OpenSSL managed certificate provider "
            "(AZ_IOT_WITH_CERT_PROVIDER_MANAGED=ON).")
    endif()
    az_iot_add_e2e_suite(az_iot_tests_e2e_csr
        SOURCES tests/e2e_csr_test.c
        LIBS az_iot_mqttv3 az_iot_certificate_provider_managed)
endif()

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_sas, az_iot_tests_e2e_csr_sas
#
# SAS from a DPS symmetric-key enrollment group: DPS and hub over SAS; and,
# with AZ_IOT_BUILD_E2E_CSR (group linked to a signing CA), SAS onboarding with
# a DPS-issued certificate for the hub. Every e2e environment provides the
# group, so both are built with the suites they sit beside.
# ---------------------------------------------------------------------------
if(NOT TARGET az_iot_crypto_openssl)
    message(FATAL_ERROR
        "AZ_IOT_BUILD_E2E=ON requires the OpenSSL crypto backend (az_iot_crypto_openssl) "
        "for the SAS e2e suites.")
endif()
# OpenSSL::Crypto: the user-provided token test signs its own tokens.
az_iot_add_e2e_suite(az_iot_tests_e2e_sas
    SOURCES tests/e2e_sas_test.c tests/e2e_sas_device.c
    LIBS az_iot_mqttv3 az_iot_crypto_openssl OpenSSL::Crypto)
if(AZ_IOT_BUILD_E2E_CSR)
    az_iot_add_e2e_suite(az_iot_tests_e2e_csr_sas
        SOURCES tests/e2e_csr_sas_test.c tests/e2e_sas_device.c
        LIBS az_iot_mqttv3 az_iot_crypto_openssl az_iot_certificate_provider_managed)
endif()

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_tpm
#
# DPS TPM attestation with a TPM simulated in software (samples/common): an
# individual enrollment for a new EK (DPS service API), registration over
# HTTPS, then the assigned mqttv3 hub with TPM-signed SAS tokens. Needs
# IOT_DPS_CONNECTION_STRING to create the enrollment.
# ---------------------------------------------------------------------------
set(_e2e_sample_common ${PROJECT_SOURCE_DIR}/samples/common)
az_iot_add_e2e_suite(az_iot_tests_e2e_tpm
    SOURCES tests/e2e_tpm_test.c
            ${_e2e_sample_common}/sample_tpm_sim.c
            tests/e2e_dps_tpm.c
            ${_e2e_sample_common}/sample_utils.c
    LIBS az_iot_e2e_service az_iot_mqttv3 az_iot_crypto_openssl OpenSSL::Crypto)
foreach(_t az_iot_tests_e2e_tpm az_iot_tests_e2e_tpm_az_mqtt)
    if(TARGET ${_t})
        target_include_directories(${_t} PRIVATE ${_e2e_sample_common} service)
    endif()
endforeach()

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_fileupload
#
# MQTTv3 file-upload device flow against a real hub that has an Azure Storage
# account associated for file upload. It plays both halves like the other
# suites: the device requests a SAS URI, PUTs the blob, reads it back and reports
# completion, while the service side watches the hub's file-upload NOTIFICATION
# endpoint -- the only cloud-side proof that a round trip completed. It also
# covers the failure report (accepted, but never notified), a completion carrying
# an unknown correlation id, local argument rejection, and an upload by a SAS-key
# device (DPS symmetric-key group, as az_iot_tests_e2e_sas) whose hub calls are
# authorized by the SDK's hub SAS token.
#
# The MQTTv3 HTTP hook and the Storage PUT/GET go through the e2e service lib's
# HTTPS helper (the harness az_amqp TLS transport), so there is no external HTTP
# dependency. It requires the standard e2e device env, the service connection
# strings, and a file-upload-capable hub. The `e2e` name matches the fast job's
# `ctest -R e2e` filter.
# ---------------------------------------------------------------------------
az_iot_add_e2e_suite(az_iot_tests_e2e_fileupload
    SOURCES tests/e2e_fileupload_test.c tests/e2e_device.c tests/e2e_sas_device.c
    LIBS az_iot_e2e_service az_iot_mqttv3 az_iot_crypto_openssl)

# ---------------------------------------------------------------------------
# az_iot_samples_e2e_<group>_<name>_<adapter>
#
# Each connecting sample of samples/, built with the pipeline hooks, run against an e2e hub by
# az_iot_tests_e2e_samples (tests/e2e_samples_test.c, which names what each sample is given and
# checked for): one test per sample and adapter, labelled samples_mqttv3, or samples_csr for the
# samples that need a CA-linked enrollment (certificate management). A sample that is in scope but
# not built fails the configure rather than dropping its test.
# ---------------------------------------------------------------------------
if(AZ_IOT_BUILD_E2E_SAMPLES)
    az_iot_add_cmocka_test(az_iot_tests_e2e_samples NO_CTEST
        tests/e2e_samples_test.c tests/e2e_json.c)
    target_link_libraries(az_iot_tests_e2e_samples PRIVATE
        az_iot_e2e_service az_iot_e2e_process OpenSSL::Crypto)

    # <group>/<name>: unified/<name> is target az_iot_sample_unified_<name>, authentication/<name>
    # az_iot_sample_auth_<name>; az_mqtt adds _az_mqtt.
    set(_samples
        unified/telemetry unified/connect_first unified/websockets unified/proxy
        unified/multithreaded_telemetry unified/twin_get_patch unified/direct_method_responder
        unified/direct_method_slow_responder unified/c2d_receiver unified/file_upload
        authentication/dps_sas_key authentication/user_provided_sas_token
        authentication/dps_tpm)
    # Built only in some configurations: a test where the sample is built.
    set(_optional_samples)
    # CSR: a bootstrap identity of an X.509 enrollment group linked to a signing CA, and the SAS
    # group linked to it (ci-c-samples' CSR legs). Labelled samples_csr.
    set(_csr_samples
        authentication/dps_csr_managed authentication/custom_certificate_provider
        authentication/dps_persist_assignment authentication/hub_renew
        authentication/dps_sas_key_issued_cert)
    if(NOT WIN32)
        # A PKCS#11 token (SoftHSM2) and the OpenSSL pkcs11 provider: Linux CI only. The az_mqtt
        # build exists only over OpenSSL TLS.
        list(APPEND _samples authentication/hsm_pkcs11)
        # tpm2-tss against swtpm, built where tpm2-tss is found.
        list(APPEND _optional_samples authentication/dps_tpm_tss)
        find_program(AZ_IOT_SWTPM swtpm)
        # Not found: run from PATH, so the test fails rather than the sample being dropped.
        set(_swtpm swtpm)
        if(AZ_IOT_SWTPM)
            set(_swtpm ${AZ_IOT_SWTPM})
        endif()
        target_compile_definitions(az_iot_tests_e2e_samples PRIVATE
            AZ_IOT_E2E_SAMPLES_SWTPM="${_swtpm}")
        unset(_swtpm)
    endif()

    foreach(_adapter IN LISTS AZ_IOT_E2E_ADAPTERS)
        set(_suffix "")
        if(NOT _adapter STREQUAL "paho")
            set(_suffix _${_adapter})
        endif()
        foreach(_sample IN LISTS _samples _optional_samples _csr_samples)
            string(REPLACE "/" ";" _parts "${_sample}")
            list(GET _parts 0 _group)
            list(GET _parts 1 _name)
            set(_target_group ${_group})
            if(_group STREQUAL "authentication")
                set(_target_group auth)
            endif()
            set(_target az_iot_sample_${_target_group}_${_name}${_suffix})
            if(NOT TARGET ${_target})
                if(_sample IN_LIST _optional_samples
                   OR (_sample STREQUAL "authentication/hsm_pkcs11" AND _adapter STREQUAL "az_mqtt"))
                    continue()
                endif()
                message(FATAL_ERROR "AZ_IOT_BUILD_E2E_SAMPLES=ON: sample ${_target} is not built")
            endif()
            if(_sample STREQUAL "unified/file_upload")
                get_target_property(_defs ${_target} COMPILE_DEFINITIONS)
                if(NOT "AZ_IOT_SAMPLE_WITH_CURL" IN_LIST _defs)
                    message(FATAL_ERROR "AZ_IOT_BUILD_E2E_SAMPLES=ON needs libcurl and OpenSSL: "
                        "unified/file_upload uploads with them")
                endif()
                unset(_defs)
            endif()
            add_dependencies(az_iot_tests_e2e_samples ${_target})
            set(_test az_iot_samples_e2e_${_group}_${_name}_${_adapter})
            set(_work_directory ${CMAKE_CURRENT_BINARY_DIR}/samples/${_test})
            file(MAKE_DIRECTORY ${_work_directory})
            set(_label samples_mqttv3)
            set(_extra_arguments "")
            if(_sample IN_LIST _csr_samples)
                set(_label samples_csr)
            endif()
            if(_sample STREQUAL "authentication/hub_renew")
                # dps_csr_managed issues the operational certificate hub_renew renews.
                set(_extra_arguments $<TARGET_FILE:az_iot_sample_auth_dps_csr_managed${_suffix}>)
            endif()
            add_test(NAME ${_test}
                COMMAND az_iot_tests_e2e_samples ${_group}_${_name} $<TARGET_FILE:${_target}>
                    ${_work_directory} ${_extra_arguments}
                WORKING_DIRECTORY ${_work_directory})
            # The test stops the sample first: its own limit is at most 300 s, plus the checks;
            # hub_renew runs dps_csr_managed first.
            set_tests_properties(${_test} PROPERTIES LABELS ${_label} TIMEOUT 600)
        endforeach()
    endforeach()
    unset(_samples)
    unset(_optional_samples)
    unset(_csr_samples)
    unset(_label)
    unset(_extra_arguments)
endif()

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_transport
#
# The egress transports: MQTT over WebSockets, and MQTT tunnelled through an
# HTTP CONNECT proxy. Each scenario connects a device with one transport
# configuration and round-trips a uniquely marked telemetry message to the
# hub's Event Hub-compatible endpoint, so a tunnel that connects but carries
# nothing fails. It also asserts that an unreachable proxy leaves the device
# disconnected instead of silently connecting around it.
#
# The proxy scenarios skip themselves when AZ_IOT_PROXY_HOST is unset, so the
# suite is useful on a CI leg with no proxy (the WebSocket scenario still runs)
# and complete on one that provisions a proxy. The `e2e` name matches the fast
# job's `ctest -R e2e` filter.
# ---------------------------------------------------------------------------
# setenv()/unsetenv() are POSIX, not C99, and these targets build -std=c99:
# without _POSIX_C_SOURCE the calls are implicit declarations and the build fails
# rather than warning. Same reason as az_iot_tests_paho_transport_proxy.
set(_e2e_posix "")
if(UNIX)
    set(_e2e_posix _POSIX_C_SOURCE=200809L)
endif()
az_iot_add_e2e_suite(az_iot_tests_e2e_transport
    SOURCES tests/e2e_transport_test.c tests/e2e_device.c
    LIBS az_iot_e2e_service az_iot_mqttv3)
foreach(_t az_iot_tests_e2e_transport az_iot_tests_e2e_transport_az_mqtt)
    if(TARGET ${_t} AND _e2e_posix)
        target_compile_definitions(${_t} PRIVATE ${_e2e_posix})
    endif()
endforeach()

# ---------------------------------------------------------------------------
# az_iot_tests_e2e_custody
#
# The "Storage / custody" group of docs/eng/certificate-management.md: DPS
# issuance and hub traffic performed by a device whose private key is inside a
# PKCS#11 token and cannot be read by the process. Nothing is stubbed -- if the
# token were not signing the TLS handshake the hub would refuse the connection,
# so reaching CONNECTED is itself the assertion.
#
# The token is provisioned by eng/setup-softhsm.sh (SoftHSM2 in CI) and named
# through AZ_IOT_CLIENT_KEY_URI. Built only when that is going to be supplied;
# it fails rather than skipping when it is not.
# ---------------------------------------------------------------------------
if(AZ_IOT_BUILD_E2E_PKCS11)
    az_iot_add_e2e_suite(az_iot_tests_e2e_custody ADAPTERS ${AZ_IOT_E2E_KEY_REF_ADAPTERS}
        SOURCES tests/e2e_custody_test.c
        LIBS az_iot_e2e_service az_iot_mqttv3)
endif()

