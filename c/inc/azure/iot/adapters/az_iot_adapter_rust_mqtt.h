// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Public entry points for the Rust MQTT adapter shell (Phase 6 / P0 stub).
 *
 * The C side ships a pure-C wrapper that adapts the `az_iot_mqtt_iface`
 * vtable to a small FFI surface (`az_iot_rust_mqtt_ffi`). At runtime
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
extern "C"
{
#endif

  /* FFI table the Rust cdylib installs at startup. All function pointers must
   * be non-NULL or `_install()` will fail. The Rust impl is responsible for
   * thread safety and for not calling back into az_iot from inside any of
   * these calls except the inbound event callback the core registers via
   * `set_inbound_cb`. */
  typedef struct az_iot_rust_mqtt_ffi az_iot_rust_mqtt_ffi;

  /* Install a Rust-side FFI table. Pass NULL to uninstall (e.g. on Rust
   * shutdown). Returns AZ_IOT_OK on success, AZ_IOT_ERR_INVALID_ARG if
   * any pointer in the table is NULL. After a successful install, the v5
   * factory will start producing live clients; before that it returns NULL. */
  AZ_NODISCARD az_iot_result az_iot_rust_mqtt_install(const az_iot_rust_mqtt_ffi* table);

  /**
   * @brief Create a factory of MQTTv5 Rust clients.
   * @return The factory, or NULL until a non-NULL FFI table is installed via
   * az_iot_rust_mqtt_install() (or when out of memory). Ownership matches the
   * Paho factory: the caller owns it until
   * az_iot_connection_client_register_mqtt_factory() returns AZ_IOT_OK, then
   * the connection client owns it and az_iot_connection_client_deinit()
   * destroys it.
   */
  az_iot_mqtt_factory* az_iot_rust_mqtt_factory_create_v5(void);

  /**
   * @brief Destroy a factory from az_iot_rust_mqtt_factory_create_v5() that is
   * not registered with a connection client (a registered one is destroyed by
   * az_iot_connection_client_deinit()). Does not destroy clients handed out.
   */
  void az_iot_rust_mqtt_factory_destroy(az_iot_mqtt_factory* factory);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADAPTER_RUST_MQTT_H */
