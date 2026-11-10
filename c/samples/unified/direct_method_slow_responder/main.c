// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/direct_method_slow_responder - sample.
 *
 * Answer a direct method AFTER the handler returned, for work that does not fit
 * inside a callback, on whichever hub DPS assigns (MQTTv3). Read
 * unified/direct_method_responder first: it answers inline, which is all a fast
 * method needs.
 *
 * The handler keeps the request and returns; the pump loop answers it a couple
 * of seconds later. The request is a value, so keeping it is a struct copy --
 * nothing is allocated and nothing has to be freed. What does NOT outlive the
 * handler is `method_name` and `payload`: they point into the SDK's transient
 * buffers, so the payload is copied out below.
 *
 * MQTTv3 never learns the caller's timeout, so the device sets its own deadline
 * for that late answer with set_response_timeout(), and a caller who cannot wait
 * is only told 429 after its arguments arrived.
 *
 * Runs ~60 seconds.
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

#define SLOW_ECHO_METHOD "slowEcho"

/* Direct-method status codes are chosen by the application; these mirror the
 * HTTP meanings the service tooling already displays. */
#define STATUS_OK 200
#define STATUS_NOT_FOUND 404
#define STATUS_TRY_AGAIN_LATER 429
#define STATUS_PAYLOAD_TOO_LARGE 413

/* How long the pretend work takes. */
#define SLOW_ECHO_WORK_MS 2000u

/* Comfortably longer than the work, and well under the 300 s
 * service maximum. */
#define RESPONSE_TIMEOUT_SECONDS 60u

/** @brief How long the sample listens. */
#define SAMPLE_RUN_MS 60000u

/* One invocation held while its work runs.
 *
 * Two rules come with holding a request past its handler:
 *
 *   - Answer it on the thread that runs az_iot_connection_client_do_work().
 *     This SDK is a single-threaded pump, so "respond later" means later on
 *     that thread -- not from a worker.
 *
 *   - Never hold one across a deinit() and re-init() of the client that
 *     issued it. Teardown resets the pool, so the rebuilt client can hand out
 *     the slot the old request names and the SDK cannot tell the two apart.
 */
typedef struct
{
  az_iot_direct_method_request request;
  uint8_t payload[256];
  size_t payload_len;
  uint64_t due_at_ms;
  int pending;
} deferred_call;

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
  deferred_call deferred;
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

/* Answers a request, inline or held. */
static az_iot_result respond(
    sample_state* s,
    az_iot_direct_method_request request,
    int status,
    const uint8_t* payload,
    size_t payload_len)
{
  return az_iot_mqttv3_direct_method_respond(&s->mqttv3, request, status, payload, payload_len);
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

/* Reached through on_method_mqttv3 for SLOW_ECHO_METHOD. */
static void on_slow_echo(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  printf(
      "method '%s' invoked, answering in ~%u ms\n",
      method_name ? method_name : "(null)",
      (unsigned)SLOW_ECHO_WORK_MS);

  /* This sample holds one at a time. */
  if (ctx->deferred.pending)
  {
    (void)respond(ctx->state, request, STATUS_TRY_AGAIN_LATER, NULL, 0);
    return;
  }
  if (payload_len > sizeof(ctx->deferred.payload))
  {
    (void)respond(ctx->state, request, STATUS_PAYLOAD_TOO_LARGE, NULL, 0);
    return;
  }

  if (payload_len > 0)
  {
    memcpy(ctx->deferred.payload, payload, payload_len);
  }
  ctx->deferred.payload_len = payload_len;
  ctx->deferred.request = request;
  ctx->deferred.due_at_ms = sample_now_ms() + SLOW_ECHO_WORK_MS;
  ctx->deferred.pending = 1;
}

/* MQTTv3 routes nothing for you: this handler receives every name the service
 * sends, so it dispatches and turns down the rest. */
static void on_method_mqttv3(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;

  if (method_name != NULL && strcmp(method_name, SLOW_ECHO_METHOD) == 0)
  {
    on_slow_echo(request, method_name, payload, payload_len, user_ctx);
    return;
  }

  printf("method '%s' is not implemented here\n", method_name ? method_name : "(null)");
  (void)az_iot_mqttv3_direct_method_respond(
      &ctx->state->mqttv3, request, STATUS_NOT_FOUND, NULL, 0);
}

/* Driven from the pump loop. Measured on a clock rather than counted in pump
 * calls: do_work()'s timeout is an upper bound on an idle wait and it returns
 * early whenever there is traffic, so a pump count is not a duration. */
static void deferred_pump(user_context* ctx)
{
  if (!ctx->deferred.pending || sample_now_ms() < ctx->deferred.due_at_ms)
  {
    return;
  }
  ctx->deferred.pending = 0;

  /* Past the device's own deadline the request is refused and nothing is sent. */
  az_iot_result result = respond(
      ctx->state,
      ctx->deferred.request,
      STATUS_OK,
      ctx->deferred.payload,
      ctx->deferred.payload_len);
  if (result != AZ_IOT_OK)
  {
    printf("deferred response was not sent: %s\n", az_iot_result_to_string(result));
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
    /* A deferred answer has to land inside this window or the slot is gone. */
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

    deferred_pump(&user_ctx);
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
