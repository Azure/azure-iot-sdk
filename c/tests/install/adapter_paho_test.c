// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* azure::iot::adapter_paho: both factories build clients, and one registers
 * with a connection client. No network I/O. */
#include <stddef.h>

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_connection_client.h"

#include "install_test.h"

static az_iot_connection_client g_conn;

static void check_factory(az_iot_mqtt_factory* factory, az_iot_mqtt_version version)
{
  CHECK(factory != NULL);
  CHECK(factory->version == version);
  az_iot_mqtt_client* client = factory->create(factory->factory_ctx);
  CHECK(client != NULL);
  CHECK(client->iface->version == version);
  client->iface->destroy(client);
}

int main(void)
{
  az_iot_mqtt_factory* v3 = az_iot_paho_factory_create_v3_1_1();
  check_factory(v3, AZ_IOT_MQTT_VERSION_3_1_1);
  az_iot_mqtt_factory* v5 = az_iot_paho_factory_create_v5();
  check_factory(v5, AZ_IOT_MQTT_VERSION_5);

  az_iot_connection_client_options options = az_iot_connection_client_options_default();
  options.host = "hub.example";
  options.client_id = "install-test";
  CHECK(az_iot_connection_client_init(&g_conn, &options) == AZ_IOT_OK);
  /* A registered factory is destroyed by deinit(). */
  CHECK(az_iot_connection_client_register_mqtt_factory(&g_conn, v3) == AZ_IOT_OK);
  az_iot_connection_client_deinit(&g_conn);

  az_iot_paho_factory_destroy(v5);
  return 0;
}
