// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* direct_method_slow_responder - sample.
 *
 * The same shape as direct_method_responder, for work that does not fit inside
 * a callback. Read that one first: it answers from inside its handler, which is
 * the simpler case and all a fast method needs.
 *
 * Here the handler keeps the request and returns, and the pump loop answers it
 * a couple of seconds later -- standing in for a sensor read, a file write, or
 * a reply handed back by a worker thread. The request is a value, so keeping it
 * is a struct copy: nothing is allocated and nothing has to be freed.
 *
 * What does NOT outlive the handler is everything the invocation arrived with.
 * `method_name` and `payload` point into the SDK's transient buffers, so
 * anything still needed after the handler returns must be copied out, as the
 * payload is below. This is the trap the inline sample never has to mention.
 *
 * Provision via DPS, open connection, run for ~60 seconds then exit. DPS is
 * handled internally by the connection client when host == NULL and
 * dps.id_scope is set.
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

/* How long the pretend work takes, and the floor declared to gen2 so the two
 * cannot drift apart. */
#define SLOW_ECHO_WORK_MS 2000u
#define SLOW_ECHO_SECONDS 3u

/* One invocation held while its work runs.
 *
 * Two rules come with holding a request past its handler:
 *
 *   - Answer it on the thread that runs az_iot_connection_client_do_work().
 *     This SDK is a single-threaded pump, so "respond later" means later on
 *     that thread -- not from a worker. A worker must hand its result back to
 *     the pump and let the pump call respond(). Nothing here enforces that;
 *     it is a contract.
 *
 *   - Never hold one across a destroy() and re-init() of the client that
 *     issued it. Teardown resets the pool, so the rebuilt client can hand out
 *     the slot the old request names and the SDK cannot tell the two apart.
 *     methods_rebuild() below drops any held request for exactly this reason.
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
  az_iot_gen1_direct_method_client gen1_methods;
  az_iot_gen2_direct_method_client gen2_methods;
  az_iot_connection_profile methods_profile;
  int methods_initialized;
} sample_state;

static void methods_destroy(sample_state* s)
{
  if (!s->methods_initialized)
  {
    return;
  }
  if (s->methods_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    az_iot_gen2_direct_method_client_destroy(&s->gen2_methods);
  }
  else
  {
    az_iot_gen1_direct_method_client_destroy(&s->gen1_methods);
  }
  s->methods_initialized = 0;
}

static void sample_state_destroy(sample_state* s)
{
  methods_destroy(s);
  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);
}

typedef struct
{
  sample_state* state;
  az_iot_connection_state conn_state;
  az_iot_result methods_status;
  int rebuild_pending;
  deferred_call deferred;
} user_context;

static void on_method(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx);

static void on_slow_echo(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx);

static az_iot_gen2_direct_method_probe_result on_probe(
    const az_iot_gen2_direct_method_probe* probe,
    void* user_ctx);

/* The generation is only known once the connection resolves it, and a
 * re-provision can move the device to the other one -- so the client is built
 * from the profile the event carries rather than constructed once up front. */
static az_iot_result methods_rebuild(
    sample_state* s,
    az_iot_connection_profile profile,
    user_context* handler_ctx)
{
  /* A held request names a slot in the client about to be destroyed, and the
   * rebuilt one can hand that slot out again. Drop it rather than risk
   * answering into whatever takes its place. */
  handler_ctx->deferred.pending = 0;

  methods_destroy(s);

  az_iot_result result;
  if (profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    result = az_iot_gen2_direct_method_client_init(&s->gen2_methods, &s->connection_client);
    if (result == AZ_IOT_OK)
    {
      /* Declared with the time the work needs rather than 0, which is what that
       * argument is for: a caller whose response timeout cannot cover it is
       * turned away at the probe, before its arguments are sent, instead of
       * waiting for a result that could never arrive in time. */
      result = az_iot_gen2_direct_method_client_register_method(
          &s->gen2_methods, SLOW_ECHO_METHOD, SLOW_ECHO_SECONDS, on_slow_echo, handler_ctx);
    }
    if (result == AZ_IOT_OK)
    {
      /* Optional: only for conditions the SDK cannot know about. */
      result = az_iot_gen2_direct_method_client_set_probe_handler(
          &s->gen2_methods, on_probe, handler_ctx);
    }
  }
  else if (profile == AZ_IOT_CONNECTION_PROFILE_CLASSIC)
  {
    result = az_iot_gen1_direct_method_client_init(&s->gen1_methods, &s->connection_client);
    if (result == AZ_IOT_OK)
    {
      result
          = az_iot_gen1_direct_method_client_set_handler(&s->gen1_methods, on_method, handler_ctx);
    }
  }
  else
  {
    return AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED;
  }

  if (result == AZ_IOT_OK)
  {
    s->methods_profile = profile;
    s->methods_initialized = 1;
  }
  return result;
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->conn_state = event->state;
  if (event->state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    ctx->methods_status = event->profile
        ? methods_rebuild(ctx->state, event->profile->connection_profile, ctx)
        : AZ_IOT_ERR_INTERNAL;
  }
  else if (event->reason == AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH && event->profile)
  {
    /* Re-provisioning moved the device to the other generation, so the pinned
     * client can never connect again. Rebuilding releases the old pin and takes
     * the new one; the reopen is driven from the main loop. */
    printf("Reassigned to the other hub generation; rebuilding the method client.\n");
    ctx->methods_status = methods_rebuild(ctx->state, event->profile->connection_profile, ctx);
    ctx->rebuild_pending = (ctx->methods_status == AZ_IOT_OK);
  }
}

static az_iot_gen2_direct_method_probe_result on_probe(
    const az_iot_gen2_direct_method_probe* probe,
    void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  printf(
      "probe for method '%s', caller waits %u second(s) for a result\n",
      probe->method_name,
      (unsigned)probe->response_timeout_seconds);

  /* The name and the timing were already checked against the declared methods,
   * so this only has to answer whether now is a good moment. With one slot to
   * hold an invocation in, that means saying so while it is occupied. */
  return ctx->deferred.pending ? AZ_IOT_GEN2_DM_PROBE_REJECT_DEVICE_BUSY
                               : AZ_IOT_GEN2_DM_PROBE_ACCEPT;
}

/* Answer on whichever generation delivered the invocation. */
static az_iot_result respond(
    user_context* ctx,
    az_iot_direct_method_request request,
    int status_code,
    const uint8_t* payload,
    size_t payload_len)
{
  return ctx->state->methods_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5
      ? az_iot_gen2_direct_method_respond(
            &ctx->state->gen2_methods, request, status_code, payload, payload_len)
      : az_iot_gen1_direct_method_respond(
            &ctx->state->gen1_methods, request, status_code, payload, payload_len);
}

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

  if (ctx->deferred.pending)
  {
    /* This sample holds one at a time. The SDK holds up to
     * AZ_IOT_DM_MAX_INFLIGHT on gen1 and AZ_IOT_GEN2_DM_MAX_CONCURRENT on gen2
     * -- the latter only defaults to the former and can be set apart -- so an
     * application that defers should size its own store against the limit of
     * the generation it runs on, and refuse rather than accept work it cannot
     * track. */
    (void)respond(ctx, request, STATUS_TRY_AGAIN_LATER, NULL, 0);
    return;
  }
  if (payload_len > sizeof(ctx->deferred.payload))
  {
    (void)respond(ctx, request, STATUS_PAYLOAD_TOO_LARGE, NULL, 0);
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

/* gen1 delivers every invocation to one handler, so routing by name -- and
 * turning down the names this device does not serve -- is the application's
 * job. gen2 declares each name up front, so it rejects an unknown one at the
 * probe and the handler only ever sees a name it registered. */
static void on_method(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  if (method_name != NULL && strcmp(method_name, SLOW_ECHO_METHOD) == 0)
  {
    on_slow_echo(request, method_name, payload, payload_len, user_ctx);
    return;
  }

  printf("method '%s' is not implemented here\n", method_name ? method_name : "(null)");
  (void)respond((user_context*)user_ctx, request, STATUS_NOT_FOUND, NULL, 0);
}

/* Driven from the pump loop: finishes the held invocation once its work is
 * done. Measured on a clock rather than counted in pump calls -- do_work()'s
 * timeout is an upper bound on an idle wait, and it returns early whenever
 * there is traffic to dispatch, so a pump count is not a duration. */
static void deferred_pump(user_context* ctx)
{
  if (!ctx->deferred.pending || sample_now_ms() < ctx->deferred.due_at_ms)
  {
    return;
  }
  ctx->deferred.pending = 0;

  /* Worth checking, which answering inline never has to: a request answered
   * past its deadline is refused and nothing is sent, because the service
   * stopped waiting. Deferring is what puts that within reach. */
  az_iot_result result = respond(
      ctx, ctx->deferred.request, STATUS_OK, ctx->deferred.payload, ctx->deferred.payload_len);
  if (result != AZ_IOT_OK)
  {
    printf("deferred response was not sent: %s\n", az_iot_result_to_string(result));
  }
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
  user_context user_ctx = { .state = &state, .methods_status = AZ_IOT_ERR_NOT_INITIALIZED };

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

  /* MQTT adapters: register both v3.1.1 (DPS + Classic) and v5 (Next).
   * The connection client selects the appropriate factory based on the
   * session role. Both may be backed by different MQTT libraries. */
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
   * method client is created from the state callback, once the profile is
   * known. */
  if (az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  for (int i = 0; i < 1200 && user_ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
  {
    (void)az_iot_connection_client_do_work(&state.connection_client, 50);
    if (user_ctx.rebuild_pending)
    {
      user_ctx.rebuild_pending = 0;
      if (az_iot_connection_client_open(&state.connection_client) != AZ_IOT_OK)
      {
        break;
      }
      continue;
    }
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }

  if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    printf("Connected. Listening for '" SLOW_ECHO_METHOD "' invocations (~60s)...\n");

    /* Pump for ~60 seconds, finishing any held invocation as its work comes
     * due. A real application does its own work here too. */
    for (int i = 0; i < 600; ++i)
    {
      (void)az_iot_connection_client_do_work(&state.connection_client, 100);
      deferred_pump(&user_ctx);
    }

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
