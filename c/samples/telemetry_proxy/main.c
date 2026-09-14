// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* telemetry through an HTTP proxy - sample.
 *
 * The same flow as the `telemetry` sample -- provision via DPS, connect, send
 * one telemetry message, close -- with every MQTT session tunnelled through an
 * HTTP CONNECT proxy.
 *
 * WHY: many industrial and corporate networks permit no direct outbound
 * connection at all; the only way out is a proxy. This reaches IoT Hub through
 * one without changing anything above the transport.
 *
 * WHAT THE FEATURE COSTS YOU, in full:
 *
 *     copts.proxy.host = "proxy.corp.example";
 *     copts.proxy.port = 3128;
 *
 * plus the two optional credential fields below. Everything else here is
 * identical to the `telemetry` sample.
 *
 * TLS IS UNAFFECTED. The proxy carries the tunnel; the TLS session is
 * negotiated end to end with IoT Hub INSIDE it. The proxy therefore sees only
 * ciphertext, and server certificate and hostname validation happen exactly as
 * they would on a direct connection. The proxy is not a TLS peer and is not
 * trusted with anything.
 *
 * It applies to the DPS connect as well as the hub connect: a device that needs
 * a proxy to reach the hub needs it to reach the provisioning service first.
 *
 * It also works with either transport. This sample stays on TCP (8883 through
 * the tunnel) to keep the proxy the only variable; see telemetry_websockets for
 * the transport, and set both fields together if your network requires it.
 *
 * IF THE PROXY CANNOT BE REACHED, THE CONNECT FAILS. The SDK never falls back
 * to a direct connection, because doing so would bypass the egress control the
 * proxy exists to enforce -- while reporting success.
 *
 * Configuration: the same environment variables as the `telemetry` sample, plus
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

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  az_iot_gen1_telemetry_client gen1_telemetry;
  az_iot_gen2_telemetry_client gen2_telemetry;
  az_iot_connection_profile telemetry_profile;
  int telemetry_initialized;
  /* Owned by the sample, not by the SDK: az_iot_connection_client_init() copies
   * the options struct but not the strings it points at, so anything handed to
   * it must stay alive for as long as the client does. */
  char* proxy_host;
  char* proxy_port;
  char* proxy_username;
  char* proxy_password;
} sample_state;

static void telemetry_destroy(sample_state* state)
{
  if (!state->telemetry_initialized)
  {
    return;
  }
  if (state->telemetry_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    az_iot_gen2_telemetry_client_destroy(&state->gen2_telemetry);
  }
  else
  {
    az_iot_gen1_telemetry_client_destroy(&state->gen1_telemetry);
  }
  state->telemetry_initialized = 0;
}

static az_iot_result telemetry_rebuild(sample_state* state, az_iot_connection_profile profile)
{
  telemetry_destroy(state);

  az_iot_result result;
  if (profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    result = az_iot_gen2_telemetry_client_init(&state->gen2_telemetry, &state->connection_client);
  }
  else if (profile == AZ_IOT_CONNECTION_PROFILE_CLASSIC)
  {
    result = az_iot_gen1_telemetry_client_init(&state->gen1_telemetry, &state->connection_client);
  }
  else
  {
    return AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED;
  }

  if (result == AZ_IOT_OK)
  {
    state->telemetry_profile = profile;
    state->telemetry_initialized = 1;
  }
  return result;
}

static az_iot_result telemetry_send(
    sample_state* state,
    const az_iot_telemetry_message* message,
    az_iot_telemetry_send_callback callback,
    void* user_ctx)
{
  if (!state->telemetry_initialized)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }
  return state->telemetry_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5
      ? az_iot_gen2_telemetry_client_send(&state->gen2_telemetry, message, callback, user_ctx)
      : az_iot_gen1_telemetry_client_send(&state->gen1_telemetry, message, callback, user_ctx);
}

static void sample_state_destroy(sample_state* state)
{
  telemetry_destroy(state);
  az_iot_connection_client_destroy(&state->connection_client);
  az_iot_certificate_provider_pem_destroy(&state->certs);
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

typedef struct
{
  sample_state* state;
  az_iot_connection_state conn_state;
  az_iot_result telemetry_status;
  int send_done;
  az_iot_result send_status;
} user_context;

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->conn_state = event->state;
  if (event->state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    ctx->telemetry_status = event->profile
        ? telemetry_rebuild(ctx->state, event->profile->connection_profile)
        : AZ_IOT_ERR_INTERNAL;
  }
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->send_status = status;
  ctx->send_done = 1;
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

  int rc = 1;
  user_context user_ctx = { .state = &state, .telemetry_status = AZ_IOT_ERR_NOT_INITIALIZED };

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

  /* Connection client (DPS provisioning is internal when host==NULL) */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.dps.id_scope = state.config.id_scope;
  copts.dps.registration_id = state.config.reg_id;
  copts.certificate_provider = &state.certs.base;

  /* THE FEATURE: tunnel every MQTT session through an HTTP CONNECT proxy.
   *
   * Required here rather than optional, so that a run which appears to succeed
   * cannot silently have gone direct -- that is the whole property this sample
   * is meant to demonstrate. */
  state.proxy_host = sample_env_dup("AZ_IOT_PROXY_HOST", NULL);
  if (state.proxy_host == NULL)
  {
    (void)fprintf(stderr, "AZ_IOT_PROXY_HOST is not set; nothing to demonstrate.\n");
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
  /* Both may be NULL: a proxy that does not authenticate needs neither. The
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
  az_iot_connection_client_set_state_callback(&state.connection_client, on_conn_state, &user_ctx);

  /* MQTT adapters: register both v3.1.1 (DPS + Classic) and v5 (Next).
   * The connection client selects the appropriate factory based on the
   * session role. Both may be backed by different MQTT libraries. */
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

  /* Open (internally provisions via DPS then connects to assigned hub) */
  if (az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  for (int i = 0; i < 1200 && user_ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }

  if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED && user_ctx.telemetry_status == AZ_IOT_OK)
  {
    /* Send one telemetry message */
    static const uint8_t payload[] = "{\"temp\":23}";
    az_iot_telemetry_property props[] = {
      { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
    };
    az_iot_telemetry_message msg = { 0 };
    msg.payload = payload;
    msg.payload_len = sizeof(payload) - 1;
    msg.properties = props;
    msg.properties_count = sizeof(props) / sizeof(props[0]);

    if (telemetry_send(&state, &msg, on_send_done, &user_ctx) == AZ_IOT_OK)
    {
      for (int i = 0; i < 600 && !user_ctx.send_done; ++i)
      {
        (void)az_iot_connection_client_do_work(&state.connection_client, 50);
      }

      if (user_ctx.send_done && user_ctx.send_status == AZ_IOT_OK)
      {
        rc = 0;
      }
    }
  }

  /* Close connection */
  az_iot_connection_client_close(&state.connection_client);

  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
  }

  sample_state_destroy(&state);

  return rc;
}
