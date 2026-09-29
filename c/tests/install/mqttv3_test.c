// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* azure::iot::mqttv3: a feature client attaches to a connection client. */
#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/mqttv3/az_iot_telemetry_client.h"

#include "install_test.h"

static az_iot_connection_client g_conn;

int main(void)
{
  az_iot_connection_client_options options = az_iot_connection_client_options_default();
  options.host = "hub.example";
  options.client_id = "install-test";
  options.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V3;
  CHECK(az_iot_connection_client_init(&g_conn, &options) == AZ_IOT_OK);

  az_iot_mqttv3_telemetry_client telemetry;
  CHECK(az_iot_mqttv3_telemetry_client_init(&telemetry, &g_conn) == AZ_IOT_OK);
  az_iot_mqttv3_telemetry_client_deinit(&telemetry);

  az_iot_connection_client_deinit(&g_conn);
  return 0;
}
