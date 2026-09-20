// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "e2e_device.h"
#include "support/test_env.h"

#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* DPS provision + MQTT connect can take a while on a cold hub. */
#define E2E_DEVICE_CONNECT_TIMEOUT_S 90

/* Read the egress configuration: which transport carries MQTT, and whether it
 * goes through an HTTP proxy. All optional; unset means "TCP, no proxy", which
 * is what every existing e2e leg gets. */
static void egress_config_load(e2e_device* dev)
{
  char* transport = az_iot_test_env_dup("AZ_IOT_MQTT_TRANSPORT");
  dev->transport = AZ_IOT_MQTT_TRANSPORT_TCP;
  if (transport != NULL)
  {
    if (strcmp(transport, "websocket") == 0 || strcmp(transport, "websockets") == 0)
    {
      dev->transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;
    }
    free(transport);
  }
  dev->websocket_path = az_iot_test_env_dup("AZ_IOT_MQTT_WEBSOCKET_PATH");

  dev->proxy_host = az_iot_test_env_dup("AZ_IOT_PROXY_HOST");
  dev->proxy_username = az_iot_test_env_dup("AZ_IOT_PROXY_USERNAME");
  dev->proxy_password = az_iot_test_env_dup("AZ_IOT_PROXY_PASSWORD");
  dev->proxy_port = 0;
  char* proxy_port = az_iot_test_env_dup("AZ_IOT_PROXY_PORT");
  if (proxy_port != NULL)
  {
    unsigned long p = strtoul(proxy_port, NULL, 10);
    if (p > 0 && p <= 65535)
    {
      dev->proxy_port = (uint16_t)p;
    }
    free(proxy_port);
  }
}

static int device_config_load(e2e_device* dev)
{
  dev->id_scope = az_iot_test_env_dup("AZ_IOT_DPS_ID_SCOPE");
  dev->reg_id = az_iot_test_env_dup("AZ_IOT_DPS_REGISTRATION_ID");
  dev->cert = az_iot_test_env_dup("AZ_IOT_CLIENT_CERT");
  dev->key = az_iot_test_env_dup("AZ_IOT_CLIENT_KEY");
  dev->ca = az_iot_test_env_dup("AZ_IOT_TRUSTED_CA");
  dev->global_endpoint = az_iot_test_env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT");
  dev->hub_hostname = az_iot_test_env_dup("AZ_IOT_HUB_HOSTNAME");
  dev->hub_device_id = az_iot_test_env_dup("AZ_IOT_DEVICE_ID");
  egress_config_load(dev);

  if (dev->cert == NULL || dev->key == NULL || dev->ca == NULL)
  {
    fprintf(
        stderr,
        "[e2e] missing required device env vars: AZ_IOT_CLIENT_CERT/"
        "AZ_IOT_CLIENT_KEY/AZ_IOT_TRUSTED_CA\n");
    return 1;
  }

  /* Direct-hub mode wins when it is fully configured; otherwise DPS, which is
   * what every existing leg uses. */
  if (dev->hub_hostname != NULL && dev->hub_device_id != NULL)
  {
    dev->device_id = dev->hub_device_id;
    return 0;
  }

  if (dev->id_scope == NULL || dev->reg_id == NULL)
  {
    fprintf(
        stderr,
        "[e2e] missing required device env vars: AZ_IOT_DPS_ID_SCOPE/"
        "AZ_IOT_DPS_REGISTRATION_ID (or AZ_IOT_HUB_HOSTNAME/AZ_IOT_DEVICE_ID)\n");
    return 1;
  }
  dev->device_id = dev->reg_id;
  return 0;
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  az_iot_connection_state s = event->state;
  az_iot_result reason = event->reason;
  (void)reason;
  ((e2e_device*)user_ctx)->conn_state = s;
}

void e2e_device_do_work(e2e_device* dev, int ms)
{
  (void)az_iot_connection_client_do_work(&dev->conn, ms);
}

int e2e_device_connect(e2e_device* dev)
{
  if (device_config_load(dev) != 0)
  {
    return 1;
  }

  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  pem.trusted_ca_pem_path = dev->ca;
  pem.client_cert_pem_path = dev->cert;
  pem.client_key_pem_path = dev->key;
  if (az_iot_certificate_provider_pem_init(&dev->certs, &pem) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] certificate provider init failed\n");
    return 1;
  }
  dev->certs_ok = true;

  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  if (dev->hub_hostname != NULL && dev->hub_device_id != NULL)
  {
    copts.host = dev->hub_hostname;
    copts.client_id = dev->hub_device_id;
  }
  else
  {
    copts.dps.id_scope = dev->id_scope;
    copts.dps.registration_id = dev->reg_id;
    if (dev->global_endpoint != NULL)
    {
      copts.dps.global_endpoint = dev->global_endpoint;
    }
  }
  copts.certificate_provider = &dev->certs.base;
  copts.transport = dev->transport;
  copts.websocket_path = dev->websocket_path;
  copts.proxy.host = dev->proxy_host;
  copts.proxy.port = dev->proxy_port;
  copts.proxy.username = dev->proxy_username;
  copts.proxy.password = dev->proxy_password;

  if (az_iot_connection_client_init(&dev->conn, &copts) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] connection client init failed\n");
    return 1;
  }
  dev->conn_ok = true;
  az_iot_connection_client_add_state_observer(&dev->conn, on_conn_state, dev);

  if (az_iot_connection_client_register_mqtt_factory(
          &dev->conn, az_iot_paho_factory_create_v3_1_1())
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&dev->conn, az_iot_paho_factory_create_v5())
          != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] MQTT factory registration failed\n");
    return 1;
  }

  if (az_iot_connection_client_open(&dev->conn) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] connection open failed\n");
    return 1;
  }

  time_t start = time(NULL);
  while (dev->conn_state != AZ_IOT_CONN_STATE_CONNECTED
         && (time(NULL) - start) < E2E_DEVICE_CONNECT_TIMEOUT_S)
  {
    e2e_device_do_work(dev, 50);
    if (dev->conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }

  if (dev->conn_state != AZ_IOT_CONN_STATE_CONNECTED)
  {
    fprintf(stderr, "[e2e] device did not reach CONNECTED (state=%d)\n", (int)dev->conn_state);
    return 1;
  }
  return 0;
}

void e2e_device_disconnect(e2e_device* dev)
{
  if (dev->conn_ok)
  {
    az_iot_connection_client_close(&dev->conn);
    for (int i = 0; i < 100 && dev->conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
    {
      e2e_device_do_work(dev, 50);
    }
    az_iot_connection_client_destroy(&dev->conn);
    dev->conn_ok = false;
  }
  if (dev->certs_ok)
  {
    az_iot_certificate_provider_pem_destroy(&dev->certs);
    dev->certs_ok = false;
  }
  free(dev->id_scope);
  free(dev->reg_id);
  free(dev->cert);
  free(dev->key);
  free(dev->ca);
  free(dev->global_endpoint);
  free(dev->hub_hostname);
  free(dev->hub_device_id);
  free(dev->websocket_path);
  free(dev->proxy_host);
  free(dev->proxy_username);
  free(dev->proxy_password);
  dev->id_scope = NULL;
  dev->reg_id = NULL;
  dev->cert = NULL;
  dev->key = NULL;
  dev->ca = NULL;
  dev->global_endpoint = NULL;
  dev->hub_hostname = NULL;
  dev->hub_device_id = NULL;
  dev->websocket_path = NULL;
  dev->proxy_host = NULL;
  dev->proxy_username = NULL;
  dev->proxy_password = NULL;
  dev->device_id = NULL;
}
