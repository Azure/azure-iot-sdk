// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/direct_method_slow_responder - sample.
 *
 * Answer a direct method AFTER the handler returned, for work that does not fit
 * inside a callback, on whichever hub DPS assigns: MQTTv3 (mqttv3) or MQTTv5
 * (mqttv5). Read unified/direct_method_responder first: it answers inline, which
 * is all a fast method needs. The MQTTv5-only route is
 * mqttv5/direct_method_slow_responder.
 *
 * The handler keeps the request and returns; the pump loop answers it a couple
 * of seconds later. The request is a value, so keeping it is a struct copy --
 * nothing is allocated and nothing has to be freed. What does NOT outlive the
 * handler is `method_name` and `payload`: they point into the SDK's transient
 * buffers, so the payload is copied out below.
 *
 * The deadline for that late answer is where the generations differ:
 *
 *   - MQTTv3 never learns the caller's timeout. The device sets its own with
 *     set_response_timeout(), and a caller who cannot wait is only told 429
 *     after its arguments arrived.
 *   - MQTTv5 carries the caller's budget. The method is declared WITH THE TIME IT
 *     NEEDS, so a caller whose timeout cannot cover it is refused at the probe;
 *     and while one invocation is held, the probe answers DEVICE_BUSY -- both
 *     before the arguments are sent.
 *
 * Runs ~60 seconds and, like unified/telemetry, builds for an assumed
 * generation before open() and rebuilds when DPS assigns the other one --
 * including after the device is moved while it runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include "sample_utils.h"

#define SLOW_ECHO_METHOD "slowEcho"

/* Direct-method status codes are chosen by the application; these mirror the
 * HTTP meanings the service tooling already displays. */
#define STATUS_OK 200
#define STATUS_NOT_FOUND 404
#define STATUS_TRY_AGAIN_LATER 429
#define STATUS_PAYLOAD_TOO_LARGE 413

/* How long the pretend work takes, and the floor declared to MQTTv5 so the two
 * cannot drift apart. */
#define SLOW_ECHO_WORK_MS 2000u
#define SLOW_ECHO_SECONDS 3u

/* MQTTv3 only: comfortably longer than the work, and well under the 300 s
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
  /* Only the one matching `profile` is ever initialized. */
  az_iot_mqttv3_direct_method_client mqttv3;
  az_iot_mqttv5_direct_method_client mqttv5;
  az_iot_connection_profile profile;
  int methods_initialized;
} sample_state;

typedef struct
{
  sample_state* state;
  az_iot_connection_state conn_state;
  int connected_count;
  int faulted; /* terminal: nothing this sample can do about it */
  int rebuild; /* DPS assigned `assigned_profile`; rebuild for it */
  az_iot_connection_profile assigned_profile;
  deferred_call deferred;
} user_context;

static void clients_destroy(sample_state* s, user_context* ctx)
{
  /* A held request never outlives the client that issued it. */
  if (ctx != NULL)
  {
    ctx->deferred.pending = 0;
  }
  if (!s->methods_initialized)
  {
    return;
  }
  if (s->profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    az_iot_mqttv5_direct_method_client_deinit(&s->mqttv5);
  }
  else
  {
    az_iot_mqttv3_direct_method_client_deinit(&s->mqttv3);
  }
  s->methods_initialized = 0;
}

static void sample_state_destroy(sample_state* s)
{
  clients_destroy(s, NULL);
  az_iot_connection_client_deinit(&s->connection_client);
  az_iot_certificate_provider_pem_deinit(&s->certs);
  sample_config_release(&s->config);
}

/* The one call that differs by generation once a request is in hand. */
static az_iot_result respond(
    sample_state* s,
    az_iot_direct_method_request request,
    int status,
    const uint8_t* payload,
    size_t payload_len)
{
  return s->profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5
      ? az_iot_mqttv5_direct_method_respond(&s->mqttv5, request, status, payload, payload_len)
      : az_iot_mqttv3_direct_method_respond(&s->mqttv3, request, status, payload, payload_len);
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

/* Serves both generations: on MQTTv3 it is reached through on_method_mqttv3, on
 * MQTTv5 it is the handler registered for SLOW_ECHO_METHOD. */
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

  /* This sample holds one at a time. The MQTTv5 probe declines while one is held,
   * but two probes can both be accepted before either execute arrives, so this
   * branch is needed on both generations. */
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

/* MQTTv5 only: saying busy at the probe spares the caller a wasted argument
 * transfer. */
static az_iot_mqttv5_direct_method_probe_result on_probe_mqttv5(
    const az_iot_mqttv5_direct_method_probe* probe,
    void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  printf(
      "probe for method '%s', caller waits %u second(s) for a result\n",
      probe->method_name,
      (unsigned)probe->response_timeout_seconds);

  return ctx->deferred.pending ? AZ_IOT_MQTTV5_DM_PROBE_REJECT_DEVICE_BUSY
                               : AZ_IOT_MQTTV5_DM_PROBE_ACCEPT;
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

  /* Past the deadline -- the device's own on MQTTv3, the caller's on MQTTv5 --
   * the request is refused and nothing is sent. */
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
      result = az_iot_mqttv5_direct_method_client_init(&s->mqttv5, &s->connection_client);
      if (result != AZ_IOT_OK)
      {
        return result;
      }
      s->profile = profile;
      s->methods_initialized = 1;
      /* Declared with the time the work needs, so a caller whose timeout cannot
       * cover it is turned away at the probe. */
      result = az_iot_mqttv5_direct_method_client_register_method(
          &s->mqttv5, SLOW_ECHO_METHOD, SLOW_ECHO_SECONDS, on_slow_echo, ctx);
      if (result == AZ_IOT_OK)
      {
        result = az_iot_mqttv5_direct_method_client_set_probe_handler(
            &s->mqttv5, on_probe_mqttv5, ctx);
      }
      return result;
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V3:
      result = az_iot_mqttv3_direct_method_client_init(&s->mqttv3, &s->connection_client);
      if (result != AZ_IOT_OK)
      {
        return result;
      }
      s->profile = profile;
      s->methods_initialized = 1;
      result = az_iot_mqttv3_direct_method_client_set_handler(&s->mqttv3, on_method_mqttv3, ctx);
      if (result == AZ_IOT_OK)
      {
        /* A deferred answer has to land inside this window or the slot is gone. */
        result = az_iot_mqttv3_direct_method_client_set_response_timeout(
            &s->mqttv3, RESPONSE_TIMEOUT_SECONDS);
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
   * and identity recovery policies keep the device reconnecting on its own. */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &state.config);
  copts.certificate_provider = &state.certs.base;

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_add_state_observer(&state.connection_client, on_conn_state, &user_ctx);

  /* Both adapters: v3.1.1 serves DPS and an MQTTv3 hub, v5 serves an MQTTv5 hub. */
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

    deferred_pump(&user_ctx);
  }

  int rc = (user_ctx.connected_count > 0 && !user_ctx.faulted) ? 0 : 1;

  az_iot_connection_client_close(&state.connection_client);
  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    pump(&state, 50);
  }

  sample_state_destroy(&state);

  return rc;
}
