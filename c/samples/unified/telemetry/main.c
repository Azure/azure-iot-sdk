// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/telemetry - sample.
 *
 * Send telemetry for ~60 seconds to the mqttv3 hub (MQTT v3.1.1) DPS assigns
 * the device.
 *
 * Most unified samples have this shape, and this is the one to read first:
 *
 *   1. Give the connection client an MQTT adapter and a workspace: it builds an
 *      MQTT v3.1.1 client for DPS and for the hub.
 *   2. Build the feature clients BEFORE open().
 *   3. If DPS assigns a hub generation this SDK does not support, the
 *      connection faults with AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED before
 *      the hub is reached, and the event carries the assigned profile.
 *
 * Building before open() keeps CONNECTED meaning "subscriptions granted" on the
 * first connect too. unified/connect_first shows the alternative: open first,
 * then build.
 *
 * DPS is handled internally by the connection client when dps.id_scope is set.
 */
#include <stdio.h>
#include <stdlib.h>

#include "azure/iot/az_iot.h"
/* The MQTT adapter this build of the sample uses (see samples/CMakeLists.txt). */
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#else
#error "Define AZ_IOT_SAMPLE_ADAPTER_PAHO or AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT"
#endif

#include "sample_utils.h"

/** @brief How long the sample runs. */
#define SAMPLE_RUN_MS 60000u
/** @brief Time between two messages while connected. */
#define SAMPLE_SEND_INTERVAL_MS 5000u

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  az_iot_mqttv3_telemetry_client mqttv3;
  int telemetry_initialized;
} sample_state;

typedef struct
{
  az_iot_connection_state conn_state;
  int faulted; /* terminal: nothing this sample can do about it */
  int send_pending;
  int sent_ok;
  int send_failed;
} user_context;

static void telemetry_destroy(sample_state* state)
{
  if (!state->telemetry_initialized)
  {
    return;
  }
  az_iot_mqttv3_telemetry_client_deinit(&state->mqttv3);
  state->telemetry_initialized = 0;
}

static az_iot_result telemetry_build(sample_state* state)
{
  az_iot_result result
      = az_iot_mqttv3_telemetry_client_init(&state->mqttv3, &state->connection_client);
  if (result == AZ_IOT_OK)
  {
    state->telemetry_initialized = 1;
  }
  return result;
}

static void sample_state_destroy(sample_state* state)
{
  telemetry_destroy(state);
  az_iot_connection_client_deinit(&state->connection_client);
  az_iot_certificate_provider_pem_deinit(&state->certs);
  sample_config_release(&state->config);
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;

  if (event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    ctx->faulted = 1;
    /* DPS assigned a hub generation this SDK does not support. */
    if (event->reason == AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED && event->profile != NULL)
    {
      printf(
          "Unsupported hub generation \"%s\"%s. Upgrade the SDK.\n",
          event->profile->connection_profile_raw != NULL ? event->profile->connection_profile_raw
                                                         : "(none)",
          event->profile->connection_profile_raw_truncated ? " (truncated)" : "");
    }
  }

  /* The provisioning lifecycle reports on its own scope; only the hub one says
   * whether telemetry can flow. */
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    ctx->conn_state = event->state;
  }
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->send_pending = 0;
  if (status == AZ_IOT_OK)
  {
    ctx->sent_ok++;
  }
  else
  {
    ctx->send_failed++;
  }
}

static az_iot_result send_one(sample_state* state, user_context* ctx)
{
  /* MQTTv3 URL-encodes every property into the topic ("$.ct" goes out as
   * "%24.ct", "deg C" as "deg%20C"). */
  static const uint8_t payload[] = "{\"temp\":23}";
  az_iot_telemetry_property props[] = {
    { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
    { "unit", "deg C" },
  };
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;
  msg.properties = props;
  msg.properties_count = sizeof(props) / sizeof(props[0]);

  az_iot_result result
      = az_iot_mqttv3_telemetry_client_send(&state->mqttv3, &msg, on_send_done, ctx);
  ctx->send_pending = (result == AZ_IOT_OK);
  return result;
}

int main(void)
{
  az_iot_log_sink log = sample_log_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  sample_state state = { 0 };
  if (sample_config_load(&state.config) != 0)
  {
    return 1;
  }

  user_context user_ctx = { 0 };

  /* Certificate provider */
  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  pem.trusted_ca_pem_path = state.config.ca;
  pem.client_cert_pem_path = state.config.cert;
  pem.client_key_pem_path = state.config.key;

  if (az_iot_certificate_provider_pem_init(&state.certs, &pem) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* Connection client (DPS provisioning is internal). The default reconnection
   * and identity recovery policies keep the device reconnecting on its own. */
  /* Memory for the connection client's entries, sized for the default limits. */
  static uint8_t connection_workspace[AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE];
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.workspace = connection_workspace;
  copts.workspace_size = sizeof(connection_workspace);
  copts.dps.id_scope = state.config.id_scope;
  copts.dps.registration_id = state.config.reg_id;
  copts.dps.global_endpoint = state.config.dps_global_endpoint; /* NULL: the global default */
  copts.certificate_provider = &state.certs.base;
  /* The adapter builds each session's MQTT 3.1.1 client in this workspace, one at a time: DPS,
   * then the hub. Limits: sample_utils.h. */
  static uint8_t mqtt_workspace[SAMPLE_MQTT_WORKSPACE_SIZE];
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
  copts.mqtt_adapter = az_iot_paho_get_adapter();
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
  copts.mqtt_adapter = az_iot_az_mqtt_get_adapter();
#endif
  copts.mqtt_limits = SAMPLE_MQTT_LIMITS;
  copts.mqtt_workspace = mqtt_workspace;
  copts.mqtt_workspace_size = sizeof(mqtt_workspace);

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_add_state_observer(&state.connection_client, on_conn_state, &user_ctx);

  if (telemetry_build(&state) != AZ_IOT_OK
      || az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  uint64_t end_ms = sample_now_ms() + SAMPLE_RUN_MS;
  uint64_t next_send_ms = 0;
  while (!user_ctx.faulted && sample_now_ms() < end_ms)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 100);

    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED && !user_ctx.send_pending
        && sample_now_ms() >= next_send_ms)
    {
      next_send_ms = sample_now_ms() + SAMPLE_SEND_INTERVAL_MS;
      if (send_one(&state, &user_ctx) != AZ_IOT_OK)
      {
        user_ctx.send_failed++;
      }
    }
  }

  printf("Sent %d message(s), %d failed.\n", user_ctx.sent_ok, user_ctx.send_failed);

  az_iot_connection_client_close(&state.connection_client);
  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
  }

  sample_state_destroy(&state);

  return (user_ctx.sent_ok > 0 && !user_ctx.faulted) ? 0 : 1;
}
