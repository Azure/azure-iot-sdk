# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# ESP-IDF component build, included by the top-level CMakeLists.txt under ESP_PLATFORM.
# Options come from Kconfig (CONFIG_AZ_MQTT_*), not from the CMake options.

# Kconfig values are not set during ESP-IDF's requirement expansion.
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT CONFIG_AZ_MQTT_ENABLE_MQTTV3 AND NOT CONFIG_AZ_MQTT_ENABLE_MQTTV5)
  message(FATAL_ERROR "Enable at least one of AZ_MQTT_ENABLE_MQTTV3 and AZ_MQTT_ENABLE_MQTTV5")
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT CONFIG_AZ_MQTT_BUILD_AZ_CORE
   AND CONFIG_AZ_MQTT_AZ_CORE_COMPONENT STREQUAL "")
  message(FATAL_ERROR "AZ_MQTT_BUILD_AZ_CORE is off: set AZ_MQTT_AZ_CORE_COMPONENT to the component "
    "that compiles the azure-sdk-for-c sources")
endif()

set(_root "${CMAKE_CURRENT_LIST_DIR}/../..")
set(_az_core "${_root}/deps/azure-sdk-for-c/sdk")

set(_srcs
  "${_root}/src/core/az_mqtt_core.c"
  "${_root}/src/core/az_mqtt_codec_common.c"
  "${_root}/src/core/az_mqtt_transport.c"
  "${_root}/src/platform/transport_stack.c"
  "${_root}/src/platform/transport_socket_posix.c"
  "${_root}/src/platform/az_mqtt_socket_posix.c")
if(CONFIG_AZ_MQTT_TLS_MBEDTLS)
  list(APPEND _srcs "${_root}/src/platform/tls_mbedtls.c")
endif()
if(CONFIG_AZ_MQTT_ENABLE_PROXY OR CONFIG_AZ_MQTT_ENABLE_WEBSOCKETS)
  list(APPEND _srcs "${_root}/src/platform/az_mqtt_http_connect.c")
endif()
if(CONFIG_AZ_MQTT_ENABLE_PROXY)
  list(APPEND _srcs "${_root}/src/core/az_mqtt_proxy.c")
endif()
if(CONFIG_AZ_MQTT_ENABLE_WEBSOCKETS)
  list(APPEND _srcs "${_root}/src/core/az_mqtt_websocket.c")
endif()
if(CONFIG_AZ_MQTT_ENABLE_MQTTV3)
  list(APPEND _srcs "${_root}/src/mqtt3/az_mqtt3_client.c" "${_root}/src/mqtt3/az_mqtt3_codec.c")
endif()
if(CONFIG_AZ_MQTT_ENABLE_MQTTV5)
  list(APPEND _srcs "${_root}/src/mqtt5/az_mqtt5_client.c" "${_root}/src/mqtt5/az_mqtt5_codec.c")
endif()

set(_az_core_srcs)
set(_include_dirs "${_root}/inc")
if(CONFIG_AZ_MQTT_BUILD_AZ_CORE)
  list(APPEND _include_dirs "${_az_core}/inc")
  list(APPEND _az_core_srcs
    "${_az_core}/src/azure/core/az_span.c"
    "${_az_core}/src/azure/core/az_log.c"
    "${_az_core}/src/azure/core/az_precondition.c")
  if(CONFIG_AZ_MQTT_ENABLE_PROXY OR CONFIG_AZ_MQTT_ENABLE_WEBSOCKETS)
    list(APPEND _az_core_srcs "${_az_core}/src/azure/core/az_base64.c")
  endif()
endif()

idf_component_register(
  SRCS ${_srcs} ${_az_core_srcs}
  INCLUDE_DIRS ${_include_dirs}
  PRIV_INCLUDE_DIRS "${_root}/src/core" "${_root}/src/platform"
  # Private, and not per Kconfig: requirements are resolved before Kconfig values exist.
  PRIV_REQUIRES lwip mbedtls)

set(_public_defs)
if(NOT CONFIG_AZ_MQTT_ENABLE_PROXY)
  list(APPEND _public_defs AZ_MQTT_NO_PROXY)
endif()
if(NOT CONFIG_AZ_MQTT_ENABLE_WEBSOCKETS)
  list(APPEND _public_defs AZ_MQTT_NO_WEBSOCKETS)
endif()
if(NOT CONFIG_AZ_MQTT_LOGGING)
  list(APPEND _public_defs AZ_NO_LOGGING)
endif()
if(NOT CONFIG_AZ_MQTT_PRECONDITIONS)
  list(APPEND _public_defs AZ_NO_PRECONDITION_CHECKING)
endif()
target_compile_definitions(${COMPONENT_LIB} PUBLIC ${_public_defs})
if(CONFIG_AZ_MQTT_TLS_MBEDTLS)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE AZ_MQTT_TLS_MBEDTLS)
endif()
if(NOT CONFIG_AZ_MQTT_BUILD_AZ_CORE)
  idf_component_get_property(_az_core_lib ${CONFIG_AZ_MQTT_AZ_CORE_COMPONENT} COMPONENT_LIB)
  target_link_libraries(${COMPONENT_LIB} PUBLIC ${_az_core_lib})
endif()

