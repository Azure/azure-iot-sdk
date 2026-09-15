// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* c2d_receiver_gen1 - sample.
 *
 * Receive cloud-to-device messages from a Classic IoT Hub. The AEG route is a
 * separate sample, c2d_receiver_gen2.
 *
 * Classic owns its own subscription: init() takes out
 * devices/<id>/messages/devicebound/# and every message arrives on a topic
 * built from that prefix. Properties ride in the topic, so the client has to
 * split the bag and percent-decode it before the handler sees anything -- the
 * work the gen2 client never does.
 *
 * The client is created before the connection opens, because
 * az_iot_gen1_c2d_client_init() records the generation it needs rather than
 * reading one off a live connection. Only the v3.1.1 factory is registered:
 * Classic speaks v3.1.1 and so does DPS, so one adapter covers both legs.
 *
 * Provision via DPS, open, listen for ~60 seconds, close. DPS is handled
 * internally by the connection client when host == NULL and dps.id_scope is
 * set.
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
  az_iot_gen1_c2d_client c2d;
  int c2d_initialized;
} sample_state;

static void sample_state_destroy(sample_state* s)
{
  if (s->c2d_initialized)
  {
    az_iot_gen1_c2d_client_destroy(&s->c2d);
    s->c2d_initialized = 0;
  }
  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);
}

typedef struct
{
  az_iot_connection_state conn_state;
  int messages_received;
} user_context;

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->conn_state = event->state;

  /* The device provisioned to an AEG hub, so this Classic client can never
   * serve it. The connection faults before reporting CONNECTED rather than
   * subscribing to a topic shape that hub does not publish. */
  if (event->reason == AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH)
  {
    printf("This device is assigned to an AEG hub. Run the c2d_receiver_gen2 sample instead.\n");
  }
}

static void on_c2d(const az_iot_c2d_message* msg, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->messages_received++;

  printf("C2D #%d: %zu bytes", ctx->messages_received, msg->payload_len);
  if (msg->content_type)
  {
    /* Classic has no native content type: this is the "$.ct" property, looked
     * up out of the decoded bag and offered here so both generations read the
     * same way. */
    printf(" [%s]", msg->content_type);
  }

  /* Plain text by the time it reaches here. On the wire these were percent-
   * encoded into the topic -- "%24.mid" for "$.mid", "a%20b" for "a b" -- and
   * the client undid all of it.
   *
   * The two bounds fail differently, which matters if you are sizing them. Past
   * AZ_IOT_C2D_MAX_PROPERTIES the first few are kept and the rest dropped. But
   * if the decoded text overruns AZ_IOT_C2D_PROPERTY_BUFFER, or any escape is
   * malformed, the client surfaces NO properties at all rather than risk
   * handing over a truncated key or value. Either way the message itself is
   * still delivered, and either way there is a warning. */
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

  /* Connection client (DPS provisioning is internal when host==NULL) */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &state.config);
  copts.certificate_provider = &state.certs.base;

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_set_state_callback(&state.connection_client, on_conn_state, &user_ctx);

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
   * the other one. The subscription itself is taken out at connect time, once
   * DPS has assigned the device id the topic is built from. */
  if (az_iot_gen1_c2d_client_init(&state.c2d, &state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  state.c2d_initialized = 1;

  if (az_iot_gen1_c2d_client_set_handler(&state.c2d, on_c2d, &user_ctx) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

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

  if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    printf("Connected. Listening for cloud-to-device messages (~60s)...\n");

    for (int i = 0; i < 600; ++i)
    {
      (void)az_iot_connection_client_do_work(&state.connection_client, 100);
    }

    printf("Received %d message(s).\n", user_ctx.messages_received);
    rc = 0;
  }

  az_iot_connection_client_close(&state.connection_client);

  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
  }

  sample_state_destroy(&state);

  return rc;
}
