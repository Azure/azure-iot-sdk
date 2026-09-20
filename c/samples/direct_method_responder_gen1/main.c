// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* direct_method_responder_gen1 - sample.
 *
 * Answer direct methods on a Classic IoT Hub, from inside the handler. The AEG
 * route is a separate sample, direct_method_responder_gen2, and it looks quite
 * different: Classic and AEG disagree more about direct methods than about any
 * other feature.
 *
 * Classic has no method registry and no probe. One handler receives EVERY
 * invocation the service sends, whatever it is called, and the arguments are
 * already on the wire by the time the device sees the name. So routing by name
 * -- and turning down the names this device does not serve -- is the
 * application's job, which is what on_method below is.
 *
 * It also has no idea how long the caller is willing to wait. IoT Hub never
 * tells the device the caller's responseTimeoutInSeconds, so the SDK applies a
 * local deadline instead; see the set_response_timeout() call in main().
 *
 * Provision via DPS, open, listen for ~60 seconds, close. DPS is handled
 * internally by the connection client when host == NULL and dps.id_scope is set.
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
#define STATUS_NOT_FOUND 404

/* Longer than this device's slowest method, and well under the 300 s service
 * maximum. */
#define RESPONSE_TIMEOUT_SECONDS 60u

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
} user_context;

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->conn_state = event->state;

  /* The device provisioned to an AEG hub, so this Classic client can never
   * serve it. The connection faults before reporting CONNECTED. */
  if (event->reason == AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH)
  {
    printf("This device is assigned to an AEG hub. Run the "
           "direct_method_responder_gen2 sample instead.\n");
  }
}

/* Every invocation lands here, including names this device has never heard of:
 * Classic sends the arguments first and asks questions never. Answering 404 is
 * the closest this generation gets to the METHOD_NOT_FOUND that AEG returns at
 * probe time, and it arrives after the caller's arguments have already crossed
 * the wire. */
static void on_method(
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
    (void)az_iot_gen1_direct_method_respond(
        &ctx->state->methods, request, STATUS_NOT_FOUND, NULL, 0);
    return;
  }

  printf("method '%s' invoked, %zu byte payload\n", method_name, payload_len);
  (void)az_iot_gen1_direct_method_respond(
      &ctx->state->methods, request, STATUS_OK, payload, payload_len);
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

  /* Before open, and before any profile is known: this declares which hub the
   * application is built for, and the connection is failed if it resolves to
   * the other one. */
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

  /* Purely local, and it has no equivalent on AEG. Classic never learns the
   * caller's timeout, so this is how long a request stays answerable before the
   * SDK reclaims its slot -- without it, handlers that return without answering
   * would eventually consume all AZ_IOT_DM_MAX_INFLIGHT slots and the device
   * would stop accepting methods. */
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
