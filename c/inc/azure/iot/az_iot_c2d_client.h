// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_C2D_CLIENT_H
#define AZ_IOT_C2D_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "az_iot_result.h"
#include "az_iot_connection_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief Callback invoked when a cloud-to-device message is received.
   *
   * @param payload       Message payload bytes (may be NULL if payload_len == 0).
   * @param payload_len   Length of the payload in bytes.
   * @param content_type  Content-type string (from MQTT v5 property or topic
   *                      property in Classic). May be NULL if not provided.
   * @param user_ctx      User context passed to az_iot_c2d_client_set_handler().
   */
  typedef void (*az_iot_c2d_handler_callback)(
      const uint8_t* payload,
      size_t payload_len,
      const char* content_type,
      void* user_ctx);

  typedef struct az_iot_c2d_client
  {
    struct
    {
      az_iot_connection_client* conn;
      az_iot_c2d_handler_callback handler;
      void* handler_ctx;
    } _internal;
  } az_iot_c2d_client;

  /**
   * @brief Initialize the C2D client.
   *
   * Registers the appropriate subscription and inbound handler for the active
   * protocol flavor (Classic or Hub-Next).
   *
   * @param client  C2D client instance to initialize.
   * @param conn    Connection client (must already be initialized).
   * @return AZ_IOT_OK on success.
   */
  AZ_NODISCARD az_iot_result
  az_iot_c2d_client_init(az_iot_c2d_client* client, az_iot_connection_client* conn);

  /**
   * @brief Deinitialize the C2D client and unregister inbound handlers.
   */
  void az_iot_c2d_client_destroy(az_iot_c2d_client* client);

  /**
   * @brief Set the handler for incoming C2D messages.
   *
   * @param client    C2D client instance.
   * @param cb        Callback to invoke on each received message.
   * @param user_ctx  User context forwarded to the callback.
   * @return AZ_IOT_OK on success.
   */
  az_iot_result az_iot_c2d_client_set_handler(
      az_iot_c2d_client* client,
      az_iot_c2d_handler_callback cb,
      void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_C2D_CLIENT_H */
