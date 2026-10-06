# Copyright (c) Microsoft. All rights reserved.
# Licensed under the MIT license. See LICENSE file in the project root for full license information.

# Rust MQTT adapter shell (Phase 6, P0 stub).
#
# Builds a pure-C library that wraps a runtime-installed FFI table
# (`az_iot_rust_mqtt_ffi`) into the iface vtable. There is no
# link-time dependency on a Rust toolchain - the Rust cdylib is expected
# to call az_iot_rust_mqtt_install() at startup.

add_library(az_iot_adapter_rust_mqtt STATIC
    az_iot_mqtt_rust.c
)
add_library(azure::iot::adapter_rust_mqtt ALIAS az_iot_adapter_rust_mqtt)

# az_iot_mqtt_rust_ffi.h is the interop contract the Rust side implements, so
# it is public.
target_include_directories(az_iot_adapter_rust_mqtt
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/inc>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(az_iot_adapter_rust_mqtt
    PUBLIC
        az_iot_core
)
az_iot_apply_warnings(az_iot_adapter_rust_mqtt)
az_iot_apply_coverage(az_iot_adapter_rust_mqtt)
