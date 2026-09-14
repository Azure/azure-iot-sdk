// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_GEN2_TELEMETRY_CLIENT_H
#define AZ_IOT_GEN2_TELEMETRY_CLIENT_H

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_message.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* How many MQTT v5 user properties one telemetry message can carry, counting
 * the two the client adds itself: `type` and `content-type`. Properties past it
 * are dropped, and the send warns once naming the first one lost.
 *
 * Override it at compile time (for example
 * -DAZ_IOT_GEN2_TELEMETRY_MAX_USER_PROPERTIES=32) **on the whole build**. It
 * sizes a buffer inside the send path rather than any struct the application
 * declares, so setting it for the application alone changes nothing -- the SDK
 * keeps the value it was compiled with, and the extra properties are still
 * dropped. */
#ifndef AZ_IOT_GEN2_TELEMETRY_MAX_USER_PROPERTIES
#define AZ_IOT_GEN2_TELEMETRY_MAX_USER_PROPERTIES 16
#endif

  typedef struct az_iot_gen2_telemetry_client
  {
    struct
    {
      az_iot_connection_client* conn;
    } _internal;
  } az_iot_gen2_telemetry_client;

  /* Records that the connection must resolve to the MQTT v5 profile; it need
   * not be open yet. A connection already known to be Classic, or one a gen1
   * client is already attached to, is rejected with
   * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH. Otherwise a conflict fails the
   * connection when the profile resolves, before it reports CONNECTED. */
  AZ_NODISCARD az_iot_result az_iot_gen2_telemetry_client_init(
      az_iot_gen2_telemetry_client* client,
      az_iot_connection_client* conn);

  void az_iot_gen2_telemetry_client_destroy(az_iot_gen2_telemetry_client* client);

  AZ_NODISCARD az_iot_result az_iot_gen2_telemetry_client_send(
      az_iot_gen2_telemetry_client* client,
      const az_iot_telemetry_message* message,
      az_iot_telemetry_send_callback callback,
      void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_GEN2_TELEMETRY_CLIENT_H */
