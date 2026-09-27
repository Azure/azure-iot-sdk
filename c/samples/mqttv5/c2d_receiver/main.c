// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* mqttv5/c2d_receiver - sample.
 *
 * Receive cloud-to-device messages from an AEG (Hub-Next) hub over MQTT v5. For
 * a device that may be assigned either generation, see unified/c2d_receiver.
 *
 * The difference worth knowing is that this client subscribes to nothing. AEG
 * connections complete a presence handshake before the SDK reports CONNECTED,
 * and that handshake already holds ih/<id>/dev/#, which covers the c2d topic.
 * init() therefore only registers a handler for ih/<id>/dev/c2d. The Classic
 * client has no such handshake, so it takes out its own subscription.
 *
 * Properties arrive as MQTT v5 user properties, already decoded by the adapter,
 * and the content type has a native field of its own -- so nothing here is
 * percent-decoded, which is most of what the Classic client spends its time on.
 *
 * Two adapters are registered even though only the hub is v5: the DPS leg is
 * still v3.1.1, and registering only the v5 factory makes provisioning fail
 * with AZ_IOT_ERR_NOT_SUPPORTED before the hub is ever reached.
 *
 * Provision via DPS, open, listen for ~60 seconds, close. DPS is handled
 * internally by the connection client when dps.id_scope is set.
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
  az_iot_mqttv5_c2d_client c2d;
  int c2d_initialized;
} sample_state;

static void sample_state_destroy(sample_state* s)
{
  if (s->c2d_initialized)
  {
    az_iot_mqttv5_c2d_client_destroy(&s->c2d);
    s->c2d_initialized = 0;
  }
  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);
}

typedef struct
{
  az_iot_connection_state conn_state;
  int provisioning_faulted;
  int messages_received;
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
        printf("This device is assigned to a Classic hub. Run the unified c2d_receiver sample "
               "instead.\n");
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
}

static void on_c2d(const az_iot_c2d_message* msg, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->messages_received++;

  printf("C2D #%d: %zu bytes", ctx->messages_received, msg->payload_len);
  if (msg->content_type)
  {
    /* The native MQTT v5 Content Type, not a property. On Classic the same
     * field is filled in from "$.ct" out of the topic bag, so an application
     * reads it the same way on either generation. */
    printf(" [%s]", msg->content_type);
  }

  /* The adapter decoded these; the client only copied the pointers across. The
   * count is bounded by AZ_IOT_C2D_MAX_PROPERTIES and anything past it is
   * dropped with a warning, but the message is still delivered -- there is no
   * text buffer to overflow here, which is the one bound Classic also has. */
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

  /* Connection client (DPS provisioning is internal) */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &state.config);
  copts.certificate_provider = &state.certs.base;

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_add_state_observer(&state.connection_client, on_conn_state, &user_ctx);

  /* v3.1.1 for the DPS leg, v5 for the hub leg. Both are required even though
   * only the hub is v5. */
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

  /* Before open, and before any profile is known: this declares which hub the
   * application is built for, and the connection is failed if it resolves to
   * the other one. No subscription is issued -- the presence handshake covers
   * this topic -- so this only records where deliveries should be routed. */
  if (az_iot_mqttv5_c2d_client_init(&state.c2d, &state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  state.c2d_initialized = 1;

  if (az_iot_mqttv5_c2d_client_set_handler(&state.c2d, on_c2d, &user_ctx) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  if (az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* CONNECTED is later here than on Classic: an AEG session is not up at
   * CONNACK, it subscribes and then exchanges a birth message with the service
   * first. Nothing to do about it but keep pumping. */
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
