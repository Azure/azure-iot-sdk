# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Samples require the Paho adapter to actually connect, but the sources
# themselves only depend on the public headers + the core library.
# See README.md for the layout and the environment variables each sample reads.

set(AZ_IOT_SAMPLES_COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/common)

# az_iot_add_sample(<group> <name> <libs...>): builds <name>/main.c of the
# calling folder as target az_iot_sample_<group>_<name>. The binary is named
# az_iot_sample_<name> in every group, like the .NET projects, and lands in
# <build>/samples/<group>/.
function(az_iot_add_sample group name)
    set(target az_iot_sample_${group}_${name})
    add_executable(${target}
        ${CMAKE_CURRENT_SOURCE_DIR}/${name}/main.c ${AZ_IOT_SAMPLES_COMMON_DIR}/sample_utils.c)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME az_iot_sample_${name})
    target_include_directories(${target} PRIVATE ${AZ_IOT_SAMPLES_COMMON_DIR})
    target_link_libraries(${target} PRIVATE ${ARGN} az_iot_adapter_paho)
    az_iot_apply_warnings(${target})
endfunction()

if(AZ_IOT_WITH_PAHO)
    # mqttv3 or mqttv5, picked from the DPS assignment.
    add_subdirectory(unified)
    # MQTTv5 hubs only.
    add_subdirectory(mqttv5)

    # Software Update PC samples; they need the OpenSSL crypto adapter.
    add_subdirectory(software_update)
endif()

# Authentication / certificate-management samples. The subdirectory decides
# per-sample which dependencies (Paho, managed provider) are required; the
# BYO-provider template builds unconditionally.
add_subdirectory(authentication)
