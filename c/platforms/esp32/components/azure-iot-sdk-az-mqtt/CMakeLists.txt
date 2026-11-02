# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ESP-IDF component for the az_mqtt MQTT adapter (adapters/az_mqtt), over the
# az_mqtt component (deps/az_mqtt) with ESP-IDF's mbedTLS. The application sets buffer sizes
# in az_iot_connection_client_options::mqtt_limits (the samples take them from Kconfig).

# c/, four levels up from platforms/esp32/components/azure-iot-sdk-az-mqtt/.
get_filename_component(AZ_IOT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

idf_component_register(
    SRCS
        "${AZ_IOT_ROOT}/adapters/az_mqtt/az_iot_mqtt_az_mqtt.c"
        "${AZ_IOT_ROOT}/adapters/az_mqtt/az_iot_mqtt_az_mqtt_v3.c"
        "${AZ_IOT_ROOT}/adapters/az_mqtt/az_iot_mqtt_az_mqtt_v5.c"
        "${AZ_IOT_ROOT}/adapters/az_mqtt/az_iot_mqtt_az_mqtt_adapter.c"
    PRIV_INCLUDE_DIRS "${AZ_IOT_ROOT}/src/core"
    REQUIRES azure-iot-sdk
    PRIV_REQUIRES az_mqtt mbedtls  # mbedtls: esp_crt_bundle_attach()
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE AZ_IOT_AZ_MQTT_MBEDTLS)

# Kconfig values are not set during ESP-IDF's requirement expansion.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX=${CONFIG_AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX})
endif()
