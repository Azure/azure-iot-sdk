// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Phase 2.4 - ConnectionClient unit tests, driven through the public API and
 * the in-memory mock_mqtt_iface. */
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
#include "azure/iot/az_iot_version.h"

#include "internal/cert_util.h"
#include "internal/connection_client_internal.h"
#include "internal/mono_time.h"
#include "internal/retry_policy.h"

#include "support/connection_test_harness.h"
#include "support/mock_mqtt_iface.h"
#include "support/test_provider.h"

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct state_record
{
  az_iot_connection_state states[16];
  az_iot_result reasons[16];
  size_t count;
} state_record;

static void on_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  az_iot_connection_state state = event->state;
  az_iot_result reason = event->reason;
  state_record* r = (state_record*)user_ctx;
  if (r->count < (sizeof(r->states) / sizeof(r->states[0])))
  {
    r->states[r->count] = state;
    r->reasons[r->count] = reason;
    r->count++;
  }
}

typedef struct fixture
{
  az_iot_connection_client client_storage;
  az_iot_connection_client* client;
  az_iot_mqtt_factory* factory;
  state_record rec;
  uint8_t csr_buf[AZ_IOT_CSR_PAYLOAD_BUFFER_MIN];
} fixture;

static int setup(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.csr_payload_buffer = az_span_create(fx->csr_buf, sizeof(fx->csr_buf));
  assert_int_equal(az_iot_test_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, on_state, &fx->rec), AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  *state = fx;
  return 0;
}

static int teardown(void** state)
{
  fixture* fx = (fixture*)*state;
  if (fx)
  {
    /* A factory that was registered with the client is adopted by it and
     * freed from deinit() (via factory.destroy). A factory that was never
     * registered is still owned by the test, so we must destroy it here to
     * avoid leaking it. Check before deinit() clears factory_count. */
    bool factory_adopted = (fx->client->factory_count > 0);
    az_iot_connection_client_deinit(&fx->client_storage);
    if (!factory_adopted)
    {
      az_iot_mock_mqtt_factory_destroy(fx->factory);
    }
    free(fx);
  }
  return 0;
}

/* Setup variant with reconnect enabled. initial_delay/max_delay are large
 * enough (20ms) that a single (valgrind-slowed) do_work cannot cross the
 * reconnect deadline in the same call that schedules it -- otherwise the
 * RETRY_PENDING state would be skipped straight to CONNECTING. No jitter so
 * timing is deterministic; max_attempts 2 so we can drive the give-up branch. */
static int setup_with_reconnect(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.reconnection_policy.initial_delay_ms = 20;
  opts.reconnection_policy.max_delay_ms = 20;
  opts.reconnection_policy.max_attempts = 2;
  opts.reconnection_policy.jitter_pct = 0;
  assert_int_equal(az_iot_test_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, on_state, &fx->rec), AZ_IOT_OK);
  az_iot_connection_client__seed_rng(fx->client, 0xC0FFEEFEEDFACEull);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(fx->factory);

  *state = fx;
  return 0;
}

/* Spin until the monotonic clock advances by `ms`. Used in reconnect tests so
 * we don't depend on a sleep helper / feature macros. */
static void wait_ms(unsigned ms)
{
  uint64_t deadline = az_iot_time_mono_ms() + ms;
  while (az_iot_time_mono_ms() < deadline)
  { /* spin */
  }
}

/* ------------------------------------------------------------------------- */
/* tests                                                                     */
/* ------------------------------------------------------------------------- */

static void register_rejects_null_create(void** state)
{
  fixture* fx = (fixture*)*state;

  /* A factory with a NULL create function is rejected. */
  az_iot_mqtt_factory bad = { 0 };
  bad.version = AZ_IOT_MQTT_VERSION_3_1_1;
  bad.create = NULL;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, &bad), AZ_IOT_ERR_INVALID_ARG);
}

static void registering_the_same_factory_twice_does_not_grow_the_registry(void** state)
{
  fixture* fx = (fixture*)*state;

  /* The duplicate entry was never reachable -- find_factory() returns the
   * first match for a version -- but deinit() calls every entry's destroy
   * hook, so it freed the same factory_ctx twice and corrupted the heap. */
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(fx->client->factory_count, 1);

  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(fx->client->factory_count, 1);
}

static void a_duplicate_registration_leaves_the_connection_usable(void** state)
{
  fixture* fx = (fixture*)*state;

  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);

  /* Swallowing the duplicate must not swallow the registration itself. */
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(mock);
  assert_true(az_iot_mock_mqtt_client_inject_connected(mock, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(fx->client, 0), AZ_IOT_OK);
  assert_true(fx->rec.count > 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTED);
}

static void a_distinct_factory_for_the_same_version_still_registers(void** state)
{
  fixture* fx = (fixture*)*state;

  /* Only an EXACT duplicate is folded away. A different factory is a different
   * registration, even when it serves a version that already has one. */
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(fx->client->factory_count, 1);

  az_iot_mqtt_factory* other = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(other);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(fx->client, other), AZ_IOT_OK);
  assert_int_equal(fx->client->factory_count, 2);

  /* Both are now adopted by the client, which frees each exactly once from
   * deinit(); the fixture teardown must not free either. */
}

static void open_without_factory_returns_not_supported(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_ERR_NOT_SUPPORTED);
  /* The attempt is reported although it failed before anything was sent. */
  assert_int_equal(fx->rec.count, 2);
  assert_int_equal(fx->rec.states[0], AZ_IOT_CONN_STATE_SETTING_UP);
  assert_int_equal(fx->rec.states[1], AZ_IOT_CONN_STATE_IDLE);
  assert_int_equal(fx->rec.reasons[1], AZ_IOT_ERR_NOT_SUPPORTED);
}

static void open_invokes_connect_and_transitions_to_connecting(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  /* Fired twice: IDLE -> SETTING_UP -> CONNECTING. */
  assert_int_equal(fx->rec.count, 2);
  assert_int_equal(fx->rec.states[0], AZ_IOT_CONN_STATE_SETTING_UP);
  assert_int_equal(fx->rec.states[1], AZ_IOT_CONN_STATE_CONNECTING);

  /* The factory created exactly one client and connect() was issued on it. */
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  assert_int_equal(az_iot_mock_mqtt_client_call_count(m), 1);
  const az_iot_mock_call* c0 = az_iot_mock_mqtt_client_call_at(m, 0);
  assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_CONNECT);
  assert_string_equal(c0->topic, "broker.example");
}

static void connected_event_transitions_to_connected(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));

  assert_int_equal(az_iot_connection_client_do_work(fx->client, 0), AZ_IOT_OK);

  /* SETTING_UP and CONNECTING (from open), then CONNECTED (from event). */
  assert_int_equal(fx->rec.count, 3);
  assert_int_equal(fx->rec.states[1], AZ_IOT_CONN_STATE_CONNECTING);
  assert_int_equal(fx->rec.states[2], AZ_IOT_CONN_STATE_CONNECTED);
  assert_int_equal(fx->rec.reasons[2], AZ_IOT_OK);
}

static void connack_failure_transitions_to_faulted(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_AUTH));
  assert_int_equal(az_iot_connection_client_do_work(fx->client, 0), AZ_IOT_OK);

  /* SETTING_UP, CONNECTING, then FAULTED with the reason from the CONNACK. */
  assert_int_equal(fx->rec.count, 3);
  assert_int_equal(fx->rec.states[2], AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->rec.reasons[2], AZ_IOT_ERR_AUTH);

  /* Active adapter was torn down (last_client cache cleared by mock_destroy). */
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

static void close_disconnect_returns_to_idle(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  assert_int_equal(az_iot_connection_client_do_work(fx->client, 0), AZ_IOT_OK);

  /* Initiate close; expect DISCONNECTING transition + disconnect call. */
  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_DISCONNECTING);

  /* Inject DISCONNECTED and pump - should transition to IDLE and tear down adapter. */
  az_iot_mqtt_event evt = { 0 };
  evt.kind = AZ_IOT_MQTT_EVT_DISCONNECTED;
  evt.status = AZ_IOT_OK;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &evt));
  assert_int_equal(az_iot_connection_client_do_work(fx->client, 0), AZ_IOT_OK);

  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_IDLE);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

static void close_when_idle_is_noop(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(fx->rec.count, 0);
}

static void open_twice_is_rejected(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_ERR_ALREADY_INITIALIZED);
}

/* ------------------------------------------------------------------------- */
/* reconnect (Phase 2.2) tests                                               */
/* ------------------------------------------------------------------------- */

static size_t count_states(const state_record* r, az_iot_connection_state s)
{
  size_t n = 0;
  for (size_t i = 0; i < r->count; ++i)
  {
    if (r->states[i] == s)
    {
      ++n;
    }
  }
  return n;
}

static void connack_fail_with_reconnect_schedules_retry(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_AUTH));
  /* First do_work delivers the failed CONNACK; deferred apply schedules a
   * reconnect (state -> RETRY_PENDING) and tears down the active adapter. */
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));

  /* After the reconnect delay elapses, the next do_work fires another connect. */
  wait_ms(100);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTING);
  az_iot_mock_mqtt_client* m2 = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m2);
  /* m2 may legally alias m's old address (the heap reuses freed slots);
   * what matters is that a fresh client exists and CONNECT was issued on it. */
  assert_int_equal(az_iot_mock_mqtt_client_call_count(m2), 1);
  assert_int_equal(az_iot_mock_mqtt_client_call_at(m2, 0)->kind, AZ_IOT_MOCK_CALL_CONNECT);

  /* Now succeed on the retry. */
  assert_true(az_iot_mock_mqtt_client_inject_connected(m2, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTED);
}

static void max_attempts_exhausted_transitions_to_faulted(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  /* max_attempts == 2 means we tolerate up to 2 reconnect attempts; the
   * third unsuccessful attempt-end transitions us to FAULTED. */
  for (int i = 0; i < 3; ++i)
  {
    az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_non_null(m);
    assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_AUTH));
    (void)az_iot_connection_client_do_work(fx->client, 0);
    if (i < 2)
    {
      assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_RETRY_PENDING);
      wait_ms(100);
      (void)az_iot_connection_client_do_work(fx->client, 0);
      assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTING);
    }
  }

  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->rec.reasons[fx->rec.count - 1], AZ_IOT_ERR_AUTH);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

static void peer_disconnect_with_reconnect_drives_retry(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTED);

  /* Peer-initiated drop. */
  az_iot_mqtt_event evt = { 0 };
  evt.kind = AZ_IOT_MQTT_EVT_DISCONNECTED;
  evt.status = AZ_IOT_ERR_NOT_CONNECTED;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &evt));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_RETRY_PENDING);

  wait_ms(100);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTING);
}

static void close_during_reconnecting_goes_idle(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_ERR_AUTH));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_RETRY_PENDING);

  /* User-initiated close while waiting to reconnect: cancels the schedule. */
  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_IDLE);

  /* Subsequent do_work must not start any retries. */
  wait_ms(100);
  size_t before = fx->rec.count;
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.count, before);
  assert_int_equal(
      count_states(&fx->rec, AZ_IOT_CONN_STATE_CONNECTING), 1); /* only the initial open */
}

static void user_close_after_connected_does_not_reconnect(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTED);

  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_DISCONNECTING);

  /* Adapter delivers DISCONNECTED -> we must end up IDLE, NOT RETRY_PENDING. */
  az_iot_mqtt_event evt = { 0 };
  evt.kind = AZ_IOT_MQTT_EVT_DISCONNECTED;
  evt.status = AZ_IOT_OK;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &evt));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_IDLE);
  assert_int_equal(count_states(&fx->rec, AZ_IOT_CONN_STATE_RETRY_PENDING), 0);
}

/* ------------------------------------------------------------------------- */
/* dispatch + profile (Phase 2.3) integration                                */
/* ------------------------------------------------------------------------- */

typedef struct inbound_record
{
  size_t hits;
  char last_topic[128];
} inbound_record;

static void inbound_record_cb(void* user_ctx, const az_iot_mqtt_message* msg)
{
  inbound_record* r = (inbound_record*)user_ctx;
  r->hits++;
  if (msg && msg->topic)
  {
    size_t n = strlen(msg->topic);
    if (n >= sizeof(r->last_topic))
    {
      n = sizeof(r->last_topic) - 1;
    }
    memcpy(r->last_topic, msg->topic, n);
    r->last_topic[n] = '\0';
  }
}

static void inbound_message_routes_through_dispatch(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);

  /* The twin response prefix is the MQTTv3 wire contract, stated literally so
   * this test pins the routing rather than mirroring a table. */
  inbound_record twin_rec = { 0 };
  assert_int_equal(
      az_iot_connection_client__register_inbound_handler(
          fx->client, "$iothub/twin/res/", inbound_record_cb, &twin_rec),
      AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_true(az_iot_mock_mqtt_client_inject_message(
      m, "$iothub/twin/res/200/?$rid=42", (const uint8_t*)"{}", 2, AZ_IOT_MQTT_QOS_0));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(twin_rec.hits, 1);
  assert_string_equal(twin_rec.last_topic, "$iothub/twin/res/200/?$rid=42");

  /* Unmatched topics drop silently and don't disturb state. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m, "some/other/topic", (const uint8_t*)"x", 1, AZ_IOT_MQTT_QOS_0));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(twin_rec.hits, 1);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTED);
}

/* ------------------------------------------------------------------------- */
/* MQTTv5 presence (birth) handshake                                          */
/* ------------------------------------------------------------------------- */

/* Fixture variant: a direct HUB_MQTT_V5 (MQTTv5, MQTT v5) connection. session_role
 * becomes HUB_MQTT_V5 from opts.connection_profile, so open() drives the birth handshake
 * after CONNACK instead of announcing CONNECTED immediately. */
static int setup_mqtt_v5_ex(void** state, bool push_desired, bool push_reported)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  opts.twin_push.push_desired = push_desired;
  opts.twin_push.push_reported = push_reported;
  opts.csr_payload_buffer = az_span_create(fx->csr_buf, sizeof(fx->csr_buf));
  assert_int_equal(az_iot_test_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, on_state, &fx->rec), AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  assert_non_null(fx->factory);

  *state = fx;
  return 0;
}

static int setup_mqtt_v5(void** state) { return setup_mqtt_v5_ex(state, false, false); }

/* Fixture variant: HUB_MQTT_V5 with both twin push bits configured by the
 * application, to verify the birth advertises what was asked for. */
static int setup_mqtt_v5_twin_push(void** state) { return setup_mqtt_v5_ex(state, true, true); }

/* Most recent recorded call of `kind`, or NULL if none. */
static const az_iot_mock_call* last_call_of_kind(
    az_iot_mock_mqtt_client* m,
    az_iot_mock_call_kind kind)
{
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = n; i > 0; --i)
  {
    const az_iot_mock_call* call = az_iot_mock_mqtt_client_call_at(m, i - 1);
    if (call->kind == kind)
    {
      return call;
    }
  }
  return NULL;
}

/* Drive open -> CONNACK -> dev/presence SUBACK and return the recorded birth
 * PUBLISH. Leaves the client in CONNECTING with presence phase BIRTH. */
static const az_iot_mock_call* drive_to_birth_published(
    fixture* fx,
    bool session_present,
    az_iot_mock_mqtt_client** out_m)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);

  /* CONNACK must NOT announce CONNECTED; it subscribes to dev/presence. */
  az_iot_mqtt_event connack;
  memset(&connack, 0, sizeof(connack));
  connack.kind = AZ_IOT_MQTT_EVT_CONNECTED;
  connack.status = AZ_IOT_OK;
  connack.session_present = session_present;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &connack));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTING);
  const az_iot_mock_call* sub = last_call_of_kind(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  assert_string_equal(sub->topic, "ih/ut-device/dev/#");

  /* SUBACK -> publish the birth message. */
  az_iot_mqtt_event suback;
  memset(&suback, 0, sizeof(suback));
  suback.kind = AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK;
  suback.status = AZ_IOT_OK;
  suback.packet_id = sub->packet_id;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &suback));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTING);
  const az_iot_mock_call* birth = last_call_of_kind(m, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(birth);
  if (out_m)
  {
    *out_m = m;
  }
  return birth;
}

/* Full happy path: birth is published with the right shape, and CONNECTED is
 * announced only when a matching birth-ack echoes the nonce back. */
static void hub_mqtt_v5_births_then_connects_on_birth_ack(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = NULL;
  const az_iot_mock_call* birth = drive_to_birth_published(fx, false, &m);

  assert_string_equal(birth->topic, "ih/ut-device/srv/presence");
  assert_int_equal(birth->qos, AZ_IOT_MQTT_QOS_0);
  assert_string_equal(birth->user_type, "birth:1");
  assert_int_equal(birth->correlation_data_len, 16);
  /* proto3 Birth: session_present=false and both push bits default to false,
   * so every field is at its default and the payload is empty. */
  assert_int_equal(birth->payload_len, 0);

  /* Echo the nonce back as a birth-ack -> CONNECTED. nonce + ack_type must
   * outlive the delivering do_work below. */
  uint8_t nonce[16];
  memcpy(nonce, birth->correlation_data, sizeof(nonce));
  az_iot_mqtt_user_property ack_type = { "type", "birth-ack:1" };
  az_iot_mqtt_message ack_msg;
  memset(&ack_msg, 0, sizeof(ack_msg));
  ack_msg.topic = "ih/ut-device/dev/presence";
  ack_msg.correlation_data = nonce;
  ack_msg.correlation_data_len = sizeof(nonce);
  ack_msg.user_properties = &ack_type;
  ack_msg.user_properties_count = 1;
  az_iot_mqtt_event ack;
  memset(&ack, 0, sizeof(ack));
  ack.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  ack.message = &ack_msg;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &ack));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTED);
  assert_int_equal(fx->rec.reasons[fx->rec.count - 1], AZ_IOT_OK);
}

/* MQTTv5 requires a non-empty CONNECT username (WebhookAuthUserNameMissing
 * otherwise). It must carry the connection nonce as correlationId plus the
 * clientVersion, and the same nonce must be reused as the birth Correlation
 * Data so the service can correlate the CONNECT with the birth. */
static void hub_mqtt_v5_connect_username_carries_correlation_nonce(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = NULL;
  const az_iot_mock_call* birth = drive_to_birth_published(fx, false, &m);

  /* CONNECT is the first recorded call on the adapter. */
  const az_iot_mock_call* conn = az_iot_mock_mqtt_client_call_at(m, 0);
  assert_non_null(conn);
  assert_int_equal(conn->kind, AZ_IOT_MOCK_CALL_CONNECT);

  /* username = correlationId=<32 hex>&clientVersion=c%2F<version> */
  assert_memory_equal(conn->username, "correlationId=", 14);
  assert_non_null(strstr(conn->username, "&clientVersion=c%2F"));

  /* correlationId must equal the lowercase hex of the birth's 16-byte
   * Correlation Data (same nonce on the CONNECT and the birth). */
  assert_int_equal(birth->correlation_data_len, 16);
  static const char hexd[] = "0123456789abcdef";
  char expect_hex[33];
  for (size_t i = 0; i < 16; ++i)
  {
    expect_hex[i * 2] = hexd[(birth->correlation_data[i] >> 4) & 0x0F];
    expect_hex[i * 2 + 1] = hexd[birth->correlation_data[i] & 0x0F];
  }
  expect_hex[32] = '\0';

  const char* cid = conn->username + strlen("correlationId=");
  assert_memory_equal(cid, expect_hex, 32);
  /* SemVer characters are all URL-unreserved, so the encoded version is the
   * version string itself. */
  assert_string_equal(cid + 32, "&clientVersion=c%2F" AZ_IOT_VERSION_STRING);
}

/* A birth-ack whose correlation data doesn't match our nonce is discarded; the
 * client stays in CONNECTING waiting for the real one. */
static void hub_mqtt_v5_ignores_mismatched_birth_ack(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = NULL;
  (void)drive_to_birth_published(fx, false, &m);

  uint8_t wrong[16];
  memset(wrong, 0xAB, sizeof(wrong));
  az_iot_mqtt_user_property ack_type = { "type", "birth-ack:1" };
  az_iot_mqtt_message ack_msg;
  memset(&ack_msg, 0, sizeof(ack_msg));
  ack_msg.topic = "ih/ut-device/dev/presence";
  ack_msg.correlation_data = wrong;
  ack_msg.correlation_data_len = sizeof(wrong);
  ack_msg.user_properties = &ack_type;
  ack_msg.user_properties_count = 1;
  az_iot_mqtt_event ack;
  memset(&ack, 0, sizeof(ack));
  ack.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  ack.message = &ack_msg;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &ack));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTING);
}

/* On the MQTTv5 path it is the birth-ack, not the CONNACK, that completes
 * the connection. Suppressing only the late CONNACK would therefore leave this
 * route able to announce CONNECTED for an attempt the application has already
 * abandoned: the broker's birth-ack was on the wire before close() reached it
 * and lands in a later process_loop batch. */
static void hub_mqtt_v5_birth_ack_after_close_is_ignored(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = NULL;
  const az_iot_mock_call* birth = drive_to_birth_published(fx, false, &m);

  /* Capture the nonce before closing: it must outlive the delivering do_work. */
  uint8_t nonce[16];
  memcpy(nonce, birth->correlation_data, sizeof(nonce));

  /* The application gives up while the birth is still in flight. */
  assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);

  /* A birth-ack that matches the nonce now arrives anyway. */
  az_iot_mqtt_user_property ack_type = { "type", "birth-ack:1" };
  az_iot_mqtt_message ack_msg;
  memset(&ack_msg, 0, sizeof(ack_msg));
  ack_msg.topic = "ih/ut-device/dev/presence";
  ack_msg.correlation_data = nonce;
  ack_msg.correlation_data_len = sizeof(nonce);
  ack_msg.user_properties = &ack_type;
  ack_msg.user_properties_count = 1;
  az_iot_mqtt_event ack;
  memset(&ack, 0, sizeof(ack));
  ack.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  ack.message = &ack_msg;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &ack));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  /* CONNECTED must never be announced, at any point in the sequence. */
  for (size_t i = 0; i < fx->rec.count; ++i)
  {
    assert_int_not_equal(fx->rec.states[i], AZ_IOT_CONN_STATE_CONNECTED);
  }
}

/* CONNACK with Session Present = 1 is reflected in the birth payload (proto3
 * field 1). The push bits stay absent at their default. */
static void hub_mqtt_v5_birth_reports_session_present(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = NULL;
  const az_iot_mock_call* birth = drive_to_birth_published(fx, true, &m);

  const uint8_t expect_body[] = { 0x08, 0x01 };
  assert_int_equal(birth->payload_len, sizeof(expect_body));
  assert_memory_equal(birth->payload, expect_body, sizeof(expect_body));
}

/* The birth advertises which twin traffic the application asked for, so the
 * service only dispatches what this client can consume. With both bits set the
 * payload carries proto3 fields 12 and 13. */
static void hub_mqtt_v5_birth_advertises_configured_twin_push(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = NULL;
  const az_iot_mock_call* birth = drive_to_birth_published(fx, false, &m);

  const uint8_t expect_body[] = { 0x60, 0x01, 0x68, 0x01 };
  assert_int_equal(birth->payload_len, sizeof(expect_body));
  assert_memory_equal(birth->payload, expect_body, sizeof(expect_body));
}

/* The connection nonce must be a well-formed RFC 4122 version 4 UUID: the
 * service treats it as a UUID, and the .NET client produces one via
 * Guid.NewGuid(). */
static void hub_mqtt_v5_connect_nonce_is_uuid_v4(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = NULL;
  const az_iot_mock_call* birth = drive_to_birth_published(fx, false, &m);

  assert_int_equal(birth->correlation_data_len, 16);
  assert_int_equal(birth->correlation_data[6] & 0xF0, 0x40); /* version 4   */
  assert_int_equal(birth->correlation_data[8] & 0xC0, 0x80); /* variant 10xx */
}

/* A dev/presence SUBACK that fails (e.g. the broker refused the subscription)
 * must fault the handshake instead of publishing the birth. */
static void hub_mqtt_v5_suback_failure_faults(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);

  az_iot_mqtt_event connack;
  memset(&connack, 0, sizeof(connack));
  connack.kind = AZ_IOT_MQTT_EVT_CONNECTED;
  connack.status = AZ_IOT_OK;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &connack));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  const az_iot_mock_call* sub = last_call_of_kind(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);

  az_iot_mqtt_event suback;
  memset(&suback, 0, sizeof(suback));
  suback.kind = AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK;
  suback.status = AZ_IOT_ERR_MQTT;
  suback.packet_id = sub->packet_id;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &suback));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  /* No reconnect policy -> the subscribe failure faults the connection. The
   * adapter is torn down, so don't touch `m` past this point. */
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->rec.reasons[fx->rec.count - 1], AZ_IOT_ERR_MQTT);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

/* A message on dev/presence that echoes our nonce but is NOT a birth-ack (wrong
 * type) is ignored; the handshake keeps waiting (stays CONNECTING). */
static void hub_mqtt_v5_ignores_wrong_type_ack(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = NULL;
  const az_iot_mock_call* birth = drive_to_birth_published(fx, false, &m);

  uint8_t nonce[16];
  memcpy(nonce, birth->correlation_data, sizeof(nonce));
  az_iot_mqtt_user_property wrong_type = { "type", "twin-push:1" };
  az_iot_mqtt_message msg;
  memset(&msg, 0, sizeof(msg));
  msg.topic = "ih/ut-device/dev/presence";
  msg.correlation_data = nonce;
  msg.correlation_data_len = sizeof(nonce);
  msg.user_properties = &wrong_type;
  msg.user_properties_count = 1;
  az_iot_mqtt_event evt;
  memset(&evt, 0, sizeof(evt));
  evt.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  evt.message = &msg;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &evt));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTING);
}

/* MQTTv3 (v3.1.1) connections must NOT run the birth handshake: CONNACK goes
 * straight to CONNECTED and no presence publish happens. */
static void mqtt_v3_connect_skips_birth_handshake(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTED);
  /* No SUBSCRIBE to a presence topic and no birth PUBLISH were issued. */
  assert_null(last_call_of_kind(m, AZ_IOT_MOCK_CALL_PUBLISH));
  const az_iot_mock_call* sub = last_call_of_kind(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_null(sub);
}

/* ------------------------------------------------------------------------- */
/* session options per role                                                  */
/* ------------------------------------------------------------------------- */

static const uint8_t k_will_body[] = { 'g', 'o', 'n', 'e' };

/* Fixture variants that configure a Last Will, so the tests can assert the
 * core forwards it on the hub connect (and never on the DPS one). */
static int setup_mqtt_v3_with_will(void** state)
{
  int rc = setup(state);
  fixture* fx = (fixture*)*state;
  fx->client->opts.lwt.topic = "app/ut-device/gone";
  fx->client->opts.lwt.payload = k_will_body;
  fx->client->opts.lwt.payload_len = sizeof(k_will_body);
  fx->client->opts.lwt.qos = AZ_IOT_MQTT_QOS_1;
  fx->client->opts.lwt.retain = true;
  fx->client->opts.lwt.will_delay_seconds = 30;
  return rc;
}

static int setup_mqtt_v5_with_will(void** state)
{
  int rc = setup_mqtt_v5(state);
  fixture* fx = (fixture*)*state;
  fx->client->opts.lwt.topic = "app/ut-device/gone";
  fx->client->opts.lwt.payload = k_will_body;
  fx->client->opts.lwt.payload_len = sizeof(k_will_body);
  fx->client->opts.lwt.qos = AZ_IOT_MQTT_QOS_1;
  fx->client->opts.lwt.retain = true;
  fx->client->opts.lwt.will_delay_seconds = 30;
  return rc;
}

/* Open and return the CONNECT the core issued. */
static const az_iot_mock_call* connect_options_of_first_attempt(fixture* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  const az_iot_mock_call* connect = last_call_of_kind(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(connect);
  return connect;
}

/* MQTTv3 keeps a PERSISTENT session: the hub holds this device's
 * subscriptions and its queued cloud-to-device messages only while the session
 * is not clean, so connecting clean would drop whatever arrived during an
 * outage. */
static void mqtt_v3_connect_asks_to_resume_the_session(void** state)
{
  fixture* fx = (fixture*)*state;
  const az_iot_mock_call* connect = connect_options_of_first_attempt(fx);

  assert_false(connect->connect.clean_start);
  /* v3.1.1 has no property field to carry either of these. */
  assert_int_equal(connect->connect.session_expiry_seconds, 0);
  assert_int_equal(connect->connect.disconnect_reason_code, 0);
  assert_string_equal(connect->connect.lwt_topic, "");
}

/* MQTTv5 resumes its session and asks for an expiry long enough that there is
 * something left to resume. Both halves matter: a session that expires the
 * instant the connection closes is gone before any reconnect could resume it.
 * This is a transport-efficiency choice -- the presence handshake is what
 * establishes readiness on this generation either way. */
static void hub_mqtt_v5_connect_resumes_the_session_with_an_expiry(void** state)
{
  fixture* fx = (fixture*)*state;
  const az_iot_mock_call* connect = connect_options_of_first_attempt(fx);

  assert_false(connect->connect.clean_start);
  assert_int_equal(connect->connect.session_expiry_seconds, AZ_IOT_DEFAULT_SESSION_EXPIRY_SECONDS);
  /* No Will configured, so nothing to announce on an orderly close. */
  assert_int_equal(connect->connect.disconnect_reason_code, 0);
  assert_string_equal(connect->connect.lwt_topic, "");
}

/* The caller can override session continuity on a hub session, and asking for a
 * clean one must actually reach the wire. The expiry still rides along: it
 * governs what happens to THIS session once it ends, which is independent of
 * whether the previous one was discarded at CONNECT. */
static void hub_mqtt_v5_honors_a_caller_requested_clean_session(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->client->opts.session_continuity = AZ_IOT_SESSION_CONTINUITY_CLEAN;
  const az_iot_mock_call* connect = connect_options_of_first_attempt(fx);

  assert_true(connect->connect.clean_start);
  assert_int_equal(connect->connect.session_expiry_seconds, AZ_IOT_DEFAULT_SESSION_EXPIRY_SECONDS);
}

/* A caller-supplied expiry wins over the default. */
static void hub_mqtt_v5_honors_a_caller_requested_session_expiry(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->client->opts.session_expiry_seconds = 900;
  const az_iot_mock_call* connect = connect_options_of_first_attempt(fx);

  assert_int_equal(connect->connect.session_expiry_seconds, 900);
  assert_false(connect->connect.clean_start);
}

/* MQTTv3 honours the same override, but Session Expiry is an MQTT 5 property
 * and must never be sent to a v3.1.1 broker even when one was configured. */
static void mqtt_v3_honors_continuity_but_sends_no_expiry_property(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->client->opts.session_continuity = AZ_IOT_SESSION_CONTINUITY_CLEAN;
  fx->client->opts.session_expiry_seconds = 900;
  const az_iot_mock_call* connect = connect_options_of_first_attempt(fx);

  assert_true(connect->connect.clean_start);
  assert_int_equal(connect->connect.session_expiry_seconds, 0);
}

/* A Will configured by the application rides the MQTTv3 CONNECT, but the
 * v5-only parts of it do not: v3.1.1 has no Will Delay Interval and no
 * DISCONNECT reason codes. */
static void a_will_rides_the_mqtt_v3_connect_without_v5_fields(void** state)
{
  fixture* fx = (fixture*)*state;
  const az_iot_mock_call* connect = connect_options_of_first_attempt(fx);

  assert_string_equal(connect->connect.lwt_topic, "app/ut-device/gone");
  assert_int_equal(connect->connect.lwt_payload_len, sizeof(k_will_body));
  assert_memory_equal(connect->connect.lwt_payload, k_will_body, sizeof(k_will_body));
  assert_int_equal(connect->connect.lwt_qos, AZ_IOT_MQTT_QOS_1);
  assert_true(connect->connect.lwt_retain);
  assert_int_equal(connect->connect.lwt_will_delay_seconds, 0);
  assert_int_equal(connect->connect.session_expiry_seconds, 0);
  assert_int_equal(connect->connect.disconnect_reason_code, 0);
  assert_false(connect->connect.clean_start);
}

/* On MQTTv5 the Will carries its delay, the session is held open long enough
 * for that delay to mean anything, and the close announces the departure
 * instead of discarding the Will. */
static void a_will_rides_the_hub_mqtt_v5_connect_with_delay_and_reason(void** state)
{
  fixture* fx = (fixture*)*state;
  const az_iot_mock_call* connect = connect_options_of_first_attempt(fx);

  assert_string_equal(connect->connect.lwt_topic, "app/ut-device/gone");
  assert_int_equal(connect->connect.lwt_payload_len, sizeof(k_will_body));
  assert_memory_equal(connect->connect.lwt_payload, k_will_body, sizeof(k_will_body));
  assert_int_equal(connect->connect.lwt_qos, AZ_IOT_MQTT_QOS_1);
  assert_true(connect->connect.lwt_retain);
  assert_int_equal(connect->connect.lwt_will_delay_seconds, 30);
  /* The will delay is shorter than the session expiry, so the expiry is left
   * where the role put it -- MQTT 5 ends the delay at whichever comes first,
   * and the delay is already the earlier of the two. */
  assert_int_equal(connect->connect.session_expiry_seconds, AZ_IOT_DEFAULT_SESSION_EXPIRY_SECONDS);
  assert_int_equal(connect->connect.disconnect_reason_code, 0x04);
  assert_false(connect->connect.clean_start);
}

/* A will delay longer than the session expiry would otherwise be silently
 * truncated, because MQTT 5 ends the delay when the session ends. The expiry is
 * raised to cover it rather than accepting the option and ignoring it. */
static void a_will_delay_past_the_session_expiry_extends_it(void** state)
{
  fixture* fx = (fixture*)*state;
  fx->client->opts.session_expiry_seconds = 60;
  fx->client->opts.lwt.will_delay_seconds = 300;
  const az_iot_mock_call* connect = connect_options_of_first_attempt(fx);

  assert_int_equal(connect->connect.lwt_will_delay_seconds, 300);
  assert_int_equal(connect->connect.session_expiry_seconds, 300);
}

/* The session options are a property of the ROLE, not of what the broker
 * answered: a resumed session must not change what the next CONNECT asks for. */
static void hub_mqtt_v5_session_options_do_not_depend_on_session_present(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = NULL;
  (void)drive_to_birth_published(fx, true, &m);

  const az_iot_mock_call* connect = last_call_of_kind(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(connect);
  assert_false(connect->connect.clean_start);
  assert_int_equal(connect->connect.session_expiry_seconds, AZ_IOT_DEFAULT_SESSION_EXPIRY_SECONDS);
}

/* With no reconnection policy, a birth-ack that never arrives faults the client
 * once the handshake deadline passes. */
static void hub_mqtt_v5_birth_ack_timeout_faults(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);

  az_iot_mqtt_event connack;
  memset(&connack, 0, sizeof(connack));
  connack.kind = AZ_IOT_MQTT_EVT_CONNECTED;
  connack.status = AZ_IOT_OK;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &connack));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTING);

  /* Force the handshake deadline to expire, then pump: FAULTED with TIMEOUT. */
  az_iot_connection_client__presence_force_timeout(fx->client);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->rec.reasons[fx->rec.count - 1], AZ_IOT_ERR_TIMEOUT);
  assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

/* Drive the handshake to CONNECTED with `payload` as the birth-ack body. */
static void drive_to_connected_with_birth_ack(
    fixture* fx,
    const uint8_t* payload,
    size_t payload_len)
{
  az_iot_mock_mqtt_client* m = NULL;
  const az_iot_mock_call* birth = drive_to_birth_published(fx, false, &m);

  uint8_t nonce[16];
  memcpy(nonce, birth->correlation_data, sizeof(nonce));
  az_iot_mqtt_user_property ack_type = { "type", "birth-ack:1" };
  az_iot_mqtt_message ack_msg;
  memset(&ack_msg, 0, sizeof(ack_msg));
  ack_msg.topic = "ih/ut-device/dev/presence";
  ack_msg.correlation_data = nonce;
  ack_msg.correlation_data_len = sizeof(nonce);
  ack_msg.user_properties = &ack_type;
  ack_msg.user_properties_count = 1;
  ack_msg.payload = payload;
  ack_msg.payload_len = payload_len;
  az_iot_mqtt_event ack;
  memset(&ack, 0, sizeof(ack));
  ack.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  ack.message = &ack_msg;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &ack));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTED);
}

/* The birth-ack carries the authoritative twin versions as of birth admission.
 * The twin client needs them as the if_match anchor for its next reported
 * patch, so they must be decoded and retained, not discarded with the ack. */
static void hub_mqtt_v5_birth_ack_records_twin_versions(void** state)
{
  fixture* fx = (fixture*)*state;
  /* proto3 BirthAck: desired_version(10)=7, reported_version(11)=300. */
  const uint8_t ack_body[] = { 0x50, 0x07, 0x58, 0xAC, 0x02 };
  drive_to_connected_with_birth_ack(fx, ack_body, sizeof(ack_body));

  uint64_t desired = 0, reported = 0;
  assert_int_equal(
      az_iot_connection_client__presence_twin_versions(fx->client, &desired, &reported), AZ_IOT_OK);
  assert_int_equal(desired, 7);
  assert_int_equal(reported, 300);
}

/* proto3 omits default-valued fields, so an empty birth-ack body is legal and
 * means both versions are 0. */
static void hub_mqtt_v5_birth_ack_without_versions_yields_zero(void** state)
{
  fixture* fx = (fixture*)*state;
  drive_to_connected_with_birth_ack(fx, NULL, 0);

  uint64_t desired = 1, reported = 1;
  assert_int_equal(
      az_iot_connection_client__presence_twin_versions(fx->client, &desired, &reported), AZ_IOT_OK);
  assert_int_equal(desired, 0);
  assert_int_equal(reported, 0);
}

/* Fields the SDK does not know (a later schema revision adding per-feature
 * recovery state) must be skipped rather than aborting the decode, so the
 * versions that follow them are still read. */
static void hub_mqtt_v5_birth_ack_skips_unknown_fields(void** state)
{
  fixture* fx = (fixture*)*state;
  const uint8_t ack_body[] = {
    0x12, 0x03, 0x61, 0x62, 0x63, /* f2  length-delimited "abc"  */
    0x50, 0x07, /* f10 desired_version  = 7    */
    0x2D, 0x01, 0x02, 0x03, 0x04, /* f5  32-bit                  */
    0x58, 0xAC, 0x02, /* f11 reported_version = 300  */
    0x41, 0,    0,    0,    0,    0, 0, 0, 0, /* f8  64-bit                  */
  };
  drive_to_connected_with_birth_ack(fx, ack_body, sizeof(ack_body));

  uint64_t desired = 0, reported = 0;
  assert_int_equal(
      az_iot_connection_client__presence_twin_versions(fx->client, &desired, &reported), AZ_IOT_OK);
  assert_int_equal(desired, 7);
  assert_int_equal(reported, 300);
}

/* A twin version is a uint64, and proto3 spends 10 bytes on any value with bit
 * 63 set — the widest legal varint. The 10th byte contributes only bit 63, so
 * the shift bound has to admit shift == 63 and stop after it. Getting that
 * boundary wrong would not fail loudly: the decode would abort mid-message and
 * silently drop every field after the version, leaving the device to patch
 * against a stale if_match. */
static void hub_mqtt_v5_birth_ack_decodes_ten_byte_versions(void** state)
{
  fixture* fx = (fixture*)*state;
  const uint8_t ack_body[] = {
    /* f10 desired_version = UINT64_MAX */
    0x50,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0xFF,
    0x01,
    /* f11 reported_version = 2^63 */
    0x58,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x01,
  };
  drive_to_connected_with_birth_ack(fx, ack_body, sizeof(ack_body));

  uint64_t desired = 0, reported = 0;
  assert_int_equal(
      az_iot_connection_client__presence_twin_versions(fx->client, &desired, &reported), AZ_IOT_OK);
  assert_true(desired == UINT64_MAX);
  assert_true(reported == (uint64_t)1 << 63);
}

/* Eleven bytes cannot encode a uint64, so the value is corrupt and everything
 * after it is unparseable. Stop rather than accept a truncated interpretation. */
static void hub_mqtt_v5_birth_ack_rejects_over_long_varint(void** state)
{
  fixture* fx = (fixture*)*state;
  const uint8_t ack_body[] = {
    0x50, 0x07, /* f10 desired_version = 7 */
    0x58, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, /* f11, 11-byte varint     */
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01,
  };
  drive_to_connected_with_birth_ack(fx, ack_body, sizeof(ack_body));

  uint64_t desired = 0, reported = 99;
  assert_int_equal(
      az_iot_connection_client__presence_twin_versions(fx->client, &desired, &reported), AZ_IOT_OK);
  assert_int_equal(desired, 7);
  assert_int_equal(reported, 0);
}

/* A ten-byte varint whose last byte carries more than bit 63 encodes a value
 * uint64_t cannot hold. The surplus bits shift out, so accepting it would turn
 * a corrupt version into a plausible wrapped one and the device would anchor
 * its next reported patch on it. Stop the decode instead. */
static void hub_mqtt_v5_birth_ack_rejects_tenth_byte_overflow(void** state)
{
  fixture* fx = (fixture*)*state;
  const uint8_t ack_body[] = {
    0x50,
    0x07, /* f10 desired_version = 7 */
    /* f11, 10 bytes but the last carries 0x03: bit 63 plus a bit 64 that does
     * not fit. Shifting drops the surplus and leaves a plausible 2^63, which
     * is exactly the wrapped value that must not be accepted. */
    0x58,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x80,
    0x03,
  };
  drive_to_connected_with_birth_ack(fx, ack_body, sizeof(ack_body));

  uint64_t desired = 0, reported = 99;
  assert_int_equal(
      az_iot_connection_client__presence_twin_versions(fx->client, &desired, &reported), AZ_IOT_OK);
  assert_int_equal(desired, 7);
  assert_int_equal(reported, 0);
}

/* A truncated body (varint with the continuation bit set at the end) must stop
 * the decode without reading past the buffer. */
static void hub_mqtt_v5_birth_ack_truncated_payload_is_safe(void** state)
{
  fixture* fx = (fixture*)*state;
  const uint8_t ack_body[] = { 0x50, 0x07, 0x58, 0xAC }; /* f11 varint cut short */
  drive_to_connected_with_birth_ack(fx, ack_body, sizeof(ack_body));

  uint64_t desired = 0, reported = 99;
  assert_int_equal(
      az_iot_connection_client__presence_twin_versions(fx->client, &desired, &reported), AZ_IOT_OK);
  assert_int_equal(desired, 7);
  assert_int_equal(reported, 0);
}

/* D2: request_operational_certificate requires a certificate_provider whose
 * vtable exposes get_csr (ABI version >= 2). open() must reject otherwise. */
static void open_rejects_operational_cert_without_csr_provider(void** state)
{
  (void)state;

  az_iot_connection_client_options opts = { 0 };
  opts.client_id = "ut-device";
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-device";
  opts.dps.request_operational_certificate = true;

  /* Case 1: no certificate_provider at all: refused as missing, before the CSR
   * capability is considered. */
  az_iot_connection_client c1;
  assert_int_equal(az_iot_connection_client_init(&c1, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c1), AZ_IOT_ERR_CREDENTIAL_INCOMPLETE);
  az_iot_connection_client_deinit(&c1);

  /* Case 2: a v2 provider that does not implement get_csr (all hooks NULL;
   * open() rejects before any hook is invoked). */
  static const az_iot_certificate_provider_vtable no_csr_vtable = {
    .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  };
  az_iot_certificate_provider prov = { .vtable = &no_csr_vtable };
  opts.certificate_provider = &prov;

  az_iot_connection_client c2;
  assert_int_equal(az_iot_test_connection_client_init(&c2, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c2), AZ_IOT_ERR_NOT_SUPPORTED);
  az_iot_connection_client_deinit(&c2);
}

/* ---- DPS CSR issuance flow (increment 3) ---- */

typedef struct fake_csr_provider
{
  az_iot_certificate_provider base;
  int get_csr_calls;
  int store_calls;
  size_t stored_count;
  char stored_leaf[256];
  az_iot_cert_role last_load_role;
} fake_csr_provider;

static az_iot_result fake_csr_load(
    az_iot_certificate_provider* s,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  fake_csr_provider* f = (fake_csr_provider*)s;
  f->last_load_role = role;
  memset(out, 0, sizeof(*out));
  out->client_cert_pem = "cert";
  out->client_key_pem = "key";
  return AZ_IOT_OK;
}
static void fake_csr_release(az_iot_certificate_provider* s, az_iot_certificate_material* m)
{
  (void)s;
  (void)m;
}
static void fake_csr_destroy(az_iot_certificate_provider* s) { (void)s; }
static az_iot_result fake_get_csr(
    az_iot_certificate_provider* s,
    const char* cn,
    az_iot_certificate_signing_request* out)
{
  fake_csr_provider* f = (fake_csr_provider*)s;
  (void)cn;
  f->get_csr_calls++;
  out->csr_base64 = "TESTCSRBASE64==";
  return AZ_IOT_OK;
}
static void fake_release_csr(
    az_iot_certificate_provider* s,
    az_iot_certificate_signing_request* csr)
{
  (void)s;
  (void)csr;
}
static az_iot_result fake_store(
    az_iot_certificate_provider* s,
    const az_iot_issued_certificate* issued)
{
  fake_csr_provider* f = (fake_csr_provider*)s;
  f->store_calls++;
  f->stored_count = issued->count;
  if (issued->count > 0)
  {
    az_span leaf = issued->certificates[0];
    size_t n = (size_t)az_span_size(leaf);
    if (n >= sizeof(f->stored_leaf))
    {
      n = sizeof(f->stored_leaf) - 1;
    }
    memcpy(f->stored_leaf, az_span_ptr(leaf), n);
    f->stored_leaf[n] = '\0';
  }
  return AZ_IOT_OK;
}
static const az_iot_certificate_provider_vtable k_fake_csr_vtable = {
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = fake_csr_load,
  .release = fake_csr_release,
  .deinit = fake_csr_destroy,
  .get_csr = fake_get_csr,
  .release_csr = fake_release_csr,
  .store_issued_certificate = fake_store,
};

static int g_dps_op_cert_count = 0;
static size_t g_dps_op_cert_chain = 0;
static void on_dps_op_cert(const az_iot_issued_certificate* issued, void* uc)
{
  (void)uc;
  g_dps_op_cert_count++;
  g_dps_op_cert_chain = issued->count;
}

static void dps_csr_flow_sends_csr_and_stores_issued_chain(void** state)
{
  (void)state;

  fake_csr_provider prov = { 0 };
  prov.base.vtable = &k_fake_csr_vtable;

  az_iot_connection_client client;
  uint8_t csr_buf[AZ_IOT_CSR_PAYLOAD_BUFFER_MIN];
  az_iot_connection_client_options opts = { 0 };
  opts.host = NULL; /* DPS mode */
  opts.client_id = "ut-device";
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-device";
  opts.dps.request_operational_certificate = true;
  opts.certificate_provider = &prov.base;
  opts.csr_payload_buffer = az_span_create(csr_buf, sizeof(csr_buf));
  assert_int_equal(az_iot_test_connection_client_init(&client, &opts), AZ_IOT_OK);

  g_dps_op_cert_count = 0;
  g_dps_op_cert_chain = 0;
  az_iot_connection_client_set_operational_cert_callback(&client, on_dps_op_cert, NULL);

  az_iot_mqtt_factory* factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(factory);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&client, factory), AZ_IOT_OK);

  /* open() -> dps_start creates the DPS mock client and connects. */
  assert_int_equal(az_iot_connection_client_open(&client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* dps = az_iot_mock_mqtt_factory_last_client(factory);
  assert_non_null(dps);
  const az_iot_mock_call* connect = az_iot_mock_mqtt_client_last_of(dps, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(connect);
  assert_string_equal(
      connect->username, "0ne00000000/registrations/ut-device/api-version=2026-11-02-preview");

  /* CONNECTED -> subscribe. */
  assert_true(az_iot_mock_mqtt_client_inject_connected(dps, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&client, 0);

  /* SUBSCRIBE_ACK -> register publish carrying the CSR. */
  az_iot_mqtt_event suback;
  memset(&suback, 0, sizeof(suback));
  suback.kind = AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK;
  suback.status = AZ_IOT_OK;
  assert_true(az_iot_mock_mqtt_client_inject_event(dps, &suback));
  (void)az_iot_connection_client_do_work(&client, 0);

  assert_true(prov.get_csr_calls >= 1);
  bool found_csr_publish = false;
  for (size_t i = 0; i < az_iot_mock_mqtt_client_call_count(dps); ++i)
  {
    const az_iot_mock_call* call = az_iot_mock_mqtt_client_call_at(dps, i);
    if (call->kind == AZ_IOT_MOCK_CALL_PUBLISH && call->payload_len >= 8)
    {
      assert_memory_equal(call->payload, "{\"csr\":\"", 8);
      found_csr_publish = true;
    }
  }
  assert_true(found_csr_publish);

  /* ASSIGNED response carrying a two-cert issued chain. */
  const char* resp = "{\"operationId\":\"op1\",\"status\":\"assigned\","
                     "\"registrationState\":{\"registrationId\":\"ut-device\","
                     "\"assignedHub\":\"myhub.azure-devices.net\",\"deviceId\":\"ut-device\","
                     "\"issuedCertificateChain\":[\"TEEF\",\"SU5U\"]}}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      dps,
      "$dps/registrations/res/200/?$rid=1",
      (const uint8_t*)resp,
      strlen(resp),
      AZ_IOT_MQTT_QOS_1));

  /* Drive the flow to completion (message -> store -> deferred finalize ->
   * hub connect). Several do_work iterations cover the deferred steps. */
  for (int i = 0; i < 5; ++i)
  {
    (void)az_iot_connection_client_do_work(&client, 0);
  }

  /* Provider received the PEM-wrapped issued chain. */
  assert_int_equal(prov.store_calls, 1);
  assert_int_equal((int)prov.stored_count, 2);
  /* The stored leaf is the wire base64 DER (providers PEM-wrap when persisting). */
  assert_string_equal(prov.stored_leaf, "TEEF");

  /* Hub connect selected the OPERATIONAL identity. */
  assert_int_equal(prov.last_load_role, AZ_IOT_CRED_OPERATIONAL);

  /* The app operational-cert callback (D4) also fired with the chain. */
  assert_int_equal(g_dps_op_cert_count, 1);
  assert_int_equal((int)g_dps_op_cert_chain, 2);

  az_iot_connection_client_deinit(&client);
}

/* ---- Runtime Hub-side CSR renewal (increment 4) ---- */

typedef struct csr_test_ctx
{
  int accepted, issued, failed;
  size_t issued_count;
  char issued_leaf[128];
  int32_t service_code;
  uint32_t retry_after_s;
} csr_test_ctx;

static void on_csr_evt(const az_iot_csr_event* evt, void* uc)
{
  csr_test_ctx* t = (csr_test_ctx*)uc;
  switch (evt->kind)
  {
    case AZ_IOT_CSR_ACCEPTED:
      t->accepted++;
      break;
    case AZ_IOT_CSR_ISSUED:
      t->issued++;
      if (evt->issued)
      {
        t->issued_count = evt->issued->count;
        if (evt->issued->count > 0)
        {
          az_span leaf = evt->issued->certificates[0];
          size_t n = (size_t)az_span_size(leaf);
          if (n >= sizeof(t->issued_leaf))
          {
            n = sizeof(t->issued_leaf) - 1;
          }
          memcpy(t->issued_leaf, az_span_ptr(leaf), n);
          t->issued_leaf[n] = '\0';
        }
      }
      break;
    case AZ_IOT_CSR_FAILED:
      t->failed++;
      t->service_code = evt->service_code;
      t->retry_after_s = evt->retry_after_s;
      break;
  }
}

/* Drive the fixture client to CONNECTED and return its mock client. */
static az_iot_mock_mqtt_client* connect_fixture(fixture* fx)
{
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  return m;
}

/* request_operational_certificate also requires opts.csr_payload_buffer (the SDK
 * declares no payload buffer of its own). open() must reject when it is empty. */
static void open_rejects_operational_cert_without_payload_buffer(void** state)
{
  (void)state;

  fake_csr_provider prov = { 0 };
  prov.base.vtable = &k_fake_csr_vtable;

  az_iot_connection_client client;
  az_iot_connection_client_options opts = { 0 };
  opts.client_id = "ut-device";
  opts.dps.id_scope = "0ne00000000";
  opts.dps.registration_id = "ut-device";
  opts.dps.request_operational_certificate = true;
  opts.certificate_provider = &prov.base;
  /* csr_payload_buffer intentionally left empty (AZ_SPAN_EMPTY). */
  assert_int_equal(az_iot_test_connection_client_init(&client, &opts), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(&client), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  az_iot_connection_client_deinit(&client);
}

static void send_csr_two_phase_delivers_issued_chain(void** state)
{
  fixture* fx = *state;
  az_iot_mock_mqtt_client* m = connect_fixture(fx);

  csr_test_ctx tc = { 0 };
  az_iot_certificate_signing_request csr = { .csr_base64 = "TESTCSR==" };
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-1234", NULL, on_csr_evt, &tc),
      AZ_IOT_OK);

  /* The request must be published to the issueCertificate topic with an
   * {"id":...,"csr":...} body carrying the connected device id. */
  bool found = false;
  for (size_t i = 0; i < az_iot_mock_mqtt_client_call_count(m); ++i)
  {
    const az_iot_mock_call* call = az_iot_mock_mqtt_client_call_at(m, i);
    if (call->kind == AZ_IOT_MOCK_CALL_PUBLISH
        && strstr(call->topic, "issueCertificate/?$rid=req-1234"))
    {
      char pbuf[512];
      size_t plen = call->payload_len < sizeof(pbuf) - 1 ? call->payload_len : sizeof(pbuf) - 1;
      memcpy(pbuf, call->payload, plen);
      pbuf[plen] = '\0';
      assert_non_null(strstr(pbuf, "\"id\":\"ut-device\""));
      assert_non_null(strstr(pbuf, "\"csr\":\"TESTCSR==\""));
      found = true;
    }
  }
  assert_true(found);

  /* 202 Accepted. */
  const char* r202 = "{\"correlationId\":\"x\"}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m,
      "$iothub/credentials/res/202/?$rid=req-1234",
      (const uint8_t*)r202,
      strlen(r202),
      AZ_IOT_MQTT_QOS_1));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(tc.accepted, 1);
  assert_int_equal(tc.issued, 0);

  /* 200 Issued with a two-cert chain. */
  const char* r200 = "{\"correlationId\":\"x\",\"certificates\":[\"TEEF\",\"SU5U\"]}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m,
      "$iothub/credentials/res/200/?$rid=req-1234",
      (const uint8_t*)r200,
      strlen(r200),
      AZ_IOT_MQTT_QOS_1));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(tc.issued, 1);
  assert_int_equal((int)tc.issued_count, 2);
  /* The delivered leaf is the wire base64 DER (apps/providers wrap if needed). */
  assert_string_equal(tc.issued_leaf, "TEEF");
}

static void send_csr_error_reports_service_code(void** state)
{
  fixture* fx = *state;
  az_iot_mock_mqtt_client* m = connect_fixture(fx);

  csr_test_ctx tc = { 0 };
  az_iot_certificate_signing_request csr = { .csr_base64 = "TESTCSR==" };
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-err", "*", on_csr_evt, &tc),
      AZ_IOT_OK);

  const char* err = "{\"errorCode\":409005,\"message\":\"conflict\",\"retryAfter\":5}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m,
      "$iothub/credentials/res/409/?$rid=req-err",
      (const uint8_t*)err,
      strlen(err),
      AZ_IOT_MQTT_QOS_1));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(tc.failed, 1);
  assert_int_equal(tc.service_code, 409005);
  assert_int_equal((int)tc.retry_after_s, 5);
}

static void send_csr_cancel_frees_slot(void** state)
{
  fixture* fx = *state;
  (void)connect_fixture(fx);

  csr_test_ctx tc = { 0 };
  az_iot_certificate_signing_request csr = { .csr_base64 = "TESTCSR==" };

  /* Nothing in flight: cancel reports NOT_FOUND. */
  assert_int_equal(az_iot_connection_client_cancel_csr(fx->client), AZ_IOT_ERR_NOT_FOUND);

  /* One in flight; a second is rejected BUSY. */
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-a", NULL, on_csr_evt, &tc),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-b", NULL, on_csr_evt, &tc),
      AZ_IOT_ERR_BUSY);

  /* Cancel frees the slot without firing a callback; a new send then succeeds. */
  assert_int_equal(az_iot_connection_client_cancel_csr(fx->client), AZ_IOT_OK);
  assert_int_equal(tc.accepted + tc.issued + tc.failed, 0);
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-c", NULL, on_csr_evt, &tc),
      AZ_IOT_OK);
}

/* MQTTv5 requires MQTT v5. With only a v3.1.1 factory registered there is no
 * adapter that can speak the protocol, and open() must say so rather than
 * silently downgrading to an MQTTv3 session. */
static void hub_mqtt_v5_without_v5_factory_is_not_supported(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;

  az_iot_connection_client c;
  assert_int_equal(az_iot_test_connection_client_init(&c, &opts), AZ_IOT_OK);
  az_iot_mqtt_factory* v3 = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, v3), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_ERR_NOT_SUPPORTED);
  az_iot_connection_client_deinit(&c);
}

/* If the birth PUBLISH itself cannot be handed to the adapter the handshake
 * can never complete, so the attempt must end rather than sit in CONNECTING
 * waiting for an ack that was never solicited. */
static void hub_mqtt_v5_birth_publish_failure_faults(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  const az_iot_mock_call* sub = last_call_of_kind(m, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub);
  az_iot_mock_mqtt_client_set_next_result(m, AZ_IOT_MOCK_CALL_PUBLISH, AZ_IOT_ERR_MQTT);
  assert_true(az_iot_mock_mqtt_client_inject_suback(m, sub->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_FAULTED);
}

/* Presence traffic that arrives before the subscription is confirmed cannot be
 * an ack for a birth we have not published yet. Accepting it would announce
 * CONNECTED without ever completing the handshake. */
static void hub_mqtt_v5_birth_ack_before_suback_is_ignored(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  /* No birth has been published, so no nonce exists to match. */
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m, "ih/ut-device/dev/presence", (const uint8_t*)"", 0, AZ_IOT_MQTT_QOS_0));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTING);
  assert_false(az_iot_connection_client__is_connected(fx->client));
}

/* Fixture variant: HUB_MQTT_V5 with a reconnection policy, so a stalled handshake
 * retries instead of faulting. */
static int setup_mqtt_v5_with_reconnect(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  opts.reconnection_policy.initial_delay_ms = 20;
  opts.reconnection_policy.max_delay_ms = 20;
  opts.reconnection_policy.max_attempts = 3;
  opts.reconnection_policy.jitter_pct = 0;
  assert_int_equal(az_iot_test_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(fx->client, on_state, &fx->rec), AZ_IOT_OK);
  az_iot_connection_client__seed_rng(fx->client, 0xC0FFEEFEEDFACEull);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  assert_non_null(fx->factory);

  *state = fx;
  return 0;
}

/* The nonce identifies one connection attempt. If a retry reused it, a
 * birth-ack left over from the abandoned attempt would satisfy the new
 * handshake and the client would announce CONNECTED on a session the service
 * never acknowledged. */
static void hub_mqtt_v5_birth_timeout_retries_with_a_new_nonce(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = NULL;
  const az_iot_mock_call* first_birth = drive_to_birth_published(fx, false, &m);

  uint8_t first_nonce[64];
  size_t first_len = first_birth->correlation_data_len;
  assert_true(first_len > 0);
  memcpy(first_nonce, first_birth->correlation_data, first_len);

  /* Abandon the handshake; the policy turns it into a retry. */
  az_iot_connection_client__presence_force_timeout(fx->client);
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_RETRY_PENDING);

  wait_ms(25);
  (void)az_iot_connection_client_do_work(fx->client, 0);

  az_iot_mock_mqtt_client* m2 = az_iot_mock_mqtt_factory_last_client(fx->factory);
  assert_non_null(m2);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m2, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  const az_iot_mock_call* sub2 = last_call_of_kind(m2, AZ_IOT_MOCK_CALL_SUBSCRIBE);
  assert_non_null(sub2);
  assert_true(az_iot_mock_mqtt_client_inject_suback(m2, sub2->packet_id, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  const az_iot_mock_call* second_birth = last_call_of_kind(m2, AZ_IOT_MOCK_CALL_PUBLISH);
  assert_non_null(second_birth);
  assert_int_equal(second_birth->correlation_data_len, first_len);
  assert_memory_not_equal(second_birth->correlation_data, first_nonce, first_len);
}

/* Most recent PUBLISH to the issueCertificate topic, or NULL. */
static const az_iot_mock_call* last_csr_publish(az_iot_mock_mqtt_client* m)
{
  size_t n = az_iot_mock_mqtt_client_call_count(m);
  for (size_t i = n; i > 0; --i)
  {
    const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i - 1);
    if (c->kind == AZ_IOT_MOCK_CALL_PUBLISH && strstr(c->topic, "issueCertificate"))
    {
      return c;
    }
  }
  return NULL;
}

/* NUL-terminated copy of a recorded payload, so string searches are safe. */
static void copy_payload(const az_iot_mock_call* c, char* out, size_t cap)
{
  size_t n = c->payload_len < cap - 1 ? c->payload_len : cap - 1;
  memcpy(out, c->payload, n);
  out[n] = '\0';
}

/* The response filter has to be in place before the request goes out, or the
 * answer to a renewal arrives on a topic nobody is listening to. */
static void send_csr_subscribes_the_credentials_response_filter(void** state)
{
  fixture* fx = *state;
  az_iot_mock_mqtt_client* m = connect_fixture(fx);

  csr_test_ctx tc = { 0 };
  az_iot_certificate_signing_request csr = { .csr_base64 = "TESTCSR==" };
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-sub", NULL, on_csr_evt, &tc),
      AZ_IOT_OK);

  bool subscribed = false;
  bool published = false;
  size_t publish_at = 0;
  size_t subscribe_at = 0;
  for (size_t i = 0; i < az_iot_mock_mqtt_client_call_count(m); ++i)
  {
    const az_iot_mock_call* call = az_iot_mock_mqtt_client_call_at(m, i);
    if (!subscribed && call->kind == AZ_IOT_MOCK_CALL_SUBSCRIBE
        && strcmp(call->topic, "$iothub/credentials/res/#") == 0)
    {
      subscribed = true;
      subscribe_at = i;
    }
    if (!published && call->kind == AZ_IOT_MOCK_CALL_PUBLISH
        && strstr(call->topic, "issueCertificate"))
    {
      published = true;
      publish_at = i;
    }
  }
  /* The FIRST of each: keeping the last publish would let an earlier,
   * unsubscribed request slip through as long as a later one followed. */
  assert_true(subscribed);
  assert_true(published);
  assert_true(subscribe_at < publish_at);
}

/* "replace" tells the service to supersede an existing request. Emitting it
 * unconditionally would turn every first-time enrollment into a replacement. */
static void send_csr_emits_the_replace_field_only_when_supplied(void** state)
{
  fixture* fx = *state;
  az_iot_mock_mqtt_client* m = connect_fixture(fx);

  csr_test_ctx tc = { 0 };
  az_iot_certificate_signing_request csr = { .csr_base64 = "TESTCSR==" };
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-norep", NULL, on_csr_evt, &tc),
      AZ_IOT_OK);
  const az_iot_mock_call* first = last_csr_publish(m);
  assert_non_null(first);
  char body[512];
  copy_payload(first, body, sizeof(body));
  assert_null(strstr(body, "replace"));

  assert_int_equal(az_iot_connection_client_cancel_csr(fx->client), AZ_IOT_OK);
  az_iot_mock_mqtt_client_clear_calls(m);

  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-rep", "*", on_csr_evt, &tc),
      AZ_IOT_OK);
  const az_iot_mock_call* second = last_csr_publish(m);
  assert_non_null(second);
  copy_payload(second, body, sizeof(body));
  assert_non_null(strstr(body, "\"replace\":\"*\""));
}

/* Only one renewal is open at a time, so a response carrying someone else's
 * rid is either stale or misrouted; completing on it would hand the caller a
 * chain that was never requested. */
static void a_credentials_response_for_a_different_rid_is_ignored(void** state)
{
  fixture* fx = *state;
  az_iot_mock_mqtt_client* m = connect_fixture(fx);

  csr_test_ctx tc = { 0 };
  az_iot_certificate_signing_request csr = { .csr_base64 = "TESTCSR==" };
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-mine", NULL, on_csr_evt, &tc),
      AZ_IOT_OK);

  const char* r200 = "{\"certificates\":[\"TEEF\"]}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m,
      "$iothub/credentials/res/200/?$rid=req-someone-else",
      (const uint8_t*)r200,
      strlen(r200),
      AZ_IOT_MQTT_QOS_1));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(tc.accepted + tc.issued + tc.failed, 0);
}

static void a_400_reports_the_service_code(void** state)
{
  fixture* fx = *state;
  az_iot_mock_mqtt_client* m = connect_fixture(fx);

  csr_test_ctx tc = { 0 };
  az_iot_certificate_signing_request csr = { .csr_base64 = "TESTCSR==" };
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-400", NULL, on_csr_evt, &tc),
      AZ_IOT_OK);

  const char* err = "{\"errorCode\":400001,\"message\":\"invalid request payload\"}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m,
      "$iothub/credentials/res/400/?$rid=req-400",
      (const uint8_t*)err,
      strlen(err),
      AZ_IOT_MQTT_QOS_1));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(tc.failed, 1);
  assert_int_equal(tc.service_code, 400001);
}

/* 412 is "no matching request to replace": distinguishable from a malformed
 * request so a caller can retry without the replace field. */
static void a_412_reports_the_service_code(void** state)
{
  fixture* fx = *state;
  az_iot_mock_mqtt_client* m = connect_fixture(fx);

  csr_test_ctx tc = { 0 };
  az_iot_certificate_signing_request csr = { .csr_base64 = "TESTCSR==" };
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-412", "*", on_csr_evt, &tc),
      AZ_IOT_OK);

  const char* err = "{\"errorCode\":412001,\"message\":\"precondition failed\"}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m,
      "$iothub/credentials/res/412/?$rid=req-412",
      (const uint8_t*)err,
      strlen(err),
      AZ_IOT_MQTT_QOS_1));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(tc.failed, 1);
  assert_int_equal(tc.service_code, 412001);
}

/* Throttling carries a wait hint. Losing it turns a recoverable delay into a
 * retry storm against a service that already said it was overloaded. */
static void a_429_surfaces_the_retry_after_hint(void** state)
{
  fixture* fx = *state;
  az_iot_mock_mqtt_client* m = connect_fixture(fx);

  csr_test_ctx tc = { 0 };
  az_iot_certificate_signing_request csr = { .csr_base64 = "TESTCSR==" };
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-429", NULL, on_csr_evt, &tc),
      AZ_IOT_OK);

  const char* err = "{\"errorCode\":429001,\"message\":\"throttled\",\"retryAfter\":30}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m,
      "$iothub/credentials/res/429/?$rid=req-429",
      (const uint8_t*)err,
      strlen(err),
      AZ_IOT_MQTT_QOS_1));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(tc.failed, 1);
  assert_int_equal(tc.service_code, 429001);
  assert_int_equal((int)tc.retry_after_s, 30);
}

/* A 200 whose body has no certificates array is a broken success. Reporting it
 * as issued would hand the provider an empty chain to persist over a working
 * identity. */
static void a_200_without_a_certificates_array_reports_a_failure(void** state)
{
  fixture* fx = *state;
  az_iot_mock_mqtt_client* m = connect_fixture(fx);

  csr_test_ctx tc = { 0 };
  az_iot_certificate_signing_request csr = { .csr_base64 = "TESTCSR==" };
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-bad", NULL, on_csr_evt, &tc),
      AZ_IOT_OK);

  const char* r200 = "{\"correlationId\":\"x\"}";
  assert_true(az_iot_mock_mqtt_client_inject_message(
      m,
      "$iothub/credentials/res/200/?$rid=req-bad",
      (const uint8_t*)r200,
      strlen(r200),
      AZ_IOT_MQTT_QOS_1));
  (void)az_iot_connection_client_do_work(fx->client, 0);

  assert_int_equal(tc.issued, 0);
  assert_int_equal(tc.failed, 1);
}

/* There is no session to carry the request, so refusing up front is better
 * than opening a renewal slot that can never complete. */
static void send_csr_while_disconnected_is_refused(void** state)
{
  fixture* fx = *state;

  csr_test_ctx tc = { 0 };
  az_iot_certificate_signing_request csr = { .csr_base64 = "TESTCSR==" };
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-off", NULL, on_csr_evt, &tc),
      AZ_IOT_ERR_NOT_CONNECTED);
  assert_int_equal(tc.accepted + tc.issued + tc.failed, 0);

  /* The slot was never taken, so a renewal still works once connected. */
  (void)connect_fixture(fx);
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-on", NULL, on_csr_evt, &tc),
      AZ_IOT_OK);
}

/* The service caps a CSR at 8 KB. Publishing a larger one would be rejected on
 * the wire after burning a renewal slot for the full timeout. */
static void a_csr_larger_than_the_service_cap_is_refused(void** state)
{
  fixture* fx = *state;
  (void)connect_fixture(fx);

  /* Valid base64 (length a multiple of 4, alphabet only) one quantum past the
   * cap, so it is the size that is rejected rather than the encoding. */
  size_t oversized_len = 8196;
  char* big = (char*)malloc(oversized_len + 1);
  assert_non_null(big);
  memset(big, 'A', oversized_len);
  big[oversized_len] = '\0';

  csr_test_ctx tc = { 0 };
  az_iot_certificate_signing_request csr = { .csr_base64 = big };
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &csr, "req-big", NULL, on_csr_evt, &tc),
      AZ_IOT_ERR_INVALID_ARG);
  free(big);

  /* A refused request must not consume the single renewal slot. */
  az_iot_certificate_signing_request ok_csr = { .csr_base64 = "TESTCSR==" };
  assert_int_equal(
      az_iot_connection_client_send_csr(fx->client, &ok_csr, "req-ok", NULL, on_csr_evt, &tc),
      AZ_IOT_OK);
}

/* send_csr() generates a request id when the caller does not supply one. It has
 * to be fresh every time: repeating one would make the service treat a new
 * renewal as a duplicate of the previous request. */
static void cert_util_generates_a_distinct_request_id(void** state)
{
  (void)state;
  uint64_t rng = 0;
  char a[32] = { 0 };
  char b[32] = { 0 };
  az_iot_cert_util_gen_request_id(&rng, a, sizeof(a));
  az_iot_cert_util_gen_request_id(&rng, b, sizeof(b));

  /* 8 hex digits, a separator, 8 more. */
  assert_int_equal(strlen(a), 17);
  assert_int_equal(a[8], '-');
  for (size_t i = 0; i < strlen(a); ++i)
  {
    if (i == 8)
    {
      continue;
    }
    assert_non_null(strchr("0123456789abcdefABCDEF", a[i]));
  }
  assert_string_not_equal(a, b);
}

/* ---- TLS is required: no plaintext fallback ---- */

/* load() answers with a configurable result per role. */
typedef struct tls_provider
{
  az_iot_certificate_provider base;
  az_iot_result bootstrap_rc;
  az_iot_result operational_rc;
  int load_calls;
} tls_provider;

static az_iot_result tls_load(
    az_iot_certificate_provider* s,
    az_iot_cert_role role,
    az_iot_certificate_material* out)
{
  tls_provider* p = (tls_provider*)s;
  p->load_calls++;
  memset(out, 0, sizeof(*out));
  az_iot_result rc = (role == AZ_IOT_CRED_OPERATIONAL) ? p->operational_rc : p->bootstrap_rc;
  if (rc == AZ_IOT_OK)
  {
    out->client_cert_path = "cert.pem";
    out->client_key_path = "key.pem";
  }
  return rc;
}

static const az_iot_certificate_provider_vtable k_tls_vtable = {
  .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  .load = tls_load,
  .release = fake_csr_release,
  .deinit = fake_csr_destroy,
};

/* Real init (no plaintext hook), over the mock v3.1.1 adapter. */
static az_iot_mqtt_factory* tls_client_init(
    az_iot_connection_client* client,
    tls_provider* prov,
    bool dps,
    bool reconnect)
{
  az_iot_connection_client_options opts = az_iot_connection_client_options_default();
  if (!reconnect)
  {
    opts.reconnection_policy = az_iot_connection_client_get_disabled_retry_policy();
  }
  else
  {
    opts.reconnection_policy.initial_delay_ms = 20;
    opts.reconnection_policy.max_delay_ms = 20;
    opts.reconnection_policy.jitter_pct = 0;
  }
  if (dps)
  {
    opts.dps.id_scope = "0ne00000000";
    opts.dps.registration_id = "ut-device";
  }
  else
  {
    opts.host = "broker.example";
  }
  opts.client_id = "ut-device";
  opts.certificate_provider = prov ? &prov->base : NULL;
  assert_int_equal(az_iot_connection_client_init(client, &opts), AZ_IOT_OK);
  az_iot_mqtt_factory* factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_non_null(factory);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(client, factory), AZ_IOT_OK);
  return factory;
}

static void open_without_a_certificate_provider_is_refused(void** state)
{
  (void)state;
  for (int dps = 0; dps < 2; ++dps)
  {
    az_iot_connection_client client;
    az_iot_mqtt_factory* factory = tls_client_init(&client, NULL, dps != 0, false);
    assert_int_equal(az_iot_connection_client_open(&client), AZ_IOT_ERR_CREDENTIAL_INCOMPLETE);
    assert_null(az_iot_mock_mqtt_factory_last_client(factory));
    assert_int_equal(
        az_iot_connection_client_get_state(&client, AZ_IOT_CONN_SCOPE_HUB), AZ_IOT_CONN_STATE_IDLE);
    az_iot_connection_client_deinit(&client);
  }
}

/* A provisioning session a feature client opens, without open(), is refused
 * too rather than connecting in plaintext. */
static void a_dps_session_without_a_certificate_provider_is_refused(void** state)
{
  (void)state;
  az_iot_connection_client client;
  az_iot_mqtt_factory* factory = tls_client_init(&client, NULL, true, false);
  assert_int_equal(az_iot_connection_client__dps_user_acquire(&client), AZ_IOT_OK);
  assert_int_equal(
      az_iot_connection_client__dps_session_ensure(&client), AZ_IOT_ERR_CREDENTIAL_INCOMPLETE);
  assert_null(az_iot_mock_mqtt_factory_last_client(factory));
  az_iot_connection_client__dps_user_release(&client);
  az_iot_connection_client_deinit(&client);
}

/* A provider that cannot load an identity fails the attempt: previously the
 * client connected without TLS. */
static void a_failed_load_fails_the_connect_instead_of_going_plaintext(void** state)
{
  (void)state;
  for (int dps = 0; dps < 2; ++dps)
  {
    tls_provider prov = { .base.vtable = &k_tls_vtable,
                          .bootstrap_rc = AZ_IOT_ERR_INTERNAL,
                          .operational_rc = AZ_IOT_ERR_NOT_FOUND };
    az_iot_connection_client client;
    az_iot_mqtt_factory* factory = tls_client_init(&client, &prov, dps != 0, false);
    assert_int_equal(az_iot_connection_client_open(&client), AZ_IOT_ERR_INTERNAL);
    assert_true(prov.load_calls > 0);
    assert_null(az_iot_mock_mqtt_factory_last_client(factory));
    az_iot_connection_client_deinit(&client);
  }
}

/* A provider that stops loading between attempts fails the reconnect, which
 * is retried under the policy -- it never reconnects in plaintext. */
static void a_provider_that_stops_loading_fails_the_reconnect(void** state)
{
  (void)state;
  tls_provider prov = { .base.vtable = &k_tls_vtable, .operational_rc = AZ_IOT_ERR_NOT_FOUND };
  az_iot_connection_client client;
  az_iot_mqtt_factory* factory = tls_client_init(&client, &prov, false, true);
  assert_int_equal(az_iot_connection_client_open(&client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(factory);
  assert_non_null(m);
  const az_iot_mock_call* connect = az_iot_mock_mqtt_client_last_of(m, AZ_IOT_MOCK_CALL_CONNECT);
  assert_non_null(connect);
  assert_true(connect->connect.use_tls);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&client, 0);

  prov.bootstrap_rc = AZ_IOT_ERR_INTERNAL;
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(&client, 0);
  assert_int_equal(
      az_iot_connection_client_get_state(&client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_RETRY_PENDING);

  client.reconnect_due_ms = az_iot_time_mono_ms();
  int loads_before = prov.load_calls;
  (void)az_iot_connection_client_do_work(&client, 0);
  assert_true(prov.load_calls > loads_before);
  assert_null(az_iot_mock_mqtt_factory_last_client(factory));
  assert_int_equal(
      az_iot_connection_client_get_state(&client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_RETRY_PENDING);
  az_iot_connection_client_deinit(&client);
}

/* ---- every failed attempt is reported ---- */

/* Connected once, then dropped: HUB is RETRY_PENDING, retrying for ever. */
static az_iot_mqtt_factory* hub_retrying_for_ever(
    az_iot_connection_client* client,
    tls_provider* prov,
    az_iot_test_state_log* log)
{
  az_iot_mqtt_factory* factory = tls_client_init(client, prov, false, true);
  client->opts.reconnection_policy.max_attempts = 0;
  assert_int_equal(
      az_iot_connection_client_add_state_observer(client, az_iot_test_on_state, log), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(client), AZ_IOT_OK);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(factory);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(client, 0);
  assert_true(az_iot_mock_mqtt_client_inject_disconnected(m));
  (void)az_iot_connection_client_do_work(client, 0);
  assert_int_equal(
      az_iot_connection_client_get_state(client, AZ_IOT_CONN_SCOPE_HUB),
      AZ_IOT_CONN_STATE_RETRY_PENDING);
  return factory;
}

/* Fire the pending retry; return the log index of its RETRY_PENDING. */
static size_t fire_retry(az_iot_connection_client* client, az_iot_test_state_log* log)
{
  log->count = 0;
  client->reconnect_due_ms = az_iot_time_mono_ms();
  (void)az_iot_connection_client_do_work(client, 0);
  assert_int_equal(log->scopes[0], AZ_IOT_CONN_SCOPE_HUB);
  assert_int_equal(log->states[0], AZ_IOT_CONN_STATE_SETTING_UP);
  assert_false(log->error_present[0]);
  size_t i = az_iot_test_index_of(log, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING);
  assert_int_not_equal(i, SIZE_MAX);
  assert_int_equal(i, log->count - 1u);
  return i;
}

static void assert_local_failure(
    const az_iot_test_state_log* log,
    size_t i,
    az_iot_result reason,
    const char* step)
{
  assert_int_equal(log->reasons[i], reason);
  assert_true(log->error_present[i]);
  assert_int_equal(log->error_sources[i], AZ_IOT_CONN_ERR_SRC_LOCAL);
  assert_int_equal(log->error_codes[i], (int32_t)reason);
  assert_string_equal(log->error_message[i], step);
}

/* With retries for ever, a retry that failed before anything was sent used to
 * be invisible: HUB was already waiting to retry and the repeat was dropped. */
static void every_failed_hub_setup_is_reported_with_its_step(void** state)
{
  (void)state;
  tls_provider prov = { .base.vtable = &k_tls_vtable, .operational_rc = AZ_IOT_ERR_NOT_FOUND };
  az_iot_test_state_log log = { 0 };
  az_iot_connection_client client;
  az_iot_mqtt_factory* factory = hub_retrying_for_ever(&client, &prov, &log);

  prov.bootstrap_rc = AZ_IOT_ERR_INTERNAL;
  for (int attempt = 0; attempt < 3; ++attempt)
  {
    size_t i = fire_retry(&client, &log);
    assert_int_equal(log.count, 2);
    assert_local_failure(&log, i, AZ_IOT_ERR_INTERNAL, "certificate provider load() failed");
    assert_true(log.is_retriable[i]);
  }

  /* A different cause is reported as such. */
  client.opts.certificate_provider = NULL;
  size_t i = fire_retry(&client, &log);
  assert_local_failure(
      &log, i, AZ_IOT_ERR_CREDENTIAL_INCOMPLETE, "no certificate provider and no SAS key");
  assert_false(log.is_retriable[i]);

  /* Recovery starts clean: CONNECTED carries no stale detail. */
  client.opts.certificate_provider = &prov.base;
  prov.bootstrap_rc = AZ_IOT_OK;
  log.count = 0;
  client.reconnect_due_ms = az_iot_time_mono_ms();
  (void)az_iot_connection_client_do_work(&client, 0);
  assert_int_equal(log.states[0], AZ_IOT_CONN_STATE_SETTING_UP);
  assert_int_equal(log.states[1], AZ_IOT_CONN_STATE_CONNECTING);
  az_iot_mock_mqtt_client* m = az_iot_mock_mqtt_factory_last_client(factory);
  assert_non_null(m);
  assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
  (void)az_iot_connection_client_do_work(&client, 0);
  assert_int_equal(
      az_iot_test_last_state_for(&log, AZ_IOT_CONN_SCOPE_HUB), AZ_IOT_CONN_STATE_CONNECTED);
  assert_false(log.error_present[log.count - 1u]);
  az_iot_connection_client_deinit(&client);
}

/* A synchronous adapter connect() failure is reported after CONNECTING. */
static void a_refused_adapter_connect_is_reported_with_its_step(void** state)
{
  (void)state;
  tls_provider prov = { .base.vtable = &k_tls_vtable, .operational_rc = AZ_IOT_ERR_NOT_FOUND };
  az_iot_test_state_log log = { 0 };
  az_iot_connection_client client;
  az_iot_mqtt_factory* factory = hub_retrying_for_ever(&client, &prov, &log);

  for (int attempt = 0; attempt < 2; ++attempt)
  {
    az_iot_mock_mqtt_factory_fail_next_connect(factory, AZ_IOT_ERR_TLS);
    size_t i = fire_retry(&client, &log);
    assert_int_equal(log.count, 3);
    assert_int_equal(log.states[1], AZ_IOT_CONN_STATE_CONNECTING);
    assert_local_failure(&log, i, AZ_IOT_ERR_TLS, "MQTT adapter connect() failed");
  }
  az_iot_connection_client_deinit(&client);
}

/* open()'s own setup failure is reported, settled at IDLE and returned. */
static void open_setup_failure_settles_idle_with_its_step(void** state)
{
  (void)state;
  tls_provider prov = { .base.vtable = &k_tls_vtable,
                        .operational_rc = AZ_IOT_ERR_NOT_FOUND,
                        .bootstrap_rc = AZ_IOT_ERR_INTERNAL };
  az_iot_test_state_log log = { 0 };
  az_iot_connection_client client;
  (void)tls_client_init(&client, &prov, false, true);
  assert_int_equal(
      az_iot_connection_client_add_state_observer(&client, az_iot_test_on_state, &log), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&client), AZ_IOT_ERR_INTERNAL);
  assert_int_equal(log.count, 2);
  assert_int_equal(log.states[0], AZ_IOT_CONN_STATE_SETTING_UP);
  assert_int_equal(log.states[1], AZ_IOT_CONN_STATE_IDLE);
  assert_local_failure(&log, 1, AZ_IOT_ERR_INTERNAL, "certificate provider load() failed");
  az_iot_connection_client_deinit(&client);
}

typedef struct closing_observer
{
  az_iot_connection_client* client;
  az_iot_connection_state on;
  bool armed;
} closing_observer;

static void close_on_state(const az_iot_connection_state_event* event, void* ctx)
{
  closing_observer* o = (closing_observer*)ctx;
  if (o->armed && event->state == o->on)
  {
    o->armed = false;
    assert_int_equal(az_iot_connection_client_close(o->client), AZ_IOT_OK);
  }
}

/* close() from either announcement of a retry cancels it: nothing connects
 * behind the application's back, and no failure is reported. */
static void close_from_a_hub_announcement_cancels_the_attempt(void** state)
{
  (void)state;
  const az_iot_connection_state k_on[]
      = { AZ_IOT_CONN_STATE_SETTING_UP, AZ_IOT_CONN_STATE_CONNECTING };
  for (size_t k = 0; k < 2; ++k)
  {
    tls_provider prov = { .base.vtable = &k_tls_vtable, .operational_rc = AZ_IOT_ERR_NOT_FOUND };
    az_iot_test_state_log log = { 0 };
    az_iot_connection_client client;
    az_iot_mqtt_factory* factory = hub_retrying_for_ever(&client, &prov, &log);
    closing_observer o = { .client = &client, .on = k_on[k], .armed = true };
    assert_int_equal(
        az_iot_connection_client_add_state_observer(&client, close_on_state, &o), AZ_IOT_OK);
    log.count = 0;
    client.reconnect_due_ms = az_iot_time_mono_ms();
    (void)az_iot_connection_client_do_work(&client, 0);

    assert_false(o.armed);
    assert_null(client.active_client);
    assert_int_equal(az_iot_test_count_state(&log, AZ_IOT_CONN_STATE_RETRY_PENDING), 0);
    assert_int_equal(
        az_iot_connection_client_get_state(&client, AZ_IOT_CONN_SCOPE_HUB), AZ_IOT_CONN_STATE_IDLE);
    /* No adapter left behind. */
    assert_null(az_iot_mock_mqtt_factory_last_client(factory));
    az_iot_connection_client_deinit(&client);
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(register_rejects_null_create, setup, teardown),
    cmocka_unit_test_setup_teardown(
        registering_the_same_factory_twice_does_not_grow_the_registry, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_duplicate_registration_leaves_the_connection_usable, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_distinct_factory_for_the_same_version_still_registers, setup, teardown),
    cmocka_unit_test_setup_teardown(open_without_factory_returns_not_supported, setup, teardown),
    cmocka_unit_test_setup_teardown(
        open_invokes_connect_and_transitions_to_connecting, setup, teardown),
    cmocka_unit_test_setup_teardown(connected_event_transitions_to_connected, setup, teardown),
    cmocka_unit_test_setup_teardown(connack_failure_transitions_to_faulted, setup, teardown),
    cmocka_unit_test_setup_teardown(close_disconnect_returns_to_idle, setup, teardown),
    cmocka_unit_test_setup_teardown(close_when_idle_is_noop, setup, teardown),
    cmocka_unit_test_setup_teardown(open_twice_is_rejected, setup, teardown),
    cmocka_unit_test_setup_teardown(
        connack_fail_with_reconnect_schedules_retry, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        max_attempts_exhausted_transitions_to_faulted, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        peer_disconnect_with_reconnect_drives_retry, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        close_during_reconnecting_goes_idle, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(
        user_close_after_connected_does_not_reconnect, setup_with_reconnect, teardown),
    cmocka_unit_test_setup_teardown(inbound_message_routes_through_dispatch, setup, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_births_then_connects_on_birth_ack, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_connect_username_carries_correlation_nonce, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_birth_reports_session_present, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_birth_advertises_configured_twin_push, setup_mqtt_v5_twin_push, teardown),
    cmocka_unit_test_setup_teardown(hub_mqtt_v5_connect_nonce_is_uuid_v4, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_birth_ack_records_twin_versions, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_birth_ack_without_versions_yields_zero, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_birth_ack_skips_unknown_fields, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_birth_ack_decodes_ten_byte_versions, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_birth_ack_rejects_over_long_varint, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_birth_ack_rejects_tenth_byte_overflow, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_birth_ack_truncated_payload_is_safe, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_ignores_mismatched_birth_ack, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_birth_ack_after_close_is_ignored, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(hub_mqtt_v5_ignores_wrong_type_ack, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(hub_mqtt_v5_suback_failure_faults, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(hub_mqtt_v5_birth_ack_timeout_faults, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(mqtt_v3_connect_skips_birth_handshake, setup, teardown),
    cmocka_unit_test_setup_teardown(mqtt_v3_connect_asks_to_resume_the_session, setup, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_connect_resumes_the_session_with_an_expiry, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_honors_a_caller_requested_clean_session, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_honors_a_caller_requested_session_expiry, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        mqtt_v3_honors_continuity_but_sends_no_expiry_property, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_will_delay_past_the_session_expiry_extends_it, setup_mqtt_v5_with_will, teardown),
    cmocka_unit_test_setup_teardown(
        a_will_rides_the_mqtt_v3_connect_without_v5_fields, setup_mqtt_v3_with_will, teardown),
    cmocka_unit_test_setup_teardown(
        a_will_rides_the_hub_mqtt_v5_connect_with_delay_and_reason,
        setup_mqtt_v5_with_will,
        teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_session_options_do_not_depend_on_session_present, setup_mqtt_v5, teardown),
    cmocka_unit_test(hub_mqtt_v5_without_v5_factory_is_not_supported),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_birth_publish_failure_faults, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_birth_ack_before_suback_is_ignored, setup_mqtt_v5, teardown),
    cmocka_unit_test_setup_teardown(
        hub_mqtt_v5_birth_timeout_retries_with_a_new_nonce, setup_mqtt_v5_with_reconnect, teardown),
    cmocka_unit_test(open_rejects_operational_cert_without_csr_provider),
    cmocka_unit_test(dps_csr_flow_sends_csr_and_stores_issued_chain),
    cmocka_unit_test(open_rejects_operational_cert_without_payload_buffer),
    cmocka_unit_test_setup_teardown(send_csr_two_phase_delivers_issued_chain, setup, teardown),
    cmocka_unit_test_setup_teardown(send_csr_error_reports_service_code, setup, teardown),
    cmocka_unit_test_setup_teardown(send_csr_cancel_frees_slot, setup, teardown),
    cmocka_unit_test_setup_teardown(
        send_csr_subscribes_the_credentials_response_filter, setup, teardown),
    cmocka_unit_test_setup_teardown(
        send_csr_emits_the_replace_field_only_when_supplied, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_credentials_response_for_a_different_rid_is_ignored, setup, teardown),
    cmocka_unit_test_setup_teardown(a_400_reports_the_service_code, setup, teardown),
    cmocka_unit_test_setup_teardown(a_412_reports_the_service_code, setup, teardown),
    cmocka_unit_test_setup_teardown(a_429_surfaces_the_retry_after_hint, setup, teardown),
    cmocka_unit_test_setup_teardown(
        a_200_without_a_certificates_array_reports_a_failure, setup, teardown),
    cmocka_unit_test_setup_teardown(send_csr_while_disconnected_is_refused, setup, teardown),
    cmocka_unit_test_setup_teardown(a_csr_larger_than_the_service_cap_is_refused, setup, teardown),
    cmocka_unit_test(cert_util_generates_a_distinct_request_id),
    cmocka_unit_test(open_without_a_certificate_provider_is_refused),
    cmocka_unit_test(a_dps_session_without_a_certificate_provider_is_refused),
    cmocka_unit_test(a_failed_load_fails_the_connect_instead_of_going_plaintext),
    cmocka_unit_test(a_provider_that_stops_loading_fails_the_reconnect),
    cmocka_unit_test(every_failed_hub_setup_is_reported_with_its_step),
    cmocka_unit_test(a_refused_adapter_connect_is_reported_with_its_step),
    cmocka_unit_test(open_setup_failure_settles_idle_with_its_step),
    cmocka_unit_test(close_from_a_hub_announcement_cancels_the_attempt),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
