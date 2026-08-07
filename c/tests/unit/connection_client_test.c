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

#include "internal/connection_client_internal.h"
#include "internal/reconnect.h"

#include "support/mock_mqtt_iface.h"

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct state_record
{
  az_iot_connection_state states[16];
  az_iot_result reasons[16];
  size_t count;
} state_record;

static void on_state(az_iot_connection_state state, az_iot_result reason, void* user_ctx)
{
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
  assert_int_equal(az_iot_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_set_state_callback(fx->client, on_state, &fx->rec), AZ_IOT_OK);

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
    az_iot_connection_client_destroy(&fx->client_storage);
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
 * RECONNECTING state would be skipped straight to CONNECTING. No jitter so
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
  assert_int_equal(az_iot_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_set_state_callback(fx->client, on_state, &fx->rec), AZ_IOT_OK);
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
   * first match for a version -- but destroy() calls every entry's destroy
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
   * destroy(); the fixture teardown must not free either. */
}

static void open_without_factory_returns_not_supported(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_ERR_NOT_SUPPORTED);
  assert_int_equal(fx->rec.count, 0);
}

static void open_invokes_connect_and_transitions_to_connecting(void** state)
{
  fixture* fx = (fixture*)*state;
  assert_int_equal(
      az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

  /* Fired once: IDLE -> CONNECTING. */
  assert_int_equal(fx->rec.count, 1);
  assert_int_equal(fx->rec.states[0], AZ_IOT_CONN_STATE_CONNECTING);

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

  /* CONNECTING (from open) then CONNECTED (from event). */
  assert_int_equal(fx->rec.count, 2);
  assert_int_equal(fx->rec.states[0], AZ_IOT_CONN_STATE_CONNECTING);
  assert_int_equal(fx->rec.states[1], AZ_IOT_CONN_STATE_CONNECTED);
  assert_int_equal(fx->rec.reasons[1], AZ_IOT_OK);
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

  /* CONNECTING then FAULTED with the reason from the failed CONNACK. */
  assert_int_equal(fx->rec.count, 2);
  assert_int_equal(fx->rec.states[1], AZ_IOT_CONN_STATE_FAULTED);
  assert_int_equal(fx->rec.reasons[1], AZ_IOT_ERR_AUTH);

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
   * reconnect (state -> RECONNECTING) and tears down the active adapter. */
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_RECONNECTING);
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
      assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_RECONNECTING);
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
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_RECONNECTING);

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
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_RECONNECTING);

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

  /* Adapter delivers DISCONNECTED -> we must end up IDLE, NOT RECONNECTING. */
  az_iot_mqtt_event evt = { 0 };
  evt.kind = AZ_IOT_MQTT_EVT_DISCONNECTED;
  evt.status = AZ_IOT_OK;
  assert_true(az_iot_mock_mqtt_client_inject_event(m, &evt));
  (void)az_iot_connection_client_do_work(fx->client, 0);
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_IDLE);
  assert_int_equal(count_states(&fx->rec, AZ_IOT_CONN_STATE_RECONNECTING), 0);
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

  /* Profile-driven prefix: feature clients in Phase 3 will get this from
   * the active profile. Here we drive the same code path by hand. */
  const az_iot_protocol_profile* p = az_iot_connection_client__profile(fx->client);
  assert_non_null(p);
  assert_int_equal(p->flavor, AZ_IOT_HUB_FLAVOR_CLASSIC);

  inbound_record twin_rec = { 0 };
  assert_int_equal(
      az_iot_connection_client__register_inbound_handler(
          fx->client, p->twin_response_topic_prefix, inbound_record_cb, &twin_rec),
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
/* AEG/Hub-Next presence (birth) handshake                                    */
/* ------------------------------------------------------------------------- */

/* Fixture variant: a direct HUB_NEXT (AEG, MQTT v5) connection. session_role
 * becomes HUB_NEXT from opts.hub_protocol, so open() drives the birth handshake
 * after CONNACK instead of announcing CONNECTED immediately. */
static int setup_next(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.hub_protocol = AZ_IOT_HUB_PROTOCOL_NEXT;
  opts.csr_payload_buffer = az_span_create(fx->csr_buf, sizeof(fx->csr_buf));
  assert_int_equal(az_iot_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_set_state_callback(fx->client, on_state, &fx->rec), AZ_IOT_OK);

  fx->factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
  assert_non_null(fx->factory);

  *state = fx;
  return 0;
}

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
static void hub_next_births_then_connects_on_birth_ack(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = NULL;
  const az_iot_mock_call* birth = drive_to_birth_published(fx, false, &m);

  assert_string_equal(birth->topic, "ih/ut-device/srv/presence");
  assert_int_equal(birth->qos, AZ_IOT_MQTT_QOS_0);
  assert_string_equal(birth->user_type, "birth:1");
  assert_int_equal(birth->correlation_data_len, 16);
  /* proto3 Birth (session_present=false): push_desired=true, push_reported=true. */
  const uint8_t expect_body[] = { 0x60, 0x01, 0x68, 0x01 };
  assert_int_equal(birth->payload_len, sizeof(expect_body));
  assert_memory_equal(birth->payload, expect_body, sizeof(expect_body));

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

/* AEG requires a non-empty CONNECT username (WebhookAuthUserNameMissing
 * otherwise). It must carry the connection nonce as correlationId plus the
 * clientVersion, and the same nonce must be reused as the birth Correlation
 * Data so the service can correlate the CONNECT with the birth. */
static void hub_next_connect_username_carries_correlation_nonce(void** state)
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

  /* correlationId must equal the uppercase hex of the birth's 16-byte
   * Correlation Data (same nonce on the CONNECT and the birth). */
  assert_int_equal(birth->correlation_data_len, 16);
  static const char hexd[] = "0123456789ABCDEF";
  char expect_hex[33];
  for (size_t i = 0; i < 16; ++i)
  {
    expect_hex[i * 2] = hexd[(birth->correlation_data[i] >> 4) & 0x0F];
    expect_hex[i * 2 + 1] = hexd[birth->correlation_data[i] & 0x0F];
  }
  expect_hex[32] = '\0';

  const char* cid = conn->username + strlen("correlationId=");
  assert_memory_equal(cid, expect_hex, 32);
  assert_int_equal(cid[32], '&');
}

/* A birth-ack whose correlation data doesn't match our nonce is discarded; the
 * client stays in CONNECTING waiting for the real one. */
static void hub_next_ignores_mismatched_birth_ack(void** state)
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

/* On the Hub-Next path it is the birth-ack, not the CONNACK, that completes
 * the connection. Suppressing only the late CONNACK would therefore leave this
 * route able to announce CONNECTED for an attempt the application has already
 * abandoned: the broker's birth-ack was on the wire before close() reached it
 * and lands in a later process_loop batch. */
static void hub_next_birth_ack_after_close_is_ignored(void** state)
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
 * field 1), on top of the always-present push_desired/push_reported. */
static void hub_next_birth_reports_session_present(void** state)
{
  fixture* fx = (fixture*)*state;
  az_iot_mock_mqtt_client* m = NULL;
  const az_iot_mock_call* birth = drive_to_birth_published(fx, true, &m);

  const uint8_t expect_body[] = { 0x08, 0x01, 0x60, 0x01, 0x68, 0x01 };
  assert_int_equal(birth->payload_len, sizeof(expect_body));
  assert_memory_equal(birth->payload, expect_body, sizeof(expect_body));
}

/* A dev/presence SUBACK that fails (e.g. the broker refused the subscription)
 * must fault the handshake instead of publishing the birth. */
static void hub_next_suback_failure_faults(void** state)
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
static void hub_next_ignores_wrong_type_ack(void** state)
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

/* Classic (v3.1.1) connections must NOT run the birth handshake: CONNACK goes
 * straight to CONNECTED and no presence publish happens. */
static void classic_connect_skips_birth_handshake(void** state)
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

/* With no reconnection policy, a birth-ack that never arrives faults the client
 * once the handshake deadline passes. */
static void hub_next_birth_ack_timeout_faults(void** state)
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

  /* Case 1: no certificate_provider at all. */
  az_iot_connection_client c1;
  assert_int_equal(az_iot_connection_client_init(&c1, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c1), AZ_IOT_ERR_NOT_SUPPORTED);
  az_iot_connection_client_destroy(&c1);

  /* Case 2: a v2 provider that does not implement get_csr (all hooks NULL;
   * open() rejects before any hook is invoked). */
  static const az_iot_certificate_provider_vtable no_csr_vtable = {
    .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
  };
  az_iot_certificate_provider prov = { .vtable = &no_csr_vtable };
  opts.certificate_provider = &prov;

  az_iot_connection_client c2;
  assert_int_equal(az_iot_connection_client_init(&c2, &opts), AZ_IOT_OK);
  assert_int_equal(az_iot_connection_client_open(&c2), AZ_IOT_ERR_NOT_SUPPORTED);
  az_iot_connection_client_destroy(&c2);
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
  assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_OK);

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

  az_iot_connection_client_destroy(&client);
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
  assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(&client), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  az_iot_connection_client_destroy(&client);
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

/* Hub-Next requires MQTT v5. With only a v3.1.1 factory registered there is no
 * adapter that can speak the protocol, and open() must say so rather than
 * silently downgrading to a Classic session. */
static void hub_next_without_v5_factory_is_not_supported(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.hub_protocol = AZ_IOT_HUB_PROTOCOL_NEXT;

  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  az_iot_mqtt_factory* v3 = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
  assert_int_equal(az_iot_connection_client_register_mqtt_factory(&c, v3), AZ_IOT_OK);

  assert_int_equal(az_iot_connection_client_open(&c), AZ_IOT_ERR_NOT_SUPPORTED);
  az_iot_connection_client_destroy(&c);
}

/* If the birth PUBLISH itself cannot be handed to the adapter the handshake
 * can never complete, so the attempt must end rather than sit in CONNECTING
 * waiting for an ack that was never solicited. */
static void hub_next_birth_publish_failure_faults(void** state)
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
static void hub_next_birth_ack_before_suback_is_ignored(void** state)
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

/* Fixture variant: HUB_NEXT with a reconnection policy, so a stalled handshake
 * retries instead of faulting. */
static int setup_next_with_reconnect(void** state)
{
  fixture* fx = (fixture*)calloc(1, sizeof(*fx));
  assert_non_null(fx);

  az_iot_connection_client_options opts = { 0 };
  opts.host = "broker.example";
  opts.port = 8883;
  opts.client_id = "ut-device";
  opts.hub_protocol = AZ_IOT_HUB_PROTOCOL_NEXT;
  opts.reconnection_policy.initial_delay_ms = 20;
  opts.reconnection_policy.max_delay_ms = 20;
  opts.reconnection_policy.max_attempts = 3;
  opts.reconnection_policy.jitter_pct = 0;
  assert_int_equal(az_iot_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
  fx->client = &fx->client_storage;
  assert_int_equal(
      az_iot_connection_client_set_state_callback(fx->client, on_state, &fx->rec), AZ_IOT_OK);
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
static void hub_next_birth_timeout_retries_with_a_new_nonce(void** state)
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
  assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_RECONNECTING);

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
        hub_next_births_then_connects_on_birth_ack, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        hub_next_connect_username_carries_correlation_nonce, setup_next, teardown),
    cmocka_unit_test_setup_teardown(hub_next_birth_reports_session_present, setup_next, teardown),
    cmocka_unit_test_setup_teardown(hub_next_ignores_mismatched_birth_ack, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        hub_next_birth_ack_after_close_is_ignored, setup_next, teardown),
    cmocka_unit_test_setup_teardown(hub_next_ignores_wrong_type_ack, setup_next, teardown),
    cmocka_unit_test_setup_teardown(hub_next_suback_failure_faults, setup_next, teardown),
    cmocka_unit_test_setup_teardown(hub_next_birth_ack_timeout_faults, setup_next, teardown),
    cmocka_unit_test_setup_teardown(classic_connect_skips_birth_handshake, setup, teardown),
    cmocka_unit_test(hub_next_without_v5_factory_is_not_supported),
    cmocka_unit_test_setup_teardown(hub_next_birth_publish_failure_faults, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        hub_next_birth_ack_before_suback_is_ignored, setup_next, teardown),
    cmocka_unit_test_setup_teardown(
        hub_next_birth_timeout_retries_with_a_new_nonce, setup_next_with_reconnect, teardown),
    cmocka_unit_test(open_rejects_operational_cert_without_csr_provider),
    cmocka_unit_test(dps_csr_flow_sends_csr_and_stores_issued_chain),
    cmocka_unit_test(open_rejects_operational_cert_without_payload_buffer),
    cmocka_unit_test_setup_teardown(send_csr_two_phase_delivers_issued_chain, setup, teardown),
    cmocka_unit_test_setup_teardown(send_csr_error_reports_service_code, setup, teardown),
    cmocka_unit_test_setup_teardown(send_csr_cancel_frees_slot, setup, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
