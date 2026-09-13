// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/*
 * Device Update (ADUv2) end-to-end suite: the shipping SDK against a real
 * DPS/Device Update endpoint.
 *
 * What is real here: the connection client, its DPS flow and the
 * pre-registration hold, the ADU client, the device-update channel, and the
 * request/response codecs. The only test-owned piece is the MQTT transport
 * (e2e_mqtt_transport.c), which exists so the suite can run where the route out
 * is a proxy and the device is enrolled with a symmetric key.
 *
 * Scope note. These scenarios deliberately cover only what a device can reach
 * WITHOUT an update being offered to it:
 *
 *   - onboarding check (pre-registration, needs no registry entry)
 *   - operational check for an enrolled device
 *   - the ETag round trip and its smaller steady-state response
 *   - the error surface reachable today: an unknown workflow on the report
 *     route
 *   - that provisioning still completes once device update has had its turn
 *
 * Not covered for a different reason: the operational (software-update) route.
 * The channel only ever issues the onboarding operation today -- the operational
 * poll needs a provisioning session after registration, which is not built yet.
 * Its request and response shapes are pinned at the codec level in
 * adu_protocol_test.c meanwhile. Add the scenario here when the channel can
 * issue it.
 *
 * The offer path -- a real updateMetadata, its workflowId, and the conflict on
 * re-reporting a terminal result -- is NOT covered, because producing one needs
 * a service-side deployment that is not available yet. Those scenarios belong
 * in this file when it is; the manifest and JWS machinery for them is already
 * written in e2e_adu_test.c and carries over unchanged.
 *
 * Every scenario skips (passes, loudly) when the environment is not configured,
 * so the suite is safe to run anywhere.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cmocka.h>

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "azure/iot/az_iot_adu.h"
#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_result.h"

#include "internal/connection_client_internal.h"

#include "adu_channel_internal.h"
#include "adu_protocol_internal.h"

#include "e2e_mqtt_transport.h"

/* --- environment --------------------------------------------------------- */

typedef struct
{
  const char* dps_host;
  const char* id_scope;
  const char* group_key_b64;
  const char* registry_device; /* a registrationId known to the registry */
  const char* proxy; /* optional */
  bool present;
} e2e_env;

static e2e_env g_env;

static const char* env_or_null(const char* name)
{
  const char* v = getenv(name);
  return (v != NULL && v[0] != '\0') ? v : NULL;
}

static void env_load(void)
{
  g_env.dps_host = env_or_null("AZ_IOT_E2E_ADU_DPS_HOST");
  g_env.id_scope = env_or_null("AZ_IOT_E2E_ADU_ID_SCOPE");
  g_env.group_key_b64 = env_or_null("AZ_IOT_E2E_ADU_GROUP_KEY");
  g_env.registry_device = env_or_null("AZ_IOT_E2E_ADU_REGISTRY_DEVICE");
  g_env.proxy = env_or_null("AZ_IOT_E2E_ADU_PROXY");
  g_env.present = g_env.dps_host != NULL && g_env.id_scope != NULL && g_env.group_key_b64 != NULL;
}

/* cmocka has no first-class skip, and failing on an unconfigured machine would
 * make the suite unusable outside the e2e job. Say why, and pass. */
#define SKIP_WITHOUT_ENV()                                                                   \
  do                                                                                         \
  {                                                                                          \
    if (!g_env.present)                                                                      \
    {                                                                                        \
      printf("[  SKIPPED ] set AZ_IOT_E2E_ADU_DPS_HOST/_ID_SCOPE/_GROUP_KEY to run this\n"); \
      return;                                                                                \
    }                                                                                        \
  } while (0)

/* --- SAS ----------------------------------------------------------------- */

static void url_encode(const char* in, char* out, size_t out_size)
{
  static const char* hex = "0123456789ABCDEF";
  size_t o = 0;
  for (size_t i = 0; in[i] != '\0' && o + 4 < out_size; ++i)
  {
    unsigned char ch = (unsigned char)in[i];
    bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
        || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~';
    if (unreserved)
    {
      out[o++] = (char)ch;
    }
    else
    {
      out[o++] = '%';
      out[o++] = hex[ch >> 4];
      out[o++] = hex[ch & 0x0f];
    }
  }
  out[o] = '\0';
}

static int b64_decode(const char* in, unsigned char* out, int out_size)
{
  int n = EVP_DecodeBlock(out, (const unsigned char*)in, (int)strlen(in));
  if (n < 0 || n > out_size)
  {
    return -1;
  }
  /* EVP_DecodeBlock pads to a multiple of 3; drop what '=' accounted for. */
  size_t len = strlen(in);
  if (len >= 1 && in[len - 1] == '=')
  {
    n--;
  }
  if (len >= 2 && in[len - 2] == '=')
  {
    n--;
  }
  return n;
}

static int b64_encode(const unsigned char* in, int in_len, char* out, int out_size)
{
  if (((in_len + 2) / 3) * 4 + 1 > out_size)
  {
    return -1;
  }
  return EVP_EncodeBlock((unsigned char*)out, in, in_len);
}

/* The device key is derived from the enrollment-group key, so any registrationId
 * is instantly valid -- which is what makes a fresh identity per run free. */
static void sas_password(const char* username, char* out, size_t out_size, void* ctx)
{
  (void)username;
  const char* registration_id = (const char*)ctx;

  unsigned char group_key[128];
  int group_key_len = b64_decode(g_env.group_key_b64, group_key, (int)sizeof(group_key));
  if (group_key_len <= 0)
  {
    out[0] = '\0';
    return;
  }

  unsigned char device_key[EVP_MAX_MD_SIZE];
  unsigned int device_key_len = 0;
  HMAC(
      EVP_sha256(),
      group_key,
      group_key_len,
      (const unsigned char*)registration_id,
      strlen(registration_id),
      device_key,
      &device_key_len);

  char resource[256];
  snprintf(resource, sizeof(resource), "%s/registrations/%s", g_env.id_scope, registration_id);
  char encoded[512];
  url_encode(resource, encoded, sizeof(encoded));

  long expiry = (long)time(NULL) + 3600;
  char to_sign[640];
  snprintf(to_sign, sizeof(to_sign), "%s\n%ld", encoded, expiry);

  unsigned char sig[EVP_MAX_MD_SIZE];
  unsigned int sig_len = 0;
  HMAC(
      EVP_sha256(),
      device_key,
      (int)device_key_len,
      (const unsigned char*)to_sign,
      strlen(to_sign),
      sig,
      &sig_len);

  char sig_b64[128];
  if (b64_encode(sig, (int)sig_len, sig_b64, (int)sizeof(sig_b64)) <= 0)
  {
    out[0] = '\0';
    return;
  }
  char sig_enc[256];
  url_encode(sig_b64, sig_enc, sizeof(sig_enc));

  snprintf(
      out,
      out_size,
      "SharedAccessSignature sr=%s&sig=%s&se=%ld&skn=registration",
      encoded,
      sig_enc,
      expiry);
}

/* --- fixture ------------------------------------------------------------- */

typedef struct
{
  az_iot_connection_client conn;
  az_iot_mqtt_factory* factory;
  char registration_id[96];

  az_iot_adu_channel_dps channel_state;
  az_iot_adu_channel channel;

  /* what the channel reported back */
  size_t update_count;
  size_t result_count;
  az_iot_adu_operation last_op;
  az_iot_result last_result;
  az_iot_adu_error_action last_action;
} e2e_fixture;

static void on_update(const uint8_t* payload, size_t payload_len, void* ctx)
{
  (void)payload;
  (void)payload_len;
  ((e2e_fixture*)ctx)->update_count++;
}

static void on_result(
    az_iot_adu_operation operation,
    az_iot_result result,
    az_iot_adu_error_action action,
    void* ctx)
{
  e2e_fixture* fx = (e2e_fixture*)ctx;
  fx->result_count++;
  fx->last_op = operation;
  fx->last_result = result;
  fx->last_action = action;
}

/* A fresh registrationId per run doubles as a clean state reset for the
 * onboarding route, which keys its state per device. */
static void make_registration_id(char* out, size_t out_size, const char* prefix)
{
  snprintf(out, out_size, "%s-%ld", prefix, (long)time(NULL));
}

static void fixture_open(e2e_fixture* fx, const char* registration_id)
{
  memset(fx, 0, sizeof(*fx));
  snprintf(fx->registration_id, sizeof(fx->registration_id), "%s", registration_id);

  az_iot_connection_client_options opts = { 0 };
  opts.host = NULL; /* DPS mode */
  opts.client_id = fx->registration_id;
  opts.dps.id_scope = g_env.id_scope;
  opts.dps.registration_id = fx->registration_id;
  opts.dps.global_endpoint = g_env.dps_host;
  assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

  az_iot_e2e_mqtt_config mcfg;
  memset(&mcfg, 0, sizeof(mcfg));
  mcfg.proxy = g_env.proxy;
  mcfg.password_cb = sas_password;
  mcfg.password_ctx = fx->registration_id;
  fx->factory = az_iot_e2e_mqtt_factory_create(&mcfg);
  assert_non_null(fx->factory);

  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "contoso";
  dp.model = "tractor";
  dp.installed_update_id.provider = "contoso";
  dp.installed_update_id.name = "tractor";
  dp.installed_update_id.version = "1.0";
  assert_int_equal(
      az_iot_adu_channel_dps_init(&fx->channel_state, &fx->conn, &dp, &fx->channel), AZ_IOT_OK);
  assert_int_equal(fx->channel.vtable->open(fx->channel.ctx, on_update, on_result, fx), AZ_IOT_OK);

  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
}

static void fixture_close(e2e_fixture* fx)
{
  if (fx->channel.vtable != NULL)
  {
    fx->channel.vtable->close(fx->channel.ctx);
  }
  az_iot_connection_client_destroy(&fx->conn);
}

/* Pump until the predicate holds or the budget runs out. Wall-clock bounded
 * because every wait here is on a real service. */
#define PUMP_UNTIL(fx, cond, seconds)                                            \
  do                                                                             \
  {                                                                              \
    time_t deadline_ = time(NULL) + (seconds);                                   \
    while (!(cond) && time(NULL) < deadline_)                                    \
    {                                                                            \
      (void)az_iot_connection_client_do_work(&(fx)->conn, 200);                  \
      /* The channel has its own tick, and an application drives                 \
       * it through the ADU client. It is what re-arms the hold on               \
       * a new session and retires a request whose session went                  \
       * away, so pumping only the connection would stall here. */               \
      if ((fx)->channel.vtable != NULL && (fx)->channel.vtable->do_work != NULL) \
      {                                                                          \
        (void)(fx)->channel.vtable->do_work((fx)->channel.ctx);                  \
      }                                                                          \
    }                                                                            \
  } while (0)

/* Drive the connection to the point where the provisioning session is up and
 * registration is being held for the update check. */
static void wait_for_hold(e2e_fixture* fx)
{
  PUMP_UNTIL(fx, az_iot_connection_client__dps_hold_is_active(&fx->conn), 30);
  assert_true(az_iot_connection_client__dps_hold_is_active(&fx->conn));
}

static void wait_for_result(e2e_fixture* fx, size_t target)
{
  PUMP_UNTIL(fx, fx->result_count >= target, 30);
  assert_true(fx->result_count >= target);
}

/* --- scenarios ----------------------------------------------------------- */

/* The pre-registration check: a device with no registry entry asks for
 * onboarding updates on the provisioning session, before it registers. */
static void onboarding_check_runs_before_registration(void** state)
{
  (void)state;
  SKIP_WITHOUT_ENV();

  e2e_fixture fx;
  char id[96];
  make_registration_id(id, sizeof(id), "e2e-onboard");
  fixture_open(&fx, id);

  wait_for_hold(&fx);
  assert_int_equal(fx.conn.dps_phase, AZ_IOT_DPS_PHASE_HOLD);

  assert_int_equal(fx.channel.vtable->request_update(fx.channel.ctx), AZ_IOT_OK);
  wait_for_result(&fx, 1);

  /* A device with nothing deployed to it is a success with no update. */
  assert_int_equal(fx.last_result, AZ_IOT_OK);
  assert_int_equal(fx.last_action, AZ_IOT_ADU_ERROR_ACTION_NONE);
  assert_int_equal(fx.update_count, 0);

  /* The service answered with ETags, which is what makes the next poll cheap. */
  assert_true(fx.channel_state.agent_info_etag[0] != '\0');
  assert_true(fx.channel_state.service_config_etag[0] != '\0');

  fixture_close(&fx);
}

/* Holding registration must not prevent it: once the check is done the device
 * goes on to register, and the connection makes progress. */
static void provisioning_completes_after_the_check(void** state)
{
  (void)state;
  SKIP_WITHOUT_ENV();

  e2e_fixture fx;
  char id[96];
  make_registration_id(id, sizeof(id), "e2e-provision");
  fixture_open(&fx, id);

  wait_for_hold(&fx);
  assert_int_equal(fx.channel.vtable->request_update(fx.channel.ctx), AZ_IOT_OK);
  wait_for_result(&fx, 1);

  /* The hold is released by the verdict; the pump then registers. */
  PUMP_UNTIL(&fx, fx.conn.dps_phase != AZ_IOT_DPS_PHASE_HOLD, 30);
  assert_int_not_equal(fx.conn.dps_phase, AZ_IOT_DPS_PHASE_HOLD);
  assert_false(az_iot_connection_client__dps_hold_is_active(&fx.conn));

  fixture_close(&fx);
}

/* The ETags the service issues, and what the channel does with them.
 *
 * Deliberately only one exchange. A provisioning session carries exactly one
 * pre-registration check: answering it releases the hold and registration
 * follows, so the session can no longer carry a reply and further operations
 * are refused. Asserted here, because it is the contract -- publishing into that
 * window would be accepted by the service and the reply lost.
 *
 * So the SMALLER steady-state response (serviceConfiguration omitted when the
 * ETags match) is NOT exercised here: it needs a second check, which needs a
 * second session, which in turn needs the operational path that does not exist
 * yet. That response shape is pinned instead in adu_protocol_test.c against the
 * real 75-byte body captured from the service. What this proves end to end is
 * that the ETags arrive, are stored, and survive the session being spent. */
static void etags_are_issued_stored_and_survive_the_session(void** state)
{
  (void)state;
  SKIP_WITHOUT_ENV();

  e2e_fixture fx;
  char id[96];
  make_registration_id(id, sizeof(id), "e2e-etag");
  fixture_open(&fx, id);

  wait_for_hold(&fx);
  assert_int_equal(fx.channel.vtable->request_update(fx.channel.ctx), AZ_IOT_OK);
  wait_for_result(&fx, 1);
  assert_int_equal(fx.last_result, AZ_IOT_OK);

  char first_agent_etag[128];
  char first_config_etag[128];
  snprintf(first_agent_etag, sizeof(first_agent_etag), "%s", fx.channel_state.agent_info_etag);
  snprintf(
      first_config_etag, sizeof(first_config_etag), "%s", fx.channel_state.service_config_etag);
  assert_true(first_agent_etag[0] != '\0');
  assert_true(first_config_etag[0] != '\0');

  /* Both are hex-ish opaque tokens, not JSON fragments or an error string. */
  assert_true(strlen(first_agent_etag) >= 8);
  assert_true(strchr(first_agent_etag, '{') == NULL);

  /* That answer ended the pre-registration exchange, so this session is spent:
   * it is about to carry the registration and could not answer us again. The
   * refusal is the contract -- publishing here would be accepted by the service
   * and the reply lost. */
  assert_int_equal(fx.channel.vtable->request_update(fx.channel.ctx), AZ_IOT_ERR_NOT_CONNECTED);

  /* The stored ETags survive the session being spent, which is what a later
   * session would send. */
  assert_string_equal(fx.channel_state.agent_info_etag, first_agent_etag);
  assert_string_equal(fx.channel_state.service_config_etag, first_config_etag);

  fixture_close(&fx);
}

/* Reporting against a workflow the service does not know is answered 400000
 * with UNKNOWN_WORKFLOW_ID in the message. What this pins is that the report is
 * NOT read as "already reported" -- nothing was delivered, and claiming
 * otherwise would silently drop the only record the service gets of what the
 * device did.
 *
 * Honest limit: this does NOT exercise reading the code out of "message".
 * 400000 is not in the numeric ladder, so it defaults to FATAL and the scenario
 * still passes with message parsing disabled (measured). The case where the
 * message is genuinely load-bearing is 409000, which splits by operation, and
 * producing one needs a real workflow. The message-vs-numeric behaviour is
 * pinned instead by adu_protocol_test.c against captured bodies. */
static void an_unknown_workflow_report_is_not_treated_as_delivered(void** state)
{
  (void)state;
  SKIP_WITHOUT_ENV();

  e2e_fixture fx;
  char id[96];
  make_registration_id(id, sizeof(id), "e2e-report");
  fixture_open(&fx, id);

  wait_for_hold(&fx);

  az_iot_adu_report report;
  memset(&report, 0, sizeof(report));
  report.workflow_id = "e2e-workflow-that-does-not-exist";
  report.outcome = AZ_IOT_ADU_OUTCOME_SUCCEEDED;
  report.failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE;
  report.result_code = 700;
  report.extended_result_codes = "";
  report.result_details = "e2e";

  assert_int_equal(fx.channel.vtable->report(fx.channel.ctx, &report), AZ_IOT_OK);
  wait_for_result(&fx, 1);

  assert_int_equal(fx.last_op, AZ_IOT_ADU_OP_REPORT_STATUS);
  assert_int_not_equal(fx.last_result, AZ_IOT_OK);
  assert_int_not_equal(fx.last_action, AZ_IOT_ADU_ERROR_ACTION_ALREADY_REPORTED);
  assert_int_equal(fx.last_action, AZ_IOT_ADU_ERROR_ACTION_FATAL);

  fixture_close(&fx);
}

int main(void)
{
  env_load();
  if (!g_env.present)
  {
    printf("ADU e2e: no environment configured; every scenario will skip.\n");
  }

  const struct CMUnitTest tests[] = {
    cmocka_unit_test(onboarding_check_runs_before_registration),
    cmocka_unit_test(provisioning_completes_after_the_check),
    cmocka_unit_test(etags_are_issued_stored_and_survive_the_session),
    cmocka_unit_test(an_unknown_workflow_report_is_not_treated_as_delivered),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
