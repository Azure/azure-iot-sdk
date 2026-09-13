// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* direct_method_responder - sample.
 *
 * Provision via DPS, open connection, subscribe for direct method invocations,
 * echo request payloads back as responses with status 200. Runs for ~60 seconds
 * then exits. DPS is handled internally by the connection client when
 * host == NULL and dps.id_scope is set.
 *
 * On gen2 (AEG) the service asks before it calls: a probe names the method and
 * the caller's response timeout, and only a device that accepts is sent the
 * arguments. The probe handler below shows the useful shape of that answer --
 * turning down a name this sample does not implement, so the caller gets a
 * reason instead of a timeout. gen1 has no probe phase, so it goes straight to
 * on_method.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include "sample_utils.h"

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
} user_context;

static void on_method(
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
    void* handler_ctx)
{
  methods_destroy(s);

  az_iot_result result;
  if (profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    result = az_iot_gen2_direct_method_client_init(&s->gen2_methods, &s->connection_client);
    if (result == AZ_IOT_OK)
    {
      /* Name, handler and declared run time together: probes are answered from
       * this list, so an undeclared name is turned away before its arguments
       * are ever sent. 0 = no minimum run time. */
      result = az_iot_gen2_direct_method_client_register_method(
          &s->gen2_methods, "echo", 0, on_method, handler_ctx);
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
  (void)user_ctx;
  printf(
      "probe for method '%s', caller waits %u second(s) for a result\n",
      probe->method_name,
      (unsigned)probe->response_timeout_seconds);

  /* The name and the timing were already checked against the declared methods,
   * so this only has to answer whether now is a good moment. A real device
   * would decline here while low on battery or mid-update, with
   * AZ_IOT_GEN2_DM_PROBE_REJECT_DEVICE_BUSY. */
  return AZ_IOT_GEN2_DM_PROBE_ACCEPT;
}

static void on_method(
    az_iot_direct_method_request request,
    const char* method_name,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  printf(
      "method '%s' invoked, %zu byte payload\n", method_name ? method_name : "(null)", payload_len);
  /* Echo the request payload back to the caller, on whichever generation
   * delivered it. */
  if (ctx->state->methods_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    (void)az_iot_gen2_direct_method_respond(
        &ctx->state->gen2_methods, request, 200, payload, payload_len);
  }
  else
  {
    (void)az_iot_gen1_direct_method_respond(
        &ctx->state->gen1_methods, request, 200, payload, payload_len);
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
    printf("Connected. Listening for direct method invocations (~60s)...\n");

    /* Pump for ~60 seconds (600 * 100ms) */
    for (int i = 0; i < 600; ++i)
    {
      (void)az_iot_connection_client_do_work(&state.connection_client, 100);
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
