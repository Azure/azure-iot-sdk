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

/* ---- DPS CSR issuance flow (increment 3) ---- */

typedef struct fake_csr_provider_tag
{
    az_iot_certificate_provider_t base;
    int    get_csr_calls;
    int    store_calls;
    size_t stored_count;
    char   stored_leaf[256];
    az_iot_cert_role_t last_load_role;
} fake_csr_provider_t;

static az_iot_result_t fake_csr_load(
    az_iot_certificate_provider_t* s, az_iot_cert_role_t role, az_iot_certificate_material_t* out)
{
    fake_csr_provider_t* f = (fake_csr_provider_t*)s;
    f->last_load_role = role;
    memset(out, 0, sizeof(*out));
    out->client_cert_pem = "cert";
    out->client_key_pem = "key";
    return AZ_IOT_OK;
}
static void fake_csr_release(az_iot_certificate_provider_t* s, az_iot_certificate_material_t* m)
{ (void)s; (void)m; }
static void fake_csr_deinit(az_iot_certificate_provider_t* s) { (void)s; }
static az_iot_result_t fake_get_csr(
    az_iot_certificate_provider_t* s, const char* cn, az_iot_certificate_signing_request_t* out)
{
    fake_csr_provider_t* f = (fake_csr_provider_t*)s;
    (void)cn;
    f->get_csr_calls++;
    out->csr_base64 = "TESTCSRBASE64==";
    return AZ_IOT_OK;
}
static void fake_release_csr(az_iot_certificate_provider_t* s, az_iot_certificate_signing_request_t* csr)
{ (void)s; (void)csr; }
static az_iot_result_t fake_store(
    az_iot_certificate_provider_t* s, const az_iot_issued_certificate_t* issued)
{
    fake_csr_provider_t* f = (fake_csr_provider_t*)s;
    f->store_calls++;
    f->stored_count = issued->count;
    if (issued->count > 0 && issued->client_cert_chain_pem[0])
    {
        size_t n = strlen(issued->client_cert_chain_pem[0]);
        if (n >= sizeof(f->stored_leaf)) n = sizeof(f->stored_leaf) - 1;
        memcpy(f->stored_leaf, issued->client_cert_chain_pem[0], n);
        f->stored_leaf[n] = '\0';
    }
    return AZ_IOT_OK;
}
static const az_iot_certificate_provider_vtable_t k_fake_csr_vtable = {
    .version = AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION,
    .load = fake_csr_load,
    .release = fake_csr_release,
    .deinit = fake_csr_deinit,
    .get_csr = fake_get_csr,
    .release_csr = fake_release_csr,
    .store_issued_certificate = fake_store,
};

static int g_dps_op_cert_count = 0;
static size_t g_dps_op_cert_chain = 0;
static void on_dps_op_cert(const az_iot_issued_certificate_t* issued, void* uc)
{
    (void)uc;
    g_dps_op_cert_count++;
    g_dps_op_cert_chain = issued->count;
}

static void dps_csr_flow_sends_csr_and_stores_issued_chain(void** state)
{
    (void)state;

    fake_csr_provider_t prov = {0};
    prov.base.vtable = &k_fake_csr_vtable;

    az_iot_connection_client_t client;
    az_iot_connection_client_options_t opts = {0};
    opts.host = NULL; /* DPS mode */
    opts.client_id = "ut-device";
    opts.dps.id_scope = "0ne00000000";
    opts.dps.registration_id = "ut-device";
    opts.dps.request_operational_certificate = true;
    opts.certificate_provider = &prov.base;
    assert_int_equal(az_iot_connection_client_init(&client, &opts), AZ_IOT_OK);

    g_dps_op_cert_count = 0;
    g_dps_op_cert_chain = 0;
    az_iot_connection_client_set_operational_cert_callback(&client, on_dps_op_cert, NULL);

    az_iot_mqtt_factory_t* factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_3_1_1);
    assert_non_null(factory);
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(&client, factory), AZ_IOT_OK);

    /* open() -> dps_start creates the DPS mock client and connects. */
    assert_int_equal(az_iot_connection_client_open(&client), AZ_IOT_OK);
    az_iot_mock_mqtt_client_t* dps = az_iot_mock_mqtt_factory_last_client(factory);
    assert_non_null(dps);

    /* CONNECTED -> subscribe. */
    assert_true(az_iot_mock_mqtt_client_inject_connected(dps, AZ_IOT_OK));
    (void)az_iot_connection_client_do_work(&client, 0);

    /* SUBSCRIBE_ACK -> register publish carrying the CSR. */
    az_iot_mqtt_event_t suback;
    memset(&suback, 0, sizeof(suback));
    suback.kind = AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK;
    suback.status = AZ_IOT_OK;
    assert_true(az_iot_mock_mqtt_client_inject_event(dps, &suback));
    (void)az_iot_connection_client_do_work(&client, 0);

    assert_true(prov.get_csr_calls >= 1);
    bool found_csr_publish = false;
    for (size_t i = 0; i < az_iot_mock_mqtt_client_call_count(dps); ++i)
    {
        const az_iot_mock_call_t* call = az_iot_mock_mqtt_client_call_at(dps, i);
        if (call->kind == AZ_IOT_MOCK_CALL_PUBLISH && call->payload_len >= 8)
        {
            assert_memory_equal(call->payload, "{\"csr\":\"", 8);
            found_csr_publish = true;
        }
    }
    assert_true(found_csr_publish);

    /* ASSIGNED response carrying a two-cert issued chain. */
    const char* resp =
        "{\"operationId\":\"op1\",\"status\":\"assigned\","
        "\"registrationState\":{\"registrationId\":\"ut-device\","
        "\"assignedHub\":\"myhub.azure-devices.net\",\"deviceId\":\"ut-device\","
        "\"issuedCertificateChain\":[\"TEEF\",\"SU5U\"]}}";
    assert_true(az_iot_mock_mqtt_client_inject_message(
        dps, "$dps/registrations/res/200/?$rid=1",
        (const uint8_t*)resp, strlen(resp), AZ_IOT_MQTT_QOS_1));

    /* Drive the flow to completion (message -> store -> deferred finalize ->
     * hub connect). Several do_work iterations cover the deferred steps. */
    for (int i = 0; i < 5; ++i)
        (void)az_iot_connection_client_do_work(&client, 0);

    /* Provider received the PEM-wrapped issued chain. */
    assert_int_equal(prov.store_calls, 1);
    assert_int_equal((int)prov.stored_count, 2);
    assert_non_null(strstr(prov.stored_leaf, "-----BEGIN CERTIFICATE-----"));
    assert_non_null(strstr(prov.stored_leaf, "TEEF"));

    /* Hub connect selected the OPERATIONAL identity. */
    assert_int_equal(prov.last_load_role, AZ_IOT_CRED_OPERATIONAL);

    /* The app operational-cert callback (D4) also fired with the chain. */
    assert_int_equal(g_dps_op_cert_count, 1);
    assert_int_equal((int)g_dps_op_cert_chain, 2);

    az_iot_connection_client_deinit(&client);
}

/* ---- Runtime Hub-side CSR renewal (increment 4) ---- */

typedef struct csr_test_ctx_tag
{
    int accepted, issued, failed;
    size_t issued_count;
    char issued_leaf[128];
    int32_t service_code;
    uint32_t retry_after_s;
} csr_test_ctx_t;

static void on_csr_evt(const az_iot_csr_event_t* evt, void* uc)
{
    csr_test_ctx_t* t = (csr_test_ctx_t*)uc;
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
                if (evt->issued->count > 0 && evt->issued->client_cert_chain_pem[0])
                {
                    size_t n = strlen(evt->issued->client_cert_chain_pem[0]);
                    if (n >= sizeof(t->issued_leaf)) n = sizeof(t->issued_leaf) - 1;
                    memcpy(t->issued_leaf, evt->issued->client_cert_chain_pem[0], n);
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
static az_iot_mock_mqtt_client_t* connect_fixture(fixture_t* fx)
{
    assert_int_equal(
        az_iot_connection_client_register_mqtt_factory(fx->client, fx->factory), AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(fx->client), AZ_IOT_OK);
    az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_non_null(m);
    assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
    (void)az_iot_connection_client_do_work(fx->client, 0);
    return m;
}

static void send_csr_two_phase_delivers_issued_chain(void** state)
{
    fixture_t* fx = *state;
    az_iot_mock_mqtt_client_t* m = connect_fixture(fx);

    csr_test_ctx_t tc = {0};
    az_iot_certificate_signing_request_t csr = { .csr_base64 = "TESTCSR==" };
    assert_int_equal(
        az_iot_connection_client_send_csr(fx->client, &csr, "req-1234", NULL, on_csr_evt, &tc),
        AZ_IOT_OK);

    /* The request must be published to the issueCertificate topic with an
     * {"id":...,"csr":...} body carrying the connected device id. */
    bool found = false;
    for (size_t i = 0; i < az_iot_mock_mqtt_client_call_count(m); ++i)
    {
        const az_iot_mock_call_t* call = az_iot_mock_mqtt_client_call_at(m, i);
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
        m, "$iothub/credentials/res/202/?$rid=req-1234",
        (const uint8_t*)r202, strlen(r202), AZ_IOT_MQTT_QOS_1));
    (void)az_iot_connection_client_do_work(fx->client, 0);
    assert_int_equal(tc.accepted, 1);
    assert_int_equal(tc.issued, 0);

    /* 200 Issued with a two-cert chain. */
    const char* r200 = "{\"correlationId\":\"x\",\"certificates\":[\"TEEF\",\"SU5U\"]}";
    assert_true(az_iot_mock_mqtt_client_inject_message(
        m, "$iothub/credentials/res/200/?$rid=req-1234",
        (const uint8_t*)r200, strlen(r200), AZ_IOT_MQTT_QOS_1));
    (void)az_iot_connection_client_do_work(fx->client, 0);
    assert_int_equal(tc.issued, 1);
    assert_int_equal((int)tc.issued_count, 2);
    assert_non_null(strstr(tc.issued_leaf, "-----BEGIN CERTIFICATE-----"));
    assert_non_null(strstr(tc.issued_leaf, "TEEF"));
}

static void send_csr_error_reports_service_code(void** state)
{
    fixture_t* fx = *state;
    az_iot_mock_mqtt_client_t* m = connect_fixture(fx);

    csr_test_ctx_t tc = {0};
    az_iot_certificate_signing_request_t csr = { .csr_base64 = "TESTCSR==" };
    assert_int_equal(
        az_iot_connection_client_send_csr(fx->client, &csr, "req-err", "*", on_csr_evt, &tc),
        AZ_IOT_OK);

    const char* err = "{\"errorCode\":409005,\"message\":\"conflict\",\"retryAfter\":5}";
    assert_true(az_iot_mock_mqtt_client_inject_message(
        m, "$iothub/credentials/res/409/?$rid=req-err",
        (const uint8_t*)err, strlen(err), AZ_IOT_MQTT_QOS_1));
    (void)az_iot_connection_client_do_work(fx->client, 0);

    assert_int_equal(tc.failed, 1);
    assert_int_equal(tc.service_code, 409005);
    assert_int_equal((int)tc.retry_after_s, 5);
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
        cmocka_unit_test(dps_csr_flow_sends_csr_and_stores_issued_chain),
        cmocka_unit_test_setup_teardown(send_csr_two_phase_delivers_issued_chain, setup, teardown),
        cmocka_unit_test_setup_teardown(send_csr_error_reports_service_code, setup, teardown),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
