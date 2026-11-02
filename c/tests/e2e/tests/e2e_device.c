// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "e2e_device.h"
#include "e2e_connect_clock.h"
#include "e2e_defensive_timeout.h"
#include "e2e_mqtt_adapter.h"
#include "support/test_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The hub connect; DPS provisioning has its own budget (e2e_connect_clock.h). */
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
    char* profile = az_iot_test_env_dup("AZ_IOT_HUB_CONNECTION_PROFILE");
    dev->hub_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V3;
    if (profile != NULL && strcmp(profile, "mqttv5") == 0)
    {
      dev->hub_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
    }
    else if (profile != NULL && strcmp(profile, "mqttv3") != 0)
    {
      fprintf(stderr, "[e2e] AZ_IOT_HUB_CONNECTION_PROFILE must be mqttv3 or mqttv5\n");
      free(profile);
      return 1;
    }
    free(profile);
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
  /* Hub lifecycle only: the provisioning session reports on its own scope,
   * and storing its state here would overwrite the hub state this code acts
   * on. */
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }

  az_iot_connection_state s = event->state;
  /* A failed attempt: the transport code (the TLS stack's own, e.g. a SECURITY_STATUS) says why. */
  if (event->error != NULL && event->error->source != AZ_IOT_CONN_ERR_SRC_NONE)
  {
    fprintf(
        stderr,
        "[e2e] hub %s reason %s: error source %d code %ld (0x%08lx): %.*s\n",
        az_iot_connection_state_to_string(s),
        az_iot_result_to_string(event->reason),
        (int)event->error->source,
        (long)event->error->code,
        (unsigned long)(uint32_t)event->error->code,
        (int)az_span_size(event->error->message),
        az_span_size(event->error->message) > 0 ? (const char*)az_span_ptr(event->error->message)
                                                : "");
  }
  ((e2e_device*)user_ctx)->conn_state = s;
}

void e2e_device_do_work(e2e_device* dev, int ms)
{
  (void)az_iot_connection_client_do_work(&dev->conn, ms);
}

int e2e_device_connect(e2e_device* dev) { return e2e_device_connect_with(dev, NULL); }

int e2e_device_connect_with(e2e_device* dev, const e2e_device_connect_options* opts)
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
    copts.iothub_hostname = dev->hub_hostname;
    copts.device_id = dev->hub_device_id;
    copts.connection_profile = dev->hub_profile;
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
  copts.twin_push.push_desired = opts != NULL && opts->twin_push_desired;
  if (opts != NULL)
  {
    if (opts->saved_hub != NULL)
    {
      copts.iothub_hostname = opts->saved_hub->hostname;
      copts.device_id = opts->saved_hub->device_id;
      copts.connection_profile = opts->saved_hub->profile;
    }
    if (opts->reconnection_policy != NULL)
    {
      copts.reconnection_policy = *opts->reconnection_policy;
    }
    if (opts->identity_recovery_mode != NULL)
    {
      copts.identity_recovery.mode = *opts->identity_recovery_mode;
    }
    if (opts->identity_recovery_policy != NULL)
    {
      copts.identity_recovery.policy = *opts->identity_recovery_policy;
    }
    if (opts->max_hub_connect_attempts_before_reprovision != 0)
    {
      copts.dps.max_hub_connect_attempts_before_reprovision
          = opts->max_hub_connect_attempts_before_reprovision;
    }
  }
  copts.twin_push.push_reported = opts != NULL && opts->twin_push_reported;
  copts.workspace = dev->connection_workspace;
  copts.workspace_size = sizeof(dev->connection_workspace);
  /* The MQTT adapter this build of the suite uses (see tests/e2e/CMakeLists.txt). */
  dev->mqtt_workspace = e2e_use_mqtt_adapter(&copts);
  if (dev->mqtt_workspace == NULL)
  {
    fprintf(stderr, "[e2e] no memory for the MQTT workspace\n");
    return 1;
  }

  if (az_iot_connection_client_init(&dev->conn, &copts) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] connection client init failed\n");
    return 1;
  }
  dev->conn_ok = true;
  if (e2e_use_short_defensive_timeouts(&dev->conn) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] defensive-timeout schedule rejected\n");
    return 1;
  }
  az_iot_connection_client_add_state_observer(&dev->conn, on_conn_state, dev);

  if (opts != NULL && opts->before_open != NULL
      && opts->before_open(dev, opts->before_open_ctx) != 0)
  {
    fprintf(stderr, "[e2e] before_open failed\n");
    return 1;
  }

  if (az_iot_connection_client_open(&dev->conn) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] connection open failed\n");
    return 1;
  }

  const int timeout_s = opts != NULL && opts->connect_timeout_s > 0 ? opts->connect_timeout_s
                                                                    : E2E_DEVICE_CONNECT_TIMEOUT_S;
  e2e_connect_clock clock;
  e2e_connect_clock_start(&clock);
  while (dev->conn_state != AZ_IOT_CONN_STATE_CONNECTED
         && e2e_connect_clock_running(&clock, &dev->conn, timeout_s))
  {
    e2e_device_do_work(dev, 50);
    if (opts != NULL && opts->during_connect != NULL)
    {
      opts->during_connect(opts->during_connect_ctx);
    }
    if (dev->conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }

  e2e_connect_clock_log(&clock, dev->device_id);
  if (dev->conn_state != AZ_IOT_CONN_STATE_CONNECTED)
  {
    fprintf(stderr, "[e2e] device did not reach CONNECTED (state=%d)\n", (int)dev->conn_state);
    return 1;
  }
  return 0;
}

int e2e_device_get_hub(const e2e_device* dev, e2e_device_hub* out)
{
  if (!dev->conn_ok || dev->conn_state != AZ_IOT_CONN_STATE_CONNECTED)
  {
    return 1;
  }
  /* @p out may be the saved hub the connection still borrows: read everything first. */
  const char* host = az_iot_connection_client_get_iothub_address(&dev->conn);
  const char* device_id = az_iot_connection_client_get_device_id(&dev->conn);
  az_iot_hub_profile profile;
  if (host == NULL || device_id == NULL
      || az_iot_connection_client_get_hub_profile(&dev->conn, &profile) != AZ_IOT_OK
      || profile.connection_profile == AZ_IOT_CONNECTION_PROFILE_UNKNOWN
      || strlen(host) >= sizeof(out->hostname) || strlen(device_id) >= sizeof(out->device_id))
  {
    return 1;
  }
  e2e_device_hub hub;
  memset(&hub, 0, sizeof(hub));
  snprintf(hub.hostname, sizeof(hub.hostname), "%s", host);
  snprintf(hub.device_id, sizeof(hub.device_id), "%s", device_id);
  hub.profile = profile.connection_profile;
  *out = hub;
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
    az_iot_connection_client_deinit(&dev->conn);
    dev->conn_ok = false;
  }
  free(dev->mqtt_workspace);
  dev->mqtt_workspace = NULL;
  if (dev->certs_ok)
  {
    az_iot_certificate_provider_pem_deinit(&dev->certs);
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
