# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# PC Software Update samples, with simulated download/install. They need a
# crypto backend (OpenSSL or mbedTLS) for manifest JWS verification and SHA-256,
# so they are built only when one exists. esp32/ is an ESP-IDF project, built
# with idf.py.
if(NOT AZ_IOT_SAMPLE_CRYPTO)
    message(STATUS "Software Update samples: no crypto backend (OpenSSL or mbedTLS); not built.")
    return()
endif()

# pc/<name> builds target az_iot_sample_software_update_<name>, binary
# az_iot_sample_<name>, like az_iot_add_sample().
foreach(name simulated_onboarding simulated_regular)
    az_iot_add_adapter_sample(az_iot_sample_software_update_${name} CRYPTO
        OUTPUT_NAME az_iot_sample_${name}
        SOURCES pc/${name}/main.c pc/common/su_sim.c ${AZ_IOT_SAMPLES_COMMON_DIR}/sample_utils.c
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/pc/common
        LIBS az_iot_core
        # nanosleep()/struct timespec need the POSIX feature-test macros under -std=c99.
        DEFINES $<$<NOT:$<PLATFORM_ID:Windows>>:_POSIX_C_SOURCE=200809L>
                $<$<NOT:$<PLATFORM_ID:Windows>>:_DEFAULT_SOURCE>)
endforeach()

# pc/http_download downloads for real with libcurl; built when libcurl is found.
find_package(CURL QUIET)
if(CURL_FOUND)
    az_iot_add_adapter_sample(az_iot_sample_software_update_http_download CRYPTO
        OUTPUT_NAME az_iot_sample_http_download
        SOURCES pc/http_download/main.c pc/common/su_sim.c ${AZ_IOT_SAMPLES_COMMON_DIR}/sample_utils.c
        INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/pc/common
        LIBS az_iot_core CURL::libcurl
        DEFINES $<$<NOT:$<PLATFORM_ID:Windows>>:_POSIX_C_SOURCE=200809L>
                $<$<NOT:$<PLATFORM_ID:Windows>>:_DEFAULT_SOURCE>)
else()
    message(STATUS "libcurl not found: samples/software_update/pc/http_download is not built")
endif()
