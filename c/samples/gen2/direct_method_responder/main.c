// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* gen2/direct_method_responder - sample.
 *
 * Answer direct methods on an AEG (Hub-Next) hub over MQTT v5, from inside the
 * handler. unified/direct_method_responder serves either generation and shows
 * the Classic route side by side; it is worth reading both, as this is the
 * feature where the two generations differ most.
 *
 * AEG asks before it calls. Every invocation begins with a probe naming the
 * method and carrying the caller's response timeout, and only a device that
 * accepts is sent the arguments. Two things follow from that, and they are the
 * whole reason this sample does not look like the Classic one:
 *
 *   - Methods are DECLARED, with register_method(). A probe for a name this
 *     device never declared is answered METHOD_NOT_FOUND by the SDK and never
 *     reaches the application, so an unknown name costs two small messages and
 *     no state instead of a full argument transfer. There is no 404 path here
 *     because the handler only ever sees names it registered.
 *
 *   - The device can decline, with a reason. set_probe_handler() below is
 *     consulted only for a method that is already declared and whose timing
 *     fits: it answers whether now is a good moment. The caller gets
 *     DEVICE_BUSY rather than a timeout.
 *
 * Two adapters are registered even though only the hub is v5: the DPS leg is
 * still v3.1.1, and registering only the v5 factory makes provisioning fail
 * before the hub is ever reached.
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

#define ECHO_METHOD "echo"

/* Direct-method status codes are chosen by the application; these mirror the
 * HTTP meanings the service tooling already displays. */
#define STATUS_OK 200
#define STATUS_PAYLOAD_TOO_LARGE 413

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  az_iot_gen2_direct_method_client methods;
  int methods_initialized;
} sample_state;

static void sample_state_destroy(sample_state* s)
{
  if (s->methods_initialized)
  {
    az_iot_gen2_direct_method_client_destroy(&s->methods);
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
  int provisioning_faulted;
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
               "direct_method_responder sample instead.\n");
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

/* Optional. The SDK has already checked that the name is declared and that the
 * caller's timeout covers the declared run time, so this only answers whether
 * the device will take the work right now -- the question no registry can
 * answer for it. A real device declines here while low on battery, mid-update,
 * or otherwise unable to do the job well.
 *
 * Answer promptly: the service is holding the caller's connect timeout open. */
static az_iot_gen2_direct_method_probe_result on_probe(
    const az_iot_gen2_direct_method_probe* probe,
    void* user_ctx)
{
  (void)user_ctx;
  printf(
      "probe for method '%s', caller waits %u second(s) for a result\n",
      probe->method_name,
      (unsigned)probe->response_timeout_seconds);
  return AZ_IOT_GEN2_DM_PROBE_ACCEPT;
}

/* Only ever called for a name registered below, after the probe was accepted
 * and the service sent the arguments. Nothing here has to validate the name. */
static void on_echo(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  printf(
      "method '%s' invoked, %zu byte payload\n", method_name ? method_name : "(null)", payload_len);

  /* Echoing is not automatically safe: the arguments the caller sent are bounded
   * by the message, not by AZ_IOT_GEN2_DM_RESULT_BODY_MAX, which is what bounds
   * the reply. Sending one back that does not fit is refused with
   * AZ_IOT_ERR_NOT_ENOUGH_SPACE and the slot is KEPT -- so ignoring that result
   * would leave the caller with no answer at all until its budget ran out.
   * Answering short is the recovery the SDK leaves open. */
  if (payload_len > AZ_IOT_GEN2_DM_RESULT_BODY_MAX)
  {
    (void)az_iot_gen2_direct_method_respond(
        &ctx->state->methods, request, STATUS_PAYLOAD_TOO_LARGE, NULL, 0);
    return;
  }

  az_iot_result result = az_iot_gen2_direct_method_respond(
      &ctx->state->methods, request, STATUS_OK, payload, payload_len);
  if (result != AZ_IOT_OK)
  {
    printf("response was not sent: %s\n", az_iot_result_to_string(result));
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

  /* Before open, and before any profile is known: this declares which hub the
   * application is built for, and the connection is failed if it resolves to
   * the other one. */
  if (az_iot_gen2_direct_method_client_init(&state.methods, &state.connection_client) != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }
  state.methods_initialized = 1;

  /* Name, handler and declared run time together, because none of them is any
   * use alone. 0 means this method needs no floor, so any positive remaining
   * budget is enough; a method that genuinely takes time should say so, and
   * then a caller whose timeout cannot cover it is turned away at the probe
   * instead of being abandoned later. Names match byte for byte -- no case
   * folding, no normalization. */
  if (az_iot_gen2_direct_method_client_register_method(
          &state.methods, ECHO_METHOD, 0, on_echo, &user_ctx)
      != AZ_IOT_OK)
  {
    sample_state_destroy(&state);
    return 1;
  }

  /* Optional: only for conditions the SDK cannot know about. Without it, a
   * declared method with feasible timing and free capacity is simply accepted. */
  if (az_iot_gen2_direct_method_client_set_probe_handler(&state.methods, on_probe, &user_ctx)
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

  /* CONNECTED is later here than on Classic: an AEG session is not up at
   * CONNACK, it subscribes and then exchanges a birth message first. */
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
    printf("Connected. Listening for '" ECHO_METHOD "' invocations (~60s)...\n");

    for (int i = 0; i < 600; ++i)
    {
      (void)az_iot_connection_client_do_work(&state.connection_client, 100);
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
