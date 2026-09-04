// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_GEN1_TELEMETRY_CLIENT_H
#define AZ_IOT_GEN1_TELEMETRY_CLIENT_H

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_message.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct az_iot_gen1_telemetry_client
  {
    struct
    {
      az_iot_connection_client* conn;
    } _internal;
  } az_iot_gen1_telemetry_client;

  /* The connection must be CONNECTED and resolved to the Classic profile. */
  AZ_NODISCARD az_iot_result az_iot_gen1_telemetry_client_init(
      az_iot_gen1_telemetry_client* client,
      az_iot_connection_client* conn);

  void az_iot_gen1_telemetry_client_destroy(az_iot_gen1_telemetry_client* client);

  AZ_NODISCARD az_iot_result az_iot_gen1_telemetry_client_send(
      az_iot_gen1_telemetry_client* client,
      const az_iot_telemetry_message* message,
      az_iot_telemetry_send_callback callback,
      void* user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_GEN1_TELEMETRY_CLIENT_H */
