// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* proxy - sample.
 *
 * telemetry_gen1, with every MQTT session tunnelled through an HTTP CONNECT
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
 * Classic-only, like telemetry_gen1: the proxy is independent of the hub
 * generation, so serving one route keeps the sample about the proxy. The same
 * options work unchanged on the AEG route.
 *
 * Configuration: the same environment variables as telemetry_gen1, plus
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
  az_iot_gen1_telemetry_client telemetry;
  int telemetry_initialized;
  /* Owned by the sample, not by the SDK: az_iot_connection_client_init() copies
   * the options struct but not the strings it points at, so anything handed to
   * it must outlive the client. */
  char* proxy_host;
  char* proxy_port;
  char* proxy_username;
  char* proxy_password;
} sample_state;

static void sample_state_destroy(sample_state* state)
{
  if (state->telemetry_initialized)
  {
    az_iot_gen1_telemetry_client_destroy(&state->telemetry);
    state->telemetry_initialized = 0;
  }
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
  az_iot_connection_state conn_state;
  int provisioning_faulted;
  az_iot_result conn_reason;
  int send_done;
  az_iot_result send_status;
} user_context;

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  /* A rejected assignment or a failed registration faults the provisioning
   * lifecycle and leaves the hub IDLE, so the wait below must watch for it. */
  if (event->scope == AZ_IOT_CONN_SCOPE_DPS)
  {
    if (event->state == AZ_IOT_CONN_STATE_FAULTED)
    {
      ctx->provisioning_faulted = 1;
      if (event->reason == AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH)
      {
        printf("This device is assigned to an AEG hub. Run the telemetry_gen2 sample instead.\n");
      }
    }
    return;
  }

  /* Hub lifecycle only: the provisioning session reports on its own scope,
   * and storing its state here would overwrite the hub state this code acts
   * on. */
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }

  ctx->conn_state = event->state;
  ctx->conn_reason = event->reason;
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

  /* Connection client (DPS provisioning is internal) */
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

  /* One adapter covers both legs here: DPS always speaks v3.1.1, and so does a
   * Classic hub. */
  if (az_iot_connection_client_register_mqtt_factory(
          &state.connection_client, az_iot_paho_factory_create_v3_1_1())
      != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* Before open, and before any profile is known: this declares which hub the
   * application is built for, and the connection is failed if it resolves to
   * the other one. */
  if (az_iot_gen1_telemetry_client_init(&state.telemetry, &state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  state.telemetry_initialized = 1;

  if (az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  for (int i = 0; i < 1200 && user_ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED || user_ctx.provisioning_faulted)
    {
      break;
    }
  }

  if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    /* Classic carries every property in the TOPIC, as a URL-encoded bag after
     * devices/<id>/messages/events/. So the key "$.ct" goes on the wire as
     * "%24.ct", and "deg C" becomes "deg%20C" -- the SDK encodes both, and the
     * application always reads and writes the plain text. The practical limit
     * is topic length, not a property count. */
    static const uint8_t payload[] = "{\"temp\":23}";
    az_iot_telemetry_property props[] = {
      { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
      { AZ_IOT_MSG_PROP_MESSAGE_ID, "sample-1" },
      { "unit", "deg C" },
    };
    az_iot_telemetry_message msg = { 0 };
    msg.payload = payload;
    msg.payload_len = sizeof(payload) - 1;
    msg.properties = props;
    msg.properties_count = sizeof(props) / sizeof(props[0]);

    if (az_iot_gen1_telemetry_client_send(&state.telemetry, &msg, on_send_done, &user_ctx)
        == AZ_IOT_OK)
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

  az_iot_connection_client_close(&state.connection_client);

  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
  }

  sample_state_destroy(&state);

  return rc;
}
