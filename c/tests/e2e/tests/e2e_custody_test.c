// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* End-to-end custody: a device whose private key is inside a PKCS#11 token
 * (design decision D8), against a real Azure DPS + IoT Hub.
 *
 * This is the "Storage / custody" group of the certificate-management design:
 * the same DPS issuance and hub traffic every other e2e suite performs, but
 * with a key the process cannot read. Nothing here is stubbed -- DPS really
 * provisions, the hub really accepts the TLS handshake, and the service side
 * really observes the telemetry over the Event Hub-compatible endpoint. If the
 * token were not signing the handshake, the hub would reject the connection.
 *
 * The token is provisioned by eng/setup-softhsm.sh, which imports the device
 * X.509 key CI already generates and prints the URI below. Any PKCS#11 module
 * works; CI uses SoftHSM2 driven through the OpenSSL pkcs11 provider.
 *
 * Configuration (in addition to the standard e2e device/service variables):
 *   AZ_IOT_CLIENT_KEY_URI    pkcs11: URI of the device private key
 *   AZ_IOT_CRYPTO_ENGINE_ID  OpenSSL provider id (default "pkcs11")
 *
 * There is no self-skip: the suite is built only when the token is going to be
 * provided (AZ_IOT_BUILD_E2E_PKCS11), and it fails when it is not.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot.h"

#include "az_iot_e2e_service.h"
#include "e2e_log.h"

#define E2E_CONNECT_TIMEOUT_S 90
#define E2E_TELEMETRY_TIMEOUT_S 90
#define E2E_PUMP_MS 20

/* ------------------------------------------------------------------------- */
/* device half: certificate from a file, key from the token                   */
/* ------------------------------------------------------------------------- */

typedef struct
{
  char* id_scope;
  char* reg_id;
  char* cert;
  char* ca;
  char* key_uri;
  char* engine_id;
  char* global_endpoint; /* optional */
} custody_config;

typedef struct
{
  az_iot_certificate_provider base;
  const custody_config* config;
  int load_calls;
} custody_provider;

typedef struct
{
  custody_config config;
  custody_provider provider;
  az_iot_connection_client conn;
  az_iot_connection_state conn_state;
  bool conn_ok;
  az_iot_e2e_service* service;
  az_iot_mqttv3_telemetry_client telemetry;
  bool telemetry_ok;
  int send_done;
  az_iot_result send_status;
} custody_fixture;

static custody_fixture g_fixture;

static char* str_dup(const char* value)
{
  size_t n = strlen(value) + 1;
  char* copy = (char*)malloc(n);
  if (copy != NULL)
  {
    memcpy(copy, value, n);
  }
  return copy;
}

static char* env_dup(const char* name)
{
  const char* value = getenv(name);
  if (value == NULL || value[0] == '\0')
  {
    return NULL;
  }
  return str_dup(value);
}

static az_iot_result custody_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  custody_provider* p = (custody_provider*)self;
  (void)role;
  p->load_calls++;
  memset(out, 0, sizeof(*out));
  out->client_cert_path = p->config->cert;
  out->trusted_ca_path = p->config->ca;
  /* The key is named, never handed over. */
  out->client_key_uri = p->config->key_uri;
  out->crypto_engine_id = p->config->engine_id;
  return AZ_IOT_OK;
}

static void custody_release(az_iot_certificate_provider* self, az_iot_certificate_material* m)
{
  (void)self;
  (void)m;
}

static void custody_deinit(az_iot_certificate_provider* self) { (void)self; }

static const az_iot_certificate_provider_vtable k_custody_vtable = {
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = custody_load,
  .release = custody_release,
  .deinit = custody_deinit,
};

static void on_conn_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  /* Hub lifecycle only: the provisioning session reports on its own scope,
   * and storing its state here would overwrite the hub state this code acts
   * on. */
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }

  az_iot_connection_state s = event->state;
  az_iot_result reason = event->reason;
  (void)reason;
  ((custody_fixture*)user_ctx)->conn_state = s;
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  custody_fixture* fx = (custody_fixture*)user_ctx;
  fx->send_status = status;
  fx->send_done = 1;
}

static int config_load(custody_config* c)
{
  c->id_scope = env_dup("AZ_IOT_DPS_ID_SCOPE");
  c->reg_id = env_dup("AZ_IOT_DPS_REGISTRATION_ID");
  c->cert = env_dup("AZ_IOT_CLIENT_CERT");
  c->ca = env_dup("AZ_IOT_TRUSTED_CA");
  c->key_uri = env_dup("AZ_IOT_CLIENT_KEY_URI");
  c->engine_id = env_dup("AZ_IOT_CRYPTO_ENGINE_ID");
  c->global_endpoint = env_dup("AZ_IOT_DPS_GLOBAL_ENDPOINT");
  if (c->engine_id == NULL)
  {
    c->engine_id = str_dup("pkcs11");
  }

  if (c->id_scope == NULL || c->reg_id == NULL || c->cert == NULL || c->ca == NULL
      || c->key_uri == NULL)
  {
    fprintf(
        stderr,
        "[e2e-custody] missing required env vars: AZ_IOT_DPS_ID_SCOPE/"
        "AZ_IOT_DPS_REGISTRATION_ID/AZ_IOT_CLIENT_CERT/AZ_IOT_TRUSTED_CA/"
        "AZ_IOT_CLIENT_KEY_URI. This suite was built with AZ_IOT_BUILD_E2E_PKCS11=ON, so a "
        "PKCS#11 token is required; provision one with eng/setup-softhsm.sh or configure with "
        "AZ_IOT_BUILD_E2E_PKCS11=OFF.\n");
    return 1;
  }
  return 0;
}

/* Provision through DPS and reach CONNECTED using only the token key. */
static int device_connect(custody_fixture* fx)
{
  fx->provider.base.vtable = &k_custody_vtable;
  fx->provider.config = &fx->config;

  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.dps.id_scope = fx->config.id_scope;
  copts.dps.registration_id = fx->config.reg_id;
  copts.certificate_provider = &fx->provider.base;
  if (fx->config.global_endpoint != NULL)
  {
    copts.dps.global_endpoint = fx->config.global_endpoint;
  }

  if (az_iot_connection_client_init(&fx->conn, &copts) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e-custody] connection client init failed\n");
    return 1;
  }
  fx->conn_ok = true;
  az_iot_connection_client_add_state_observer(&fx->conn, on_conn_state, fx);

  if (az_iot_connection_client_register_mqtt_factory(&fx->conn, az_iot_paho_factory_create_v3_1_1())
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&fx->conn, az_iot_paho_factory_create_v5())
          != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e-custody] MQTT factory registration failed\n");
    return 1;
  }

  az_iot_result open_rc = az_iot_connection_client_open(&fx->conn);
  if (open_rc != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e-custody] open failed: %s\n", az_iot_result_to_string(open_rc));
    return 1;
  }

  time_t start = time(NULL);
  while (fx->conn_state != AZ_IOT_CONN_STATE_CONNECTED
         && (time(NULL) - start) < E2E_CONNECT_TIMEOUT_S)
  {
    (void)az_iot_connection_client_do_work(&fx->conn, 50);
    if (fx->conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }
  if (fx->conn_state != AZ_IOT_CONN_STATE_CONNECTED)
  {
    fprintf(
        stderr, "[e2e-custody] device did not reach CONNECTED (state=%d)\n", (int)fx->conn_state);
    return 1;
  }
  return 0;
}

static void device_disconnect(custody_fixture* fx)
{
  if (fx->telemetry_ok)
  {
    az_iot_mqttv3_telemetry_client_deinit(&fx->telemetry);
    fx->telemetry_ok = false;
  }
  if (fx->conn_ok)
  {
    az_iot_connection_client_close(&fx->conn);
    for (int i = 0; i < 100 && fx->conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
    {
      (void)az_iot_connection_client_do_work(&fx->conn, 50);
    }
    az_iot_connection_client_deinit(&fx->conn);
    fx->conn_ok = false;
  }
  free(fx->config.id_scope);
  free(fx->config.reg_id);
  free(fx->config.cert);
  free(fx->config.ca);
  free(fx->config.key_uri);
  free(fx->config.engine_id);
  free(fx->config.global_endpoint);
  memset(&fx->config, 0, sizeof(fx->config));
}

/* ------------------------------------------------------------------------- */

static int group_setup(void** state)
{
  (void)state;
  memset(&g_fixture, 0, sizeof(g_fixture));
  if (config_load(&g_fixture.config) != 0)
  {
    return 1;
  }
  e2e_install_log_sink();
  fprintf(
      stderr,
      "[e2e-custody] device '%s': key stays in the token (%s via '%s')\n",
      g_fixture.config.reg_id,
      g_fixture.config.key_uri,
      g_fixture.config.engine_id);

  const char* error = NULL;
  g_fixture.service = az_iot_e2e_service_create(&error);
  if (g_fixture.service == NULL)
  {
    fprintf(stderr, "[e2e-custody] service facade unavailable: %s\n", error ? error : "(unknown)");
    return 1;
  }
  if (device_connect(&g_fixture) != 0)
  {
    return 1;
  }
  if (az_iot_mqttv3_telemetry_client_init(&g_fixture.telemetry, &g_fixture.conn) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e-custody] telemetry client init failed\n");
    return 1;
  }
  g_fixture.telemetry_ok = true;
  return 0;
}

static int group_teardown(void** state)
{
  (void)state;
  device_disconnect(&g_fixture);
  if (g_fixture.service != NULL)
  {
    az_iot_e2e_service_destroy(g_fixture.service);
    g_fixture.service = NULL;
  }
  return 0;
}

/* DPS provisioned and the hub accepted the handshake, using a key no part of
 * this process could read. Reaching CONNECTED IS the assertion: the hub
 * validates the client certificate against a signature only the token could
 * have produced. */
static void dps_provisions_and_the_hub_accepts_a_token_key(void** state)
{
  (void)state;
  assert_int_equal(g_fixture.conn_state, AZ_IOT_CONN_STATE_CONNECTED);
  /* Both connect paths ran: the DPS/bootstrap one and the operational one. */
  assert_true(g_fixture.provider.load_calls >= 2);
}

/* Traffic actually flows on the connection the token authenticated: the
 * service side sees the message on the Event Hub-compatible endpoint. */
static void telemetry_flows_over_the_token_authenticated_connection(void** state)
{
  (void)state;
  char needle[128];
  snprintf(needle, sizeof(needle), "custody-%ld", (long)time(NULL));

  /* The cold AMQP handshake to the Event Hub-compatible endpoint (TLS ->
   * connection -> CBS SAS -> per-partition receivers) can fail transiently
   * against a freshly provisioned hub, so retry with a short backoff and pump
   * the device in between to keep its MQTT connection warm. A single attempt
   * whose reason was discarded is what this test did before, and a transient
   * open failure was indistinguishable from a custody fault. Mirrors
   * e2e_scenarios_test.c. */
  bool watching = az_iot_e2e_service_telemetry_watch_begin(g_fixture.service);
  for (time_t connect_start = time(NULL);
       !watching && (time(NULL) - connect_start) < E2E_CONNECT_TIMEOUT_S;)
  {
    for (int i = 0; i < 50; i++) /* ~1s backoff, device kept alive */
    {
      (void)az_iot_connection_client_do_work(&g_fixture.conn, E2E_PUMP_MS);
    }
    watching = az_iot_e2e_service_telemetry_watch_begin(g_fixture.service);
  }
  if (!watching)
  {
    const char* err = az_iot_e2e_service_last_error(g_fixture.service);
    fprintf(stderr, "[e2e] telemetry watch begin failed: %s\n", (err != NULL) ? err : "unknown");
  }
  assert_true(watching);

  char payload[192];
  int payload_len = snprintf(payload, sizeof(payload), "{\"marker\":\"%s\"}", needle);
  /* snprintf reports what it WOULD have written, so a truncated result would
   * send a length past the end of the buffer. */
  assert_true(payload_len > 0 && (size_t)payload_len < sizeof(payload));

  az_iot_telemetry_message msg = { 0 };
  msg.payload = (const uint8_t*)payload;
  msg.payload_len = (size_t)payload_len;

  g_fixture.send_done = 0;
  assert_int_equal(
      az_iot_mqttv3_telemetry_client_send(&g_fixture.telemetry, &msg, on_send_done, &g_fixture),
      AZ_IOT_OK);

  time_t start = time(NULL);
  bool seen = false;
  while (!seen && (time(NULL) - start) < E2E_TELEMETRY_TIMEOUT_S)
  {
    (void)az_iot_connection_client_do_work(&g_fixture.conn, E2E_PUMP_MS);
    (void)az_iot_e2e_service_do_work(g_fixture.service, E2E_PUMP_MS);
    seen = az_iot_e2e_service_telemetry_seen(g_fixture.service, needle);
  }

  /* The Event Hub can observe the message before the device's own send
   * acknowledgement (PUBACK) has been pumped in, so breaking on `seen` alone
   * races the send. Drain the device briefly until the send completes, the
   * same way e2e_scenarios_test.c does. */
  for (time_t ack = time(NULL); !g_fixture.send_done && (time(NULL) - ack) < 5;)
  {
    (void)az_iot_connection_client_do_work(&g_fixture.conn, E2E_PUMP_MS);
  }

  az_iot_e2e_service_telemetry_watch_end(g_fixture.service);

  assert_true(g_fixture.send_done);
  assert_int_equal(g_fixture.send_status, AZ_IOT_OK);
  assert_true(seen);
}

/* A reconnect re-resolves the key through the token. This is the path that
 * would break if the key reference reached only the first connect site: the
 * device would come back with no client key and the hub would refuse it. */
static void a_reconnect_resolves_the_token_key_again(void** state)
{
  (void)state;
  int loads_before = g_fixture.provider.load_calls;

  az_iot_connection_client_close(&g_fixture.conn);
  for (int i = 0; i < 200 && g_fixture.conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&g_fixture.conn, 50);
  }
  assert_int_equal(g_fixture.conn_state, AZ_IOT_CONN_STATE_IDLE);

  assert_int_equal(az_iot_connection_client_open(&g_fixture.conn), AZ_IOT_OK);
  time_t start = time(NULL);
  while (g_fixture.conn_state != AZ_IOT_CONN_STATE_CONNECTED
         && (time(NULL) - start) < E2E_CONNECT_TIMEOUT_S)
  {
    (void)az_iot_connection_client_do_work(&g_fixture.conn, 50);
    if (g_fixture.conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }
  assert_int_equal(g_fixture.conn_state, AZ_IOT_CONN_STATE_CONNECTED);
  assert_true(g_fixture.provider.load_calls > loads_before);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(dps_provisions_and_the_hub_accepts_a_token_key),
    cmocka_unit_test(telemetry_flows_over_the_token_authenticated_connection),
    cmocka_unit_test(a_reconnect_resolves_the_token_key_again),
  };
  return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
