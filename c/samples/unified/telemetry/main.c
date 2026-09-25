// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/telemetry - sample.
 *
 * Send telemetry for ~60 seconds to whichever hub DPS assigns the device: a
 * Classic IoT Hub (gen1, MQTT v3.1.1) or an AEG hub (gen2, MQTT v5), including
 * when the device is moved to a hub of the other generation while it runs. The
 * AEG-only route is gen2/telemetry.
 *
 * Every unified sample has this shape, and this is the one to read first:
 *
 *   1. Register BOTH MQTT adapters: DPS and Classic speak v3.1.1, AEG speaks v5.
 *   2. Build the feature clients for an assumed generation BEFORE open(). This
 *      one assumes Classic, which is what DPS assigns when it names no
 *      connectionProfile; a real device would persist the last assigned one.
 *   3. If DPS assigns the other generation -- on the first connect, or later
 *      when the device is moved and re-provisions -- the connection stops with
 *      AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH before the hub is reached, and
 *      the event carries the assigned profile. Destroy the clients, build the
 *      other generation's, then close() and open() again. In-flight operations
 *      of the old clients are lost.
 *
 * Building before open() keeps CONNECTED meaning "subscriptions granted" on the
 * first connect too. unified/connect_first shows the more conservative
 * alternative: open first, read the profile, then build.
 *
 * DPS is handled internally by the connection client when dps.id_scope is set.
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
  az_iot_gen1_telemetry_client gen1;
  az_iot_gen2_telemetry_client gen2;
  az_iot_connection_profile profile;
  int telemetry_initialized;
} sample_state;

typedef struct
{
  az_iot_connection_state conn_state;
  int faulted; /* terminal: nothing this sample can do about it */
  int rebuild; /* DPS assigned `assigned_profile`; rebuild for it */
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
    az_iot_gen2_telemetry_client_destroy(&state->gen2);
  }
  else
  {
    az_iot_gen1_telemetry_client_destroy(&state->gen1);
  }
  state->telemetry_initialized = 0;
}

/* Pins the connection to `profile`: an assignment to the other generation is
 * then refused with AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH. */
static az_iot_result telemetry_build(sample_state* state, az_iot_connection_profile profile)
{
  az_iot_result result;
  switch (profile)
  {
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V5:
      result = az_iot_gen2_telemetry_client_init(&state->gen2, &state->connection_client);
      break;
    case AZ_IOT_CONNECTION_PROFILE_CLASSIC:
      result = az_iot_gen1_telemetry_client_init(&state->gen1, &state->connection_client);
      break;
    case AZ_IOT_CONNECTION_PROFILE_UNKNOWN:
    default:
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
  az_iot_connection_client_destroy(&state->connection_client);
  az_iot_certificate_provider_pem_destroy(&state->certs);
  sample_config_release(&state->config);
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;

  /* Only record it: feature clients must not be destroyed from inside the
   * callback, which runs nested in do_work(). */
  if (sample_event_is_profile_mismatch(event, &ctx->assigned_profile))
  {
    ctx->rebuild = 1;
    return;
  }

  if (event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    ctx->faulted = 1;
    /* A generation newer than this SDK; the verbatim value is kept. */
    if (event->reason == AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED && event->profile)
    {
      printf(
          "Unsupported hub generation \"%s\". Upgrade the SDK.\n",
          event->profile->connection_profile_raw);
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
  /* The same message on either generation. Classic URL-encodes every
   * property into the topic ("$.ct" goes out as "%24.ct", "deg C" as
   * "deg%20C"); AEG carries them as MQTT v5 user properties, byte for byte,
   * with $.ct mapped to the native content type. */
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

  az_iot_result result = state->profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5
      ? az_iot_gen2_telemetry_client_send(&state->gen2, &msg, on_send_done, ctx)
      : az_iot_gen1_telemetry_client_send(&state->gen1, &msg, on_send_done, ctx);
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
   * policy is what re-provisions a device its hub no longer accepts. */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &state.config);
  copts.certificate_provider = &state.certs.base;

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_add_state_observer(&state.connection_client, on_conn_state, &user_ctx);

  /* Both adapters: v3.1.1 serves DPS and a Classic hub, v5 serves an AEG hub.
   * The connection picks the one the assigned hub needs. */
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

  if (telemetry_build(&state, AZ_IOT_CONNECTION_PROFILE_CLASSIC) != AZ_IOT_OK
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

    if (user_ctx.rebuild)
    {
      user_ctx.rebuild = 0;
      printf(
          "DPS assigned %s; rebuilding the telemetry client.\n",
          sample_connection_profile_name(user_ctx.assigned_profile));
      telemetry_destroy(&state);
      user_ctx.send_pending = 0; /* lost with the old client */
      az_iot_connection_client_close(&state.connection_client);
      if (telemetry_build(&state, user_ctx.assigned_profile) != AZ_IOT_OK
          || az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
      {
        break;
      }
      continue;
    }

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

  printf(
      "Sent %d message(s) on %s, %d failed.\n",
      user_ctx.sent_ok,
      sample_connection_profile_name(state.profile),
      user_ctx.send_failed);

  az_iot_connection_client_close(&state.connection_client);
  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
  }

  sample_state_destroy(&state);

  return (user_ctx.sent_ok > 0 && !user_ctx.faulted) ? 0 : 1;
}
