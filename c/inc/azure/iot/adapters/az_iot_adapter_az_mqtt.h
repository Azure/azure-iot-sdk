// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_adapter_az_mqtt.h
 * @brief MQTT adapter over az_mqtt (c/deps/az_mqtt): single-threaded, no adapter thread.
 *
 * connect() only prepares: name resolution, the socket connect, the TLS and MQTT handshakes and
 * every receive run in process_loop(). Sends (publish, subscribe, unsubscribe, disconnect) are
 * written when called and wait only while the socket send buffer is full, at most
 * AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS (over WebSockets, up to twice that).
 *
 * Register one or both factories with the connection client; the SDK picks one by MQTT
 * version (v3.1.1 for DPS and mqttv3, v5 for mqttv5). Each client the factory creates owns its
 * buffers, sized at build time (see the AZ_IOT_AZ_MQTT_* CMake cache variables).
 *
 * TLS backend: OpenSSL 3 or mbedTLS on Linux, Schannel on Windows, ESP-IDF's mbedTLS on ESP32
 * (platforms/esp32; with no trusted CA, the server is verified against the ESP-IDF certificate
 * bundle). Non-extractable keys:
 * az_iot_mqtt_tls_options.client_key_uri + crypto_engine_id (an OpenSSL 3 provider), OpenSSL
 * builds only. Not supported (connect() returns AZ_IOT_ERR_NOT_SUPPORTED): the sign callback and
 * client_key_password. On Windows the client key (unencrypted PKCS#8, PKCS#1 RSA, or SEC1 EC on
 * P-256, P-384 or P-521) is imported into the user's CNG key store for each connection, and
 * deleted when that connection closes; chain certificates after the client certificate are added
 * to the user's intermediate CA store (Schannel sends only those it finds there), and stay. With a
 * client certificate, TLS 1.2 only: the server is validated before the certificate is sent.
 */
#ifndef AZ_IOT_ADAPTER_AZ_MQTT_H
#define AZ_IOT_ADAPTER_AZ_MQTT_H

#include "../az_iot_mqtt_iface.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Create a factory of MQTT 3.1.1 clients.
   * @return The factory, or NULL when out of memory. Release it with
   * az_iot_az_mqtt_factory_destroy().
   */
  az_iot_mqtt_factory* az_iot_az_mqtt_factory_create_v3_1_1(void);

  /**
   * @brief Create a factory of MQTT 5 clients.
   * @return The factory, or NULL when out of memory. Release it with
   * az_iot_az_mqtt_factory_destroy().
   */
  az_iot_mqtt_factory* az_iot_az_mqtt_factory_create_v5(void);

  /**
   * @brief Destroy a factory from either function above (NULL: no-op). Clients it created are
   * not destroyed; they follow the az_iot_mqtt_iface destroy() contract.
   *
   * The application owns the factory; registering it only lends it to the connection client.
   * A registered factory must stay alive until az_iot_connection_client_deinit() returns;
   * destroy it after that, or at any time if it was never registered or registration failed.
   */
  void az_iot_az_mqtt_factory_destroy(az_iot_mqtt_factory* factory);

  /**
   * @brief The az_mqtt adapter: builds MQTT 3.1.1 and MQTT 5 clients in application memory, with
   * no allocation. A constant; nothing to create or destroy.
   */
  const az_iot_mqtt_adapter* az_iot_az_mqtt_get_adapter(void);

  /**
   * @brief Workspace bytes for the largest az_mqtt client the connection client may run with
   * @p limits: over the MQTT versions compiled in, with this build's TLS backend.
   *
   * @return The size, or 0 when @p limits is invalid (see az_iot_mqtt_limits).
   */
  size_t az_iot_az_mqtt_get_workspace_size(const az_iot_mqtt_limits* limits);

/**
 * @brief Upper bound of one az_mqtt client's own state, in bytes. The adapter fails to compile if
 * its state is larger: raise this then (e.g. with a larger AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX).
 */
#ifndef AZ_IOT_AZ_MQTT_CLIENT_STATE_SIZE
#define AZ_IOT_AZ_MQTT_CLIENT_STATE_SIZE 4096u
#endif

/** @brief Upper bound of one az_mqtt in-flight entry, in bytes; checked by the adapter. */
#define AZ_IOT_AZ_MQTT_IN_FLIGHT_ENTRY_SIZE 8u

/** @brief @p size rounded up to the 16-byte alignment of each workspace part. */
#define AZ_IOT_AZ_MQTT_WORKSPACE_ALIGN(size) ((((size_t)(size)) + 15u) & ~(size_t)15u)

/**
 * @brief Workspace bytes for az_iot_mqtt_limits with these values, as a constant expression for
 * static or stack arrays. At least az_iot_az_mqtt_get_workspace_size() for the same values when
 * @p max_tls_state_size is at least the TLS backend's state (az_mqtt_transport_sizeof(); measured:
 * OpenSSL 560 B, mbedTLS 4.2-4.6 KB, Schannel about 197 KB). init() reports a workspace that is
 * still too small with AZ_IOT_ERR_NOT_ENOUGH_SPACE.
 */
#define AZ_IOT_AZ_MQTT_WORKSPACE_SIZE(                                              \
    send_buffer_size,                                                               \
    receive_buffer_size,                                                            \
    max_in_flight_messages,                                                         \
    message_store_size,                                                             \
    connect_strings_size,                                                           \
    max_tls_state_size)                                                             \
  (15u + (size_t)AZ_IOT_AZ_MQTT_CLIENT_STATE_SIZE                                   \
   + AZ_IOT_AZ_MQTT_WORKSPACE_ALIGN(                                                \
       2u * (size_t)(max_in_flight_messages) * AZ_IOT_AZ_MQTT_IN_FLIGHT_ENTRY_SIZE) \
   + AZ_IOT_AZ_MQTT_WORKSPACE_ALIGN(send_buffer_size)                               \
   + AZ_IOT_AZ_MQTT_WORKSPACE_ALIGN(receive_buffer_size)                            \
   + AZ_IOT_AZ_MQTT_WORKSPACE_ALIGN((size_t)(receive_buffer_size) + 3u)             \
   + AZ_IOT_AZ_MQTT_WORKSPACE_ALIGN(max_tls_state_size)                             \
   + AZ_IOT_AZ_MQTT_WORKSPACE_ALIGN(connect_strings_size)                           \
   + AZ_IOT_AZ_MQTT_WORKSPACE_ALIGN(message_store_size))

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADAPTER_AZ_MQTT_H */
