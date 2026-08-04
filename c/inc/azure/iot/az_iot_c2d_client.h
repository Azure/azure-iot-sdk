// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_C2D_CLIENT_H
#define AZ_IOT_C2D_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "az_iot_result.h"
#include "az_iot_message.h"
#include "az_iot_connection_client.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Maximum number of properties surfaced on one received message. A message
 * carrying more is still delivered; the properties past this bound are dropped
 * and a warning is logged. */
#ifndef AZ_IOT_C2D_MAX_PROPERTIES
#define AZ_IOT_C2D_MAX_PROPERTIES 8
#endif
/* Bytes available to hold the decoded property names and values of one
 * message. Decoding never grows the text, so this only has to cover the
 * property-bag portion of the topic. It lives on the stack for the duration of
 * the handler callback, so it costs nothing per client. */
#ifndef AZ_IOT_C2D_PROPERTY_BUFFER
#define AZ_IOT_C2D_PROPERTY_BUFFER 256
#endif

  /**
   * @brief One property carried by a cloud-to-device message.
   *
   * Plain text, already decoded: the SDK reverses the percent-encoding IoT Hub
   * applies to the topic property bag, so a value sent as `application%2Fjson`
   * arrives as `application/json` and a system property key arrives as `$.ct`
   * rather than `%24.ct`. This is the same shape az_iot_telemetry_property uses
   * on the way out, so a property survives a round trip unchanged.
   */
  typedef struct az_iot_c2d_property
  {
    const char* key;
    const char* value; /**< NULL when the property was sent with no value. */
  } az_iot_c2d_property;

  /**
   * @brief A received cloud-to-device message.
   *
   * Every pointer is owned by the SDK and valid only for the duration of the
   * handler callback. Copy anything the application needs to keep.
   */
  typedef struct az_iot_c2d_message
  {
    const uint8_t* payload; /**< May be NULL when @p payload_len is 0. */
    size_t payload_len;
    /** Convenience view of the `$.ct` property (Classic) or the MQTT v5
     * content-type (Next). NULL when the message carries none. */
    const char* content_type;
    const az_iot_c2d_property* properties;
    size_t properties_count;
  } az_iot_c2d_message;

  /**
   * @brief Callback invoked when a cloud-to-device message is received.
   *
   * @param msg       The received message. Valid only during this call.
   * @param user_ctx  User context passed to az_iot_c2d_client_set_handler().
   */
  typedef void (*az_iot_c2d_handler_callback)(const az_iot_c2d_message* msg, void* user_ctx);

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
   * @return AZ_IOT_OK on success;
   *         AZ_IOT_ERR_ALREADY_INITIALIZED when this connection already has a
   *         C2D client for the same identity -- there is one device-bound
   *         stream per identity, so a second client could never receive
   *         anything.
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
   * @param cb        Callback to invoke on each received message. NULL pauses
   *                  delivery without tearing the subscription down.
   * @param user_ctx  User context forwarded to the callback.
   * @return AZ_IOT_OK on success.
   */
  az_iot_result az_iot_c2d_client_set_handler(
      az_iot_c2d_client* client,
      az_iot_c2d_handler_callback cb,
      void* user_ctx);

  /**
   * @brief Look up one property by name.
   *
   * @param msg  The message delivered to the handler.
   * @param key  Plain-text property name, e.g. "$.ct" or an application key.
   * @return The property value, or NULL when the message carries no such
   *         property. A property sent with no value also yields NULL; walk
   *         @p msg->properties directly to tell the two apart.
   */
  const char* az_iot_c2d_message_property(const az_iot_c2d_message* msg, const char* key);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_C2D_CLIENT_H */
