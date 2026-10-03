// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/direct_method_responder - sample.
 *
 * Answer direct methods for ~60 seconds, from inside the handler, on whichever
 * hub DPS assigns: MQTTv3 or MQTTv5, including after the device is
 * moved to a hub of the other generation. The MQTTv5-only route is
 * mqttv5/direct_method_responder. Same shape as unified/telemetry: build for an
 * assumed generation before open(), rebuild when DPS assigns the other one.
 *
 * Direct methods are where the two generations differ most, so each gets its
 * own setup:
 *
 *   - MQTTv3 has no method registry and no probe. One handler receives EVERY
 *     invocation, with its arguments already on the wire, so routing by name and
 *     turning down unknown names (404) is the application's job. MQTTv3 never
 *     learns the caller's timeout either, so the SDK applies a local one.
 *
 *   - MQTTv5 asks before it calls. Methods are DECLARED with register_method(); a
 *     probe for any other name is answered METHOD_NOT_FOUND by the SDK before
 *     the arguments are sent. An optional probe handler lets the device decline
 *     a declared method with a reason (e.g. DEVICE_BUSY).
 *
 * DPS is handled internally by the connection client when dps.id_scope is set.
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
#define STATUS_PAYLOAD_TOO_LARGE 413

/* MQTTv3 only: longer than this device's slowest method, and well under the
 * 300 s service maximum. */
#define RESPONSE_TIMEOUT_SECONDS 60u

/** @brief How long the sample listens. */
#define SAMPLE_RUN_MS 60000u

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
} user_context;

static void clients_destroy(sample_state* s, user_context* ctx)
{
  (void)ctx;
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

/* ---- MQTTv3 (mqttv3) -------------------------------------------------------- */

/* Every invocation lands here, including names this device has never heard of.
 * Answering 404 is the closest MQTTv3 gets to MQTTv5's METHOD_NOT_FOUND, and it
 * arrives after the caller's arguments have already crossed the wire. */
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
  (void)az_iot_mqttv3_direct_method_respond(
      &ctx->state->mqttv3, request, STATUS_OK, payload, payload_len);
}

/* ---- MQTTv5 (mqttv5) ------------------------------------------------------------ */

/* Optional. The SDK has already checked that the name is declared and that the
 * caller's timeout covers the declared run time, so this only answers whether
 * the device will take the work right now. Answer promptly: the service is
 * holding the caller's connect timeout open. */
static az_iot_mqttv5_direct_method_probe_result on_probe_mqttv5(
    const az_iot_mqttv5_direct_method_probe* probe,
    void* user_ctx)
{
  (void)user_ctx;
  printf(
      "probe for method '%s', caller waits %u second(s) for a result\n",
      probe->method_name,
      (unsigned)probe->response_timeout_seconds);
  return AZ_IOT_MQTTV5_DM_PROBE_ACCEPT;
}

/* Only ever called for ECHO_METHOD, after the probe was accepted and the
 * service sent the arguments. */
static void on_echo_mqttv5(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  printf(
      "method '%s' invoked, %zu byte payload\n", method_name ? method_name : "(null)", payload_len);

  /* The reply is bounded by AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX, the arguments are
   * not. A reply that does not fit is refused and the slot KEPT, so answer
   * short rather than ignore that result. */
  if (payload_len > AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX)
  {
    (void)az_iot_mqttv5_direct_method_respond(
        &ctx->state->mqttv5, request, STATUS_PAYLOAD_TOO_LARGE, NULL, 0);
    return;
  }

  az_iot_result result = az_iot_mqttv5_direct_method_respond(
      &ctx->state->mqttv5, request, STATUS_OK, payload, payload_len);
  if (result != AZ_IOT_OK)
  {
    printf("response was not sent: %s\n", az_iot_result_to_string(result));
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
      /* 0: no declared run time, so any positive caller budget is enough. Names
       * match byte for byte. */
      result = az_iot_mqttv5_direct_method_client_register_method(
          &s->mqttv5, ECHO_METHOD, 0, on_echo_mqttv5, ctx);
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
        /* How long a request stays answerable before the SDK reclaims its slot.
         * Without it, handlers that never answer would eventually consume all
         * AZ_IOT_DM_MAX_INFLIGHT slots. */
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
