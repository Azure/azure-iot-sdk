// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/c2d_receiver - sample.
 *
 * Receive cloud-to-device messages for ~60 seconds. C2D is an MQTTv3 IoT Hub
 * feature: the client subscribes to devices/<id>/messages/devicebound/#, and
 * the properties ride in the topic, so the client percent-decodes them before
 * the handler sees anything.
 *
 * There is no MQTT v5 counterpart: C2D is not carried on the MQTTv5 hub. The mqttv3
 * client pins MQTTv3 at init(), before open(), so DPS assigning an MQTTv5 hub --
 * on the first connect or after a move -- stops the connection with
 * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH. The sample reports that and exits
 * non-zero; it does not rebuild. A move to another MQTTv3 hub needs nothing
 * from the application.
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
  az_iot_mqttv3_c2d_client c2d;
  int c2d_initialized;
} sample_state;

typedef struct
{
  az_iot_connection_state conn_state;
  int connected_count;
  int faulted; /* terminal: nothing this sample can do about it */
  int unsupported_hub; /* DPS assigned `assigned_profile`, which has no C2D */
  az_iot_connection_profile assigned_profile;
  int messages_received;
} user_context;

static void sample_state_destroy(sample_state* s)
{
  if (s->c2d_initialized)
  {
    az_iot_mqttv3_c2d_client_deinit(&s->c2d);
    s->c2d_initialized = 0;
  }
  az_iot_connection_client_deinit(&s->connection_client);
  az_iot_certificate_provider_pem_deinit(&s->certs);
  sample_config_release(&s->config);
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;

  if (sample_event_is_profile_mismatch(event, &ctx->assigned_profile))
  {
    ctx->unsupported_hub = 1;
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
    /* The "$.ct" property out of the topic. */
    printf(" [%s]", msg->content_type);
  }

  /* Plain text. Past AZ_IOT_C2D_MAX_PROPERTIES the rest are dropped with a
   * warning. A decoded bag that overruns AZ_IOT_C2D_PROPERTY_BUFFER, or a
   * malformed escape, surfaces NO properties rather than a truncated one. The
   * message itself is always delivered. */
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

  /* The mock bypass is MQTT v5 only. */
  if (sample_initial_profile(&state.config) != AZ_IOT_CONNECTION_PROFILE_MQTT_V3)
  {
    printf("Cloud-to-device messages are not available on this hub generation.\n");
    sample_state_destroy(&state);
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

  /* v3.1.1 serves both DPS and an MQTTv3 hub. */
  if (az_iot_connection_client_register_mqtt_factory(
          &state.connection_client, az_iot_paho_factory_create_v3_1_1())
      != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* Pins MQTTv3: an MQTTv5 assignment is refused before that hub is reached. */
  if (az_iot_mqttv3_c2d_client_init(&state.c2d, &state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  state.c2d_initialized = 1;

  if (az_iot_mqttv3_c2d_client_set_handler(&state.c2d, on_c2d, &user_ctx) != AZ_IOT_OK
      || az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  uint64_t end_ms = sample_now_ms() + SAMPLE_RUN_MS;
  while (!user_ctx.faulted && !user_ctx.unsupported_hub && sample_now_ms() < end_ms)
  {
    pump(&state, 100);
  }

  if (user_ctx.unsupported_hub)
  {
    printf(
        "DPS assigned %s. Cloud-to-device messages are not available on this hub generation.\n",
        sample_connection_profile_name(user_ctx.assigned_profile));
  }

  printf("Received %d message(s).\n", user_ctx.messages_received);
  int rc = (user_ctx.connected_count > 0 && !user_ctx.faulted && !user_ctx.unsupported_hub) ? 0 : 1;

  az_iot_connection_client_close(&state.connection_client);
  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    pump(&state, 50);
  }

  sample_state_destroy(&state);

  return rc;
}
