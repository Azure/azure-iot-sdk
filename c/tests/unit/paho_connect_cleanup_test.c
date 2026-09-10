// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Paho adapter: what happens to a non-extractable key reference when connect()
 * gives up part way through (D8).
 *
 * az_iot_paho_key_custody_prepare() takes custody of a private key BEFORE the
 * adapter builds anything else, and on a token-backed credential that custody
 * is a reference file on disk. Every path out of connect() after that point
 * therefore has to release it; one that does not leaves the file behind until
 * the next connect or destroy. Nothing about that is visible in the return
 * code, so each case here drives connect() to one specific failure and then
 * looks for the file.
 *
 * The suite compiles az_iot_mqtt_paho.c directly against two doubles -- see
 * mock_paho_async.h and fake_paho_key_custody.h for why neither the real Paho
 * nor the real custody module can produce these situations on a machine with no
 * PKCS#11 token. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/adapters/az_iot_adapter_paho.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "fake_paho_key_custody.h"
#include "mock_paho_async.h"

/* ------------------------------------------------------------------------- */
/* malloc fault injection                                                     */
/*                                                                            */
/* The one early return in connect() that no mock can reach is the allocation  */
/* failure for server_uri / client_id: both inputs are already validated       */
/* non-NULL, so only malloc itself can send it there. --wrap is a GNU ld /     */
/* lld feature, so the case is registered only where the build system could    */
/* ask for it.                                                                 */
/* ------------------------------------------------------------------------- */
#if defined(AZ_IOT_TEST_WRAP_MALLOC)

void* __real_malloc(size_t size);
void* __wrap_malloc(size_t size);

/* -1 disables injection; otherwise the number of allocations still to be let
 * through before one is failed. */
static int s_malloc_countdown = -1;

void* __wrap_malloc(size_t size)
{
  if (s_malloc_countdown >= 0)
  {
    if (s_malloc_countdown == 0)
    {
      s_malloc_countdown = -1;
      return NULL;
    }
    --s_malloc_countdown;
  }
  return __real_malloc(size);
}

static void fail_malloc_after(int allowed) { s_malloc_countdown = allowed; }
static void stop_failing_malloc(void) { s_malloc_countdown = -1; }

#endif /* AZ_IOT_TEST_WRAP_MALLOC */

/* ------------------------------------------------------------------------- */
/* fixture                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct
{
  az_iot_mqtt_factory* factory;
  az_iot_mqtt_client* client;
} fixture;

static int setup(void** state)
{
  mock_paho_reset();
  fake_custody_reset();

  fixture* fx = (fixture*)calloc(1, sizeof(fixture));
  assert_non_null(fx);
  fx->factory = az_iot_paho_factory_create_v3_1_1();
  assert_non_null(fx->factory);
  fx->client = fx->factory->create(fx->factory->factory_ctx);
  assert_non_null(fx->client);
  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    if (fx->client)
    {
      fx->client->iface->destroy(fx->client);
    }
    az_iot_paho_factory_destroy(fx->factory);
    free(fx);
  }
  fake_custody_reset();
  return 0;
}

/* A credential the adapter must route through key custody: a URI plus the
 * provider that owns it, and no readable key file anywhere. */
static void custody_connect_options(az_iot_mqtt_connect_options* opts)
{
  memset(opts, 0, sizeof(*opts));
  opts->host = "broker.invalid";
  opts->port = 8883;
  opts->client_id = "ut-device";
  opts->tls.client_cert_path = "/dev/null/device.pem";
  opts->tls.client_key_uri = "pkcs11:object=device-key;type=private";
  opts->tls.crypto_engine_id = "pkcs11";
  opts->tls.use_tls = true;
}

/* Drive connect() once with a custody credential and hand back what it
 * returned, having first checked that custody really was taken -- otherwise a
 * case could "pass" because there was never a file to leak. */
static az_iot_result connect_with_custody(fixture* fx)
{
  az_iot_mqtt_connect_options opts;
  custody_connect_options(&opts);
  int before = fake_custody_prepare_calls();
  az_iot_result rc = fx->client->iface->connect(fx->client, &opts);
  assert_int_equal(fake_custody_prepare_calls(), before + 1);
  return rc;
}

/* ------------------------------------------------------------------------- */
/* the early returns after prepare()                                          */
/* ------------------------------------------------------------------------- */

/* Nothing was taken, so nothing has to be given back, and the caller sees the
 * reason custody failed rather than a generic MQTT error. */
static void a_refused_key_reference_fails_the_connect(void** state)
{
  fixture* fx = (fixture*)*state;
  fake_custody_set_prepare_result(AZ_IOT_ERR_NOT_SUPPORTED);

  assert_int_equal(connect_with_custody(fx), AZ_IOT_ERR_NOT_SUPPORTED);
  assert_null(fake_custody_reference_path());
  assert_int_equal(mock_paho_create_calls(), 0);
}

static void a_create_failure_releases_the_key_reference(void** state)
{
  fixture* fx = (fixture*)*state;
  mock_paho_set_create_rc(MQTTASYNC_FAILURE);

  assert_int_equal(connect_with_custody(fx), AZ_IOT_ERR_MQTT);
  assert_non_null(fake_custody_reference_path());
  assert_false(fake_custody_reference_file_exists());
}

static void a_set_callbacks_failure_releases_the_key_reference(void** state)
{
  fixture* fx = (fixture*)*state;
  mock_paho_set_set_callbacks_rc(MQTTASYNC_FAILURE);

  assert_int_equal(connect_with_custody(fx), AZ_IOT_ERR_MQTT);
  assert_non_null(fake_custody_reference_path());
  assert_false(fake_custody_reference_file_exists());
}

static void a_set_disconnected_failure_releases_the_key_reference(void** state)
{
  fixture* fx = (fixture*)*state;
  mock_paho_set_set_disconnected_rc(MQTTASYNC_FAILURE);

  assert_int_equal(connect_with_custody(fx), AZ_IOT_ERR_MQTT);
  assert_non_null(fake_custody_reference_path());
  assert_false(fake_custody_reference_file_exists());
}

static void a_connect_failure_releases_the_key_reference(void** state)
{
  fixture* fx = (fixture*)*state;
  mock_paho_set_connect_rc(MQTTASYNC_FAILURE);

  assert_int_equal(connect_with_custody(fx), AZ_IOT_ERR_MQTT);
  assert_int_equal(mock_paho_connect_calls(), 1);
  assert_non_null(fake_custody_reference_path());
  assert_false(fake_custody_reference_file_exists());
}

#if defined(AZ_IOT_TEST_WRAP_MALLOC)
/* The allocation failure between prepare() and the Paho handle. One allocation
 * is let through -- the double's own copy of the reference path -- and the next,
 * which is the adapter's server_uri, fails. */
static void an_allocation_failure_releases_the_key_reference(void** state)
{
  fixture* fx = (fixture*)*state;

  fail_malloc_after(1);
  az_iot_result rc = connect_with_custody(fx);
  stop_failing_malloc();

  assert_int_equal(rc, AZ_IOT_ERR_OUT_OF_MEMORY);
  assert_int_equal(mock_paho_create_calls(), 0);
  assert_non_null(fake_custody_reference_path());
  assert_false(fake_custody_reference_file_exists());
}
#endif

/* ------------------------------------------------------------------------- */
/* the paths that must NOT release                                            */
/* ------------------------------------------------------------------------- */

/* MQTTAsync_connect is asynchronous: Paho opens the TLS session on its own
 * thread afterwards and reads the key file then. Releasing on the success path
 * would delete the file out from under the handshake -- so an accepted connect
 * must leave it in place. */
static void an_accepted_connect_keeps_the_key_reference(void** state)
{
  fixture* fx = (fixture*)*state;

  assert_int_equal(connect_with_custody(fx), AZ_IOT_OK);
  assert_true(fake_custody_reference_file_exists());
}

/* ...and it is released at the next connect, so a reconnecting client does not
 * accumulate one reference file per attempt. */
static void reconnecting_replaces_the_key_reference(void** state)
{
  fixture* fx = (fixture*)*state;

  assert_int_equal(connect_with_custody(fx), AZ_IOT_OK);
  const char* first = fake_custody_reference_path();
  assert_non_null(first);
  char kept[512];
  snprintf(kept, sizeof(kept), "%s", first);

  assert_int_equal(connect_with_custody(fx), AZ_IOT_OK);
  const char* second = fake_custody_reference_path();
  assert_non_null(second);
  assert_string_not_equal(kept, second);

  FILE* stale = fopen(kept, "rb");
  assert_null(stale);
  assert_true(fake_custody_reference_file_exists());
}

/* ...and at destroy, which is the only other place it can go. */
static void destroy_releases_the_key_reference(void** state)
{
  fixture* fx = (fixture*)*state;

  assert_int_equal(connect_with_custody(fx), AZ_IOT_OK);
  assert_true(fake_custody_reference_file_exists());

  fx->client->iface->destroy(fx->client);
  fx->client = NULL; /* teardown must not destroy it twice */

  assert_false(fake_custody_reference_file_exists());
}

/* A credential with no key reference at all still goes through prepare() and
 * release(), and must come out with the caller's own key path and no temporary
 * file. This is the ordinary PEM connect, which the custody work must not have
 * changed. */
static void a_plain_credential_takes_no_custody(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_mqtt_connect_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = "broker.invalid";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.tls.client_cert_path = "/dev/null/device.pem";
  opts.tls.client_key_path = "/dev/null/device.key";
  opts.tls.use_tls = true;

  assert_int_equal(fx->client->iface->connect(fx->client, &opts), AZ_IOT_OK);
  assert_int_equal(fake_custody_prepare_calls(), 1);
  assert_null(fake_custody_reference_path());
}

/* ------------------------------------------------------------------------- */
/* server certificate validation                                              */
/* ------------------------------------------------------------------------- */

/* Validation is a property of the client, not a caller policy. There is no
 * field that can ask for an unverified session any more, so the case to pin is
 * the one that used to produce one: TLS selected purely by credential material,
 * with every byte of tls left at its zero default. The session must still be
 * verified -- chain (enableServerCertAuth) AND hostname (verify).
 *
 * This is the case that mattered, because the slot now called use_tls used to
 * be verify_server, and false is what a zero-initialized
 * az_iot_mqtt_connect_options holds -- so "forgot to set it" and "asked for it
 * to be off" were the same bytes, and both yielded an unverified connection. */
static void server_validation_cannot_be_switched_off(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_mqtt_connect_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = "broker.invalid";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.tls.client_cert_path = "/dev/null/device.pem";
  opts.tls.client_key_path = "/dev/null/device.key";
  /* use_tls deliberately left false: the certificate selects TLS, and nothing
   * here consents to validation. */

  assert_int_equal(fx->client->iface->connect(fx->client, &opts), AZ_IOT_OK);

  assert_true(mock_paho_last_connect_had_ssl());
  assert_int_equal(mock_paho_last_enable_server_cert_auth(), 1);
  assert_int_equal(mock_paho_last_verify(), 1);
}

/* The same for a custody credential, which is the connect shape this change
 * introduced: it reaches the TLS branch through the key reference rather than
 * through a certificate path. */
static void a_custody_connect_verifies_the_server(void** state)
{
  fixture* fx = (fixture*)*state;

  assert_int_equal(connect_with_custody(fx), AZ_IOT_OK);

  assert_true(mock_paho_last_connect_had_ssl());
  assert_int_equal(mock_paho_last_enable_server_cert_auth(), 1);
  assert_int_equal(mock_paho_last_verify(), 1);
}

/* A key reference alone must select TLS. Without it the adapter would build a
 * tcp:// URI and connect in the clear with the very credential that exists
 * because the key must never be exposed -- and the key reference would be
 * handed to a session that never uses it. */
static void a_key_reference_alone_selects_tls(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_mqtt_connect_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = "broker.invalid";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.tls.client_key_uri = "pkcs11:object=device-key;type=private";
  opts.tls.crypto_engine_id = "pkcs11";
  /* No certificate, no CA, no use_tls: the key reference is the only
   * thing asking for TLS. */

  assert_int_equal(fx->client->iface->connect(fx->client, &opts), AZ_IOT_OK);

  const char* uri = mock_paho_last_server_uri();
  assert_non_null(uri);
  assert_int_equal(strncmp(uri, "ssl://", 6), 0);
  assert_true(mock_paho_last_connect_had_ssl());
  assert_int_equal(mock_paho_last_enable_server_cert_auth(), 1);
  assert_int_equal(mock_paho_last_verify(), 1);

  /* ...and the reference file, not the caller's NULL client_key_path, is what
   * the TLS stack was pointed at. */
  const char* key = mock_paho_last_private_key();
  assert_non_null(key);
  assert_string_equal(key, fake_custody_reference_path());
}

/* A connection with no TLS material at all stays plaintext, and carries no SSL
 * options to verify anything with. The verification rule above must not have
 * turned every connect into a TLS connect. */
static void a_plaintext_connect_stays_plaintext(void** state)
{
  fixture* fx = (fixture*)*state;

  az_iot_mqtt_connect_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = "broker.invalid";
  opts.port = 1883;
  opts.client_id = "ut-device";

  assert_int_equal(fx->client->iface->connect(fx->client, &opts), AZ_IOT_OK);

  const char* uri = mock_paho_last_server_uri();
  assert_non_null(uri);
  assert_int_equal(strncmp(uri, "tcp://", 6), 0);
  assert_false(mock_paho_last_connect_had_ssl());
}

/* ------------------------------------------------------------------------- */
/* client-initiated disconnect completion                                     */
/*                                                                            */
/* Paho reports a disconnect the PEER caused through connectionLost() and the  */
/* v5 disconnected() callback, but reports the one the application asked for   */
/* only through the completion callbacks passed to MQTTAsync_disconnect. With  */
/* none supplied the event never arrived and a caller waiting for the session  */
/* to settle waited forever.                                                   */
/* ------------------------------------------------------------------------- */

/* Collects the events the adapter raises. */
typedef struct
{
  int disconnected;
  az_iot_result last_status;
} evt_sink;

static void on_evt(const az_iot_mqtt_event* e, void* ctx)
{
  evt_sink* s = (evt_sink*)ctx;
  if (e->kind == AZ_IOT_MQTT_EVT_DISCONNECTED)
  {
    s->disconnected++;
    s->last_status = e->status;
  }
}

/* Connect, disconnect, and drain whatever the adapter queued. */
static void connect_then_disconnect(fixture* fx, evt_sink* sink, az_iot_result* disc_rc)
{
  fx->client->iface->set_inbound_cb(fx->client, on_evt, sink);

  az_iot_mqtt_connect_options opts;
  memset(&opts, 0, sizeof(opts));
  opts.host = "broker.invalid";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.tls.client_cert_path = "/dev/null/device.pem";
  opts.tls.client_key_path = "/dev/null/device.key";
  assert_int_equal(fx->client->iface->connect(fx->client, &opts), AZ_IOT_OK);

  *disc_rc = fx->client->iface->disconnect(fx->client);
  for (int i = 0; i < 20 && sink->disconnected == 0; ++i)
  {
    (void)fx->client->iface->process_loop(fx->client, 1);
  }
}

/* The completion of a disconnect the application asked for is reported. */
static void a_client_initiated_disconnect_reports_disconnected(void** state)
{
  fixture* fx = (fixture*)*state;
  evt_sink sink = { 0 };
  az_iot_result rc = AZ_IOT_OK;

  connect_then_disconnect(fx, &sink, &rc);

  assert_int_equal(rc, AZ_IOT_OK);
  assert_int_equal(mock_paho_disconnect_calls(), 1);
  /* Without callbacks and a context the real client reports this to nobody. */
  assert_true(mock_paho_disconnect_had_callbacks());
  assert_int_equal(sink.disconnected, 1);
  assert_int_equal(sink.last_status, AZ_IOT_OK);
}

/* Same on MQTT v5. Only the v3 completion callbacks are set, which is correct:
 * for a DISCONNECT command Paho calls onSuccess when it is set and falls back
 * to onSuccess5 only when it is not, so one pair serves both versions. This
 * pins that, because setting the wrong pair fails silently -- the disconnect
 * still succeeds and the event simply never arrives. */
static void a_v5_client_initiated_disconnect_reports_disconnected(void** state)
{
  (void)state;
  mock_paho_reset();
  fake_custody_reset();

  az_iot_mqtt_factory* f = az_iot_paho_factory_create_v5();
  assert_non_null(f);
  fixture local;
  memset(&local, 0, sizeof(local));
  local.factory = f;
  local.client = f->create(f->factory_ctx);
  assert_non_null(local.client);

  evt_sink sink = { 0 };
  az_iot_result rc = AZ_IOT_OK;
  connect_then_disconnect(&local, &sink, &rc);

  assert_int_equal(rc, AZ_IOT_OK);
  assert_true(mock_paho_disconnect_had_callbacks());
  assert_int_equal(sink.disconnected, 1);

  local.client->iface->destroy(local.client);
  az_iot_paho_factory_destroy(f);
}

/* A disconnect that completes with a FAILURE still ends the session, so it is
 * still DISCONNECTED -- but it carries the error. The core surfaces
 * evt->status as the reason the connection ended, so reporting AZ_IOT_OK here
 * would tell the application the opposite of what happened. */
static void a_failed_disconnect_completion_carries_the_error(void** state)
{
  fixture* fx = (fixture*)*state;
  mock_paho_set_disconnect_completion(false);

  evt_sink sink = { 0 };
  az_iot_result rc = AZ_IOT_OK;
  connect_then_disconnect(fx, &sink, &rc);

  assert_int_equal(sink.disconnected, 1);
  assert_int_equal(sink.last_status, AZ_IOT_ERR_MQTT);
}

/* A refused MQTTAsync_disconnect runs no completion callback at all, so the
 * adapter has to raise the event itself -- otherwise this is the one path that
 * still hangs. */
static void a_refused_disconnect_still_reports_disconnected(void** state)
{
  fixture* fx = (fixture*)*state;
  mock_paho_set_disconnect_rc(MQTTASYNC_FAILURE);

  evt_sink sink = { 0 };
  az_iot_result rc = AZ_IOT_OK;
  connect_then_disconnect(fx, &sink, &rc);

  assert_int_equal(rc, AZ_IOT_ERR_MQTT);
  assert_int_equal(sink.disconnected, 1);
  assert_int_equal(sink.last_status, AZ_IOT_ERR_MQTT);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(a_refused_key_reference_fails_the_connect, setup, teardown),
    cmocka_unit_test_setup_teardown(a_create_failure_releases_the_key_reference, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_set_callbacks_failure_releases_the_key_reference, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_set_disconnected_failure_releases_the_key_reference, setup, teardown),
    cmocka_unit_test_setup_teardown(a_connect_failure_releases_the_key_reference, setup, teardown),
#if defined(AZ_IOT_TEST_WRAP_MALLOC)
    cmocka_unit_test_setup_teardown(
        an_allocation_failure_releases_the_key_reference, setup, teardown),
#endif
    cmocka_unit_test_setup_teardown(an_accepted_connect_keeps_the_key_reference, setup, teardown),
    cmocka_unit_test_setup_teardown(reconnecting_replaces_the_key_reference, setup, teardown),
    cmocka_unit_test_setup_teardown(destroy_releases_the_key_reference, setup, teardown),
    cmocka_unit_test_setup_teardown(a_plain_credential_takes_no_custody, setup, teardown),
    cmocka_unit_test_setup_teardown(server_validation_cannot_be_switched_off, setup, teardown),
    cmocka_unit_test_setup_teardown(a_custody_connect_verifies_the_server, setup, teardown),
    cmocka_unit_test_setup_teardown(a_key_reference_alone_selects_tls, setup, teardown),
    cmocka_unit_test_setup_teardown(a_plaintext_connect_stays_plaintext, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_client_initiated_disconnect_reports_disconnected, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_v5_client_initiated_disconnect_reports_disconnected, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_failed_disconnect_completion_carries_the_error, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_refused_disconnect_still_reports_disconnected, setup, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
