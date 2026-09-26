// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/c2d_receiver - sample.
 *
 * Receive cloud-to-device messages for ~60 seconds on whichever hub DPS
 * assigns: Classic (mqttv3) or AEG (mqttv5), including after the device is moved to
 * a hub of the other generation. The AEG-only route is mqttv5/c2d_receiver. See
 * unified/telemetry for the shape every unified sample shares: build for an
 * assumed generation before open(), rebuild when DPS assigns the other one.
 *
 * Both clients deliver through the same handler type, so on_c2d below serves
 * either generation. What differs is underneath:
 *
 *   - Classic: the client subscribes to devices/<id>/messages/devicebound/#,
 *     and the properties ride in the topic, so the client percent-decodes them
 *     before the handler sees anything.
 *   - AEG: the client subscribes to nothing -- the presence handshake that
 *     precedes CONNECTED already holds ih/<id>/dev/# -- and the properties
 *     arrive as MQTT v5 user properties, already decoded.
 *
 * DPS is handled internally by the connection client when dps.id_scope is set.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include "sample_utils.h"

/** @brief How long the sample listens. */
#define SAMPLE_RUN_MS 60000u

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  /* Only the one matching `profile` is initialized. */
  az_iot_mqttv3_c2d_client mqttv3;
  az_iot_mqttv5_c2d_client mqttv5;
  az_iot_connection_profile profile;
  int c2d_initialized;
} sample_state;

typedef struct
{
  sample_state* state;
  az_iot_connection_state conn_state;
  int connected_count;
  int faulted; /* terminal: nothing this sample can do about it */
  int rebuild; /* DPS assigned `assigned_profile`; rebuild for it */
  az_iot_connection_profile assigned_profile;
  int messages_received;
} user_context;

static void clients_destroy(sample_state* s, user_context* ctx)
{
  (void)ctx;
  if (!s->c2d_initialized)
  {
    return;
  }
  if (s->profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    az_iot_mqttv5_c2d_client_destroy(&s->mqttv5);
  }
  else
  {
    az_iot_mqttv3_c2d_client_destroy(&s->mqttv3);
  }
  s->c2d_initialized = 0;
}

static void sample_state_destroy(sample_state* s)
{
  clients_destroy(s, NULL);
  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);
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
   * whether the feature client can work. */
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    if (event->state == AZ_IOT_CONN_STATE_CONNECTED)
    {
      ctx->connected_count++;
    }
    ctx->conn_state = event->state;
  }
}

static void on_c2d(const az_iot_c2d_message* msg, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->messages_received++;

  printf("C2D #%d: %zu bytes", ctx->messages_received, msg->payload_len);
  if (msg->content_type)
  {
    /* The native MQTT v5 content type on AEG; the "$.ct" property out of the
     * topic on Classic. Read the same way on either. */
    printf(" [%s]", msg->content_type);
  }

  /* Plain text on either generation. Past AZ_IOT_C2D_MAX_PROPERTIES the rest
   * are dropped with a warning. On Classic only, a decoded bag that overruns
   * AZ_IOT_C2D_PROPERTY_BUFFER, or a malformed escape, surfaces NO properties
   * rather than a truncated one. The message itself is always delivered. */
  for (size_t i = 0; i < msg->properties_count; ++i)
  {
    printf(
        " %s=%s", msg->properties[i].key, msg->properties[i].value ? msg->properties[i].value : "");
  }

  if (msg->payload_len > 0 && msg->payload_len <= 256)
  {
    printf(" => %.*s", (int)msg->payload_len, (const char*)msg->payload);
  }
  printf("\n");
}

/* Pins the connection to `profile`: an assignment to the other generation is
 * then refused with AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH. */
static az_iot_result clients_build(
    sample_state* s,
    az_iot_connection_profile profile,
    user_context* ctx)
{
  az_iot_result result;
  switch (profile)
  {
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V5:
      result = az_iot_mqttv5_c2d_client_init(&s->mqttv5, &s->connection_client);
      if (result == AZ_IOT_OK)
      {
        s->profile = profile;
        s->c2d_initialized = 1;
        result = az_iot_mqttv5_c2d_client_set_handler(&s->mqttv5, on_c2d, ctx);
      }
      return result;
    case AZ_IOT_CONNECTION_PROFILE_CLASSIC:
      result = az_iot_mqttv3_c2d_client_init(&s->mqttv3, &s->connection_client);
      if (result == AZ_IOT_OK)
      {
        s->profile = profile;
        s->c2d_initialized = 1;
        result = az_iot_mqttv3_c2d_client_set_handler(&s->mqttv3, on_c2d, ctx);
      }
      return result;
    case AZ_IOT_CONNECTION_PROFILE_UNKNOWN:
    default:
      return AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED;
  }
}

static void pump(sample_state* s, uint32_t timeout_ms)
{
  (void)az_iot_connection_client_do_work(&s->connection_client, timeout_ms);
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

  user_context user_ctx = { .state = &state };

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

  /* Both adapters: v3.1.1 serves DPS and a Classic hub, v5 serves an AEG hub. */
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

  /* Assume a generation until DPS says otherwise; see unified/telemetry. */
  if (clients_build(&state, sample_initial_profile(&state.config), &user_ctx) != AZ_IOT_OK
      || az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  uint64_t end_ms = sample_now_ms() + SAMPLE_RUN_MS;
  while (!user_ctx.faulted && sample_now_ms() < end_ms)
  {
    pump(&state, 100);

    if (user_ctx.rebuild)
    {
      user_ctx.rebuild = 0;
      printf(
          "DPS assigned %s; rebuilding the feature client.\n",
          sample_connection_profile_name(user_ctx.assigned_profile));
      clients_destroy(&state, &user_ctx);
      az_iot_connection_client_close(&state.connection_client);
      if (clients_build(&state, user_ctx.assigned_profile, &user_ctx) != AZ_IOT_OK
          || az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
      {
        user_ctx.faulted = 1; /* recovery failed */
        break;
      }
      continue;
    }
  }

  printf("Received %d message(s).\n", user_ctx.messages_received);
  int rc = (user_ctx.connected_count > 0 && !user_ctx.faulted) ? 0 : 1;

  az_iot_connection_client_close(&state.connection_client);
  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    pump(&state, 50);
  }

  sample_state_destroy(&state);

  return rc;
}
