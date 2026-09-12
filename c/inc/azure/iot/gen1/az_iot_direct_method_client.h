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

  /* Default for az_iot_gen1_direct_method_client_set_response_deadline().
   *
   * IoT Hub never tells the device the caller's responseTimeoutInSeconds, so the
   * only safe default is the service maximum. Past it the invocation has
   * certainly been abandoned service-side -- a result sent later is discarded --
   * so reclaiming cannot race an answer anyone is still waiting for. */
#ifndef AZ_IOT_GEN1_DM_RESPONSE_DEADLINE_SECONDS
#define AZ_IOT_GEN1_DM_RESPONSE_DEADLINE_SECONDS 300u
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
      /* Parallel to req_pool: when each in-flight invocation stops being worth
       * answering. Held here rather than on az_iot_direct_method_request so
       * gen2, which shares that type, does not carry storage for a deadline it
       * derives from the exec budget instead. */
      uint64_t req_deadline_ms[AZ_IOT_DM_MAX_INFLIGHT];
      uint32_t response_deadline_seconds;
      /* Next pool index to try. Allocation cycles rather than always taking the
       * lowest free slot, so a slot just reclaimed from an application that
       * never answered is the last one reused, not the first. */
      size_t next_slot;
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
   * @brief How long an invocation may hold its pool slot before the client
   *        takes it back.
   *
   * An application that never answers an invocation would otherwise hold that
   * slot for the life of the client, and once AZ_IOT_DM_MAX_INFLIGHT are held
   * every further invocation is dropped. Set this above the longest method this
   * device services; @p seconds of 0 restores
   * AZ_IOT_GEN1_DM_RESPONSE_DEADLINE_SECONDS.
   *
   * Values past the IoT Hub maximum responseTimeoutInSeconds (300) only delay
   * the reclaim -- the service has stopped waiting by then either way.
   *
   * Not AZ_NODISCARD: a configuration setter (fails only on invalid arguments).
   */
  az_iot_result az_iot_gen1_direct_method_client_set_response_deadline(
      az_iot_gen1_direct_method_client* client,
      uint32_t seconds);

  /**
   * @brief Answer an invocation, releasing its pool slot.
   *
   * Not AZ_NODISCARD: best-effort response send, commonly fire-and-forget.
   *
   * An invocation that is never answered has its slot reclaimed once its
   * response deadline passes, so dropping one costs a delay rather than the
   * client's capacity to receive methods. Answering after that is refused: the
   * service stopped waiting long before.
   *
   * @return AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH if @p request came from a
   *         gen2 client, AZ_IOT_ERR_INVALID_ARG if it was already answered or
   *         its slot was reclaimed.
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
