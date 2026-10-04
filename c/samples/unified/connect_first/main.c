// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/connect_first - sample.
 *
 * unified/telemetry, building the feature clients the conservative way: open
 * the connection with NO feature client attached, and only once CONNECTED ask
 * az_iot_connection_client_get_hub_profile() which generation DPS assigned,
 * then build the matching MQTTv3 or MQTTv5 client. Nothing is assumed, so the
 * first connect never has to be redone.
 *
 * What it costs: a client built after CONNECTED subscribes only then, so on
 * that first connect CONNECTED does not say its subscriptions were granted.
 * Telemetry has none, which is why it is the feature used here.
 *
 * Once built, the client pins its generation like in every other sample. If
 * the device is later moved to a hub of the other generation, the connection
 * stops with AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH; this sample then destroys
 * the client, reopens with none attached, and builds again once CONNECTED.
 *
 * Runs ~60 seconds. DPS is handled internally by the connection client when
 * dps.id_scope is set.
 */
#include <stdio.h>
#include <stdlib.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

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
  /* Only the one matching `profile` is initialized. */
  az_iot_mqttv3_telemetry_client mqttv3;
  az_iot_mqttv5_telemetry_client mqttv5;
  az_iot_connection_profile profile;
  int telemetry_initialized;
} sample_state;

typedef struct
{
  az_iot_connection_state conn_state;
  int faulted; /* terminal: nothing this sample can do about it */
  int reopen; /* the client was built for the other generation */
  az_iot_connection_profile assigned_profile;
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
  if (state->profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    az_iot_mqttv5_telemetry_client_deinit(&state->mqttv5);
  }
  else
  {
    az_iot_mqttv3_telemetry_client_deinit(&state->mqttv3);
  }
  state->telemetry_initialized = 0;
}

static az_iot_result telemetry_build(sample_state* state, az_iot_connection_profile profile)
{
  az_iot_result result;
  switch (profile)
  {
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V5:
      result = az_iot_mqttv5_telemetry_client_init(&state->mqttv5, &state->connection_client);
      break;
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V3:
      result = az_iot_mqttv3_telemetry_client_init(&state->mqttv3, &state->connection_client);
      break;
    case AZ_IOT_CONNECTION_PROFILE_UNKNOWN:
    default:
      /* Unreachable once CONNECTED: an unknown profile fails the connection. */
      return AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED;
  }
  if (result == AZ_IOT_OK)
  {
    state->profile = profile;
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

  /* Only record it: feature clients must not be destroyed from inside the
   * callback, which runs nested in do_work(). */
  if (sample_event_is_profile_mismatch(event, &ctx->assigned_profile))
  {
    ctx->reopen = 1;
    return;
  }

  if (event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    ctx->faulted = 1;
    sample_report_unsupported_profile(event);
  }

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
  static const uint8_t payload[] = "{\"temp\":23}";
  az_iot_telemetry_property props[] = {
    { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
  };
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;
  msg.properties = props;
  msg.properties_count = sizeof(props) / sizeof(props[0]);

  az_iot_result result = state->profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5
      ? az_iot_mqttv5_telemetry_client_send(&state->mqttv5, &msg, on_send_done, ctx)
      : az_iot_mqttv3_telemetry_client_send(&state->mqttv3, &msg, on_send_done, ctx);
  ctx->send_pending = (result == AZ_IOT_OK);
  return result;
}

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
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
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &state.config);
  copts.certificate_provider = &state.certs.base;

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_add_state_observer(&state.connection_client, on_conn_state, &user_ctx);

  /* Both adapters: v3.1.1 serves DPS and an MQTTv3 hub, v5 serves an MQTTv5 hub. */
  if (az_iot_connection_client_register_mqtt_factory(
          &state.connection_client, az_iot_paho_factory_create_v3_1_1())
      != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  if (az_iot_connection_client_register_mqtt_factory(
          &state.connection_client, az_iot_paho_factory_create_v5())
      != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* No feature client yet: which one is needed is decided after CONNECTED. */
  if (az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  uint64_t end_ms = sample_now_ms() + SAMPLE_RUN_MS;
  uint64_t next_send_ms = 0;
  while (!user_ctx.faulted && sample_now_ms() < end_ms)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 100);

    if (user_ctx.reopen)
    {
      user_ctx.reopen = 0;
      printf(
          "DPS assigned %s; reopening without a telemetry client.\n",
          sample_connection_profile_name(user_ctx.assigned_profile));
      telemetry_destroy(&state);
      user_ctx.send_pending = 0; /* lost with the old client */
      az_iot_connection_client_close(&state.connection_client);
      if (az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
      {
        user_ctx.faulted = 1; /* recovery failed */
        break;
      }
      continue;
    }

    if (user_ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED)
    {
      continue;
    }

    /* The point of the sample: connected, nothing bound yet, and only now does
     * it ask what it reached and build to match. */
    if (!state.telemetry_initialized)
    {
      /* The mock bypass skips DPS and always speaks MQTT v5; nothing to read. */
      az_iot_connection_profile profile = sample_initial_profile(&state.config);
      if ((state.config.mock_endpoint == NULL
           && sample_get_hub_profile(&state.connection_client, &profile) != AZ_IOT_OK)
          || telemetry_build(&state, profile) != AZ_IOT_OK)
      {
        user_ctx.faulted = 1;
        break;
      }
    }

    if (!user_ctx.send_pending && sample_now_ms() >= next_send_ms)
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
