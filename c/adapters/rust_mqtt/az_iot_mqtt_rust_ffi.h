// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* FFI surface that a Rust cdylib must implement to act as an MQTT client
 * for azure-iot-sdk. Mirrors `az_iot_mqtt_iface` one-for-one with
 * primitive C types so that bindgen / cbindgen can translate it directly.
 *
 * Memory ownership rules:
 *   - Strings / byte buffers passed *into* Rust are owned by C and remain
 *     valid only for the duration of the call (the Rust side must copy if
 *     it needs to retain anything).
 *   - The opaque `client` handle is allocated by Rust in `create()` and
 *     freed by Rust in `destroy()`. C never dereferences it.
 *   - The inbound event callback delivers events synchronously from inside
 *     `process_loop()` (never from another thread). The `evt` pointer and
 *     anything it transitively points to are valid only for the duration of
 *     the callback.
 */
#ifndef AZ_IOT_MQTT_RUST_FFI_H
#define AZ_IOT_MQTT_RUST_FFI_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* Opaque Rust-side client handle. */
  typedef struct az_iot_rust_mqtt_client az_iot_rust_mqtt_client;

  /* Plain `struct`, NOT `typedef struct ... az_iot_rust_mqtt_ffi`. The public
   * header az_iot_adapter_rust_mqtt.h already forward-declares the typedef, and
   * C99 forbids defining the same typedef name twice: gcc/clang reject it with
   * -Werror=pedantic (MSVC accepts it, so this only breaks the Linux legs). */
  struct az_iot_rust_mqtt_ffi
  {
    /* Construct a client of the given MQTT version. Must return NULL on
     * allocation failure. P0 only invokes this with version=v5. */
    az_iot_rust_mqtt_client* (*create)(az_iot_mqtt_version version);

    void (*destroy)(az_iot_rust_mqtt_client* client);

    az_iot_result (
        *connect)(az_iot_rust_mqtt_client* client, const az_iot_mqtt_connect_options* opts);

    az_iot_result (*disconnect)(az_iot_rust_mqtt_client* client);

    az_iot_result (*subscribe)(
        az_iot_rust_mqtt_client* client,
        const char* topic_filter,
        az_iot_mqtt_qos qos,
        uint16_t* out_packet_id);

    az_iot_result (*unsubscribe)(
        az_iot_rust_mqtt_client* client,
        const char* topic_filter,
        uint16_t* out_packet_id);

    az_iot_result (*publish)(
        az_iot_rust_mqtt_client* client,
        const az_iot_mqtt_message* message,
        uint16_t* out_packet_id);

    az_iot_result (*process_loop)(az_iot_rust_mqtt_client* client, uint32_t timeout_ms);

    void (*set_inbound_cb)(
        az_iot_rust_mqtt_client* client,
        az_iot_mqtt_event_callback cb,
        void* user_ctx);
  };

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTT_RUST_FFI_H */
