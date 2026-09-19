// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Custom DPS registration payload: the `payload` member the device sends with
 * its registration request (opts.dps.registration_payload), how it composes
 * with CSR enrollment into ONE registration body, and the payload the service
 * returns in the assignment (registrationState.payload).
 *
 * Every body assertion is on the exact bytes of the register PUBLISH, because
 * the whole contract here is what goes on the wire. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_certificate_provider.h"
#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "support/connection_test_harness.h"

#define DPS_RESPONSE_TOPIC_ASSIGNED "$dps/registrations/res/200/?$rid=1"

#define REGISTRATION_PAYLOAD "{\"modelId\":\"dtmi:com:example:Thermostat;1\"}"
#define CSR_BASE64 "TESTCSRBASE64=="

static const char k_payload_json[] = REGISTRATION_PAYLOAD;

static az_span payload_span(void)
{
  return az_span_create((uint8_t*)(uintptr_t)k_payload_json, (int32_t)(sizeof(k_payload_json) - 1));
}

static az_span span_from_literal(const char* s)
{
  return az_span_create((uint8_t*)(uintptr_t)s, (int32_t)strlen(s));
}

/* ------------------------------------------------------------------------- */
/* a certificate provider that yields a fixed CSR                            */
/* ------------------------------------------------------------------------- */

typedef struct csr_provider
{
  az_iot_certificate_provider base;
  int get_csr_calls;
  int release_csr_calls;
} csr_provider;

static az_iot_result csr_provider_load(
    az_iot_certificate_provider* s,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  (void)s;
  (void)role;
  memset(out, 0, sizeof(*out));
  out->client_cert_pem = "cert";
  out->client_key_pem = "key";
  return AZ_IOT_OK;
}

static void csr_provider_release(az_iot_certificate_provider* s, az_iot_certificate_material* m)
{
  (void)s;
  (void)m;
}

static void csr_provider_deinit(az_iot_certificate_provider* s) { (void)s; }

static az_iot_result csr_provider_get_csr(
    az_iot_certificate_provider* s,
    const char* cn,
    az_iot_certificate_signing_request* out)
{
  csr_provider* p = (csr_provider*)s;
  (void)cn;
  p->get_csr_calls++;
  out->csr_base64 = CSR_BASE64;
  return AZ_IOT_OK;
}

static void csr_provider_release_csr(
    az_iot_certificate_provider* s,
    az_iot_certificate_signing_request* csr)
{
  csr_provider* p = (csr_provider*)s;
  (void)csr;
  p->release_csr_calls++;
}

static az_iot_result csr_provider_store(
    az_iot_certificate_provider* s,
    const az_iot_issued_certificate* issued)
{
  (void)s;
  (void)issued;
  return AZ_IOT_OK;
}

static const az_iot_certificate_provider_vtable k_csr_vtable = {
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = csr_provider_load,
  .release = csr_provider_release,
  .deinit = csr_provider_deinit,
  .get_csr = csr_provider_get_csr,
  .release_csr = csr_provider_release_csr,
  .store_issued_certificate = csr_provider_store,
};

/* ------------------------------------------------------------------------- */
/* fixture                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct payload_fixture
{
  az_iot_test_conn conn;
  csr_provider provider;
  uint8_t csr_buffer[AZ_IOT_CSR_PAYLOAD_BUFFER_MIN];
  AZ_IOT_DPS_REGISTRATION_BODY_STORAGE(body_buffer);
  uint8_t tiny_buffer[8];
} payload_fixture;

static payload_fixture* fixture_create(void)
{
  payload_fixture* fx = (payload_fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);
  fx->provider.base.vtable = &k_csr_vtable;
  return fx;
}

static az_iot_connection_client_options fixture_options(payload_fixture* fx)
{
  az_iot_connection_client_options opts = { 0 };
  opts.host = NULL; /* DPS mode */
  opts.client_id = "ut-device";
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-device";
  opts.certificate_provider = &fx->provider.base;
  return opts;
}

/* init + register the mock factory + wire the state log. */
static void fixture_init(payload_fixture* fx, const az_iot_connection_client_options* opts)
{
  assert_int_equal(az_iot_connection_client_init(&fx->conn.client_storage, opts), AZ_IOT_OK);
  fx->conn.client = &fx->conn.client_storage;
  assert_int_equal(
      az_iot_connection_client_set_state_callback(
          fx->conn.client, az_iot_test_on_state, &fx->conn.log),
      AZ_IOT_OK);
  fx->conn.factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->conn.factory);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->conn.client, fx->conn.factory), AZ_IOT_OK);
}

static void fixture_destroy(payload_fixture* fx)
{
  if (!fx)
  {
    return;
  }
  if (fx->conn.client)
  {
    bool adopted = (fx->conn.client->factory_count > 0);
    az_iot_connection_client_destroy(&fx->conn.client_storage);
    if (!adopted && fx->conn.factory)
    {
      az_iot_mock_mqtt_factory_destroy(fx->conn.factory);
    }
  }
  else if (fx->conn.factory)
  {
    az_iot_mock_mqtt_factory_destroy(fx->conn.factory);
  }
  free(fx);
}

/* Drive open -> CONNACK -> SUBACK, so the register PUBLISH has been attempted.
 *
 * The returned mock is valid ONLY while the provisioning session survives that
 * attempt. A register PUBLISH the SDK refuses to build (a body buffer too
 * small) faults the client, which tears the DPS session down and FREES this
 * mock -- so a failure-path test must not touch the returned pointer. Use
 * drive_to_failed_register() there instead. */
static az_iot_mock_mqtt_client* drive_to_register(payload_fixture* fx)
{
  assert_int_equal(az_iot_connection_client_open(fx->conn.client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->conn.factory);
  assert_non_null(m);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->conn.client, 0);

  const az_iot_mock_call* sub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->conn.client, 0);
  return m;
}

/* Same drive, for the case where building the body is expected to fail.
 * Returns nothing: the provisioning session -- and with it the mock -- is gone
 * by the time this returns. What the SDK owes the caller here is observable on
 * the client, not on the adapter: it faulted with AZ_IOT_ERR_NOT_ENOUGH_SPACE
 * and tore the session down, having returned before it ever set a payload on
 * the message. That nothing truncated reaches the wire is asserted positively
 * by the exact-bytes tests above. */
static void drive_to_failed_register(payload_fixture* fx)
{
  (void)drive_to_register(fx);

  assert_int_equal(az_iot_test_last_state(&fx->conn.log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_test_reason_for(&fx->conn.log, AZ_IOT_CONN_STATE_FAULTED),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  /* The session really is gone: the mock detaches itself from the factory when
   * it is destroyed, so this also guards the use-after-free above. */
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->conn.factory));
}

/* Assert the register PUBLISH carried exactly `expected`. */
static void assert_register_body(az_iot_mock_mqtt_client* m, const char* expected)
{
  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_int_equal(pub->payload_len, strlen(expected));
  assert_memory_equal(pub->payload, expected, strlen(expected));
}

/* ------------------------------------------------------------------------- */
/* request direction: the registration body                                  */
/* ------------------------------------------------------------------------- */

/* A zero-initialized options struct must behave exactly as it always has: an
 * ordinary registration carries no body at all. */
static void no_payload_and_no_csr_sends_an_empty_body(void** state)
{
  (void)state;
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);
  fixture_init(fx, &opts);

  az_iot_mock_mqtt_client* m = drive_to_register(fx);
  const az_iot_mock_call* pub = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(pub);
  assert_int_equal(pub->payload_len, 0);

  fixture_destroy(fx);
}

static void a_payload_alone_is_wrapped_in_the_registration_body(void** state)
{
  (void)state;
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);
  opts.dps.registration_payload = payload_span();
  opts.dps.registration_body_buffer = az_span_create(fx->body_buffer, sizeof(fx->body_buffer));
  fixture_init(fx, &opts);

  az_iot_mock_mqtt_client* m = drive_to_register(fx);
  assert_register_body(m, "{\"payload\":" REGISTRATION_PAYLOAD "}");
  /* No CSR was requested, so the provider was never asked for one. */
  assert_int_equal(fx->provider.get_csr_calls, 0);

  fixture_destroy(fx);
}

/* The pre-existing CSR body is byte-for-byte unchanged. */
static void a_csr_alone_still_sends_the_csr_body(void** state)
{
  (void)state;
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);
  opts.dps.request_operational_certificate = true;
  opts.csr_payload_buffer = az_span_create(fx->csr_buffer, sizeof(fx->csr_buffer));
  fixture_init(fx, &opts);

  az_iot_mock_mqtt_client* m = drive_to_register(fx);
  assert_register_body(m, "{\"csr\":\"" CSR_BASE64 "\"}");
  assert_int_equal(fx->provider.get_csr_calls, 1);
  assert_int_equal(fx->provider.release_csr_calls, 1);

  fixture_destroy(fx);
}

/* The crux: one well-formed object carrying both members, never two bodies and
 * never a nested one. */
static void a_csr_and_a_payload_share_one_registration_body(void** state)
{
  (void)state;
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);
  opts.dps.request_operational_certificate = true;
  opts.csr_payload_buffer = az_span_create(fx->csr_buffer, sizeof(fx->csr_buffer));
  opts.dps.registration_payload = payload_span();
  opts.dps.registration_body_buffer = az_span_create(fx->body_buffer, sizeof(fx->body_buffer));
  fixture_init(fx, &opts);

  az_iot_mock_mqtt_client* m = drive_to_register(fx);
  assert_register_body(m, "{\"csr\":\"" CSR_BASE64 "\",\"payload\":" REGISTRATION_PAYLOAD "}");
  assert_int_equal(fx->provider.get_csr_calls, 1);
  assert_int_equal(fx->provider.release_csr_calls, 1);

  fixture_destroy(fx);
}

/* Without a dedicated body buffer the combined body is built in the CSR buffer
 * the caller already provides, so an existing CSR device only has to enlarge
 * the buffer it already has. */
static void the_csr_buffer_builds_the_combined_body_when_no_body_buffer_is_set(void** state)
{
  (void)state;
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);
  opts.dps.request_operational_certificate = true;
  opts.csr_payload_buffer = az_span_create(fx->csr_buffer, sizeof(fx->csr_buffer));
  opts.dps.registration_payload = payload_span();
  fixture_init(fx, &opts);

  az_iot_mock_mqtt_client* m = drive_to_register(fx);
  assert_register_body(m, "{\"csr\":\"" CSR_BASE64 "\",\"payload\":" REGISTRATION_PAYLOAD "}");

  fixture_destroy(fx);
}

/* A body that does not fit must fail the provisioning attempt, not publish a
 * truncated one. */
static void a_body_buffer_too_small_fails_instead_of_truncating(void** state)
{
  (void)state;
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);
  opts.dps.registration_payload = payload_span();
  opts.dps.registration_body_buffer = az_span_create(fx->tiny_buffer, sizeof(fx->tiny_buffer));
  fixture_init(fx, &opts);

  drive_to_failed_register(fx);

  fixture_destroy(fx);
}

/* Same for the combined body: the CSR alone fits the buffer, the CSR plus the
 * payload does not. AZ_IOT_CSR_PAYLOAD_BUFFER_MIN no longer covers the body
 * once a payload shares it, and the SDK has to say so rather than truncate. */
static void a_csr_buffer_that_only_fits_the_csr_fails_when_a_payload_shares_it(void** state)
{
  (void)state;
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);
  opts.dps.request_operational_certificate = true;
  /* Exactly {"csr":"TESTCSRBASE64=="} and not one byte more. */
  opts.csr_payload_buffer
      = az_span_create(fx->csr_buffer, (int32_t)(strlen("{\"csr\":\"" CSR_BASE64 "\"}")));
  opts.dps.registration_payload = payload_span();
  fixture_init(fx, &opts);

  drive_to_failed_register(fx);

  /* The CSR was still released even though the body build failed. */
  assert_int_equal(fx->provider.get_csr_calls, 1);
  assert_int_equal(fx->provider.release_csr_calls, 1);

  fixture_destroy(fx);
}

/* The dedicated body buffer belongs to a body that carries the payload. A
 * CSR-only registration must ignore it and keep building in csr_payload_buffer,
 * which is what its documented contract promises -- otherwise a caller who sets
 * a body buffer sized for some other purpose would fault a registration that
 * has no payload in it at all. */
static void a_csr_only_body_ignores_the_registration_body_buffer(void** state)
{
  (void)state;
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);
  opts.dps.request_operational_certificate = true;
  opts.csr_payload_buffer = az_span_create(fx->csr_buffer, sizeof(fx->csr_buffer));
  /* No payload, and a dedicated buffer far too small for even the CSR body. */
  opts.dps.registration_body_buffer = az_span_create(fx->tiny_buffer, sizeof(fx->tiny_buffer));
  fixture_init(fx, &opts);

  az_iot_mock_mqtt_client* m = drive_to_register(fx);
  assert_register_body(m, "{\"csr\":\"" CSR_BASE64 "\"}");

  fixture_destroy(fx);
}

/* ------------------------------------------------------------------------- */
/* request direction: validation at open()                                   */
/* ------------------------------------------------------------------------- */

static void open_with_payload(const char* payload_text, az_iot_result expected)
{
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);
  opts.dps.registration_payload = span_from_literal(payload_text);
  opts.dps.registration_body_buffer = az_span_create(fx->body_buffer, sizeof(fx->body_buffer));
  fixture_init(fx, &opts);

  assert_int_equal(az_iot_connection_client_open(fx->conn.client), expected);

  fixture_destroy(fx);
}

static void open_accepts_a_well_formed_payload_object(void** state)
{
  (void)state;
  open_with_payload("{ \"a\" : [1, 2, {\"b\": null}] }", AZ_IOT_OK);
  /* Insignificant whitespace after the object is still one document. */
  open_with_payload("{\"a\":1}\n  ", AZ_IOT_OK);
}

static void open_rejects_a_malformed_payload(void** state)
{
  (void)state;
  open_with_payload("{\"a\":", AZ_IOT_ERR_INVALID_ARG);
  open_with_payload("not json at all", AZ_IOT_ERR_INVALID_ARG);
}

/* `payload` is an object on both directions of the DPS contract, so a scalar or
 * an array is rejected rather than sent. */
static void open_rejects_a_payload_that_is_not_an_object(void** state)
{
  (void)state;
  open_with_payload("[1,2,3]", AZ_IOT_ERR_INVALID_ARG);
  open_with_payload("\"just a string\"", AZ_IOT_ERR_INVALID_ARG);
  open_with_payload("42", AZ_IOT_ERR_INVALID_ARG);
}

/* Two documents spliced together would make the registration body invalid. */
static void open_rejects_trailing_content_after_the_payload(void** state)
{
  (void)state;
  open_with_payload("{\"a\":1}{\"b\":2}", AZ_IOT_ERR_INVALID_ARG);
}

/* The SDK declares no payload buffer of its own, so it has to be told where to
 * build the body. */
static void open_rejects_a_payload_with_nowhere_to_build_the_body(void** state)
{
  (void)state;
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);
  opts.dps.registration_payload = payload_span();
  fixture_init(fx, &opts);

  assert_int_equal(az_iot_connection_client_open(fx->conn.client), AZ_IOT_ERR_NOT_ENOUGH_SPACE);

  fixture_destroy(fx);
}

/* The payload is copied INTO the body buffer, so sharing storage would have the
 * build overwrite its own source. Rejected rather than silently corrupted. */
static void open_rejects_a_payload_that_overlaps_the_body_buffer(void** state)
{
  (void)state;
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);

  /* Park the payload inside the very buffer the body would be built in. */
  memcpy(fx->body_buffer, k_payload_json, sizeof(k_payload_json) - 1u);
  opts.dps.registration_payload
      = az_span_create(fx->body_buffer, (int32_t)(sizeof(k_payload_json) - 1u));
  opts.dps.registration_body_buffer = az_span_create(fx->body_buffer, sizeof(fx->body_buffer));
  fixture_init(fx, &opts);

  assert_int_equal(az_iot_connection_client_open(fx->conn.client), AZ_IOT_ERR_INVALID_ARG);

  fixture_destroy(fx);
}

/* Same hazard through the csr_payload_buffer fallback. */
static void open_rejects_a_payload_that_overlaps_the_csr_buffer_fallback(void** state)
{
  (void)state;
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);

  memcpy(fx->csr_buffer, k_payload_json, sizeof(k_payload_json) - 1u);
  opts.csr_payload_buffer = az_span_create(fx->csr_buffer, sizeof(fx->csr_buffer));
  opts.dps.registration_payload
      = az_span_create(fx->csr_buffer, (int32_t)(sizeof(k_payload_json) - 1u));
  fixture_init(fx, &opts);

  assert_int_equal(az_iot_connection_client_open(fx->conn.client), AZ_IOT_ERR_INVALID_ARG);

  fixture_destroy(fx);
}

/* Every other dps option is ignored on a direct hub connect; this one is too,
 * rather than failing a connection that has no registration to carry it. */
static void a_direct_connect_ignores_the_registration_payload(void** state)
{
  (void)state;
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);
  opts.host = "broker.example";
  opts.dps.id_scope = NULL;
  opts.dps.registration_id = NULL;
  /* Deliberately no body buffer: nothing will be built. */
  opts.dps.registration_payload = payload_span();
  fixture_init(fx, &opts);

  assert_int_equal(az_iot_connection_client_open(fx->conn.client), AZ_IOT_OK);

  fixture_destroy(fx);
}

/* ------------------------------------------------------------------------- */
/* response direction: registrationState.payload                             */
/* ------------------------------------------------------------------------- */

typedef struct received_payload
{
  int calls;
  char text[256];
} received_payload;

static void on_registration_payload(az_span payload, void* user_ctx)
{
  received_payload* r = (received_payload*)user_ctx;
  size_t n = (size_t)az_span_size(payload);
  r->calls++;
  if (n >= sizeof(r->text))
  {
    n = sizeof(r->text) - 1u;
  }
  memcpy(r->text, az_span_ptr(payload), n);
  r->text[n] = '\0';
}

#define ASSIGNED_BODY(extra)                                 \
  "{\"operationId\":\"op-1\",\"status\":\"assigned\","       \
  "\"registrationState\":{\"registrationId\":\"ut-device\"," \
  "\"assignedHub\":\"myhub.azure-devices.net\","             \
  "\"deviceId\":\"assigned-device\"" extra "}}"

static received_payload run_assignment(const char* body)
{
  payload_fixture* fx = fixture_create();
  az_iot_connection_client_options opts = fixture_options(fx);
  fixture_init(fx, &opts);

  received_payload received = { 0 };
  assert_int_equal(
      az_iot_connection_client_set_registration_payload_callback(
          fx->conn.client, on_registration_payload, &received),
      AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = drive_to_register(fx);
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m, DPS_RESPONSE_TOPIC_ASSIGNED, (const uint8_t*)body, strlen(body), AZ_IOT_MQTT_QOS_1));
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(fx->conn.client, 0);
  }

  fixture_destroy(fx);
  return received;
}

static void an_assignment_payload_reaches_the_application(void** state)
{
  (void)state;
  received_payload r = run_assignment(ASSIGNED_BODY(",\"payload\":{\"region\":\"eu\",\"tier\":2}"));
  assert_int_equal(r.calls, 1);
  assert_string_equal(r.text, "{\"region\":\"eu\",\"tier\":2}");
}

static void an_assignment_without_a_payload_does_not_fire_the_callback(void** state)
{
  (void)state;
  received_payload r = run_assignment(ASSIGNED_BODY(""));
  assert_int_equal(r.calls, 0);
}

/* The service is allowed to send null, which is "no payload", not an empty
 * one. */
static void a_null_assignment_payload_does_not_fire_the_callback(void** state)
{
  (void)state;
  received_payload r = run_assignment(ASSIGNED_BODY(",\"payload\":null"));
  assert_int_equal(r.calls, 0);
}

static void set_registration_payload_callback_rejects_a_null_client(void** state)
{
  (void)state;
  assert_int_equal(
      az_iot_connection_client_set_registration_payload_callback(NULL, NULL, NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    /* request direction */
    cmocka_unit_test(no_payload_and_no_csr_sends_an_empty_body),
    cmocka_unit_test(a_payload_alone_is_wrapped_in_the_registration_body),
    cmocka_unit_test(a_csr_alone_still_sends_the_csr_body),
    cmocka_unit_test(a_csr_and_a_payload_share_one_registration_body),
    cmocka_unit_test(the_csr_buffer_builds_the_combined_body_when_no_body_buffer_is_set),
    cmocka_unit_test(a_body_buffer_too_small_fails_instead_of_truncating),
    cmocka_unit_test(a_csr_buffer_that_only_fits_the_csr_fails_when_a_payload_shares_it),
    cmocka_unit_test(a_csr_only_body_ignores_the_registration_body_buffer),
    /* validation */
    cmocka_unit_test(open_accepts_a_well_formed_payload_object),
    cmocka_unit_test(open_rejects_a_malformed_payload),
    cmocka_unit_test(open_rejects_a_payload_that_is_not_an_object),
    cmocka_unit_test(open_rejects_trailing_content_after_the_payload),
    cmocka_unit_test(open_rejects_a_payload_with_nowhere_to_build_the_body),
    cmocka_unit_test(open_rejects_a_payload_that_overlaps_the_body_buffer),
    cmocka_unit_test(open_rejects_a_payload_that_overlaps_the_csr_buffer_fallback),
    cmocka_unit_test(a_direct_connect_ignores_the_registration_payload),
    /* response direction */
    cmocka_unit_test(an_assignment_payload_reaches_the_application),
    cmocka_unit_test(an_assignment_without_a_payload_does_not_fire_the_callback),
    cmocka_unit_test(a_null_assignment_payload_does_not_fire_the_callback),
    cmocka_unit_test(set_registration_payload_callback_rejects_a_null_client),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
