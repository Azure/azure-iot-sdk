// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_GEN2_TWIN_CLIENT_H
#define AZ_IOT_GEN2_TWIN_CLIENT_H

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

  typedef struct az_iot_gen2_twin_client
  {
    struct
    {
      az_iot_connection_client* conn;
      az_iot_twin_desired_callback desired_handler;
      void* desired_handler_ctx;
      uint32_t next_rid;
      /* Length of "ih/{device_id}/dev/twin/", resolved when the connection
       * binds. Lets each handler confirm a delivered topic is the exact leaf it
       * was registered for, since dispatch matches by prefix. */
      size_t inbound_prefix_len;
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
  } az_iot_gen2_twin_client;

  /**
   * @brief Initialize the MQTT v5 twin client.
   *
   * Registers delivery for `ih/{device_id}/dev/twin/get/response`,
   * `ih/{device_id}/dev/twin/reported/response` and
   * `ih/{device_id}/dev/twin/desired`. It issues no subscription of its own:
   * the presence handshake already holds `ih/{device_id}/dev/#`, which covers
   * all three. Requests correlate on MQTT v5 correlation data rather than a
   * topic-borne `$rid`.
   *
   * The connection need not be open: this records that it must resolve to the
   * MQTT v5 profile, and the topics are built when it connects and the assigned
   * device id is known. A connection already known to be Classic -- a direct
   * connection, or a DPS one past assignment -- is rejected here with
   * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH; otherwise a conflict surfaces when
   * the connection resolves, which fails it before it reports CONNECTED. The
   * same error is returned when a gen1 client is already attached.
   */
  AZ_NODISCARD az_iot_result
  az_iot_gen2_twin_client_init(az_iot_gen2_twin_client* client, az_iot_connection_client* conn);

  void az_iot_gen2_twin_client_deinit(az_iot_gen2_twin_client* client);

  /**
   * @brief Request the full twin document.
   *
   * @return AZ_IOT_ERR_NOT_SUPPORTED when AZ_IOT_TWIN_MAX_PENDING requests are
   *         already in flight.
   */
  AZ_NODISCARD az_iot_result az_iot_gen2_twin_client_get(
      az_iot_gen2_twin_client* twin,
      az_iot_twin_get_callback cb,
      void* user_ctx);

  /**
   * @brief Merge @p patch into the reported-properties section.
   *
   * The acknowledgement arrives on its own topic and carries no reported
   * version, so @p cb is always called with version 0.
   */
  AZ_NODISCARD az_iot_result az_iot_gen2_twin_client_patch_reported(
      az_iot_gen2_twin_client* twin,
      const uint8_t* patch,
      size_t patch_len,
      az_iot_twin_patch_complete_callback cb,
      void* user_ctx);

  /**
   * @brief Set the handler for service-pushed desired-property patches.
   *
   * The handler receives every desired update and decides whether it carries
   * keys it cares about. NULL @p cb pauses delivery without unregistering the
   * handler.
   */
  az_iot_result az_iot_gen2_twin_client_set_desired_handler(
      az_iot_gen2_twin_client* twin,
      az_iot_twin_desired_callback cb,
      void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_GEN2_TWIN_CLIENT_H */
