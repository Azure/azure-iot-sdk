// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* azure::iot::adapter_az_mqtt: the adapter builds clients of both versions in a
 * workspace, and a connection client takes it. No network I/O. */
#include <stddef.h>
#include <stdlib.h>

#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include "azure/iot/az_iot_connection_client.h"

#include "install_test.h"

static const az_iot_mqtt_limits k_limits = { 4096, 4096, 8, 8192, 4096 };

static az_iot_connection_client g_conn;
static uint8_t g_connection_workspace[AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE];

static void check_client(uint8_t* workspace, size_t workspace_size, az_iot_mqtt_version version)
{
  az_iot_mqtt_client* client = NULL;
  CHECK(
      az_iot_az_mqtt_get_adapter()->init(version, &k_limits, workspace, workspace_size, &client)
      == AZ_IOT_OK);
  CHECK(client != NULL);
  CHECK(client->iface->version == version);
  client->iface->destroy(client);
}

int main(void)
{
  /* On the heap: its size depends on the TLS backend the package was built with. */
  size_t const mqtt_workspace_size = az_iot_az_mqtt_get_workspace_size(&k_limits);
  CHECK(mqtt_workspace_size > 0);
  uint8_t* mqtt_workspace = (uint8_t*)malloc(mqtt_workspace_size);
  CHECK(mqtt_workspace != NULL);
  check_client(mqtt_workspace, mqtt_workspace_size, AZ_IOT_MQTT_VERSION_3_1_1);
  check_client(mqtt_workspace, mqtt_workspace_size, AZ_IOT_MQTT_VERSION_5);

  az_iot_connection_client_options options = az_iot_connection_client_options_default();
  options.iothub_hostname = "hub.example";
  options.device_id = "install-test";
  options.workspace = g_connection_workspace;
  options.workspace_size = sizeof(g_connection_workspace);
  options.mqtt_adapter = az_iot_az_mqtt_get_adapter();
  options.mqtt_limits = k_limits;
  options.mqtt_workspace = mqtt_workspace;
  options.mqtt_workspace_size = mqtt_workspace_size;
  CHECK(az_iot_connection_client_init(&g_conn, &options) == AZ_IOT_OK);
  az_iot_connection_client_deinit(&g_conn);
  free(mqtt_workspace);
  return 0;
}
