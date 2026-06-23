// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Public entry points for the Rust MQTT adapter shell (Phase 6 / P0 stub).
 *
 * The C side ships a pure-C wrapper that adapts the `az_iot_mqtt_iface`
 * vtable to a small FFI surface (`az_iot_rust_mqtt_ffi_t`). At runtime
 * the Rust cdylib is expected to call `az_iot_rust_mqtt_install()` from
 * its initialization to register its function pointers; until that happens,
 * the factory returns NULL and the adapter is effectively absent.
 *
 * This split means:
 *   - The C library has zero link-time dependency on a Rust toolchain.
 *   - A real Rust implementation can be developed and shipped independently.
 *   - The FFI surface is defined once (here + the FFI header) and is the
 *     authoritative interop contract.
 *
 * P0 scope: MQTTv5 only. v3.1.1 is covered by the Paho adapter; there is
 * no benefit to a second v3.1.1 path.
 */
#ifndef AZ_IOT_ADAPTER_RUST_MQTT_H
#define AZ_IOT_ADAPTER_RUST_MQTT_H

#include "../az_iot_mqtt_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FFI table the Rust cdylib installs at startup. All function pointers must
 * be non-NULL or `_install()` will fail. The Rust impl is responsible for
 * thread safety and for not calling back into az_iot from inside any of
 * these calls except the inbound event callback the core registers via
 * `set_inbound_cb`. */
typedef struct az_iot_rust_mqtt_ffi_tag az_iot_rust_mqtt_ffi_t;

/* Install a Rust-side FFI table. Pass NULL to uninstall (e.g. on Rust
 * shutdown). Returns AZ_IOT_OK on success, AZ_IOT_ERR_INVALID_ARG if
 * any pointer in the table is NULL. After a successful install, the v5
 * factory will start producing live clients; before that it returns NULL. */
az_iot_result_t az_iot_rust_mqtt_install(const az_iot_rust_mqtt_ffi_t* table);

/* Build a factory that produces MQTTv5 Rust clients. Returns NULL until a
 * non-NULL FFI table has been installed via `_install()`. Lifetime matches
 * the Paho factory: caller owns the returned pointer and must call
 * `az_iot_rust_mqtt_factory_destroy()` on it. */
az_iot_mqtt_factory_t* az_iot_rust_mqtt_factory_create_v5(void);

/* Destroy a factory produced above. Does not destroy clients handed out. */
void az_iot_rust_mqtt_factory_destroy(az_iot_mqtt_factory_t* factory);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADAPTER_RUST_MQTT_H */
