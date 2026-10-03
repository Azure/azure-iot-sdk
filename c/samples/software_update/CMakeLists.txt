# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# PC Software Update samples, with simulated download/install. They need the
# OpenSSL crypto backend for manifest JWS verification and SHA-256, so they are
# built only when it exists. esp32/ is an ESP-IDF project, built with idf.py.
if(NOT TARGET az_iot_crypto_openssl)
    return()
endif()

# pc/<name> builds target az_iot_sample_software_update_<name>, binary
# az_iot_sample_<name>, like az_iot_add_sample().
foreach(name simulated_onboarding simulated_regular)
    set(target az_iot_sample_software_update_${name})
    add_executable(${target}
        ${CMAKE_CURRENT_SOURCE_DIR}/pc/${name}/main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/pc/common/su_sim.c
        ${AZ_IOT_SAMPLES_COMMON_DIR}/sample_utils.c)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME az_iot_sample_${name})
    target_include_directories(${target} PRIVATE
        ${AZ_IOT_SAMPLES_COMMON_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/pc/common)
    target_link_libraries(${target}
        PRIVATE az_iot_core az_iot_adapter_paho az_iot_crypto_openssl)
    # nanosleep()/struct timespec need the POSIX feature-test macros under -std=c99.
    if(NOT WIN32)
        target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
    endif()
    az_iot_apply_warnings(${target})
endforeach()
