// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/direct_method_responder - sample.
 *
 * Answer direct methods for ~60 seconds, from inside the handler, on whichever
 * hub DPS assigns (MQTTv3). Same shape as unified/telemetry.
 *
 * MQTTv3 has no method registry and no probe. One handler receives EVERY
 * invocation, with its arguments already on the wire, so routing by name and
 * turning down unknown names (404) is the application's job. MQTTv3 never
 * learns the caller's timeout either, so the SDK applies a local one.
 *
 * DPS is handled internally by the connection client when dps.id_scope is set.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
/* The MQTT adapter this build of the sample uses (see samples/CMakeLists.txt). */
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#else
#error "Define AZ_IOT_SAMPLE_ADAPTER_PAHO or AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT"
#endif

#include "sample_utils.h"

#define ECHO_METHOD "echo"

/* Direct-method status codes are chosen by the application; these mirror the
 * HTTP meanings the service tooling already displays. */
#define STATUS_OK 200
#define STATUS_NOT_FOUND 404
#define STATUS_PAYLOAD_TOO_LARGE 413

/* Longer than this device's slowest method, and well under the
 * 300 s service maximum. */
#define RESPONSE_TIMEOUT_SECONDS 60u

/** @brief How long the sample listens. */
#define SAMPLE_RUN_MS 60000u

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  az_iot_mqttv3_direct_method_client mqttv3;
  /* The client's memory, for the default limits. */
  uint8_t methods_workspace[AZ_IOT_MQTTV3_DIRECT_METHOD_CLIENT_DEFAULT_WORKSPACE_SIZE];
  int methods_initialized;
} sample_state;

typedef struct
{
  sample_state* state;
  az_iot_connection_state conn_state;
  int connected_count;
  int faulted; /* terminal: nothing this sample can do about it */
} user_context;

static void clients_destroy(sample_state* s)
{
  if (!s->methods_initialized)
  {
    return;
  }
  az_iot_mqttv3_direct_method_client_deinit(&s->mqttv3);
  s->methods_initialized = 0;
}

static void sample_state_destroy(sample_state* s)
{
  clients_destroy(s);
  az_iot_connection_client_deinit(&s->connection_client);
  az_iot_certificate_provider_pem_deinit(&s->certs);
  sample_config_release(&s->config);
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;

  if (event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    ctx->faulted = 1;
    /* DPS assigned a hub generation this SDK does not support. */
    if (event->reason == AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED && event->profile != NULL)
    {
      printf(
          "Unsupported hub generation \"%s\"%s. Upgrade the SDK.\n",
          event->profile->connection_profile_raw != NULL ? event->profile->connection_profile_raw
                                                         : "(none)",
          event->profile->connection_profile_raw_truncated ? " (truncated)" : "");
    }
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

/* Every invocation lands here, including names this device has never heard of.
 * Answering 404 turns them down, but only after the caller's arguments have
 * already crossed the wire. */
static void on_method_mqttv3(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;

  if (method_name == NULL || strcmp(method_name, ECHO_METHOD) != 0)
  {
    printf("method '%s' is not implemented here\n", method_name ? method_name : "(null)");
    (void)az_iot_mqttv3_direct_method_respond(
        &ctx->state->mqttv3, request, STATUS_NOT_FOUND, NULL, 0);
    return;
  }

  printf("method '%s' invoked, %zu byte payload\n", method_name, payload_len);

  /* The echo must fit the MQTT send buffer with its topic; the arguments can be larger (up to
   * the receive buffer). An answer that does not fit is refused and the request KEPT, so answer
   * 413 rather than leave the caller waiting. */
  int status = payload_len <= SAMPLE_MQTT_ECHO_PAYLOAD_MAX ? STATUS_OK : STATUS_PAYLOAD_TOO_LARGE;
  az_iot_result result = az_iot_mqttv3_direct_method_respond(
      &ctx->state->mqttv3,
      request,
      status,
      status == STATUS_OK ? payload : NULL,
      status == STATUS_OK ? payload_len : 0);
  if (result == AZ_IOT_ERR_NOT_ENOUGH_SPACE)
  {
    status = STATUS_PAYLOAD_TOO_LARGE;
    result = az_iot_mqttv3_direct_method_respond(&ctx->state->mqttv3, request, status, NULL, 0);
  }
  if (result != AZ_IOT_OK)
  {
    printf("response was not sent: %s\n", az_iot_result_to_string(result));
  }
  else if (status != STATUS_OK)
  {
    printf(
        "payload larger than %u bytes: answered %d\n",
        (unsigned)SAMPLE_MQTT_ECHO_PAYLOAD_MAX,
        status);
  }
}

static az_iot_result clients_build(sample_state* s, user_context* ctx)
{
  /* Default limits: 0 fields. */
  az_iot_result result = az_iot_mqttv3_direct_method_client_init(
      &s->mqttv3,
      &s->connection_client,
      &(az_iot_mqttv3_direct_method_client_options){
          .workspace = s->methods_workspace, .workspace_size = sizeof(s->methods_workspace) });
  if (result != AZ_IOT_OK)
  {
    return result;
  }
  s->methods_initialized = 1;
  result = az_iot_mqttv3_direct_method_client_set_handler(&s->mqttv3, on_method_mqttv3, ctx);
  if (result == AZ_IOT_OK)
  {
    /* How long a request stays answerable before the SDK reclaims its slot.
     * Without it, handlers that never answer would eventually consume all
     * limits.max_in_flight_invocations slots. */
    result = az_iot_mqttv3_direct_method_client_set_response_timeout(
        &s->mqttv3, RESPONSE_TIMEOUT_SECONDS);
  }
  return result;
}

static void pump(sample_state* s, uint32_t timeout_ms)
{
  (void)az_iot_connection_client_do_work(&s->connection_client, timeout_ms);
}

int main(void)
{
  // Used only by automatic ci-pipeline verification of this sample.
  // When porting this code to your own application, please ignore this call.
  sample_pipeline_checks_begin();
  az_iot_log_sink log = sample_log_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  sample_state state = { 0 };
  if (sample_config_load(&state.config) != 0)
  {
    // Used only by automatic ci-pipeline verification of this sample.
    // When porting this code to your own application, please ignore this call or return the value
    // directly.
    return sample_pipeline_checks_end(1);
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
    // Used only by automatic ci-pipeline verification of this sample.
    // When porting this code to your own application, please ignore this call or return the value
    // directly.
    return sample_pipeline_checks_end(1);
  }

  /* Connection client (DPS provisioning is internal). The default reconnection
   * and identity recovery policies keep the device reconnecting on its own. */
  /* Memory for the connection client's entries, sized for the default limits. */
  static uint8_t connection_workspace[AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE];
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.workspace = connection_workspace;
  copts.workspace_size = sizeof(connection_workspace);
  copts.dps.id_scope = state.config.id_scope;
  copts.dps.registration_id = state.config.reg_id;
  copts.dps.global_endpoint = state.config.dps_global_endpoint; /* NULL: the global default */
  copts.certificate_provider = &state.certs.base;
  /* The adapter builds each session's MQTT 3.1.1 client in this workspace, one at a time: DPS,
   * then the hub. Limits: sample_utils.h. */
  static uint8_t mqtt_workspace[SAMPLE_MQTT_WORKSPACE_SIZE];
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
  copts.mqtt_adapter = az_iot_paho_get_adapter();
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
  copts.mqtt_adapter = az_iot_az_mqtt_get_adapter();
#endif
  copts.mqtt_limits = SAMPLE_MQTT_LIMITS;
  copts.mqtt_workspace = mqtt_workspace;
  copts.mqtt_workspace_size = sizeof(mqtt_workspace);

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    // Used only by automatic ci-pipeline verification of this sample.
    // When porting this code to your own application, please ignore this call or return the value
    // directly.
    return sample_pipeline_checks_end(1);
  }
  az_iot_connection_client_add_state_observer(&state.connection_client, on_conn_state, &user_ctx);

  if (clients_build(&state, &user_ctx) != AZ_IOT_OK
      || az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    // Used only by automatic ci-pipeline verification of this sample.
    // When porting this code to your own application, please ignore this call or return the value
    // directly.
    return sample_pipeline_checks_end(1);
  }

  uint64_t end_ms = sample_now_ms() + SAMPLE_RUN_MS;
  while (!user_ctx.faulted && sample_now_ms() < end_ms)
  {
    pump(&state, 100);
  }

  int rc = (user_ctx.connected_count > 0 && !user_ctx.faulted) ? 0 : 1;

  az_iot_connection_client_close(&state.connection_client);
  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    pump(&state, 50);
  }

  sample_state_destroy(&state);

  // Used only by automatic ci-pipeline verification of this sample.
  // When porting this code to your own application, please ignore this call or return the value
  // directly.
  return sample_pipeline_checks_end(rc);
}
