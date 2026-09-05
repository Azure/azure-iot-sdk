// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_GEN2_C2D_CLIENT_H
#define AZ_IOT_GEN2_C2D_CLIENT_H

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_message.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct az_iot_gen2_c2d_client
  {
    struct
    {
      az_iot_connection_client* conn;
      az_iot_c2d_handler_callback handler;
      void* handler_ctx;
    } _internal;
  } az_iot_gen2_c2d_client;

  /**
   * @brief Initialize the MQTT v5 C2D client.
   *
   * Registers delivery for `ih/{device_id}/dev/c2d`. It issues no subscription
   * of its own: the presence handshake already holds `ih/{device_id}/dev/#`.
   *
   * The connection need not be open: this records that it must resolve to the
   * MQTT v5 profile, and the topic is built when it connects and the assigned
   * device id is known. A connection already known to be Classic -- a direct
   * connection, or a DPS one past assignment -- is rejected here with
   * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH; otherwise a conflict surfaces when
   * the connection resolves, which fails it before it reports CONNECTED. The
   * same error is returned when a gen1 client is already attached.
   */
  AZ_NODISCARD az_iot_result
  az_iot_gen2_c2d_client_init(az_iot_gen2_c2d_client* client, az_iot_connection_client* conn);

  void az_iot_gen2_c2d_client_destroy(az_iot_gen2_c2d_client* client);

  /** NULL @p cb pauses delivery without unregistering the handler. */
  az_iot_result az_iot_gen2_c2d_client_set_handler(
      az_iot_gen2_c2d_client* client,
      az_iot_c2d_handler_callback cb,
      void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_GEN2_C2D_CLIENT_H */
