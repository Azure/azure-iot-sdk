// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "e2e_sas_device.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <cmocka.h>

#include "az_iot_crypto_openssl.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "support/test_env.h"

#define E2E_SAS_CONNECT_TIMEOUT_S 120
#define E2E_SAS_SEND_TIMEOUT_S 30
#define E2E_SAS_CLOSE_TIMEOUT_S 30

/* One key (DPS and hub share it), IDs up to 256 characters. */
static uint8_t g_sas_buffer[AZ_IOT_SAS_BUFFER_SIZE(1, AZ_IOT_SAS_TOKEN_SIZE(256))];

typedef struct
{
  az_iot_connection_state hub_state;
  e2e_sas_run* run;
  int send_done;
  az_iot_result send_status;
} sas_ctx;

static const char* auth_source_name(az_iot_auth_source source)
{
  switch (source)
  {
    case AZ_IOT_AUTH_SOURCE_NONE:
      return "AZ_IOT_AUTH_SOURCE_NONE";
    case AZ_IOT_AUTH_SOURCE_X509:
      return "AZ_IOT_AUTH_SOURCE_X509";
    case AZ_IOT_AUTH_SOURCE_PRIMARY_KEY:
      return "AZ_IOT_AUTH_SOURCE_PRIMARY_KEY";
    case AZ_IOT_AUTH_SOURCE_SECONDARY_KEY:
      return "AZ_IOT_AUTH_SOURCE_SECONDARY_KEY";
    case AZ_IOT_AUTH_SOURCE_USER_PROVIDED:
      return "AZ_IOT_AUTH_SOURCE_USER_PROVIDED";
    default:
      return "?";
  }
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  sas_ctx* c = (sas_ctx*)user_ctx;
  if (event->error != NULL && event->error->source == AZ_IOT_CONN_ERR_SRC_DPS)
  {
    fprintf(
        stderr,
        "[e2e-sas] DPS error %ld: %.*s\n",
        (long)event->error->code,
        (int)az_span_size(event->error->message),
        az_span_size(event->error->message) > 0 ? (const char*)az_span_ptr(event->error->message)
                                                : "");
  }
  fprintf(
      stderr,
      "[e2e-sas] %s state %s (0x%x) reason %s (0x%x) source %s (%d)\n",
      event->scope == AZ_IOT_CONN_SCOPE_DPS ? "dps" : "hub",
      az_iot_connection_state_to_string(event->state),
      (unsigned)event->state,
      az_iot_result_to_string(event->reason),
      (unsigned)event->reason,
      auth_source_name(event->auth_source),
      (int)event->auth_source);
  if (event->state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    if (event->scope == AZ_IOT_CONN_SCOPE_DPS)
    {
      c->run->dps_source = event->auth_source;
    }
    else
    {
      c->run->hub_source = event->auth_source;
    }
  }
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    c->hub_state = event->state;
  }
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  sas_ctx* c = (sas_ctx*)user_ctx;
  c->send_status = status;
  c->send_done = 1;
}

void e2e_sas_config_load(e2e_sas_config* cfg)
{
  cfg->id_scope = az_iot_test_env_dup("AZ_IOT_DPS_ID_SCOPE");
  cfg->group_key = az_iot_test_env_dup("AZ_IOT_DPS_SAS_GROUP_KEY");
  cfg->registration_id = az_iot_test_env_dup("AZ_IOT_DPS_SAS_REGISTRATION_ID");
  cfg->trusted_ca = az_iot_test_env_dup("AZ_IOT_TRUSTED_CA");
  cfg->global_endpoint = az_iot_test_env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT");
  assert_non_null(cfg->id_scope);
  assert_non_null(cfg->group_key);
  assert_non_null(cfg->registration_id);
  assert_non_null(cfg->trusted_ca);
}

void e2e_sas_config_free(e2e_sas_config* cfg)
{
  free(cfg->id_scope);
  free(cfg->group_key);
  free(cfg->registration_id);
  free(cfg->trusted_ca);
  free(cfg->global_endpoint);
}

void e2e_sas_apply_dps(const e2e_sas_config* cfg, az_iot_connection_client_options* copts)
{
  copts->dps.id_scope = cfg->id_scope;
  copts->dps.registration_id = cfg->registration_id;
  if (cfg->global_endpoint != NULL)
  {
    copts->dps.global_endpoint = cfg->global_endpoint;
  }
  copts->trusted_ca.path = cfg->trusted_ca;
  copts->crypto = az_iot_crypto_openssl();
  copts->sas_buffer.buffer = g_sas_buffer;
  copts->sas_buffer.size = sizeof(g_sas_buffer);
  copts->dps_auth.sas.primary_key_base64 = cfg->group_key;
  copts->dps_auth.sas.is_enrollment_group_key = true;
}

void e2e_sas_connect_and_send(
    e2e_sas_run* run,
    const az_iot_connection_client_options* copts,
    const char* label)
{
  run->dps_source = AZ_IOT_AUTH_SOURCE_NONE;
  run->hub_source = AZ_IOT_AUTH_SOURCE_NONE;
  sas_ctx ctx = { .hub_state = AZ_IOT_CONN_STATE_IDLE, .run = run };
  az_iot_connection_client conn = { 0 };
  az_iot_mqttv3_telemetry_client telemetry = { 0 };

  assert_int_equal(az_iot_connection_client_init(&conn, copts), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&conn, on_conn_state, &ctx), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&conn, az_iot_paho_factory_create_v3_1_1()),
      AZ_IOT_OK);
  assert_int_equal(az_iot_mqttv3_telemetry_client_init(&telemetry, &conn), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&conn), AZ_IOT_OK);

  time_t start = time(NULL);
  while (ctx.hub_state != AZ_IOT_CONN_STATE_CONNECTED && ctx.hub_state != AZ_IOT_CONN_STATE_FAULTED
         && (time(NULL) - start) < E2E_SAS_CONNECT_TIMEOUT_S)
  {
    (void)az_iot_connection_client_do_work(&conn, 50);
  }
  assert_int_equal(ctx.hub_state, AZ_IOT_CONN_STATE_CONNECTED);

  char payload[64];
  int n = snprintf(payload, sizeof(payload), "{\"source\":\"%s\"}", label);
  assert_true(n > 0 && (size_t)n < sizeof(payload));
  az_iot_telemetry_message message = { 0 };
  message.payload = (const uint8_t*)payload;
  message.payload_len = (size_t)n;
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&telemetry, &message, on_send_done, &ctx), AZ_IOT_OK);
  start = time(NULL);
  while (!ctx.send_done && ctx.hub_state == AZ_IOT_CONN_STATE_CONNECTED
         && (time(NULL) - start) < E2E_SAS_SEND_TIMEOUT_S)
  {
    (void)az_iot_connection_client_do_work(&conn, 50);
  }
  assert_int_equal(ctx.send_done, 1);
  assert_int_equal(ctx.send_status, AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_close(&conn), AZ_IOT_OK);
  start = time(NULL);
  while (ctx.hub_state != AZ_IOT_CONN_STATE_IDLE && (time(NULL) - start) < E2E_SAS_CLOSE_TIMEOUT_S)
  {
    (void)az_iot_connection_client_do_work(&conn, 50);
  }
  assert_int_equal(ctx.hub_state, AZ_IOT_CONN_STATE_IDLE);
  az_iot_mqttv3_telemetry_client_deinit(&telemetry);
  az_iot_connection_client_deinit(&conn);
}
