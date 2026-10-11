// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* mqttv5/custom_topic_publisher - sample.
 *
 * Publish one message to a custom topic on an mqttv5 hub. The hub must have a
 * topic template the topic matches; the device publishes the concrete topic,
 * with its own device id where the template has {deviceId}.
 *
 * Provision via DPS, open, publish, close.
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
  az_iot_mqttv5_custom_topic_client custom;
  int custom_initialized;
} sample_state;

static void sample_state_destroy(sample_state* state)
{
  if (state->custom_initialized)
  {
    az_iot_mqttv5_custom_topic_client_deinit(&state->custom);
    state->custom_initialized = 0;
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
        printf("This device is assigned to an MQTTv3 hub. This sample needs an "
               "mqttv5 hub.\n");
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

  /* Before open: declares that the application needs an mqttv5 hub. */
  if (az_iot_mqttv5_custom_topic_client_init(&state.custom, &state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  state.custom_initialized = 1;

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
    /* Built after CONNECTED, and again before every publish in a real device:
     * provisioning may assign a different device id. */
    const char* topic_template = getenv("AZ_IOT_CUSTOM_TOPIC_TEMPLATE");
    if (topic_template == NULL || topic_template[0] == '\0')
    {
      topic_template = "contoso/{deviceId}/telemetry";
    }
    char topic[AZ_IOT_MQTTV5_CUSTOM_TOPIC_MAX_LEN + 1];
    az_iot_result result = az_iot_mqttv5_custom_topic_client_format_topic(
        &state.custom, topic_template, topic, sizeof(topic), NULL);
    if (result != AZ_IOT_OK)
    {
      printf(
          "Cannot build a topic from '%s': %s\n", topic_template, az_iot_result_to_string(result));
    }
    else
    {
      /* Sent as given: the client adds no user properties of its own. */
      static const uint8_t payload[] = "{\"temp\":23}";
      const az_iot_mqtt_user_property props[] = { { "unit", "deg C" } };
      az_iot_mqttv5_custom_topic_message msg = { 0 };
      msg.topic = topic;
      msg.payload = payload;
      msg.payload_len = sizeof(payload) - 1;
      msg.qos = AZ_IOT_MQTT_QOS_1;
      msg.content_type = "application/json";
      msg.user_properties = props;
      msg.user_properties_count = sizeof(props) / sizeof(props[0]);

      result
          = az_iot_mqttv5_custom_topic_client_publish(&state.custom, &msg, on_send_done, &user_ctx);
      if (result != AZ_IOT_OK)
      {
        printf("Publish to '%s' not sent: %s\n", topic, az_iot_result_to_string(result));
      }
      else
      {
        for (int i = 0; i < 600 && !user_ctx.send_done; ++i)
        {
          (void)az_iot_connection_client_do_work(&state.connection_client, 50);
        }

        if (user_ctx.send_done && user_ctx.send_status == AZ_IOT_OK)
        {
          printf("Published to '%s'.\n", topic);
          rc = 0;
        }
        else if (user_ctx.send_done && user_ctx.send_status == AZ_IOT_ERR_PUBLISH_REFUSED)
        {
          printf(
              "The hub refused '%s'. Check that a topic template on the hub matches it.\n", topic);
        }
        else
        {
          printf(
              "Publish to '%s' failed: %s\n",
              topic,
              az_iot_result_to_string(
                  user_ctx.send_done ? user_ctx.send_status : AZ_IOT_ERR_TIMEOUT));
        }
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
