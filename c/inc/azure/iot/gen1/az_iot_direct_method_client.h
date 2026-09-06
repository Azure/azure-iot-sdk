// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_GEN1_DIRECT_METHOD_CLIENT_H
#define AZ_IOT_GEN1_DIRECT_METHOD_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_message.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* IoT Hub Classic direct methods (MQTT v3.1.1).
   *
   *   Subscribe  "$iothub/methods/POST/#"
   *   Inbound    "$iothub/methods/POST/{methodName}/?$rid={rid}"
   *   Respond    "$iothub/methods/res/{status}/?$rid={rid}"
   */
  typedef struct az_iot_gen1_direct_method_client
  {
    struct
    {
      az_iot_connection_client* conn;
      az_iot_direct_method_handler_callback handler;
      void* handler_ctx;
      az_iot_direct_method_request req_pool[AZ_IOT_DM_MAX_INFLIGHT];
    } _internal;
  } az_iot_gen1_direct_method_client;

  /**
   * @brief Initialize the IoT Hub Classic direct method client.
   *
   * The connection need not be open: this records that it must resolve to the
   * Classic profile. A connection already known to be MQTT_V5 -- a direct
   * connection, or a DPS one past assignment -- is rejected here with
   * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH; otherwise a conflict surfaces when
   * the connection resolves, which fails it before it reports CONNECTED. The
   * same error is returned when a gen2 client is already attached.
   */
  AZ_NODISCARD az_iot_result az_iot_gen1_direct_method_client_init(
      az_iot_gen1_direct_method_client* client,
      az_iot_connection_client* conn);

  void az_iot_gen1_direct_method_client_destroy(az_iot_gen1_direct_method_client* client);

  /* Not AZ_NODISCARD: a configuration setter (fails only on invalid arguments). */
  az_iot_result az_iot_gen1_direct_method_client_set_handler(
      az_iot_gen1_direct_method_client* client,
      az_iot_direct_method_handler_callback cb,
      void* user_ctx);

  /**
   * @brief Answer an invocation, releasing its pool slot.
   *
   * Not AZ_NODISCARD: best-effort response send, commonly fire-and-forget.
   *
   * @return AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH if @p request came from a
   *         gen2 client, AZ_IOT_ERR_INVALID_ARG if it was already answered.
   */
  az_iot_result az_iot_gen1_direct_method_respond(
      az_iot_direct_method_request* request,
      int status_code,
      const uint8_t* payload,
      size_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_GEN1_DIRECT_METHOD_CLIENT_H */
