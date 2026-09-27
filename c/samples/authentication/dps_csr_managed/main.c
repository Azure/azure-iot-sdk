// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file main.c
 * @brief DPS enrollment that obtains an operational X.509 certificate from a CSR.
 *
 * 1. Authenticates to DPS with an X.509 bootstrap identity.
 * 2. Sends a CSR over the operational key in the register request.
 * 3. The managed provider persists the issued chain.
 * 4. Connects to the assigned Classic IoT Hub with the operational identity.
 * 5. Sends telemetry over that connection and waits for send completion.
 *
 * AZ_IOT_DPS_REGISTRATION_PAYLOAD, if set, is sent alongside the CSR as the
 * custom registration payload. Requires OpenSSL 3.0+ and the Paho adapter.
 * See README.md for setup and environment variables.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "az_iot_certificate_provider_managed.h"

#include "sample_utils.h"

/** @brief Longest wait for provisioning plus the hub connection. */
#define SAMPLE_CONNECT_TIMEOUT_MS 60000u
/** @brief Longest wait for telemetry send completion. */
#define SAMPLE_SEND_TIMEOUT_MS 30000u
/** @brief Longest wait for a graceful disconnect. */
#define SAMPLE_CLOSE_TIMEOUT_MS 5000u
/** @brief Duration of one do_work() tick. */
#define SAMPLE_TICK_MS 50u

/** @brief State shared with the client callbacks. */
typedef struct
{
  az_iot_connection_state hub_state; /**< Latest hub-scope state. */
  bool failed; /**< A failure the SDK faulted on, or one retrying cannot fix. */
  az_iot_connection_scope failed_scope; /**< Scope of that failure. */
  az_iot_result failed_reason; /**< Reason of that failure. */
  az_iot_result last_error[AZ_IOT_CONN_SCOPE_COUNT]; /**< Latest non-OK reason per scope. */
  bool issued; /**< An operational certificate was issued and persisted. */
  bool send_done; /**< Telemetry send callback was invoked. */
  az_iot_result send_status; /**< Telemetry send callback result. */
  const char* operational_cert_path; /**< Where the issued chain is persisted. */
} sample_context;

/** @brief Logs every transition and records the ones the main loop acts on. */
static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  bool is_dps = event->scope == AZ_IOT_CONN_SCOPE_DPS;

  fprintf(
      stderr,
      "[dps_csr] %s: %s (%s)\n",
      is_dps ? "dps" : "hub",
      sample_connection_state_name(event->state),
      az_iot_result_to_string(event->reason));

  /* Service verdicts only; transport codes are adapter-defined. */
  const az_iot_connection_error_detail* error
      = AZ_IOT_STRUCT_HAS_FIELD(event, az_iot_connection_state_event, error) ? event->error : NULL;
  if (error != NULL
      && (error->source == AZ_IOT_CONN_ERR_SRC_DPS || error->source == AZ_IOT_CONN_ERR_SRC_MQTT))
  {
    int32_t message_len = az_span_size(error->message);
    fprintf(
        stderr,
        "[dps_csr]   %s code %ld%s%.*s\n",
        error->source == AZ_IOT_CONN_ERR_SRC_DPS ? "DPS" : "MQTT",
        (long)error->code,
        message_len > 0 ? ": " : "",
        (int)message_len,
        message_len > 0 ? (const char*)az_span_ptr(error->message) : "");
  }

  if (event->scope != AZ_IOT_CONN_SCOPE_DPS && event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    ctx->hub_state = event->state;
  }
  if (event->reason == AZ_IOT_OK)
  {
    return;
  }
  ctx->last_error[event->scope] = event->reason;

  /* The default policy retries forever, including failures that retrying
   * cannot fix (e.g. no issued certificate, a rejected bootstrap identity);
   * those end the wait instead of running into the timeout. */
  bool permanent = AZ_IOT_STRUCT_HAS_FIELD(event, az_iot_connection_state_event, is_retriable)
      && !event->is_retriable;
  if (!ctx->failed && (event->state == AZ_IOT_CONN_STATE_FAULTED || permanent))
  {
    ctx->failed = true;
    ctx->failed_scope = event->scope;
    ctx->failed_reason = event->reason;
  }
}

/** @brief Prints the custom payload returned by the allocation policy.
 *  @p payload is valid only during this call; copy anything to keep. */
static void on_registration_payload(az_span payload, void* user_ctx)
{
  (void)user_ctx;
  fprintf(
      stderr,
      "[dps_csr] registration payload from DPS: %.*s\n",
      (int)az_span_size(payload),
      (const char*)az_span_ptr(payload));
}

/** @brief Called after the provider has persisted the issued chain. */
static void on_operational_cert(const az_iot_issued_certificate* issued, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  ctx->issued = true;
  fprintf(
      stderr,
      "[dps_csr] operational certificate issued (%zu cert(s) in chain), saved to %s\n",
      issued != NULL ? issued->count : (size_t)0,
      ctx->operational_cert_path);
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  sample_context* ctx = (sample_context*)user_ctx;
  ctx->send_status = status;
  ctx->send_done = true;
}

/** @brief Name of @p r, or "none" for AZ_IOT_OK. */
static const char* error_name(az_iot_result r)
{
  return r == AZ_IOT_OK ? "none" : az_iot_result_to_string(r);
}

/** @brief Runs one do_work() tick of at least SAMPLE_TICK_MS, so the loops
 *  cannot spin when do_work() has no session to wait on. */
static void pump(az_iot_connection_client* client)
{
  uint64_t start = sample_now_ms();
  (void)az_iot_connection_client_do_work(client, SAMPLE_TICK_MS);
  uint64_t spent = sample_now_ms() - start;
  if (spent < SAMPLE_TICK_MS)
  {
    sample_sleep_ms((long)(SAMPLE_TICK_MS - spent));
  }
}

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  sample_config config = { 0 };
  if (sample_config_load(&config) != 0)
  {
    return 1;
  }

  /* Every string cleanup frees is assigned before the first goto. */
  char* op_key = sample_env_dup("AZ_IOT_OPERATIONAL_KEY", "operational_key.pem");
  char* op_cert = sample_env_dup("AZ_IOT_OPERATIONAL_CERT", "operational_cert.pem");
  char* registration_payload = sample_env_dup("AZ_IOT_DPS_REGISTRATION_PAYLOAD", NULL);

  int rc = 1;
  sample_context ctx = { 0 };
  ctx.operational_cert_path = op_cert;
  az_iot_certificate_provider_managed provider = { 0 };
  az_iot_connection_client connection_client = { 0 };
  az_iot_mqttv3_telemetry_client telemetry = { 0 };
  bool telemetry_initialized = false;

  if (op_key == NULL || op_cert == NULL)
  {
    fprintf(stderr, "[dps_csr] out of memory\n");
    goto cleanup;
  }

  /* Loads the operational key, or generates and saves one on first run. The
   * key type applies only when a key is generated. */
  az_iot_certificate_provider_managed_options provider_opts = {
    .bootstrap_cert_pem_path = config.cert,
    .bootstrap_key_pem_path = config.key,
    .trusted_ca_pem_path = config.ca,
    .operational_key_pem_path = op_key,
    .operational_cert_pem_path = op_cert,
    .key_type = AZ_IOT_MANAGED_KEY_EC_P256,
  };
  if (az_iot_certificate_provider_managed_init(&provider, &provider_opts) != AZ_IOT_OK)
  {
    fprintf(stderr, "[dps_csr] managed certificate provider init failed\n");
    goto cleanup;
  }

  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  sample_apply_dps_options(&opts, &config);
  opts.certificate_provider = &provider.base;
  AZ_IOT_CSR_PAYLOAD_STORAGE(csr_payload_buf);
  opts.csr_payload_buffer = az_span_create(csr_payload_buf, sizeof(csr_payload_buf));
  opts.dps.request_operational_certificate = true;

  /* The custom payload shares the register body with the CSR;
   * AZ_IOT_DPS_REGISTRATION_BODY_STORAGE() sizes a buffer for both. */
  AZ_IOT_DPS_REGISTRATION_BODY_STORAGE(registration_body_buf);
  if (registration_payload != NULL && registration_payload[0] != '\0')
  {
    opts.dps.registration_payload
        = az_span_create((uint8_t*)registration_payload, (int32_t)strlen(registration_payload));
    opts.dps.registration_body_buffer
        = az_span_create(registration_body_buf, sizeof(registration_body_buf));
  }

  if (az_iot_connection_client_init(&connection_client, &opts) != AZ_IOT_OK
      || az_iot_connection_client_add_state_observer(&connection_client, on_conn_state, &ctx)
          != AZ_IOT_OK
      || az_iot_connection_client_set_operational_cert_callback(
             &connection_client, on_operational_cert, &ctx)
          != AZ_IOT_OK
      || az_iot_connection_client_set_registration_payload_callback(
             &connection_client, on_registration_payload, &ctx)
          != AZ_IOT_OK
      /* DPS and Classic IoT Hub both use MQTT 3.1.1. */
      || az_iot_connection_client_register_mqtt_factory(
             &connection_client, az_iot_paho_factory_create_v3_1_1())
          != AZ_IOT_OK)
  {
    fprintf(stderr, "[dps_csr] connection client setup failed\n");
    goto cleanup;
  }

  if (az_iot_mqttv3_telemetry_client_init(&telemetry, &connection_client) != AZ_IOT_OK)
  {
    fprintf(stderr, "[dps_csr] Classic hub telemetry client setup failed\n");
    goto cleanup;
  }
  telemetry_initialized = true;

  if (az_iot_connection_client_open(&connection_client) != AZ_IOT_OK)
  {
    fprintf(stderr, "[dps_csr] open failed; see the SDK log above\n");
    goto cleanup;
  }

  uint64_t deadline = sample_now_ms() + SAMPLE_CONNECT_TIMEOUT_MS;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_CONNECTED && !ctx.failed && sample_now_ms() < deadline)
  {
    pump(&connection_client);
  }

  if (ctx.hub_state == AZ_IOT_CONN_STATE_CONNECTED && ctx.issued)
  {
    fprintf(
        stderr,
        "[dps_csr] connected to %s with the operational certificate\n",
        az_iot_connection_client_get_iothub_address(&connection_client));
    static const uint8_t payload[] = "{\"source\":\"dps_csr_managed\"}";
    az_iot_telemetry_property properties[] = {
      { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
    };
    az_iot_telemetry_message message = { 0 };
    message.payload = payload;
    message.payload_len = sizeof(payload) - 1;
    message.properties = properties;
    message.properties_count = sizeof(properties) / sizeof(properties[0]);

    az_iot_result send_result
        = az_iot_mqttv3_telemetry_client_send(&telemetry, &message, on_send_done, &ctx);
    if (send_result != AZ_IOT_OK)
    {
      fprintf(
          stderr, "[dps_csr] telemetry send failed: %s\n", az_iot_result_to_string(send_result));
    }
    else
    {
      deadline = sample_now_ms() + SAMPLE_SEND_TIMEOUT_MS;
      while (!ctx.send_done && !ctx.failed && ctx.hub_state == AZ_IOT_CONN_STATE_CONNECTED
             && sample_now_ms() < deadline)
      {
        pump(&connection_client);
      }
      if (ctx.send_done && ctx.send_status == AZ_IOT_OK)
      {
        fprintf(stderr, "[dps_csr] telemetry sent with operational certificate\n");
        rc = 0;
      }
      else if (ctx.send_done)
      {
        fprintf(
            stderr,
            "[dps_csr] telemetry send failed: %s\n",
            az_iot_result_to_string(ctx.send_status));
      }
      else if (ctx.failed || ctx.hub_state != AZ_IOT_CONN_STATE_CONNECTED)
      {
        fprintf(stderr, "[dps_csr] hub disconnected before telemetry send completed\n");
      }
      else
      {
        fprintf(stderr, "[dps_csr] timed out waiting for telemetry send completion\n");
      }
    }
  }
  else if (ctx.hub_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    fprintf(stderr, "[dps_csr] connected, but no operational certificate was issued\n");
  }
  else if (ctx.failed)
  {
    fprintf(
        stderr,
        "[dps_csr] %s failed: %s\n",
        ctx.failed_scope == AZ_IOT_CONN_SCOPE_DPS ? "provisioning" : "hub connection",
        az_iot_result_to_string(ctx.failed_reason));
  }
  else
  {
    fprintf(
        stderr,
        "[dps_csr] timed out after %u s waiting for the hub connection (last dps error: %s, "
        "last hub error: %s)\n",
        SAMPLE_CONNECT_TIMEOUT_MS / 1000u,
        error_name(ctx.last_error[AZ_IOT_CONN_SCOPE_DPS]),
        error_name(ctx.last_error[AZ_IOT_CONN_SCOPE_HUB]));
  }

  az_iot_connection_client_close(&connection_client);
  deadline = sample_now_ms() + SAMPLE_CLOSE_TIMEOUT_MS;
  while (ctx.hub_state != AZ_IOT_CONN_STATE_IDLE && sample_now_ms() < deadline)
  {
    pump(&connection_client);
  }

cleanup:
  if (telemetry_initialized)
  {
    az_iot_mqttv3_telemetry_client_deinit(&telemetry);
  }
  az_iot_connection_client_deinit(&connection_client);
  az_iot_certificate_provider_managed_deinit(&provider);
  free(op_key);
  free(op_cert);
  free(registration_payload);
  sample_config_release(&config);
  return rc;
}
