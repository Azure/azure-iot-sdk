// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* direct_method_slow_responder_gen1 - sample.
 *
 * Answer a direct method on a Classic IoT Hub AFTER the handler returned, for
 * work that does not fit inside a callback. Read direct_method_responder_gen1
 * first: it answers inline, which is all a fast method needs. The AEG route is
 * direct_method_slow_responder_gen2.
 *
 * The handler keeps the request and returns; the pump loop answers it a couple
 * of seconds later. The request is a value, so keeping it is a struct copy --
 * nothing is allocated and nothing has to be freed.
 *
 * What does NOT outlive the handler is everything the invocation arrived with:
 * `method_name` and `payload` point into the SDK's transient buffers, so
 * anything still needed afterwards must be copied out, as the payload is below.
 *
 * Classic delivers every invocation to one handler, so routing by name is the
 * application's job, and it has no idea how long the caller will wait -- see
 * the response timeout in main(), which is the device's own deadline rather
 * than the caller's.
 *
 * Provision via DPS, open, run for ~60 seconds, close.
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

/* How long the pretend work takes. */
#define SLOW_ECHO_WORK_MS 2000u

/* Comfortably longer than the work, and well under the 300 s service maximum. */
#define RESPONSE_TIMEOUT_SECONDS 60u

/* One invocation held while its work runs.
 *
 * Two rules come with holding a request past its handler:
 *
 *   - Answer it on the thread that runs az_iot_connection_client_do_work().
 *     This SDK is a single-threaded pump, so "respond later" means later on
 *     that thread -- not from a worker. A worker must hand its result back to
 *     the pump and let the pump call respond().
 *
 *   - Never hold one across a destroy() and re-init() of the client that
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
  az_iot_gen1_direct_method_client methods;
  int methods_initialized;
} sample_state;

static void sample_state_destroy(sample_state* s)
{
  if (s->methods_initialized)
  {
    az_iot_gen1_direct_method_client_destroy(&s->methods);
    s->methods_initialized = 0;
  }
  az_iot_connection_client_destroy(&s->connection_client);
  az_iot_certificate_provider_pem_destroy(&s->certs);
  sample_config_release(&s->config);
}

typedef struct
{
  sample_state* state;
  az_iot_connection_state conn_state;
  deferred_call deferred;
} user_context;

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->conn_state = event->state;

  if (event->reason == AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH)
  {
    printf("This device is assigned to an AEG hub. Run the "
           "direct_method_slow_responder_gen2 sample instead.\n");
  }
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
    /* This sample holds one at a time; the SDK would hold
     * AZ_IOT_DM_MAX_INFLIGHT. Classic cannot refuse before the arguments
     * arrive, so the best it can do is answer quickly once they have. */
    (void)az_iot_gen1_direct_method_respond(
        &ctx->state->methods, request, STATUS_TRY_AGAIN_LATER, NULL, 0);
    return;
  }
  if (payload_len > sizeof(ctx->deferred.payload))
  {
    (void)az_iot_gen1_direct_method_respond(
        &ctx->state->methods, request, STATUS_PAYLOAD_TOO_LARGE, NULL, 0);
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

/* Classic routes nothing for you: this handler receives every name the service
 * sends, so it dispatches and turns down the rest. */
static void on_method(
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
  (void)az_iot_gen1_direct_method_respond(&ctx->state->methods, request, STATUS_NOT_FOUND, NULL, 0);
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

  /* Worth checking, which answering inline never has to: past the response
   * timeout the request is refused and nothing is sent, because the SDK has
   * already reclaimed the slot. Deferring is what puts that within reach. */
  az_iot_result result = az_iot_gen1_direct_method_respond(
      &ctx->state->methods,
      ctx->deferred.request,
      STATUS_OK,
      ctx->deferred.payload,
      ctx->deferred.payload_len);
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

  /* Connection client (DPS provisioning is internal when host==NULL) */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&copts, &state.config);
  copts.certificate_provider = &state.certs.base;

  if (az_iot_connection_client_init(&state.connection_client, &copts) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  az_iot_connection_client_add_state_observer(&state.connection_client, on_conn_state, &user_ctx);

  /* One adapter covers both legs here: DPS always speaks v3.1.1, and so does a
   * Classic hub. */
  if (az_iot_connection_client_register_mqtt_factory(
          &state.connection_client, az_iot_paho_factory_create_v3_1_1())
      != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  if (az_iot_gen1_direct_method_client_init(&state.methods, &state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  state.methods_initialized = 1;

  if (az_iot_gen1_direct_method_client_set_handler(&state.methods, on_method, &user_ctx)
      != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* Matters more here than in the inline sample: a deferred answer has to land
   * inside this window or the slot is gone. It is the device's own deadline --
   * Classic never learns the caller's. */
  if (az_iot_gen1_direct_method_client_set_response_timeout(
          &state.methods, RESPONSE_TIMEOUT_SECONDS)
      != AZ_IOT_OK)
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
    printf("Connected. Listening for '" SLOW_ECHO_METHOD "' invocations (~60s)...\n");

    for (int i = 0; i < 600; ++i)
    {
      (void)az_iot_connection_client_do_work(&state.connection_client, 100);
      deferred_pump(&user_ctx);
    }

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
