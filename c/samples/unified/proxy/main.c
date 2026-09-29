// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/proxy - sample.
 *
 * unified/telemetry, with every MQTT session tunnelled through an HTTP CONNECT
 * proxy. Everything else is identical to that sample, so the difference between
 * the two files is exactly the feature.
 *
 * WHY: many industrial and corporate networks permit no direct outbound
 * connection at all; the only way out is a proxy.
 *
 * WHAT THE FEATURE COSTS YOU, in full:
 *
 *     copts.proxy.host = "proxy.corp.example";
 *     copts.proxy.port = 3128;
 *
 * plus the two optional credential fields below.
 *
 * TLS IS UNAFFECTED. The proxy carries the tunnel; the TLS session is
 * negotiated end to end with IoT Hub INSIDE it. The proxy therefore sees only
 * ciphertext, and server certificate and hostname validation happen exactly as
 * they would on a direct connection. The proxy is not a TLS peer and is trusted
 * with nothing.
 *
 * It applies to the DPS connect as well as the hub connect: a device that needs
 * a proxy to reach the hub needs it to reach the provisioning service first.
 * The proxy is independent of the hub generation, so it applies to either one
 * DPS assigns.
 *
 * IF THE PROXY CANNOT BE REACHED, THE CONNECT FAILS. The SDK never falls back
 * to a direct connection, because that would bypass the egress control the
 * proxy exists to enforce -- while reporting success.
 *
 * The websockets sample is the other half of this pair. The proxy works with
 * either transport; this one stays on TCP (8883 through the tunnel) so that the
 * proxy is the only variable. Set both options together if your network needs a
 * proxy AND only passes 443.
 *
 * Configuration: the same environment variables as unified/telemetry, plus
 *   AZ_IOT_PROXY_HOST      required here; without it the sample exits
 *   AZ_IOT_PROXY_PORT      optional, default 8080
 *   AZ_IOT_PROXY_USERNAME  optional, for a proxy requiring HTTP Basic
 *   AZ_IOT_PROXY_PASSWORD  optional, used only alongside the username
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
  /* Owned by the sample, not by the SDK: az_iot_connection_client_init() copies
   * the options struct but not the strings it points at, so anything handed to
   * it must outlive the client. */
  char* proxy_host;
  char* proxy_port;
  char* proxy_username;
  char* proxy_password;
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
    az_iot_mqttv5_telemetry_client_deinit(&state->mqttv5);
  }
  else
  {
    az_iot_mqttv3_telemetry_client_deinit(&state->mqttv3);
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
      result = az_iot_mqttv5_telemetry_client_init(&state->mqttv5, &state->connection_client);
      break;
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V3:
      result = az_iot_mqttv3_telemetry_client_init(&state->mqttv3, &state->connection_client);
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
  az_iot_connection_client_deinit(&state->connection_client);
  az_iot_certificate_provider_pem_deinit(&state->certs);
  sample_config_release(&state->config);
  free(state->proxy_host);
  free(state->proxy_port);
  free(state->proxy_username);
  free(state->proxy_password);
  state->proxy_host = NULL;
  state->proxy_port = NULL;
  state->proxy_username = NULL;
  state->proxy_password = NULL;
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
    sample_report_unsupported_profile(event);
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
  /* The same message on either generation. MQTTv3 URL-encodes every
   * property into the topic ("$.ct" goes out as "%24.ct", "deg C" as
   * "deg%20C"); MQTTv5 carries them as MQTT v5 user properties, byte for byte,
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
   * policy is what re-provisions a device its hub no longer accepts. */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &state.config);
  copts.certificate_provider = &state.certs.base;

  /* THE FEATURE: tunnel every MQTT session through an HTTP CONNECT proxy.
   *
   * Required rather than optional here, so that a run which appears to succeed
   * cannot silently have gone direct -- that is the property this sample is
   * meant to demonstrate. */
  state.proxy_host = sample_env_dup("AZ_IOT_PROXY_HOST", NULL);
  if (state.proxy_host == NULL)
  {
    printf("AZ_IOT_PROXY_HOST is not set; nothing to demonstrate.\n");
    sample_state_destroy(&state);
    return 1;
  }
  state.proxy_port = sample_env_dup("AZ_IOT_PROXY_PORT", NULL);
  state.proxy_username = sample_env_dup("AZ_IOT_PROXY_USERNAME", NULL);
  state.proxy_password = sample_env_dup("AZ_IOT_PROXY_PASSWORD", NULL);

  copts.proxy.host = state.proxy_host;
  /* 0 selects AZ_IOT_MQTT_DEFAULT_PROXY_PORT (8080). */
  copts.proxy.port
      = (state.proxy_port != NULL) ? (uint16_t)strtoul(state.proxy_port, NULL, 10) : (uint16_t)0;
  /* Both may be NULL: a proxy that does not authenticate needs neither, and the
   * password is meaningful only alongside a username. Credentials are sent as
   * HTTP Basic, so a proxy that requires them should be reached over a network
   * segment you trust. */
  copts.proxy.username = state.proxy_username;
  copts.proxy.password = state.proxy_password;

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_add_state_observer(&state.connection_client, on_conn_state, &user_ctx);

  /* Both adapters: v3.1.1 serves DPS and an MQTTv3 hub, v5 serves an MQTTv5 hub.
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

  if (telemetry_build(&state, sample_initial_profile(&state.config)) != AZ_IOT_OK
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
        user_ctx.faulted = 1; /* recovery failed */
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
