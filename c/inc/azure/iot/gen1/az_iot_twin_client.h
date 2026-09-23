// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_GEN1_TWIN_CLIENT_H
#define AZ_IOT_GEN1_TWIN_CLIENT_H

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

  typedef struct az_iot_gen1_twin_client
  {
    struct
    {
      az_iot_connection_client* conn;
      az_iot_twin_desired_callback desired_handler;
      void* desired_handler_ctx;
      uint32_t next_rid;
      struct
      {
        bool in_use;
        uint32_t rid;
        int kind; /* internal enum */
        union
        {
          az_iot_twin_get_callback get_cb;
          az_iot_twin_patch_complete_callback patch_cb;
        } cb;
        void* user_ctx;
      } pending[AZ_IOT_TWIN_MAX_PENDING];
    } _internal;
  } az_iot_gen1_twin_client;

  /**
   * @brief Initialize the IoT Hub Classic twin client.
   *
   * Subscribes `$iothub/twin/res/#` for GET and patch acknowledgements, and
   * `$iothub/twin/PATCH/properties/desired/#` for service-pushed desired
   * updates. Requests correlate on the `$rid` carried in the topic.
   *
   * The connection need not be open: this records that it must resolve to the
   * Classic profile. Unlike C2D, these topics carry no device id, so they are
   * known before the connection resolves and no connect-time bind is needed.
   * A connection already known to be MQTT_V5 -- a direct connection, or a DPS
   * one past assignment -- is rejected here with
   * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH; otherwise a conflict surfaces when
   * the connection resolves, which fails it before it reports CONNECTED. The
   * same error is returned when a gen2 client is already attached.
   */
  AZ_NODISCARD az_iot_result
  az_iot_gen1_twin_client_init(az_iot_gen1_twin_client* client, az_iot_connection_client* conn);

  void az_iot_gen1_twin_client_deinit(az_iot_gen1_twin_client* client);

  /**
   * @brief Request the full twin document.
   *
   * @return AZ_IOT_ERR_NOT_SUPPORTED when AZ_IOT_TWIN_MAX_PENDING requests are
   *         already in flight. The service reporting a throttle returns
   *         AZ_IOT_ERR_BUSY to @p cb instead, so the two are distinguishable.
   */
  AZ_NODISCARD az_iot_result az_iot_gen1_twin_client_get(
      az_iot_gen1_twin_client* twin,
      az_iot_twin_get_callback cb,
      void* user_ctx);

  /** @brief Merge @p patch into the reported-properties section. */
  AZ_NODISCARD az_iot_result az_iot_gen1_twin_client_patch_reported(
      az_iot_gen1_twin_client* twin,
      const uint8_t* patch,
      size_t patch_len,
      az_iot_twin_patch_complete_callback cb,
      void* user_ctx);

  /**
   * @brief Set the handler for service-pushed desired-property patches.
   *
   * The handler receives every desired update delivered while connected, and
   * decides whether it carries keys it cares about. NULL @p cb pauses delivery
   * without tearing the subscription down.
   *
   * Patches sent while the device is disconnected are not redelivered. To
   * catch up after a reconnect, call az_iot_gen1_twin_client_get() and ignore
   * patches whose version is not newer than the document's.
   */
  az_iot_result az_iot_gen1_twin_client_set_desired_handler(
      az_iot_gen1_twin_client* twin,
      az_iot_twin_desired_callback cb,
      void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_GEN1_TWIN_CLIENT_H */
