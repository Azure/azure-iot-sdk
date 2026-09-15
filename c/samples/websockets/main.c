// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* websockets - sample.
 *
 * telemetry_gen1, carried over MQTT inside WebSockets instead of MQTT directly
 * over TCP. Everything else is identical to that sample, so the difference
 * between the two files is exactly the feature.
 *
 * WHY: a device on a network that only allows HTTP(S) ports cannot open 8883 at
 * all. WebSockets carries the same MQTT session over 443, which such a network
 * does pass. Nothing above the transport changes.
 *
 * WHAT THE FEATURE COSTS YOU, in full:
 *
 *     copts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;
 *
 * The port follows the transport automatically -- 443 for WebSockets, 8883 for
 * TCP -- as long as copts.port is left 0, which is what
 * az_iot_connection_client_options_default() returns. Setting it would override
 * that.
 *
 * It applies to the DPS connect as well as the hub connect, because a network
 * that blocks 8883 blocks it for provisioning too.
 *
 * The resource path defaults to "/$iothub/websocket", which is what IoT Hub and
 * DPS serve. Set AZ_IOT_MQTT_WEBSOCKET_PATH only when connecting through a
 * gateway that terminates WebSockets somewhere else.
 *
 * The proxy sample is the other half of this pair; set both options together if
 * your network needs a proxy AND only passes 443.
 *
 * Classic-only, like telemetry_gen1: the transport is independent of the hub
 * generation, so serving one route keeps the sample about the transport. The
 * same option works unchanged on the AEG route.
 *
 * Configuration: the same environment variables as telemetry_gen1, plus the
 * optional AZ_IOT_MQTT_WEBSOCKET_PATH above.
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
  char* websocket_path;
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
  free(state->websocket_path);
  state->websocket_path = NULL;
}

typedef struct
{
  az_iot_connection_state conn_state;
  az_iot_result conn_reason;
  int send_done;
  az_iot_result send_status;
} user_context;

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->conn_state = event->state;
  ctx->conn_reason = event->reason;

  /* The device provisioned to an AEG hub, so this Classic client can never
   * serve it. The connection faults before reporting CONNECTED rather than
   * letting a send fail later against the wrong topic shape. */
  if (event->reason == AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH)
  {
    printf("This device is assigned to an AEG hub. Run the telemetry_gen2 sample instead.\n");
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

  /* THE FEATURE: carry MQTT inside WebSockets rather than directly over TCP.
   *
   * copts.port is deliberately not set: 0 means "derive from the transport", so
   * this reaches 443 rather than 8883 without the caller tracking which port
   * goes with which transport.
   *
   * websocket_path NULL selects AZ_IOT_MQTT_DEFAULT_WEBSOCKET_PATH
   * ("/$iothub/websocket"). The environment variable exists only so this sample
   * can also be pointed at a gateway that serves a different path. */
  copts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;
  state.websocket_path = sample_env_dup("AZ_IOT_MQTT_WEBSOCKET_PATH", NULL);
  copts.websocket_path = state.websocket_path;

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
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
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
