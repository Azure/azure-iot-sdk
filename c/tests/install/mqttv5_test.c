// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* azure::iot::mqttv5: a feature client attaches to a connection client. */
#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/mqttv5/az_iot_custom_topic_client.h"
#include "azure/iot/mqttv5/az_iot_telemetry_client.h"

#include "install_test.h"
#include "install_test_mqtt_adapter.h"

static az_iot_connection_client g_conn;
static uint8_t g_connection_workspace[AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE];

int main(void)
{
  az_iot_connection_client_options options = az_iot_connection_client_options_default();
  options.iothub_hostname = "hub.example";
  options.device_id = "install-test";
  options.workspace = g_connection_workspace;
  options.workspace_size = sizeof(g_connection_workspace);
  install_test_use_stub_mqtt_adapter(&options);
  options.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  CHECK(az_iot_connection_client_init(&g_conn, &options) == AZ_IOT_OK);

  az_iot_mqttv5_telemetry_client telemetry;
  CHECK(az_iot_mqttv5_telemetry_client_init(&telemetry, &g_conn) == AZ_IOT_OK);
  az_iot_mqttv5_telemetry_client_deinit(&telemetry);

  az_iot_mqttv5_custom_topic_client custom;
  CHECK(az_iot_mqttv5_custom_topic_client_init(&custom, &g_conn) == AZ_IOT_OK);
  az_iot_mqttv5_custom_topic_client_deinit(&custom);

  az_iot_connection_client_deinit(&g_conn);
  return 0;
}
