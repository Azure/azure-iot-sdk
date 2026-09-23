// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* authentication/direct_hub_gen1 - sample.
 *
 * Connect DIRECTLY to a Classic IoT Hub (no DPS) with a caller-supplied hub
 * FQDN and X.509 device credentials, then send one telemetry message and close.
 *
 * PREFER DPS. A device is meant to learn its hub from provisioning, and every
 * other connecting sample does. This one is for bring-up and diagnosis: someone
 * handed you a hub FQDN and a device cert and you want to check them in
 * isolation. Nothing else in the tree depends on this path.
 *
 * Because there is no DPS step, nothing on the wire announces the hub
 * generation -- the application states it. Here that is
 * AZ_IOT_CONNECTION_PROFILE_CLASSIC, fixed at compile time, because the
 * hostname you were given already decided it. direct_hub_gen2 is the same
 * sample against an AEG endpoint. A device that provisions needs neither
 * constant: DPS reports the generation, and samples/connection_profile_fallback
 * shows how to read it.
 *
 * Fill in the SAMPLE_* placeholders below, or set the matching environment
 * variables (env wins). When required values are missing the sample prints a
 * usage hint and exits 0 so it stays safe in a default build matrix.
 *
 * Environment (each overrides the matching SAMPLE_* constant):
 *   AZ_IOT_HUB_HOSTNAME   direct hub FQDN, e.g. "my-hub.azure-devices.net"
 *   AZ_IOT_DEVICE_ID      device id / MQTT client id
 *   AZ_IOT_CLIENT_CERT    device certificate PEM path
 *   AZ_IOT_CLIENT_KEY     device private key PEM path
 *   AZ_IOT_TRUSTED_CA     trusted CA PEM path (optional; system store if unset)
 *
 * Diagnostics (optional):
 *   AZ_IOT_PAHO_TRACE     enables Paho MQTT trace + verbose OpenSSL TLS errors.
 *                         Value selects verbosity: "error" < "protocol" <
 *                         "min" < "medium" < "max". Any truthy value (e.g. "1")
 *                         => minimum. Example: AZ_IOT_PAHO_TRACE=max
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

#include "sample_utils.h"

/* ---- Fill these in, or set the matching env vars (env wins) --------------- */
#define SAMPLE_HUB_HOSTNAME "" /* AZ_IOT_HUB_HOSTNAME */
#define SAMPLE_DEVICE_ID "" /* AZ_IOT_DEVICE_ID    */
#define SAMPLE_CLIENT_CERT "" /* AZ_IOT_CLIENT_CERT  */
#define SAMPLE_CLIENT_KEY "" /* AZ_IOT_CLIENT_KEY   */
#define SAMPLE_TRUSTED_CA "" /* AZ_IOT_TRUSTED_CA (optional) */

typedef struct
{
  char* host;
  char* device_id;
  char* cert;
  char* key;
  char* ca; /* NULL/empty => use the system trust store */
} direct_config;

static int is_set(const char* s) { return s != NULL && s[0] != '\0'; }

static void direct_config_release(direct_config* c)
{
  free(c->host);
  free(c->device_id);
  free(c->cert);
  free(c->key);
  free(c->ca);
  memset(c, 0, sizeof(*c));
}

/* Loads config from env vars (falling back to the SAMPLE_* constants). Returns
 * 0 when all required fields are present, non-zero otherwise. */
static int direct_config_load(direct_config* c)
{
  memset(c, 0, sizeof(*c));
  c->host = sample_env_dup("AZ_IOT_HUB_HOSTNAME", SAMPLE_HUB_HOSTNAME);
  c->device_id = sample_env_dup("AZ_IOT_DEVICE_ID", SAMPLE_DEVICE_ID);
  c->cert = sample_env_dup("AZ_IOT_CLIENT_CERT", SAMPLE_CLIENT_CERT);
  c->key = sample_env_dup("AZ_IOT_CLIENT_KEY", SAMPLE_CLIENT_KEY);
  c->ca = sample_env_dup("AZ_IOT_TRUSTED_CA", SAMPLE_TRUSTED_CA);

  return (is_set(c->host) && is_set(c->device_id) && is_set(c->cert) && is_set(c->key)) ? 0 : 1;
}

static void print_usage(void)
{
  fprintf(
      stderr,
      "[direct_hub_gen1] not configured; skipping.\n"
      "  Set these env vars (or edit the SAMPLE_* constants) and re-run:\n"
      "    AZ_IOT_HUB_HOSTNAME  direct hub FQDN\n"
      "    AZ_IOT_DEVICE_ID     device id / MQTT client id\n"
      "    AZ_IOT_CLIENT_CERT   device certificate PEM path\n"
      "    AZ_IOT_CLIENT_KEY    device private key PEM path\n"
      "    AZ_IOT_TRUSTED_CA    trusted CA PEM path (optional)\n");
}

typedef struct
{
  az_iot_connection_state conn_state;
  az_iot_result conn_reason; /* reason for the latest state transition */
  int send_done;
  az_iot_result send_status;
} user_context;

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  /* Hub lifecycle only: the provisioning session reports on its own scope,
   * and storing its state here would overwrite the hub state this code acts
   * on. */
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }

  user_context* ctx = (user_context*)user_ctx;
  ctx->conn_state = event->state;
  ctx->conn_reason = event->reason;
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  user_context* ctx = (user_context*)user_ctx;
  ctx->send_status = status;
  ctx->send_done = 1;
}

int main(void)
{
  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  direct_config config = { 0 };
  if (direct_config_load(&config) != 0)
  {
    print_usage();
    direct_config_release(&config);
    return 0; /* no-op when unconfigured */
  }

  fprintf(
      stderr,
      "[direct_hub_gen1] connecting to %s as device '%s' (Classic / MQTT v3.1.1)\n",
      config.host,
      config.device_id);

  int rc = 1;
  user_context user_ctx = { 0 };
  az_iot_certificate_provider_pem certs = { 0 };
  az_iot_connection_client connection_client = { 0 };
  az_iot_gen1_telemetry_client telemetry = { 0 };

  /* Certificate provider: device X.509 identity supplied as PEM files. */
  az_iot_certificate_provider_pem_options pem = az_iot_certificate_provider_pem_options_default();
  pem.trusted_ca_pem_path = is_set(config.ca) ? config.ca : NULL;
  pem.client_cert_pem_path = config.cert;
  pem.client_key_pem_path = config.key;

  if (az_iot_certificate_provider_pem_init(&certs, &pem) != AZ_IOT_OK)
  {
    fprintf(stderr, "[direct_hub_gen1] certificate provider init failed\n");
    goto cleanup;
  }

  /* Connection client: DIRECT hub connect (no DPS). host + client_id +
   * connection_profile select the endpoint and MQTT version. */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.host = config.host;
  copts.client_id = config.device_id;
  copts.connection_profile = AZ_IOT_CONNECTION_PROFILE_CLASSIC;
  copts.certificate_provider = &certs.base;

  if (az_iot_connection_client_init(&connection_client, &copts) != AZ_IOT_OK)
  {
    goto cleanup;
  }

  az_iot_connection_client_add_state_observer(&connection_client, on_conn_state, &user_ctx);

  /* Classic speaks v3.1.1 and there is no DPS leg to cover, so this is the only
   * adapter the sample can need. */
  if (az_iot_connection_client_register_mqtt_factory(
          &connection_client, az_iot_paho_factory_create_v3_1_1())
      != AZ_IOT_OK)
  {
    goto cleanup;
  }

  /* Bound to the profile declared above: an AEG endpoint would fail the
   * connection rather than silently downgrade. */
  if (az_iot_gen1_telemetry_client_init(&telemetry, &connection_client) != AZ_IOT_OK)
  {
    goto cleanup;
  }

  if (az_iot_connection_client_open(&connection_client) != AZ_IOT_OK)
  {
    goto cleanup;
  }

  for (int i = 0; i < 1200 && user_ctx.conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
  {
    (void)az_iot_connection_client_do_work(&connection_client, 50);
    if (user_ctx.conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }

  if (user_ctx.conn_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    fprintf(stderr, "[direct_hub_gen1] connected; sending telemetry\n");

    static const uint8_t payload[] = "{\"temp\":23}";
    az_iot_telemetry_property props[] = {
      { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
    };
    az_iot_telemetry_message msg = { 0 };
    msg.payload = payload;
    msg.payload_len = sizeof(payload) - 1;
    msg.properties = props;
    msg.properties_count = sizeof(props) / sizeof(props[0]);

    if (az_iot_gen1_telemetry_client_send(&telemetry, &msg, on_send_done, &user_ctx) == AZ_IOT_OK)
    {
      for (int i = 0; i < 600 && !user_ctx.send_done; ++i)
      {
        (void)az_iot_connection_client_do_work(&connection_client, 50);
      }

      if (user_ctx.send_done && user_ctx.send_status == AZ_IOT_OK)
      {
        fprintf(stderr, "[direct_hub_gen1] telemetry sent\n");
        rc = 0;
      }
    }
  }
  else
  {
    fprintf(
        stderr,
        "[direct_hub_gen1] failed to connect (state=%s, reason=%s)\n",
        az_iot_connection_state_to_string(user_ctx.conn_state),
        az_iot_result_to_string(user_ctx.conn_reason));
  }

  az_iot_connection_client_close(&connection_client);
  for (int i = 0; i < 100 && user_ctx.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&connection_client, 50);
  }

cleanup:
  az_iot_gen1_telemetry_client_destroy(&telemetry);
  az_iot_connection_client_destroy(&connection_client);
  az_iot_certificate_provider_pem_destroy(&certs);
  direct_config_release(&config);
  return rc;
}
