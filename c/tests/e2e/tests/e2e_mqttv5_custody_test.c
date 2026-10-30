// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* End-to-end custody on an mqttv5 hub: a device whose private key is inside a PKCS#11 token
 * (design decision D8) provisions through DPS, connects and sends telemetry with
 * az_iot_mqttv5_telemetry_client. The mqttv3 counterpart is e2e_custody_test.c.
 *
 * Nothing is stubbed: if the token were not signing the TLS handshake, the hub would refuse the
 * connection. Telemetry is read back from the hub's events endpoint (az_iot_e2e_service.h).
 *
 * The token is provisioned by eng/setup-softhsm.sh, which imports the device key and prints:
 *   AZ_IOT_CLIENT_KEY_URI    pkcs11: URI of the device private key
 *   AZ_IOT_CRYPTO_ENGINE_ID  OpenSSL provider id (default "pkcs11")
 * in addition to AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID, AZ_IOT_CLIENT_CERT,
 * AZ_IOT_TRUSTED_CA and the optional AZ_IOT_DPS_GLOBAL_ENDPOINT. Built only with
 * AZ_IOT_BUILD_E2E_PKCS11; a missing variable fails the suite.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The MQTT adapter this build of the suite uses (see tests/e2e/CMakeLists.txt). */
#if defined(AZ_IOT_E2E_ADAPTER_PAHO)
#include "azure/iot/adapters/az_iot_adapter_paho.h"
#elif defined(AZ_IOT_E2E_ADAPTER_AZ_MQTT)
#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#else
#error "Define AZ_IOT_E2E_ADAPTER_PAHO or AZ_IOT_E2E_ADAPTER_AZ_MQTT"
#endif
#include "azure/iot/az_iot.h"

#include "az_iot_e2e_service.h"
#include "e2e_defensive_timeout.h"
#include "e2e_log.h"

#define E2E_CONNECT_TIMEOUT_S 90
#define E2E_SERVICE_CONNECT_TIMEOUT_S 45
#define E2E_ROUTE_TIMEOUT_S 90
#define E2E_ACK_TIMEOUT_S 30
#define E2E_PUMP_MS 20
/* tag, '-', device id (<= 128), '-', 11 hex digits, NUL. */
#define E2E_MARKER_MAX 160

/** @brief Device configuration from the environment (heap-owned). */
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

/** @brief Certificate provider naming the token key instead of handing it over. */
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
  az_iot_mqtt_factory* factory_v3;
  az_iot_mqtt_factory* factory_v5;
  bool conn_ok;
  az_iot_connection_state conn_state;
  az_iot_result conn_reason;
  /** Hub-assigned device id; DPS may assign one other than the registration id. */
  char device_id[129];
  az_iot_e2e_service* service;
  az_iot_mqttv5_telemetry_client telemetry;
  bool telemetry_ok;
  int send_done;
  az_iot_result send_status;
} fixture;

static fixture g_fx;

static char* env_dup(const char* name)
{
  const char* value = getenv(name);
  if (value == NULL || value[0] == '\0')
  {
    return NULL;
  }
  const size_t n = strlen(value) + 1;
  char* copy = (char*)malloc(n);
  if (copy != NULL)
  {
    memcpy(copy, value, n);
  }
  return copy;
}

static az_iot_result custody_load(
    az_iot_certificate_provider* self,
    az_iot_cert_role role,
    uint8_t index,
    az_iot_certificate_material* out)
{
  (void)role;
  if (index != 0)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  custody_provider* p = (custody_provider*)self;
  p->load_calls++;
  memset(out, 0, sizeof(*out));
  out->client_cert_path = p->config->cert;
  out->trusted_ca_path = p->config->ca;
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

static void on_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }
  if (event->state != fx->conn_state)
  {
    printf(
        "[e2e] hub state %d -> %d, reason %s\n",
        (int)fx->conn_state,
        (int)event->state,
        az_iot_result_to_string(event->reason));
  }
  fx->conn_state = event->state;
  fx->conn_reason = event->reason;
}

static void on_send_done(az_iot_result status, void* user_ctx)
{
  fixture* fx = (fixture*)user_ctx;
  fx->send_status = status;
  fx->send_done = 1;
}

static void pump(fixture* fx) { (void)az_iot_connection_client_do_work(&fx->conn, E2E_PUMP_MS); }

/** @brief Pumps the device and the service once. */
static void pump_both(fixture* fx)
{
  pump(fx);
  if (!az_iot_e2e_service_do_work(fx->service, E2E_PUMP_MS))
  {
    const char* err = az_iot_e2e_service_last_error(fx->service);
    fail_msg("service pump failed: %s", err ? err : "unknown");
  }
}

/** @brief Pumps until the hub state is CONNECTED or FAULTED, or the deadline passes. */
static bool wait_connected(fixture* fx)
{
  for (time_t start = time(NULL); fx->conn_state != AZ_IOT_CONN_STATE_CONNECTED
       && fx->conn_state != AZ_IOT_CONN_STATE_FAULTED
       && (time(NULL) - start) < E2E_CONNECT_TIMEOUT_S;)
  {
    pump(fx);
  }
  if (fx->conn_state != AZ_IOT_CONN_STATE_CONNECTED)
  {
    fprintf(
        stderr,
        "[e2e] device did not reach CONNECTED: state %d, reason %s\n",
        (int)fx->conn_state,
        az_iot_result_to_string(fx->conn_reason));
    return false;
  }
  return true;
}

/* Unique per run: the events endpoint is shared by concurrent runs. */
static void make_marker(char* out, size_t cap, const char* tag, const char* device_id)
{
  static unsigned counter = 0;
  snprintf(
      out, cap, "%s-%s-%08lx%03x", tag, device_id, (unsigned long)time(NULL), (counter++) & 0xfffu);
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
    const char k_default[] = "pkcs11";
    c->engine_id = (char*)malloc(sizeof(k_default));
    if (c->engine_id != NULL)
    {
      memcpy(c->engine_id, k_default, sizeof(k_default));
    }
  }
  if (c->id_scope == NULL || c->reg_id == NULL || c->cert == NULL || c->ca == NULL
      || c->key_uri == NULL || c->engine_id == NULL)
  {
    fprintf(
        stderr,
        "[e2e] missing AZ_IOT_DPS_ID_SCOPE, AZ_IOT_DPS_REGISTRATION_ID, AZ_IOT_CLIENT_CERT, "
        "AZ_IOT_TRUSTED_CA or AZ_IOT_CLIENT_KEY_URI. Built with AZ_IOT_BUILD_E2E_PKCS11, so a "
        "PKCS#11 token is required (eng/setup-softhsm.sh).\n");
    return -1;
  }
  return 0;
}

/** @brief Provisions through DPS and waits for CONNECTED, with only the token key. */
static int device_connect(fixture* fx)
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
    fprintf(stderr, "[e2e] connection client init failed\n");
    return -1;
  }
  fx->conn_ok = true;
  if (e2e_use_short_defensive_timeouts(&fx->conn) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] defensive-timeout schedule rejected\n");
    return -1;
  }
  if (az_iot_connection_client_add_state_observer(&fx->conn, on_state, fx) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] state observer registration failed\n");
    return -1;
  }

  /* Both generations, so an mqttv3 assignment still connects and the first test names it. */
#if defined(AZ_IOT_E2E_ADAPTER_PAHO)
  fx->factory_v3 = az_iot_paho_factory_create_v3_1_1();
  fx->factory_v5 = az_iot_paho_factory_create_v5();
#elif defined(AZ_IOT_E2E_ADAPTER_AZ_MQTT)
  fx->factory_v3 = az_iot_az_mqtt_factory_create_v3_1_1();
  fx->factory_v5 = az_iot_az_mqtt_factory_create_v5();
#endif
  if (az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory_v3) != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory_v5) != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] MQTT factory registration failed\n");
    return -1;
  }

  const az_iot_result open_rc = az_iot_connection_client_open(&fx->conn);
  if (open_rc != AZ_IOT_OK)
  {
    fprintf(stderr, "[e2e] open failed: %s\n", az_iot_result_to_string(open_rc));
    return -1;
  }
  return wait_connected(fx) ? 0 : -1;
}

static int group_setup(void** state)
{
  memset(&g_fx, 0, sizeof(g_fx));
  *state = &g_fx;
  if (config_load(&g_fx.config) != 0)
  {
    return -1;
  }
  e2e_install_log_sink();
  /* Only the URI path is printed: its query may carry the token PIN (pin-value). */
  const char* query = strchr(g_fx.config.key_uri, '?');
  printf(
      "[e2e] registration '%s': key stays in the token (%.*s via '%s')\n",
      g_fx.config.reg_id,
      (int)(query != NULL ? (size_t)(query - g_fx.config.key_uri) : strlen(g_fx.config.key_uri)),
      g_fx.config.key_uri,
      g_fx.config.engine_id);

  const char* svc_err = NULL;
  g_fx.service = az_iot_e2e_service_create(&svc_err);
  if (g_fx.service == NULL)
  {
    fprintf(stderr, "[e2e] service create failed: %s\n", svc_err ? svc_err : "unknown");
    return -1;
  }
  if (device_connect(&g_fx) != 0)
  {
    return -1;
  }
  const char* assigned = az_iot_connection_client_get_device_id(&g_fx.conn);
  const size_t assigned_len = assigned != NULL ? strlen(assigned) : 0;
  if (assigned_len == 0 || assigned_len >= sizeof(g_fx.device_id))
  {
    fprintf(stderr, "[e2e] no usable hub device id\n");
    return -1;
  }
  memcpy(g_fx.device_id, assigned, assigned_len + 1);
  printf("[e2e] hub device id '%s'\n", g_fx.device_id);
  return 0;
}

static int group_teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx == NULL)
  {
    fx = &g_fx;
  }
  if (fx->telemetry_ok)
  {
    az_iot_mqttv5_telemetry_client_deinit(&fx->telemetry);
    fx->telemetry_ok = false;
  }
  if (fx->conn_ok)
  {
    az_iot_connection_client_close(&fx->conn);
    for (int i = 0; i < 100 && fx->conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
    {
      (void)az_iot_connection_client_do_work(&fx->conn, 50);
    }
    (void)az_iot_connection_client_remove_state_observer(&fx->conn, on_state, fx);
    az_iot_connection_client_deinit(&fx->conn);
    fx->conn_ok = false;
  }
#if defined(AZ_IOT_E2E_ADAPTER_PAHO)
  az_iot_paho_factory_destroy(fx->factory_v3);
  az_iot_paho_factory_destroy(fx->factory_v5);
#elif defined(AZ_IOT_E2E_ADAPTER_AZ_MQTT)
  az_iot_az_mqtt_factory_destroy(fx->factory_v3);
  az_iot_az_mqtt_factory_destroy(fx->factory_v5);
#endif
  if (fx->service != NULL)
  {
    az_iot_e2e_service_destroy(fx->service);
  }
  free(fx->config.id_scope);
  free(fx->config.reg_id);
  free(fx->config.cert);
  free(fx->config.ca);
  free(fx->config.key_uri);
  free(fx->config.engine_id);
  free(fx->config.global_endpoint);
  memset(fx, 0, sizeof(*fx));
  return 0;
}

/* DPS provisioned and the mqttv5 hub accepted the handshake signed by the token: reaching
 * CONNECTED is the assertion. Both the DPS and the hub connect loaded the key reference. */
static void dps_provisions_and_the_mqttv5_hub_accepts_a_token_key(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(fx->conn_state, AZ_IOT_CONN_STATE_CONNECTED);

  az_iot_hub_profile profile;
  memset(&profile, 0, sizeof(profile));
  const az_iot_result rc = az_iot_connection_client_get_hub_profile(&fx->conn, &profile);
  printf(
      "[e2e] hub profile: %s, %d; provider loads %d\n",
      az_iot_result_to_string(rc),
      (int)profile.connection_profile,
      fx->provider.load_calls);
  assert_int_equal(rc, AZ_IOT_OK);
  assert_int_equal(profile.connection_profile, AZ_IOT_CONNECTION_PROFILE_MQTT_V5);
  assert_true(fx->provider.load_calls >= 2);
}

/* Telemetry sent with az_iot_mqttv5_telemetry_client over the token-authenticated connection
 * is acknowledged and seen on the events endpoint. */
static void telemetry_flows_over_the_token_authenticated_connection(void** state)
{
  fixture* fx = (fixture*)*state;
  if (!fx->telemetry_ok)
  {
    const az_iot_result rc = az_iot_mqttv5_telemetry_client_init(&fx->telemetry, &fx->conn);
    if (rc != AZ_IOT_OK)
    {
      fail_msg("mqttv5 telemetry client init returned %s", az_iot_result_to_string(rc));
    }
    fx->telemetry_ok = true;
  }

  /* The cold AMQP open to the events endpoint can fail transiently; retry, device kept alive. */
  az_iot_e2e_service_telemetry_watch_end(fx->service);
  bool watching = az_iot_e2e_service_telemetry_watch_begin(fx->service);
  for (time_t start = time(NULL);
       !watching && (time(NULL) - start) < E2E_SERVICE_CONNECT_TIMEOUT_S;)
  {
    for (int i = 0; i < 50; ++i)
    {
      pump(fx);
    }
    watching = az_iot_e2e_service_telemetry_watch_begin(fx->service);
  }
  if (!watching)
  {
    const char* err = az_iot_e2e_service_last_error(fx->service);
    fprintf(stderr, "[e2e] telemetry watch failed: %s\n", err ? err : "unknown");
  }
  assert_true(watching);

  char marker[E2E_MARKER_MAX];
  make_marker(marker, sizeof(marker), "custody", fx->device_id);
  /* static: the callback may complete after the test returns. */
  static char payload[E2E_MARKER_MAX + 16];
  const int payload_len = snprintf(payload, sizeof(payload), "{\"marker\":\"%s\"}", marker);
  assert_true(payload_len > 0 && (size_t)payload_len < sizeof(payload));

  az_iot_telemetry_message msg = { 0 };
  msg.payload = (const uint8_t*)payload;
  msg.payload_len = (size_t)payload_len;
  fx->send_done = 0;
  const az_iot_result send_rc
      = az_iot_mqttv5_telemetry_client_send(&fx->telemetry, &msg, on_send_done, fx);
  if (send_rc != AZ_IOT_OK)
  {
    fail_msg("send returned %s", az_iot_result_to_string(send_rc));
  }

  bool seen = false;
  for (time_t start = time(NULL); !seen && (time(NULL) - start) < E2E_ROUTE_TIMEOUT_S;)
  {
    pump_both(fx);
    seen = az_iot_e2e_service_telemetry_seen(fx->service, marker);
  }
  /* The events endpoint can see the message before the PUBACK is pumped in. */
  for (time_t start = time(NULL); !fx->send_done && (time(NULL) - start) < E2E_ACK_TIMEOUT_S;)
  {
    pump_both(fx);
  }
  printf(
      "[e2e] '%s': completion %s, routed %s\n",
      marker,
      fx->send_done ? az_iot_result_to_string(fx->send_status) : "(none)",
      seen ? "yes" : "no");
  if (!seen)
  {
    az_iot_e2e_service_telemetry_dump(fx->service, marker);
  }
  az_iot_e2e_service_telemetry_watch_end(fx->service);

  assert_true(fx->send_done);
  assert_int_equal(fx->send_status, AZ_IOT_OK);
  assert_true(seen);
}

/* Close and reopen: the reconnect loads the key reference again and the mqttv5 hub accepts it,
 * which breaks if the reference reaches only the first connect site. */
static void a_reconnect_resolves_the_token_key_again(void** state)
{
  fixture* fx = (fixture*)*state;
  const int loads_before = fx->provider.load_calls;

  az_iot_connection_client_close(&fx->conn);
  for (int i = 0; i < 200 && fx->conn_state != AZ_IOT_CONN_STATE_IDLE; ++i)
  {
    (void)az_iot_connection_client_do_work(&fx->conn, 50);
  }
  assert_int_equal(fx->conn_state, AZ_IOT_CONN_STATE_IDLE);

  const az_iot_result open_rc = az_iot_connection_client_open(&fx->conn);
  if (open_rc != AZ_IOT_OK)
  {
    fail_msg("reopen returned %s", az_iot_result_to_string(open_rc));
  }
  assert_true(wait_connected(fx));

  az_iot_hub_profile profile;
  memset(&profile, 0, sizeof(profile));
  const az_iot_result rc = az_iot_connection_client_get_hub_profile(&fx->conn, &profile);
  printf(
      "[e2e] after reconnect: profile %s, %d; provider loads %d -> %d\n",
      az_iot_result_to_string(rc),
      (int)profile.connection_profile,
      loads_before,
      fx->provider.load_calls);
  assert_int_equal(rc, AZ_IOT_OK);
  assert_int_equal(profile.connection_profile, AZ_IOT_CONNECTION_PROFILE_MQTT_V5);
  assert_true(fx->provider.load_calls > loads_before);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(dps_provisions_and_the_mqttv5_hub_accepts_a_token_key),
    cmocka_unit_test(telemetry_flows_over_the_token_authenticated_connection),
    cmocka_unit_test(a_reconnect_resolves_the_token_key_again),
  };
  return cmocka_run_group_tests(tests, group_setup, group_teardown);
}
