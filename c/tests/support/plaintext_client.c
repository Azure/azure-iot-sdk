// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "support/plaintext_client.h"

#include "internal/connection_client_internal.h"

az_iot_result az_iot_test_connection_client_init(
    az_iot_connection_client* client,
    const az_iot_connection_client_options* opts)
{
  az_iot_result r = az_iot_connection_client_init(client, opts);
  if (r == AZ_IOT_OK)
  {
    az_iot_connection_client__allow_plaintext_for_testing(client);
  }
  return r;
}
