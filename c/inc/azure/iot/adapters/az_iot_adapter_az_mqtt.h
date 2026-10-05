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
 * AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS.
 *
 * Register one or both factories with the connection client; the SDK picks one by MQTT
 * version (v3.1.1 for DPS and mqttv3, v5 for mqttv5). Each client the factory creates owns its
 * buffers, sized at build time (see the AZ_IOT_AZ_MQTT_* CMake cache variables).
 *
 * TLS backend: OpenSSL 3 or mbedTLS on Linux, Schannel on Windows. Non-extractable keys:
 * az_iot_mqtt_tls_options.client_key_uri + crypto_engine_id (an OpenSSL 3 provider), OpenSSL
 * builds only. Not supported (connect() returns AZ_IOT_ERR_NOT_SUPPORTED): the sign callback,
 * client_key_password, and, on Windows, a client certificate.
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
   * Not for a factory registered with az_iot_connection_client_register_mqtt_factory(): the
   * connection client owns it then, and az_iot_connection_client_deinit() destroys it.
   */
  void az_iot_az_mqtt_factory_destroy(az_iot_mqtt_factory* factory);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADAPTER_AZ_MQTT_H */
