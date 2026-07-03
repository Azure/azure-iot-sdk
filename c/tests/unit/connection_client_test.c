// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

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

typedef struct state_record_tag
{
    az_iot_connection_state_t states[16];
    az_iot_result_t           reasons[16];
    size_t                        count;
} state_record_t;

static void on_state(az_iot_connection_state_t state, az_iot_result_t reason, void* user_ctx)
{
    state_record_t* r = (state_record_t*)user_ctx;
    if (r->count < (sizeof(r->states) / sizeof(r->states[0])))
    {
        r->states[r->count] = state;
        r->reasons[r->count] = reason;
        r->count++;
    }
}

typedef struct fixture_tag
{
    az_iot_connection_client_t  client_storage;
    az_iot_connection_client_t* client;
    az_iot_mqtt_factory_t*      factory;
    state_record_t                  rec;
} fixture_t;

static int setup(void** state)
{
    fixture_t* fx = (fixture_t*)calloc(1, sizeof(*fx));
    assert_non_null(fx);

    az_iot_connection_client_options_t opts = {0};
    opts.host = "broker.example";
    opts.port = 8883;
    opts.client_id = "ut-device";
    assert_int_equal(az_iot_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
    fx->client = &fx->client_storage;
    assert_int_equal(az_iot_connection_client_set_state_callback(fx->client, on_state, &fx->rec),
                     AZ_IOT_OK);

    fx->factory = az_iot_mock_mqtt_factory_create(
        AZ_IOT_MQTT_VERSION_3_1_1);
    assert_non_null(fx->factory);

    *state = fx;
    return 0;
}

static int teardown(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    if (fx)
    {
        /* A factory that was registered with the client is adopted by it and
         * freed from deinit() (via factory.destroy). A factory that was never
         * registered is still owned by the test, so we must destroy it here to
         * avoid leaking it. Check before deinit() clears factory_count. */
        bool factory_adopted = (fx->client->factory_count > 0);
        az_iot_connection_client_deinit(&fx->client_storage);
        if (!factory_adopted)
            az_iot_mock_mqtt_factory_destroy(fx->factory);
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
    fixture_t* fx = (fixture_t*)calloc(1, sizeof(*fx));
    assert_non_null(fx);

    az_iot_connection_client_options_t opts = {0};
    opts.host = "broker.example";
    opts.port = 8883;
    opts.client_id = "ut-device";
    opts.reconnect.initial_delay_ms = 20;
    opts.reconnect.max_delay_ms     = 20;
    opts.reconnect.max_attempts     = 2;
    opts.reconnect.jitter_pct       = 0;
    assert_int_equal(az_iot_connection_client_init(&fx->client_storage, &opts), AZ_IOT_OK);
    fx->client = &fx->client_storage;
    assert_int_equal(az_iot_connection_client_set_state_callback(fx->client, on_state, &fx->rec),
                     AZ_IOT_OK);
    az_iot_connection_client__seed_rng(fx->client, 0xC0FFEEFEEDFACEull);

    fx->factory = az_iot_mock_mqtt_factory_create(
        AZ_IOT_MQTT_VERSION_3_1_1);
    assert_non_null(fx->factory);

    *state = fx;
    return 0;
}

/* Spin until the monotonic clock advances by `ms`. Used in reconnect tests so
 * we don't depend on a sleep helper / feature macros. */
static void wait_ms(unsigned ms)
{
    uint64_t deadline = az_iot_time_mono_ms() + ms;
    while (az_iot_time_mono_ms() < deadline) { /* spin */ }
}

/* ------------------------------------------------------------------------- */
/* tests                                                                     */
/* ------------------------------------------------------------------------- */

static void register_rejects_null_create(void** state)
{
    fixture_t* fx = (fixture_t*)*state;

    /* A factory with a NULL create function is rejected. */
    az_iot_mqtt_factory_t bad = {0};
    bad.version = AZ_IOT_MQTT_VERSION_3_1_1;
    bad.create = NULL;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(fx->client, &bad),
                     AZ_IOT_ERR_INVALID_ARG);
}

static void open_without_factory_returns_not_supported(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_ERR_NOT_SUPPORTED);
    assert_int_equal(fx->rec.count, 0);
}

static void open_invokes_connect_and_transitions_to_connecting(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory),
                     AZ_IOT_OK);

    assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

    /* Fired once: IDLE -> CONNECTING. */
    assert_int_equal(fx->rec.count, 1);
    assert_int_equal(fx->rec.states[0], AZ_IOT_CONN_STATE_CONNECTING);

    /* The factory created exactly one client and connect() was issued on it. */
    az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_non_null(m);
    assert_int_equal(az_iot_mock_mqtt_client_call_count(m), 1);
    const az_iot_mock_call_t* c0 = az_iot_mock_mqtt_client_call_at(m, 0);
    assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_CONNECT);
    assert_string_equal(c0->topic, "broker.example");
}

static void connected_event_transitions_to_connected(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory),
                     AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

    az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
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
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory),
                     AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

    az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
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
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory),
                     AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

    az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
    assert_int_equal(az_iot_connection_client_do_work(fx->client, 0), AZ_IOT_OK);

    /* Initiate close; expect DISCONNECTING transition + disconnect call. */
    assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
    assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_DISCONNECTING);

    /* Inject DISCONNECTED and pump - should transition to IDLE and tear down adapter. */
    az_iot_mqtt_event_t evt = {0};
    evt.kind = AZ_IOT_MQTT_EVT_DISCONNECTED;
    evt.status = AZ_IOT_OK;
    assert_true(az_iot_mock_mqtt_client_inject_event(m, &evt));
    assert_int_equal(az_iot_connection_client_do_work(fx->client, 0), AZ_IOT_OK);

    assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_IDLE);
    assert_null(az_iot_mock_mqtt_factory_last_client(fx->factory));
}

static void close_when_idle_is_noop(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
    assert_int_equal(fx->rec.count, 0);
}

static void open_twice_is_rejected(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory),
                     AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(fx->client),
                     AZ_IOT_ERR_ALREADY_INITIALIZED);
}

/* ------------------------------------------------------------------------- */
/* reconnect (Phase 2.2) tests                                               */
/* ------------------------------------------------------------------------- */

static size_t count_states(const state_record_t* r, az_iot_connection_state_t s)
{
    size_t n = 0;
    for (size_t i = 0; i < r->count; ++i) if (r->states[i] == s) ++n;
    return n;
}

static void connack_fail_with_reconnect_schedules_retry(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory),
                     AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

    az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
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
    az_iot_mock_mqtt_client_t* m2 = az_iot_mock_mqtt_factory_last_client(fx->factory);
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
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory),
                     AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

    /* max_attempts == 2 means we tolerate up to 2 reconnect attempts; the
     * third unsuccessful attempt-end transitions us to FAULTED. */
    for (int i = 0; i < 3; ++i)
    {
        az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
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
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory),
                     AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

    az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
    (void)az_iot_connection_client_do_work(fx->client, 0);
    assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTED);

    /* Peer-initiated drop. */
    az_iot_mqtt_event_t evt = {0};
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
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory),
                     AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

    az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
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
    assert_int_equal(count_states(&fx->rec, AZ_IOT_CONN_STATE_CONNECTING), 1); /* only the initial open */
}

static void user_close_after_connected_does_not_reconnect(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory),
                     AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);

    az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
    (void)az_iot_connection_client_do_work(fx->client, 0);
    assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_CONNECTED);

    assert_int_equal(az_iot_connection_client_close(fx->client), AZ_IOT_OK);
    assert_int_equal(fx->rec.states[fx->rec.count - 1], AZ_IOT_CONN_STATE_DISCONNECTING);

    /* Adapter delivers DISCONNECTED -> we must end up IDLE, NOT RECONNECTING. */
    az_iot_mqtt_event_t evt = {0};
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

typedef struct inbound_record_tag
{
    size_t hits;
    char   last_topic[128];
} inbound_record_t;

static void inbound_record_cb(void* user_ctx, const az_iot_mqtt_message_t* msg)
{
    inbound_record_t* r = (inbound_record_t*)user_ctx;
    r->hits++;
    if (msg && msg->topic)
    {
        size_t n = strlen(msg->topic);
        if (n >= sizeof(r->last_topic)) n = sizeof(r->last_topic) - 1;
        memcpy(r->last_topic, msg->topic, n);
        r->last_topic[n] = '\0';
    }
}

static void inbound_message_routes_through_dispatch(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory),
                     AZ_IOT_OK);

    /* Profile-driven prefix: feature clients in Phase 3 will get this from
     * the active profile. Here we drive the same code path by hand. */
    const az_iot_protocol_profile_t* p =
        az_iot_connection_client__profile(fx->client);
    assert_non_null(p);
    assert_int_equal(p->flavor, AZ_IOT_HUB_FLAVOR_CLASSIC);

    inbound_record_t twin_rec = {0};
    assert_int_equal(
        az_iot_connection_client__register_inbound_handler(
            fx->client, p->twin_response_topic_prefix, inbound_record_cb, &twin_rec),
        AZ_IOT_OK);

    assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
    az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
    (void)az_iot_connection_client_do_work(fx->client, 0);

    assert_true(az_iot_mock_mqtt_client_inject_message(
        m, "$iothub/twin/res/200/?$rid=42",
        (const uint8_t*)"{}", 2, AZ_IOT_MQTT_QOS_0));
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

/* D2: request_operational_certificate requires a certificate_provider whose
 * vtable exposes get_csr (ABI version >= 2). open() must reject otherwise. */
static void open_rejects_operational_cert_without_csr_provider(void** state)
{
    (void)state;

    az_iot_connection_client_options_t opts = {0};
    opts.client_id = "ut-device";
    opts.dps.id_scope = "0ne00000000";
    opts.dps.registration_id = "ut-device";
    opts.dps.request_operational_certificate = true;

    /* Case 1: no certificate_provider at all. */
    az_iot_connection_client_t c1;
    assert_int_equal(az_iot_connection_client_init(&c1, &opts), AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(&c1), AZ_IOT_ERR_NOT_SUPPORTED);
    az_iot_connection_client_deinit(&c1);

    /* Case 2: a v2 provider that does not implement get_csr (all hooks NULL;
     * open() rejects before any hook is invoked). */
    static const az_iot_certificate_provider_vtable_t no_csr_vtable = {
        .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
    };
    az_iot_certificate_provider_t prov = { .vtable = &no_csr_vtable };
    opts.certificate_provider = &prov;

    az_iot_connection_client_t c2;
    assert_int_equal(az_iot_connection_client_init(&c2, &opts), AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(&c2), AZ_IOT_ERR_NOT_SUPPORTED);
    az_iot_connection_client_deinit(&c2);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(register_rejects_null_create, setup, teardown),
        cmocka_unit_test_setup_teardown(open_without_factory_returns_not_supported, setup, teardown),
        cmocka_unit_test_setup_teardown(open_invokes_connect_and_transitions_to_connecting, setup, teardown),
        cmocka_unit_test_setup_teardown(connected_event_transitions_to_connected, setup, teardown),
        cmocka_unit_test_setup_teardown(connack_failure_transitions_to_faulted, setup, teardown),
        cmocka_unit_test_setup_teardown(close_disconnect_returns_to_idle, setup, teardown),
        cmocka_unit_test_setup_teardown(close_when_idle_is_noop, setup, teardown),
        cmocka_unit_test_setup_teardown(open_twice_is_rejected, setup, teardown),
        cmocka_unit_test_setup_teardown(connack_fail_with_reconnect_schedules_retry, setup_with_reconnect, teardown),
        cmocka_unit_test_setup_teardown(max_attempts_exhausted_transitions_to_faulted, setup_with_reconnect, teardown),
        cmocka_unit_test_setup_teardown(peer_disconnect_with_reconnect_drives_retry, setup_with_reconnect, teardown),
        cmocka_unit_test_setup_teardown(close_during_reconnecting_goes_idle, setup_with_reconnect, teardown),
        cmocka_unit_test_setup_teardown(user_close_after_connected_does_not_reconnect, setup_with_reconnect, teardown),
        cmocka_unit_test_setup_teardown(inbound_message_routes_through_dispatch, setup, teardown),
        cmocka_unit_test(open_rejects_operational_cert_without_csr_provider),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
