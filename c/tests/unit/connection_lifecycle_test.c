// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* ConnectionClient lifecycle unit tests: open/close/destroy/do_work argument
 * validation, state-machine edges, adapter ownership, and the observable
 * consequences of a CONNACK rejection.
 *
 * Everything is driven through the public API against the in-memory mock
 * adapter. Companion suites: connection_reconnect_test.c (retry/backoff) and
 * connection_dps_test.c (provisioning). */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "internal/connection_client_internal.h"

#include "support/connection_test_harness.h"

/* ------------------------------------------------------------------------- */
/* fixture                                                                   */
/* ------------------------------------------------------------------------- */

static int setup(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = az_iot_test_classic_options();
  assert_int_equal(az_iot_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_set_state_callback(fx->client, az_iot_test_on_state, &fx->log),
      AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  if (fx)
  {
    /* A registered factory is adopted by the client and freed from destroy();
     * an unregistered one is still ours. */
    bool adopted = (fx->client->factory_count > 0);
    az_iot_connection_client_destroy(&fx->client_storage);
    if (!adopted)
      az_iot_mock_mqtt_factory_destroy(fx->factory);
    free(fx);
  }
  return 0;
}

/* Register the factory and open(); returns the mock adapter instance. */
static az_iot_mock_mqtt_client* open_to_connecting(az_iot_test_conn* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  return m;
}

/* Drive all the way to CONNECTED. */
static az_iot_mock_mqtt_client* open_to_connected(az_iot_test_conn* fx)
{
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
  return m;
}

/* ------------------------------------------------------------------------- */
/* open() argument + state validation                                        */
/* ------------------------------------------------------------------------- */

static void open_rejects_null_client(void** state)
{
  (void)state;
  assert_int_equal(az_iot_connection_client_open(NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void open_without_host_or_dps_is_rejected(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = { 0 };
  opts.client_id = "ut-device";
  /* No host, no dps.id_scope: there is nothing to connect to. */
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_ERR_INVALID_ARG);
  az_iot_connection_client_destroy(&c);
}

static void open_without_client_id_is_rejected(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  /* A direct hub connect needs an identity as well as an endpoint. */
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_ERR_INVALID_ARG);
  az_iot_connection_client_destroy(&c);
}

static void open_while_connecting_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_connecting(fx);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_ERR_ALREADY_INITIALIZED);
}

static void open_while_connected_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_connected(fx);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_ERR_ALREADY_INITIALIZED);
}

/* FAULTED is terminal for this client instance: open() does not restart it.
 * Recovery requires destroy() + init(). Pinning this makes the limitation
 * visible rather than folklore. */
static void open_from_faulted_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));

  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_ERR_ALREADY_INITIALIZED);
}

/* The adapter registry is keyed by MQTT version; a Classic (v3.1.1) session
 * cannot borrow a v5 factory. */
static void open_rejects_factory_of_the_wrong_version(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);

  az_iot_mqtt_factory* v5 = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  assert_non_null(v5);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, v5), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_ERR_NOT_SUPPORTED);
  az_iot_connection_client_destroy(&c);
}

static void open_connects_to_the_configured_endpoint(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(c);
  assert_string_equal(c->connect.host, "broker.example");
  assert_string_equal(c->connect.client_id, "ut-device");
  assert_int_equal(c->connect.port, 8883);
}

/* ------------------------------------------------------------------------- */
/* close()                                                                   */
/* ------------------------------------------------------------------------- */

static void close_rejects_null_client(void** state)
{
  (void)state;
  assert_int_equal(az_iot_connection_client_close(NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void close_while_connecting_disconnects_the_adapter(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_DISCONNECTING);
  assert_int_equal(az_iot_mock_mqtt_client_count_of(m, AZ_IOT_MOCK_CALL_DISCONNECT), 1);
}

/* A CONNACK that lands after the application asked to close belongs to an
 * attempt it has already abandoned. Announcing CONNECTED for it would report a
 * session nobody asked for, and any code that publishes on CONNECTED would
 * write into a socket already being torn down. */
static void close_while_connecting_suppresses_a_late_connack(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_false(az_iot_connection_client__is_connected(fx->client));
  assert_false(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_CONNECTED));

  /* The close still completes normally once the disconnect lands. */
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_IDLE);
}

/* The suppression must key on "the user asked to close", not on a stale state:
 * a normal CONNACK on a live attempt is still announced. */
static void a_connack_without_a_pending_close_still_connects(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_true(az_iot_connection_client__is_connected(fx->client));
}

static void close_twice_is_idempotent(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_IDLE);

  size_t transitions = fx->log.count;
  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(fx->log.count, transitions);
}

/* After a fault the adapter is already gone, so there is nothing to disconnect.
 * close() reports that rather than pretending it did something. */
static void close_from_faulted_reports_not_initialized(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_test_saw_state(&fx->log, AZ_IOT_CONN_STATE_FAULTED));

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_ERR_NOT_INITIALIZED);
}

/* ------------------------------------------------------------------------- */
/* destroy()                                                                 */
/* ------------------------------------------------------------------------- */

/* Register a factory the client will NOT free.
 *
 * destroy() calls every registered factory's destroy hook, and the mock's hook
 * frees the factory itself -- so a test that inspects the factory AFTER
 * destroy() would be reading freed memory. glibc happens to leave the bytes
 * looking like the values the assertions want, which is why this passes on a
 * plain Linux build; valgrind reports an invalid read, and the MSVC debug CRT
 * fills freed blocks with 0xDD so the assertion fails outright. The client
 * copies the struct, so detaching the hook on a local copy leaves the real
 * factory alive and owned by the test. */
static void register_without_adopting(az_iot_connection_client* c, az_iot_mqtt_factory* factory)
{
  az_iot_mqtt_factory borrowed = *factory;
  borrowed.destroy = NULL;
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(c, &borrowed), AZ_IOT_OK);
}

static void destroy_while_connected_destroys_the_adapter(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);

  az_iot_mqtt_factory* factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(factory);
  register_without_adopting(&c, factory);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&c, 0);

  /* The mock frees itself inside destroy(), so read the state we care about
   * from the factory: a destroyed client detaches itself from last_client. */
  az_iot_connection_client_destroy(&c);
  assert_null(az_iot_mock_mqtt_factory_last_client(factory));

  az_iot_mock_mqtt_factory_destroy(factory);
}

static void destroy_while_connecting_destroys_the_adapter(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = az_iot_test_classic_options();
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);

  az_iot_mqtt_factory* factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(factory);
  register_without_adopting(&c, factory);
  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_OK);
  assert_non_null(az_iot_mock_mqtt_factory_last_client(factory));

  az_iot_connection_client_destroy(&c);
  assert_null(az_iot_mock_mqtt_factory_last_client(factory));

  az_iot_mock_mqtt_factory_destroy(factory);
}

static void destroy_is_silent_on_the_state_callback(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_connected(fx);

  size_t transitions = fx->log.count;
  az_iot_connection_client_destroy(fx->client);
  assert_int_equal(fx->log.count, transitions);

  /* Neutralize the fixture teardown: the client is already destroyed and the
   * factory was adopted (and freed) by it. */
  memset(&fx->client_storage, 0, sizeof(fx->client_storage));
  fx->factory = NULL;
}

static void destroy_tolerates_null(void** state)
{
  (void)state;
  az_iot_connection_client_destroy(NULL);
}

/* ------------------------------------------------------------------------- */
/* do_work()                                                                 */
/* ------------------------------------------------------------------------- */

static void do_work_rejects_null_client(void** state)
{
  (void)state;
  assert_int_equal(az_iot_connection_client_do_work(NULL, 0), AZ_IOT_ERR_INVALID_ARG);
}

static void do_work_before_open_touches_no_adapter(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_do_work(fx->client, 0), AZ_IOT_OK);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
  assert_int_equal(fx->log.count, 0);
}

static void do_work_after_close_touches_no_adapter(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_IDLE);

  /* The adapter is torn down on the way to IDLE; further pumping is inert. */
  assert_int_equal(az_iot_connection_client_do_work(fx->client, 0), AZ_IOT_OK);
}

static void do_work_forwards_the_timeout_to_the_adapter(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  (void)az_iot_connection_client_do_work(fx->client, 250);
  const az_iot_mock_call* c = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_PROCESS_LOOP);
  assert_non_null(c);
  assert_int_equal(c->timeout_ms, 250);
}

static void do_work_surfaces_the_adapter_pump_error(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  az_iot_mock_mqtt_client_set_next_result(m, AZ_IOT_MOCK_CALL_PROCESS_LOOP, AZ_IOT_ERR_MQTT);
  assert_int_equal(az_iot_connection_client_do_work(fx->client, 0), AZ_IOT_ERR_MQTT);
}

/* ------------------------------------------------------------------------- */
/* reopen                                                                    */
/* ------------------------------------------------------------------------- */

static void reopen_after_close_starts_a_second_session(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* first = open_to_connected(fx);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(first));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_IDLE);

  /* Second open() must build a fresh adapter instance -- the first one was
   * destroyed on the way to IDLE. */
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* second = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(second);
  assert_true(az_iot_mock_mqtt_client_inject_connected(second, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_true(az_iot_connection_client__is_connected(fx->client));
  assert_int_equal(az_iot_test_count_state(&fx->log, AZ_IOT_CONN_STATE_CONNECTED), 2);
}

/* ------------------------------------------------------------------------- */
/* state callback                                                            */
/* ------------------------------------------------------------------------- */

static void set_state_callback_rejects_null_client(void** state)
{
  (void)state;
  assert_int_equal(
      az_iot_connection_client_set_state_callback(NULL, az_iot_test_on_state, NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

static void state_callback_carries_the_failure_reason(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_IDENTITY_REJECTED));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_IDENTITY_REJECTED);
}

static void state_callback_can_be_replaced(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_test_state_log second = { 0 };
  assert_int_equal(
      az_iot_connection_client_set_state_callback(fx->client, az_iot_test_on_state, &second),
      AZ_IOT_OK);

  size_t before = fx->log.count;
  (void)open_to_connecting(fx);

  assert_int_equal(fx->log.count, before);
  assert_true(second.count > 0);
}

/* ------------------------------------------------------------------------- */
/* traffic is gated on CONNECTED                                             */
/* ------------------------------------------------------------------------- */

static void publish_before_connected_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_connecting(fx);

  az_iot_mqtt_message msg = { 0 };
  msg.topic = "devices/ut-device/messages/events/";
  msg.qos = AZ_IOT_MQTT_QOS_1;
  assert_int_equal(
      az_iot_connection_client__publish(fx->client, &msg, NULL, NULL), AZ_IOT_ERR_NOT_CONNECTED);
}

static void subscribe_before_connected_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  (void)open_to_connecting(fx);

  uint16_t pid = 0;
  assert_int_equal(
      az_iot_connection_client__subscribe(
          fx->client, "devices/ut-device/#", AZ_IOT_MQTT_QOS_1, &pid),
      AZ_IOT_ERR_NOT_CONNECTED);
}

static void publish_after_disconnect_is_rejected(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connected(fx);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  az_iot_mqtt_message msg = { 0 };
  msg.topic = "devices/ut-device/messages/events/";
  msg.qos = AZ_IOT_MQTT_QOS_1;
  assert_int_equal(
      az_iot_connection_client__publish(fx->client, &msg, NULL, NULL), AZ_IOT_ERR_NOT_CONNECTED);
}

/* ------------------------------------------------------------------------- */
/* CONNACK rejection: what the core actually does with IDENTITY_REJECTED      */
/* ------------------------------------------------------------------------- */

/* The adapter contract singles out AZ_IOT_ERR_IDENTITY_REJECTED as the code
 * that should make the SDK re-provision instead of retrying a refused identity.
 * The core does NOT act on it today: it is retried like any other CONNACK
 * failure. These two tests pin the SHIPPING behaviour so that implementing
 * re-provisioning is a deliberate, test-visible change rather than a silent
 * one. See docs/test-coverage.md ("known gaps"). */
static void identity_rejection_faults_when_reconnect_is_disabled(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_IDENTITY_REJECTED));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(
      az_iot_test_reason_for(&fx->log, AZ_IOT_CONN_STATE_FAULTED), AZ_IOT_ERR_IDENTITY_REJECTED);
}

static void connack_rejection_tears_the_adapter_down(void** state)
{
  az_iot_test_conn* fx = (az_iot_test_conn*)*state;
  az_iot_mock_mqtt_client* m = open_to_connecting(fx);

  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_MQTT));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(az_iot_test_last_state(&fx->log), AZ_IOT_CONN_STATE_FAULTED);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    /* open() */
    cmocka_unit_test(open_rejects_null_client),
    cmocka_unit_test(open_without_host_or_dps_is_rejected),
    cmocka_unit_test(open_without_client_id_is_rejected),
    cmocka_unit_test_setup_teardown(open_while_connecting_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(open_while_connected_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(open_from_faulted_is_rejected, setup, teardown),
    cmocka_unit_test(open_rejects_factory_of_the_wrong_version),
    cmocka_unit_test_setup_teardown(open_connects_to_the_configured_endpoint, setup, teardown),
    /* close() */
    cmocka_unit_test(close_rejects_null_client),
    cmocka_unit_test_setup_teardown(
        close_while_connecting_disconnects_the_adapter, setup, teardown),
    cmocka_unit_test_setup_teardown(
        close_while_connecting_suppresses_a_late_connack, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_connack_without_a_pending_close_still_connects, setup, teardown),
    cmocka_unit_test_setup_teardown(close_twice_is_idempotent, setup, teardown),
    cmocka_unit_test_setup_teardown(close_from_faulted_reports_not_initialized, setup, teardown),
    /* destroy() */
    cmocka_unit_test(destroy_while_connected_destroys_the_adapter),
    cmocka_unit_test(destroy_while_connecting_destroys_the_adapter),
    cmocka_unit_test_setup_teardown(destroy_is_silent_on_the_state_callback, setup, teardown),
    cmocka_unit_test(destroy_tolerates_null),
    /* do_work() */
    cmocka_unit_test(do_work_rejects_null_client),
    cmocka_unit_test_setup_teardown(do_work_before_open_touches_no_adapter, setup, teardown),
    cmocka_unit_test_setup_teardown(do_work_after_close_touches_no_adapter, setup, teardown),
    cmocka_unit_test_setup_teardown(do_work_forwards_the_timeout_to_the_adapter, setup, teardown),
    cmocka_unit_test_setup_teardown(do_work_surfaces_the_adapter_pump_error, setup, teardown),
    /* reopen */
    cmocka_unit_test_setup_teardown(reopen_after_close_starts_a_second_session, setup, teardown),
    /* state callback */
    cmocka_unit_test(set_state_callback_rejects_null_client),
    cmocka_unit_test_setup_teardown(state_callback_carries_the_failure_reason, setup, teardown),
    cmocka_unit_test_setup_teardown(state_callback_can_be_replaced, setup, teardown),
    /* traffic gating */
    cmocka_unit_test_setup_teardown(publish_before_connected_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(subscribe_before_connected_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(publish_after_disconnect_is_rejected, setup, teardown),
    /* CONNACK rejection */
    cmocka_unit_test_setup_teardown(
        identity_rejection_faults_when_reconnect_is_disabled, setup, teardown),
    cmocka_unit_test_setup_teardown(connack_rejection_tears_the_adapter_down, setup, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
