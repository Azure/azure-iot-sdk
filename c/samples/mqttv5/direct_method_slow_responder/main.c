// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* mqttv5/direct_method_slow_responder - sample.
 *
 * Answer a direct method on an AEG (Hub-Next) hub AFTER the handler returned,
 * for work that does not fit inside a callback. Read
 * mqttv5/direct_method_responder first: it answers inline, which is all a fast
 * method needs. unified/direct_method_slow_responder serves either generation.
 *
 * The handler keeps the request and returns; the pump loop answers it a couple
 * of seconds later. The request is a value, so keeping it is a struct copy --
 * nothing is allocated and nothing has to be freed.
 *
 * What does NOT outlive the handler is everything the invocation arrived with:
 * `method_name` and `payload` point into the SDK's transient buffers, so
 * anything still needed afterwards must be copied out, as the payload is below.
 *
 * Slow work is where the AEG handshake earns its round trip, and this sample
 * uses both halves of it:
 *
 *   - The method is declared WITH THE TIME IT NEEDS. A caller whose response
 *     timeout cannot cover SLOW_ECHO_SECONDS is refused INSUFFICIENT_TIME at
 *     the probe, before its arguments are sent -- rather than waiting out a
 *     call this device was never going to finish in time. Classic has no way
 *     to express that.
 *
 *   - While one invocation is held, the probe handler answers DEVICE_BUSY, so
 *     the caller is turned away before the arguments cross the wire. Classic
 *     can only take the work and then answer 429.
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
#define STATUS_TRY_AGAIN_LATER 429
#define STATUS_PAYLOAD_TOO_LARGE 413

/* How long the pretend work takes, and the floor declared to the service so the
 * two cannot drift apart. */
#define SLOW_ECHO_WORK_MS 2000u
#define SLOW_ECHO_SECONDS 3u

/* One invocation held while its work runs.
 *
 * Two rules come with holding a request past its handler:
 *
 *   - Answer it on the thread that runs az_iot_connection_client_do_work().
 *     This SDK is a single-threaded pump, so "respond later" means later on
 *     that thread -- not from a worker. A worker must hand its result back to
 *     the pump and let the pump call respond().
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
  az_iot_mqttv5_direct_method_client methods;
  int methods_initialized;
} sample_state;

static void sample_state_destroy(sample_state* s)
{
  if (s->methods_initialized)
  {
    az_iot_mqttv5_direct_method_client_deinit(&s->methods);
    s->methods_initialized = 0;
  }
  az_iot_connection_client_deinit(&s->connection_client);
  az_iot_certificate_provider_pem_deinit(&s->certs);
  sample_config_release(&s->config);
}

typedef struct
{
  sample_state* state;
  az_iot_connection_state conn_state;
  int provisioning_faulted;
  deferred_call deferred;
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
        printf("This device is assigned to a Classic hub. Run the unified "
               "direct_method_slow_responder sample instead.\n");
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

/* With one slot to hold an invocation in, saying so at the probe spares the
 * caller a wasted argument transfer: it learns immediately instead of sending
 * its parameters and being told 429.
 *
 * It does not make the busy check in the handler redundant. `pending` is only
 * set once an execute arrives, so two probes racing ahead of either execute
 * both see it clear and are both accepted. This narrows the window; it does not
 * close it. */
static az_iot_mqttv5_direct_method_probe_result on_probe(
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

  /* The probe declines while one is held, but that is an optimisation rather
   * than a guarantee: the SDK admits up to AZ_IOT_MQTTV5_DM_MAX_CONCURRENT ready
   * tokens, and `pending` is only set here -- so two probes can both be
   * accepted before either execute arrives. Without this branch the second
   * execute would overwrite the first request, and that caller would never be
   * answered. */
  if (ctx->deferred.pending)
  {
    (void)az_iot_mqttv5_direct_method_respond(
        &ctx->state->methods, request, STATUS_TRY_AGAIN_LATER, NULL, 0);
    return;
  }
  if (payload_len > sizeof(ctx->deferred.payload))
  {
    (void)az_iot_mqttv5_direct_method_respond(
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

  /* Worth checking, which answering inline never has to. Here the deadline is
   * the caller's own budget, carried through the probe and the execute -- not
   * a local guess as on Classic -- so a late answer is refused and nothing is
   * sent, because the service has already given up. */
  az_iot_result result = az_iot_mqttv5_direct_method_respond(
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

  if (az_iot_mqttv5_direct_method_client_init(&state.methods, &state.connection_client)
      != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  state.methods_initialized = 1;

  /* Declared with the time the work needs rather than 0, which is what that
   * argument is for: a caller whose response timeout cannot cover it is turned
   * away at the probe, before its arguments are sent. */
  if (az_iot_mqttv5_direct_method_client_register_method(
          &state.methods, SLOW_ECHO_METHOD, SLOW_ECHO_SECONDS, on_slow_echo, &user_ctx)
      != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  if (az_iot_mqttv5_direct_method_client_set_probe_handler(&state.methods, on_probe, &user_ctx)
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
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED || user_ctx.provisioning_faulted)
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
