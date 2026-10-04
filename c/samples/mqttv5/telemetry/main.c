// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* mqttv5/telemetry - sample.
 *
 * Send one telemetry message to an MQTTv5 hub over MQTT v5. An
 * application that must serve either hub generation picks at runtime -- see
 * unified/telemetry.
 *
 * The client is created before the connection opens, because
 * az_iot_mqttv5_telemetry_client_init() records the generation it needs rather
 * than reading one off a live connection. A sample that served both generations
 * could not do that: it would have to wait for CONNECTED, read the resolved
 * profile, and only then build the right client.
 *
 * Two adapters are registered, which an MQTTv3-only app would not need: the hub
 * leg is MQTT v5, but the DPS leg is still v3.1.1. Registering only the v5
 * factory makes provisioning fail with AZ_IOT_ERR_NOT_SUPPORTED before the hub
 * is ever reached.
 *
 * Provision via DPS, open, send, close. DPS is handled internally by the
 * connection client when dps.id_scope is set.
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
  az_iot_mqttv5_telemetry_client telemetry;
  int telemetry_initialized;
} sample_state;

static void sample_state_destroy(sample_state* state)
{
  if (state->telemetry_initialized)
  {
    az_iot_mqttv5_telemetry_client_deinit(&state->telemetry);
    state->telemetry_initialized = 0;
  }
  az_iot_connection_client_deinit(&state->connection_client);
  az_iot_certificate_provider_pem_deinit(&state->certs);
  sample_config_release(&state->config);
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
        printf("This device is assigned to an MQTTv3 hub. Run the unified telemetry sample "
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
  az_iot_log_sink log = sample_log_sink(AZ_IOT_LOG_LEVEL_INFO);
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
   * the other one. */
  if (az_iot_mqttv5_telemetry_client_init(&state.telemetry, &state.connection_client) != AZ_IOT_OK)
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
    /* MQTTv5 carries properties as MQTT v5 user properties, not in the topic, so
     * the topic stays the fixed ih/<id>/srv/telemetry and nothing is
     * URL-encoded -- the reserved characters MQTTv3 has to escape travel here
     * byte for byte. Two properties are added for you: type=telemetry:1, and
     * content-type, which takes $.ct when the message sets it and
     * application/json when it does not.
     *
     * $.ct is the one system property with a native v5 field, so it does not
     * also travel under its own name. Every other one -- $.ce, $.mid, $.cid,
     * $.uid, $.ctime, $.sub -- is carried verbatim, the same names the MQTTv3
     * client percent-encodes into its topic, so a message means the same thing
     * on either generation.
     *
     * The budget is AZ_IOT_MQTTV5_TELEMETRY_MAX_USER_PROPERTIES counting the two
     * added above. Properties past it are dropped, because the buffer is fixed,
     * but the send warns once naming the first one lost. */
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

    if (az_iot_mqttv5_telemetry_client_send(&state.telemetry, &msg, on_send_done, &user_ctx)
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
