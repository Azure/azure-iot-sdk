// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file main.c
 * @brief SAS from a symmetric key, to DPS and to the assigned hub.
 *
 * The SDK signs a SAS token with the primary key for each DPS attempt and for
 * the hub, and with the secondary key when the primary is rejected.
 * At AZ_IOT_SAS_TOKEN_RENEWAL_PERCENT of the token lifetime the client reconnects to
 * the hub with a new token.
 * Takes individual enrollment keys, or enrollment-group keys from which the
 * device keys are derived. Sends one telemetry message on
 * whichever hub generation DPS assigned; a hub that does not accept SAS fails
 * the connect with AZ_IOT_ERR_IDENTITY_REJECTED.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The MQTT adapter this build of the sample uses (see samples/CMakeLists.txt). */
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#else
#error "Define AZ_IOT_SAMPLE_ADAPTER_PAHO or AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT"
#endif
#include "azure/iot/az_iot.h"
/* The crypto backend this build of the sample uses (see samples/CMakeLists.txt). */
#if defined(AZ_IOT_SAMPLE_CRYPTO_OPENSSL)
#include "az_iot_crypto_openssl.h"
#elif defined(AZ_IOT_SAMPLE_CRYPTO_MBEDTLS)
#include "az_iot_crypto_mbedtls.h"
#else
#error "Define AZ_IOT_SAMPLE_CRYPTO_OPENSSL or AZ_IOT_SAMPLE_CRYPTO_MBEDTLS"
#endif

#include "sample_utils.h"

/** @brief Longest wait for provisioning plus the hub connection. */
#define SAMPLE_CONNECT_TIMEOUT_MS 60000u
/** @brief Longest wait for telemetry send completion. */
#define SAMPLE_SEND_TIMEOUT_MS 30000u
/** @brief Duration of one do_work() tick. */
#define SAMPLE_TICK_MS 50u

/** @brief Configuration, read from the environment. */
typedef struct
{
  char* id_scope; /**< AZ_IOT_DPS_ID_SCOPE. */
  char* reg_id; /**< AZ_IOT_DPS_REGISTRATION_ID; also the device ID. */
  char* key; /**< AZ_IOT_DPS_SYMMETRIC_KEY, or NULL. */
  char* group_key; /**< AZ_IOT_DPS_ENROLLMENT_GROUP_KEY, or NULL. */
  char* secondary_key; /**< AZ_IOT_DPS_SECONDARY_KEY (same kind as the primary), or NULL. */
  char* renewal_percent; /**< AZ_IOT_SAS_TOKEN_RENEWAL_PERCENT, or NULL for the default. */
  char* ca; /**< AZ_IOT_TRUSTED_CA, or NULL for the default trust store. */
  char* endpoint; /**< AZ_IOT_DPS_GLOBAL_ENDPOINT, or NULL. */
} sas_config;

/** @brief State shared with the callbacks. */
typedef struct
{
  az_iot_connection_state hub_state; /**< Latest hub-scope state. */
  bool failed; /**< A failure retrying cannot fix. */
  az_iot_result failed_reason; /**< Reason of that failure. */
  bool send_done; /**< Telemetry callback was invoked. */
  az_iot_result send_status; /**< Telemetry callback result. */
} sample_context;

static bool is_set(const char* s) { return s != NULL && s[0] != '\0'; }

static void config_release(sas_config* c)
{
  free(c->id_scope);
  free(c->reg_id);
  free(c->key);
  free(c->group_key);
  free(c->secondary_key);
  free(c->renewal_percent);
  free(c->ca);
  free(c->endpoint);
  memset(c, 0, sizeof(*c));
}

static const char* auth_source_name(az_iot_auth_source source)
{
  switch (source)
  {
    case AZ_IOT_AUTH_SOURCE_X509:
      return "X.509";
    case AZ_IOT_AUTH_SOURCE_PRIMARY_KEY:
      return "primary key";
    case AZ_IOT_AUTH_SOURCE_SECONDARY_KEY:
      return "secondary key";
    case AZ_IOT_AUTH_SOURCE_USER_PROVIDED:
      return "user-provided token";
    case AZ_IOT_AUTH_SOURCE_NONE:
    default:
      return "-";
  }
}

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  fprintf(
      stderr,
      "[dps_sas_key] %s: %s (%s), credential: %s%s\n",
      event->scope == AZ_IOT_CONN_SCOPE_DPS ? "dps" : "hub",
      az_iot_connection_state_to_string(event->state),
      az_iot_result_to_string(event->reason),
      auth_source_name(event->auth_source),
      event->is_credential_renewal ? " (token renewal)" : "");
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    ctx->hub_state = event->state;
  }
  /* Terminal: FAULTED, or hub IDLE with an error (no retry follows). A DPS
   * failure passes DISCONNECTING and IDLE before RETRY_PENDING or FAULTED. A
   * non-retriable RETRY_PENDING stops the sample unless it is the immediate
   * retry with the next credential (no delay). */
  bool falling_back = event->state == AZ_IOT_CONN_STATE_RETRY_PENDING && event->recovery != NULL
      && event->recovery->next_attempt_delay_ms == 0;
  if (event->reason != AZ_IOT_OK
      && (event->state == AZ_IOT_CONN_STATE_FAULTED
          || (event->scope == AZ_IOT_CONN_SCOPE_HUB && event->state == AZ_IOT_CONN_STATE_IDLE)
          || (event->state == AZ_IOT_CONN_STATE_RETRY_PENDING && !event->is_retriable
              && !falling_back)))
  {
    ctx->failed = true;
    ctx->failed_reason = event->reason;
  }
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  ctx->send_status = status;
  ctx->send_done = true;
}

/** @brief One do_work() tick of at least SAMPLE_TICK_MS. */
static void pump(az_iot_connection_client* client)
{
  uint64_t start = sample_now_ms();
  (void)az_iot_connection_client_do_work(client, SAMPLE_TICK_MS);
  sample_sleep_until_tick_end(start, SAMPLE_TICK_MS);
}

/** @brief Sends one message on the generation DPS assigned. */
static az_iot_result send_telemetry(az_iot_connection_client* client, sample_context* ctx)
{
  /* The generation DPS assigned decides which telemetry client to use. */
  az_iot_hub_profile hub_profile;
  az_iot_result r = az_iot_connection_client_get_hub_profile(client, &hub_profile);
  if (r != AZ_IOT_OK)
  {
    printf("Hub profile not available: %s\n", az_iot_result_to_string(r));
    return r;
  }
  az_iot_connection_profile profile = hub_profile.connection_profile;
  printf("Hub profile: %s\n", az_iot_connection_profile_to_string(profile));
  static const uint8_t payload[] = "{\"auth\":\"sas-key\"}";
  az_iot_telemetry_message msg = { 0 };
  msg.payload = payload;
  msg.payload_len = sizeof(payload) - 1;

  az_iot_mqttv3_telemetry_client mqttv3 = { 0 };
  az_iot_mqttv5_telemetry_client mqttv5 = { 0 };
  bool v5 = profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  r = v5 ? az_iot_mqttv5_telemetry_client_init(&mqttv5, client)
         : az_iot_mqttv3_telemetry_client_init(&mqttv3, client);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  r = v5 ? az_iot_mqttv5_telemetry_client_send(&mqttv5, &msg, on_send_done, ctx)
         : az_iot_mqttv3_telemetry_client_send(&mqttv3, &msg, on_send_done, ctx);
  uint64_t deadline = sample_now_ms() + SAMPLE_SEND_TIMEOUT_MS;
  while (r == AZ_IOT_OK && !ctx->send_done && !ctx->failed && sample_now_ms() < deadline)
  {
    pump(client);
  }
  if (r == AZ_IOT_OK)
  {
    r = ctx->send_done ? ctx->send_status : AZ_IOT_ERR_TIMEOUT;
  }
  if (v5)
  {
    az_iot_mqttv5_telemetry_client_deinit(&mqttv5);
  }
  else
  {
    az_iot_mqttv3_telemetry_client_deinit(&mqttv3);
  }
  return r;
}

int main(void)
{
  az_iot_log_sink log = sample_log_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  sas_config config = {
    .id_scope = sample_env_copy("AZ_IOT_DPS_ID_SCOPE", NULL),
    .reg_id = sample_env_copy("AZ_IOT_DPS_REGISTRATION_ID", NULL),
    .key = sample_env_copy("AZ_IOT_DPS_SYMMETRIC_KEY", NULL),
    .group_key = sample_env_copy("AZ_IOT_DPS_ENROLLMENT_GROUP_KEY", NULL),
    .secondary_key = sample_env_copy("AZ_IOT_DPS_SECONDARY_KEY", NULL),
    .renewal_percent = sample_env_copy("AZ_IOT_SAS_TOKEN_RENEWAL_PERCENT", NULL),
    .ca = sample_env_copy("AZ_IOT_TRUSTED_CA", NULL),
    .endpoint = sample_env_copy("AZ_IOT_DPS_GLOBAL_ENDPOINT", NULL),
  };
  if (!is_set(config.id_scope) || !is_set(config.reg_id)
      || is_set(config.key) == is_set(config.group_key))
  {
    fprintf(
        stderr,
        "[dps_sas_key] set AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID and exactly one of "
        "AZ_IOT_DPS_SYMMETRIC_KEY / AZ_IOT_DPS_ENROLLMENT_GROUP_KEY\n");
    config_release(&config);
    return 1;
  }

  int rc = 1;
  sample_context ctx = { 0 };
  az_iot_connection_client client = { 0 };

  /* Same keys for both roles: DPS creates the hub identity with the same
   * (derived) keys. No certificate_provider: nothing uses X.509. */
  az_iot_auth sas = { 0 };
  sas.sas.primary_key_base64 = is_set(config.key) ? config.key : config.group_key;
  sas.sas.secondary_key_base64 = is_set(config.secondary_key) ? config.secondary_key : NULL;
  sas.sas.is_enrollment_group_key = is_set(config.group_key);
  /* Unset: 0, which selects AZ_IOT_DEFAULT_SAS_RENEWAL_PERCENT. */
  sas.sas.renewal_percent = 0;
  if (is_set(config.renewal_percent))
  {
    char* end = NULL;
    unsigned long pct = strtoul(config.renewal_percent, &end, 10);
    if (*end != '\0' || pct < 1u || pct > 99u)
    {
      fprintf(stderr, "[dps_sas_key] AZ_IOT_SAS_TOKEN_RENEWAL_PERCENT must be 1-99\n");
      config_release(&config);
      return 1;
    }
    sas.sas.renewal_percent = (uint8_t)pct;
  }

  /* Memory for the connection client's entries, sized for the default limits. */
  static uint8_t connection_workspace[AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE];
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  opts.workspace = connection_workspace;
  opts.workspace_size = sizeof(connection_workspace);
  opts.device_id = config.reg_id;
  opts.dps.id_scope = config.id_scope;
  opts.dps.registration_id = config.reg_id;
  opts.dps.global_endpoint = config.endpoint;
  opts.dps_auth = sas;
  opts.hub_auth = sas;
#if defined(AZ_IOT_SAMPLE_CRYPTO_OPENSSL)
  opts.crypto = az_iot_crypto_openssl(); /* HMAC-SHA256 for the tokens */
#elif defined(AZ_IOT_SAMPLE_CRYPTO_MBEDTLS)
  opts.crypto = az_iot_crypto_mbedtls(); /* HMAC-SHA256 for the tokens */
#endif
  /* SAS state lives in app memory: up to two distinct keys (DPS and the hub
   * share them) and a token for IDs (hub host + device ID) up to 256 characters. */
  static uint8_t sas_buffer[AZ_IOT_SAS_BUFFER_SIZE(2, AZ_IOT_SAS_TOKEN_SIZE(256))];
  opts.sas_buffer.buffer = sas_buffer;
  opts.sas_buffer.size = sizeof(sas_buffer);
  opts.trusted_ca.path = config.ca;

  /* The adapter builds each session's MQTT client in this workspace, one at a time: MQTT 3.1.1
   * for DPS and an MQTTv3 hub, MQTT 5 for an MQTTv5 hub. Limits: sample_utils.h. */
  static uint8_t mqtt_workspace[SAMPLE_MQTT_WORKSPACE_SIZE];
#if defined(AZ_IOT_SAMPLE_ADAPTER_PAHO)
  opts.mqtt_adapter = az_iot_paho_get_adapter();
#elif defined(AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT)
  opts.mqtt_adapter = az_iot_az_mqtt_get_adapter();
#endif
  opts.mqtt_limits = SAMPLE_MQTT_LIMITS;
  opts.mqtt_workspace = mqtt_workspace;
  opts.mqtt_workspace_size = sizeof(mqtt_workspace);

  if (az_iot_connection_client_init(&client, &opts) != AZ_IOT_OK
      || az_iot_connection_client_add_state_observer(&client, on_conn_state, &ctx) != AZ_IOT_OK
      || az_iot_connection_client_open(&client) != AZ_IOT_OK)
  {
    fprintf(stderr, "[dps_sas_key] connection client setup failed\n");
    goto cleanup;
  }

  uint64_t deadline = sample_now_ms() + SAMPLE_CONNECT_TIMEOUT_MS;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_CONNECTED && !ctx.failed && sample_now_ms() < deadline)
  {
    pump(&client);
  }
  if (ctx.hub_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    az_iot_result sent = send_telemetry(&client, &ctx);
    fprintf(stderr, "[dps_sas_key] telemetry: %s\n", az_iot_result_to_string(sent));
    rc = sent == AZ_IOT_OK ? 0 : 1;
  }
  else
  {
    fprintf(
        stderr,
        "[dps_sas_key] not connected: %s\n",
        ctx.failed ? az_iot_result_to_string(ctx.failed_reason) : "timeout");
  }

  az_iot_connection_client_close(&client);
  deadline = sample_now_ms() + 5000u;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_IDLE && sample_now_ms() < deadline)
  {
    pump(&client);
  }

cleanup:
  az_iot_connection_client_deinit(&client);
  config_release(&config);
  return rc;
}
