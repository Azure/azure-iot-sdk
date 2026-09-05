// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* c2d_receiver - sample.
 *
 * Provision via DPS, open connection, subscribe for cloud-to-device messages,
 * print received payloads. Runs for ~60 seconds then exits. DPS is handled
 * internally by the connection client when host == NULL and dps.id_scope is set.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include "sample_utils.h"

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  az_iot_gen1_c2d_client gen1_c2d;
  az_iot_gen2_c2d_client gen2_c2d;
  az_iot_connection_profile c2d_profile;
  int c2d_initialized;
} sample_state;

static void c2d_destroy(sample_state* s)
{
  if (!s->c2d_initialized)
  {
    return;
  }
  if (s->c2d_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    az_iot_gen2_c2d_client_destroy(&s->gen2_c2d);
  }
  else
  {
    az_iot_gen1_c2d_client_destroy(&s->gen1_c2d);
  }
  s->c2d_initialized = 0;
}

static void sample_state_destroy(sample_state* s)
{
  c2d_destroy(s);
  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);
}

typedef struct
{
  sample_state* state;
  az_iot_connection_state conn_state;
  az_iot_result c2d_status;
  int messages_received;
} user_context;

static void on_c2d(const az_iot_c2d_message* msg, void* user_ctx);

/* The generation is only known once CONNECTED reports the resolved profile,
 * and a reconnect can land on the other one -- so the client is rebuilt from
 * every transition rather than constructed once up front. */
static az_iot_result c2d_rebuild(
    sample_state* s,
    az_iot_connection_profile profile,
    void* handler_ctx)
{
  c2d_destroy(s);

  az_iot_result result;
  if (profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    result = az_iot_gen2_c2d_client_init(&s->gen2_c2d, &s->connection_client);
    if (result == AZ_IOT_OK)
    {
      result = az_iot_gen2_c2d_client_set_handler(&s->gen2_c2d, on_c2d, handler_ctx);
    }
  }
  else if (profile == AZ_IOT_CONNECTION_PROFILE_CLASSIC)
  {
    result = az_iot_gen1_c2d_client_init(&s->gen1_c2d, &s->connection_client);
    if (result == AZ_IOT_OK)
    {
      result = az_iot_gen1_c2d_client_set_handler(&s->gen1_c2d, on_c2d, handler_ctx);
    }
  }
  else
  {
    return AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED;
  }

  if (result == AZ_IOT_OK)
  {
    s->c2d_profile = profile;
    s->c2d_initialized = 1;
  }
  return result;
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->conn_state = event->state;
  if (event->state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    ctx->c2d_status = event->profile
        ? c2d_rebuild(ctx->state, event->profile->connection_profile, ctx)
        : AZ_IOT_ERR_INTERNAL;
  }
}

static void on_c2d(const az_iot_c2d_message* msg, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->messages_received++;

  printf("C2D #%d: %zu bytes", ctx->messages_received, msg->payload_len);
  if (msg->content_type)
  {
    printf(" [%s]", msg->content_type);
  }

  /* Properties arrive as plain text -- the same spelling the sender used, with
   * the topic's percent-encoding already undone. */
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
  user_context user_ctx = { .state = &state, .c2d_status = AZ_IOT_ERR_NOT_INITIALIZED };

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

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_set_state_callback(&state.connection_client, on_conn_state, &user_ctx);

  /* MQTT adapters: register both v3.1.1 (DPS + Classic) and v5 (Next). */
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

  /* Open (internally provisions via DPS then connects to assigned hub). The
   * C2D client is created from the state callback, once the profile is known. */
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

  if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED && user_ctx.c2d_status == AZ_IOT_OK)
  {
    printf("Connected. Listening for C2D messages (~60s)...\n");

    /* Pump for ~60 seconds (600 * 100ms) */
    for (int i = 0; i < 600; ++i)
    {
      (void)az_iot_connection_client_do_work(&state.connection_client, 100);
    }

    printf("Done. Received %d C2D message(s).\n", user_ctx.messages_received);
    rc = 0;
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
