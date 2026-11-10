// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* unified/twin_get_patch - sample.
 *
 * Issue a twin GET and a reported-properties PATCH on every connect, for ~60
 * seconds, on whichever hub DPS assigns (MQTTv3). Same shape as unified/telemetry.
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

/** @brief How long the sample runs. */
#define SAMPLE_RUN_MS 60000u

typedef struct
{
  sample_config config;
  az_iot_certificate_provider_pem certs;
  az_iot_connection_client connection_client;
  az_iot_mqttv3_twin_client mqttv3;
  /* The client's requests, for the default limits. */
  uint8_t twin_workspace[AZ_IOT_MQTTV3_TWIN_CLIENT_DEFAULT_WORKSPACE_SIZE];
  int twin_initialized;
} sample_state;

typedef struct
{
  az_iot_connection_state conn_state;
  int connected_count;
  int faulted; /* terminal: nothing this sample can do about it */
  int get_done;
  int patch_done;
  int desired_count;
  az_iot_result get_status;
  az_iot_result patch_status;
  uint64_t patch_version;
} user_context;

static void clients_destroy(sample_state* s)
{
  if (!s->twin_initialized)
  {
    return;
  }
  az_iot_mqttv3_twin_client_deinit(&s->mqttv3);
  s->twin_initialized = 0;
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

static void on_desired_mqttv3(
    const uint8_t* patch,
    size_t patch_len,
    uint64_t version,
    void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->desired_count++;
  printf(
      "twin desired (version %llu): %.*s\n",
      (unsigned long long)version,
      (int)patch_len,
      (const char*)patch);
}

static void on_get_mqttv3(az_iot_result status, const uint8_t* body, size_t len, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->get_status = status;
  ctx->get_done = 1;
  if (status == AZ_IOT_OK)
  {
    printf("twin GET: %.*s\n", (int)len, (const char*)body);
  }
}

static void on_patch_mqttv3(az_iot_result status, uint64_t version, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->patch_status = status;
  ctx->patch_version = version;
  ctx->patch_done = 1;
}

static az_iot_result clients_build(sample_state* s, user_context* ctx)
{
  az_iot_result result = az_iot_mqttv3_twin_client_init(
      &s->mqttv3,
      &s->connection_client,
      &(az_iot_mqttv3_twin_client_options){ .workspace = s->twin_workspace,
                                            .workspace_size = sizeof(s->twin_workspace) });
  if (result != AZ_IOT_OK)
  {
    return result;
  }
  s->twin_initialized = 1;
  return az_iot_mqttv3_twin_client_set_desired_handler(&s->mqttv3, on_desired_mqttv3, ctx);
}

/* One pump step: the connection also drives the twin clients' timeouts. */
static void pump(sample_state* s, uint32_t timeout_ms)
{
  (void)az_iot_connection_client_do_work(&s->connection_client, timeout_ms);
}

/* GET + PATCH, answered through the callbacks. A request that was never queued
 * has no callback coming, so it completes here. */
static void start_round(sample_state* s, user_context* ctx)
{
  static const uint8_t patch[] = "{\"sample\":\"hello\"}";
  az_iot_result get_rc;
  az_iot_result patch_rc;

  ctx->get_done = 0;
  ctx->patch_done = 0;
  get_rc = az_iot_mqttv3_twin_client_get(&s->mqttv3, on_get_mqttv3, ctx);
  patch_rc = az_iot_mqttv3_twin_client_patch_reported(
      &s->mqttv3, patch, sizeof(patch) - 1, on_patch_mqttv3, ctx);
  if (get_rc != AZ_IOT_OK)
  {
    ctx->get_status = get_rc;
    ctx->get_done = 1;
  }
  if (patch_rc != AZ_IOT_OK)
  {
    ctx->patch_status = patch_rc;
    ctx->patch_done = 1;
  }
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

  user_context user_ctx = { 0 };

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
  int rounds = 0; /* connects already served */
  int round_active = 0;
  int rounds_ok = 0;
  int rounds_failed = 0;
  while (!user_ctx.faulted && sample_now_ms() < end_ms)
  {
    pump(&state, 100);

    /* Every connect refreshes the twin. */
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED && user_ctx.connected_count > rounds)
    {
      rounds = user_ctx.connected_count;
      round_active = 1;
      start_round(&state, &user_ctx);
    }

    if (round_active && user_ctx.get_done && user_ctx.patch_done)
    {
      round_active = 0;
      printf(
          "twin_get: %s, patch_reported: %s version=%llu\n",
          az_iot_result_to_string(user_ctx.get_status),
          az_iot_result_to_string(user_ctx.patch_status),
          (unsigned long long)user_ctx.patch_version);
      if (user_ctx.get_status == AZ_IOT_OK && user_ctx.patch_status == AZ_IOT_OK)
      {
        rounds_ok++;
      }
      /* Cut short by the session ending (e.g. the device was moved): not a
       * failure, the next connect starts a new round. */
      else if (
          user_ctx.get_status != AZ_IOT_ERR_NOT_CONNECTED
          && user_ctx.patch_status != AZ_IOT_ERR_NOT_CONNECTED)
      {
        rounds_failed++;
      }
    }
  }

  int rc = (rounds_ok > 0 && rounds_failed == 0 && !user_ctx.faulted) ? 0 : 1;

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
