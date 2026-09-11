# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Samples require the Paho adapter to actually connect, but the sources
# themselves only depend on the public headers + the core library.
# Each sample is a no-op when its required env vars are unset (see the source).

if(AZ_IOT_WITH_PAHO)
    add_executable(az_iot_sample_telemetry telemetry/main.c common/sample_utils.c)
    target_include_directories(az_iot_sample_telemetry PRIVATE common)
    target_link_libraries(az_iot_sample_telemetry
        PRIVATE az_iot_gen1 az_iot_gen2 az_iot_adapter_paho)
    az_iot_apply_warnings(az_iot_sample_telemetry)

    add_executable(az_iot_sample_twin_get_patch
        twin_get_patch/main.c common/sample_utils.c)
    target_include_directories(az_iot_sample_twin_get_patch PRIVATE common)
    target_link_libraries(az_iot_sample_twin_get_patch
        PRIVATE az_iot_gen1 az_iot_gen2 az_iot_adapter_paho)
    az_iot_apply_warnings(az_iot_sample_twin_get_patch)

    add_executable(az_iot_sample_direct_method_responder
        direct_method_responder/main.c common/sample_utils.c)
    target_include_directories(az_iot_sample_direct_method_responder PRIVATE common)
    target_link_libraries(az_iot_sample_direct_method_responder
        PRIVATE az_iot_gen1 az_iot_gen2 az_iot_adapter_paho)
    az_iot_apply_warnings(az_iot_sample_direct_method_responder)

    add_executable(az_iot_sample_c2d_receiver
        c2d_receiver/main.c common/sample_utils.c)
    target_include_directories(az_iot_sample_c2d_receiver PRIVATE common)
    target_link_libraries(az_iot_sample_c2d_receiver
        PRIVATE az_iot_gen1 az_iot_gen2 az_iot_adapter_paho)
    az_iot_apply_warnings(az_iot_sample_c2d_receiver)

    add_executable(az_iot_sample_file_upload
        file_upload/main.c common/sample_utils.c)
    target_include_directories(az_iot_sample_file_upload PRIVATE common)
    target_link_libraries(az_iot_sample_file_upload
        PRIVATE az_iot_gen1 az_iot_adapter_paho)
    # File upload is HTTPS-only and the SDK ships no HTTP client, so the sample
    # uses libcurl (when available) to perform the SAS-URI request, the blob PUT
    # to Azure Storage, and the completion notification. Optional: without curl
    # the sample still provisions, connects, and prints the request it built.
    find_package(CURL QUIET)
    if(CURL_FOUND)
        target_compile_definitions(az_iot_sample_file_upload PRIVATE AZ_IOT_SAMPLE_WITH_CURL)
        target_link_libraries(az_iot_sample_file_upload PRIVATE CURL::libcurl)
    else()
        message(STATUS "az_iot_sample_file_upload: libcurl not found; building without the "
                       "HTTPS upload path. Install libcurl (e.g. 'vcpkg install curl') to enable it.")
    endif()
    az_iot_apply_warnings(az_iot_sample_file_upload)

    # The ADU sample additionally needs the OpenSSL crypto adapter for real
    # manifest JWS verification + SHA-256. Built only when that adapter exists.
    if(TARGET az_iot_adu_crypto_openssl)
        add_executable(az_iot_sample_adu
            adu/pc/main.c common/sample_utils.c)
        target_include_directories(az_iot_sample_adu PRIVATE common)
        target_link_libraries(az_iot_sample_adu
            PRIVATE az_iot_core az_iot_adapter_paho az_iot_adu_crypto_openssl)
        # nanosleep()/struct timespec need the POSIX feature-test macros under -std=c99.
        if(NOT WIN32)
            target_compile_definitions(az_iot_sample_adu PRIVATE
                _POSIX_C_SOURCE=200809L
                _DEFAULT_SOURCE)
        endif()
        az_iot_apply_warnings(az_iot_sample_adu)
    endif()
endif()

# Authentication / certificate-management samples. The subdirectory decides
# per-sample which dependencies (Paho, managed provider) are required; the
# BYO-provider template builds unconditionally.
add_subdirectory(authentication)
