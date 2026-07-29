// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Phase 3.3 - TwinClient unit tests, driven through the public API and the
 * in-memory mock_mqtt_iface. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_twin_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "internal/twin_client_internal.h"

#include "support/mock_mqtt_iface.h"

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct get_record
{
    bool                fired;
    az_iot_result status;
    char                payload[64];
    size_t              payload_len;
    char                desired[64];
    size_t              desired_len;
    uint64_t            desired_version;
    char                reported[64];
    size_t              reported_len;
    uint64_t            reported_version;
} get_record;

static void copy_section(const uint8_t* src, size_t len, char* dst, size_t cap, size_t* out_len)
{
    if (!src || len == 0 || len >= cap) return;
    memcpy(dst, src, len);
    dst[len] = '\0';
    *out_len = len;
}

static void on_get(az_iot_result status, const az_iot_twin_state* twin, void* user_ctx)
{
    get_record* r = (get_record*)user_ctx;
    r->fired = true;
    r->status = status;
    if (!twin) return;

    copy_section(twin->document, twin->document_len,
                 r->payload, sizeof(r->payload), &r->payload_len);
    copy_section(twin->desired.payload, twin->desired.payload_len,
                 r->desired, sizeof(r->desired), &r->desired_len);
    copy_section(twin->reported.payload, twin->reported.payload_len,
                 r->reported, sizeof(r->reported), &r->reported_len);
    r->desired_version = twin->desired.version;
    r->reported_version = twin->reported.version;
}

typedef struct patch_record
{
    bool                     fired;
    az_iot_result            status;
    az_iot_twin_patch_status patch_status;
    uint64_t                 version;
} patch_record;

static void on_patch(az_iot_result status, const az_iot_twin_patch_result* result, void* user_ctx)
{
    patch_record* r = (patch_record*)user_ctx;
    r->fired = true;
    r->status = status;
    if (result)
    {
        r->patch_status = result->status;
        r->version = result->version;
    }
}

typedef struct desired_record
{
    bool     fired;
    char     payload[64];
    size_t   payload_len;
    uint64_t version;
} desired_record;

static void on_desired(const uint8_t* payload, size_t payload_len, uint64_t version, void* user_ctx)
{
    desired_record* r = (desired_record*)user_ctx;
    r->fired = true;
    r->version = version;
    if (payload && payload_len > 0 && payload_len < sizeof(r->payload))
    {
        memcpy(r->payload, payload, payload_len);
        r->payload[payload_len] = '\0';
        r->payload_len = payload_len;
    }
}

typedef struct fixture
{
    az_iot_connection_client    conn;
    az_iot_twin_client        twin;
    az_iot_mqtt_factory*      factory;
    az_iot_mock_mqtt_client*  mock;
} fixture;

static int setup(void** state)
{
    fixture* fx = (fixture*)calloc(1, sizeof(*fx));
    assert_non_null(fx);

    az_iot_connection_client_options opts = {0};
    opts.host = "broker.example";
    opts.port = 8883;
    opts.client_id = "ut-device";
    assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

    fx->factory = az_iot_mock_mqtt_factory_create(
        AZ_IOT_MQTT_VERSION_3_1_1);
    assert_non_null(fx->factory);

    assert_int_equal(az_iot_twin_client_init(&fx->twin, &fx->conn), AZ_IOT_OK);

    *state = fx;
    return 0;
}

static int teardown(void** state)
{
    fixture* fx = (fixture*)*state;
    if (fx)
    {
        az_iot_twin_client_destroy(&fx->twin);
        az_iot_connection_client_destroy(&fx->conn);
        free(fx);
    }
    return 0;
}

static void open_to_connected(fixture* fx)
{
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory),
                     AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
    fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_non_null(fx->mock);
    assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
    az_iot_mock_mqtt_client_clear_calls(fx->mock);
}

/* Walk the mock call history; return the topic of the first PUBLISH found
 * (or NULL). */
static const char* first_publish_topic(az_iot_mock_mqtt_client* m)
{
    size_t n = az_iot_mock_mqtt_client_call_count(m);
    for (size_t i = 0; i < n; ++i)
    {
        const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i);
        if (c->kind == AZ_IOT_MOCK_CALL_PUBLISH) return c->topic;
    }
    return NULL;
}

static bool history_has_subscribe(az_iot_mock_mqtt_client* m, const char* expected)
{
    size_t n = az_iot_mock_mqtt_client_call_count(m);
    for (size_t i = 0; i < n; ++i)
    {
        const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i);
        if (c->kind == AZ_IOT_MOCK_CALL_SUBSCRIBE && strcmp(c->topic, expected) == 0)
        {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* tests                                                                     */
/* ------------------------------------------------------------------------- */

static void create_subscribes_response_and_desired(void** state)
{
    fixture* fx = (fixture*)*state;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory),
                     AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
    fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_non_null(fx->mock);
    assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

    assert_true(history_has_subscribe(fx->mock, "$iothub/twin/res/#"));
    assert_true(history_has_subscribe(fx->mock, "$iothub/twin/PATCH/properties/desired/#"));
}

static void get_publishes_and_response_fires_callback(void** state)
{
    fixture* fx = (fixture*)*state;
    open_to_connected(fx);

    get_record rec = {0};
    assert_int_equal(az_iot_twin_client_get(&fx->twin, on_get, &rec), AZ_IOT_OK);

    const char* topic = first_publish_topic(fx->mock);
    assert_non_null(topic);
    /* First rid is 1. */
    assert_string_equal(topic, "$iothub/twin/GET/?$rid=1");
    assert_false(rec.fired);

    /* Inject the response. */
    static const uint8_t body[] = "{\"desired\":{},\"reported\":{}}";
    assert_true(az_iot_mock_mqtt_client_inject_message(
        fx->mock, "$iothub/twin/res/200/?$rid=1",
        body, sizeof(body) - 1, AZ_IOT_MQTT_QOS_0));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

    assert_true(rec.fired);
    assert_int_equal(rec.status, AZ_IOT_OK);
    assert_int_equal(rec.payload_len, sizeof(body) - 1);
    assert_string_equal(rec.payload, "{\"desired\":{},\"reported\":{}}");
}

static void patch_publishes_and_204_response_fires_callback(void** state)
{
    fixture* fx = (fixture*)*state;
    open_to_connected(fx);

    patch_record rec = {0};
    static const uint8_t patch[] = "{\"reported\":{\"x\":1}}";
    assert_int_equal(
        az_iot_twin_client_patch_reported(
            &fx->twin, patch, sizeof(patch) - 1, on_patch, &rec),
        AZ_IOT_OK);

    const char* topic = first_publish_topic(fx->mock);
    assert_non_null(topic);
    assert_string_equal(topic, "$iothub/twin/PATCH/properties/reported/?$rid=1");

    assert_true(az_iot_mock_mqtt_client_inject_message(
        fx->mock, "$iothub/twin/res/204/?$rid=1&$version=42",
        NULL, 0, AZ_IOT_MQTT_QOS_0));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

    assert_true(rec.fired);
    assert_int_equal(rec.status, AZ_IOT_OK);
}

static void desired_message_dispatched_to_callback(void** state)
{
    fixture* fx = (fixture*)*state;
    open_to_connected(fx);

    desired_record rec = {0};
    assert_int_equal(
        az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &rec),
        AZ_IOT_OK);

    static const uint8_t body[] = "{\"x\":2}";
    assert_true(az_iot_mock_mqtt_client_inject_message(
        fx->mock, "$iothub/twin/PATCH/properties/desired/?$version=99",
        body, sizeof(body) - 1, AZ_IOT_MQTT_QOS_0));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

    assert_true(rec.fired);
    assert_int_equal(rec.payload_len, sizeof(body) - 1);
    assert_string_equal(rec.payload, "{\"x\":2}");
    assert_int_equal((int)rec.version, 99);
}

static void unknown_rid_drops_response(void** state)
{
    fixture* fx = (fixture*)*state;
    open_to_connected(fx);

    /* No request issued; response with rid=99 should not crash. */
    assert_true(az_iot_mock_mqtt_client_inject_message(
        fx->mock, "$iothub/twin/res/200/?$rid=99",
        NULL, 0, AZ_IOT_MQTT_QOS_0));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
    /* No assertion needed: surviving the call is the test. */
}

/* ------------------------------------------------------------------------- */
/* desired-property subscriber registry                                      */
/* ------------------------------------------------------------------------- */

static int g_desired_seq;

typedef struct order_record
{
    int order; /* dispatch order, captured from g_desired_seq */
} order_record;

static void on_desired_feature(const uint8_t* p, size_t n, uint64_t v, void* ctx)
{
    (void)p; (void)n; (void)v;
    ((order_record*)ctx)->order = ++g_desired_seq;
}
static void on_desired_app_a(const uint8_t* p, size_t n, uint64_t v, void* ctx)
{
    (void)p; (void)n; (void)v;
    ((order_record*)ctx)->order = ++g_desired_seq;
}
static void on_desired_app_b(const uint8_t* p, size_t n, uint64_t v, void* ctx)
{
    (void)p; (void)n; (void)v;
    ((order_record*)ctx)->order = ++g_desired_seq;
}

static void inject_desired(fixture* fx, const char* body)
{
    char topic[] = "$iothub/twin/PATCH/properties/desired/?$version=7";
    assert_true(az_iot_mock_mqtt_client_inject_message(
        fx->mock, topic, (const uint8_t*)body, strlen(body), AZ_IOT_MQTT_QOS_0));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
}

static void feature_subscribers_notified_before_app(void** state)
{
    fixture* fx = (fixture*)*state;
    open_to_connected(fx);

    g_desired_seq = 0;
    order_record feat = {0}, app_a = {0}, app_b = {0};

    /* Register application subs first, feature sub last, to prove ordering is
     * by pool (feature-before-app) and NOT by registration time. */
    assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_app_a, &app_a), AZ_IOT_OK);
    assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_app_b, &app_b), AZ_IOT_OK);
    assert_int_equal(az_iot_twin_client__subscribe_desired(&fx->twin, on_desired_feature, &feat), AZ_IOT_OK);

    inject_desired(fx, "{\"a\":1}");

    assert_int_equal(feat.order, 1);          /* feature pool first */
    assert_true(app_a.order == 2 && app_b.order == 3); /* app pool in reg order */
}

static void app_pool_full_returns_not_supported(void** state)
{
    fixture* fx = (fixture*)*state;
    open_to_connected(fx);

    int a = 0, b = 0, c = 0;
    /* Default app pool capacity is 2. */
    assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_app_a, &a), AZ_IOT_OK);
    assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_app_a, &b), AZ_IOT_OK);
    assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_app_a, &c),
                     AZ_IOT_ERR_NOT_SUPPORTED);
}

static void resubscribe_same_pair_is_idempotent(void** state)
{
    fixture* fx = (fixture*)*state;
    open_to_connected(fx);

    desired_record rec = {0};
    assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &rec), AZ_IOT_OK);
    assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &rec), AZ_IOT_OK);

    /* Idempotent: only one slot consumed, so one more distinct sub still fits. */
    int other = 0;
    assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_app_a, &other), AZ_IOT_OK);
}

static void unsubscribe_stops_delivery(void** state)
{
    fixture* fx = (fixture*)*state;
    open_to_connected(fx);

    desired_record rec = {0};
    assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired, &rec), AZ_IOT_OK);
    assert_int_equal(az_iot_twin_client_unsubscribe_desired(&fx->twin, on_desired, &rec), AZ_IOT_OK);
    /* Removing an absent entry reports invalid arg. */
    assert_int_equal(az_iot_twin_client_unsubscribe_desired(&fx->twin, on_desired, &rec),
                     AZ_IOT_ERR_INVALID_ARG);

    inject_desired(fx, "{\"a\":1}");
    assert_false(rec.fired);
}

typedef struct busy_record
{
    az_iot_twin_client* twin;
    az_iot_result       sub_result;
    az_iot_result       unsub_result;
} busy_record;

static void on_desired_reentrant(const uint8_t* p, size_t n, uint64_t v, void* ctx)
{
    (void)p; (void)n; (void)v;
    busy_record* r = (busy_record*)ctx;
    /* Mutating the registry mid-dispatch MUST be rejected. */
    r->sub_result   = az_iot_twin_client_subscribe_desired(r->twin, on_desired_app_a, NULL);
    r->unsub_result = az_iot_twin_client_unsubscribe_desired(r->twin, on_desired_reentrant, ctx);
}

static void mutating_registry_during_dispatch_is_busy(void** state)
{
    fixture* fx = (fixture*)*state;
    open_to_connected(fx);

    busy_record rec = { .twin = &fx->twin, .sub_result = AZ_IOT_OK, .unsub_result = AZ_IOT_OK };
    assert_int_equal(az_iot_twin_client_subscribe_desired(&fx->twin, on_desired_reentrant, &rec), AZ_IOT_OK);

    inject_desired(fx, "{\"a\":1}");
    assert_int_equal(rec.sub_result, AZ_IOT_ERR_BUSY);
    assert_int_equal(rec.unsub_result, AZ_IOT_ERR_BUSY);
}

/* ------------------------------------------------------------------------- */
/* Hub-Next wire (gateway/rfcs/aeg/twin.md)                                   */
/* ------------------------------------------------------------------------- */

#define NEXT_DEV_TOPIC "ih/ut-device/dev/twin"
#define NEXT_SRV_TOPIC "ih/ut-device/srv/twin"

typedef struct push_record
{
    bool     fired;
    uint64_t desired_version;
    uint64_t reported_version;
    char     desired[64];
    size_t   desired_len;
    char     reported[64];
    size_t   reported_len;
} push_record;

static void on_push(const az_iot_twin_state* twin, void* user_ctx)
{
    push_record* r = (push_record*)user_ctx;
    r->fired = true;
    if (!twin) return;
    r->desired_version = twin->desired.version;
    r->reported_version = twin->reported.version;
    copy_section(twin->desired.payload, twin->desired.payload_len,
                 r->desired, sizeof(r->desired), &r->desired_len);
    copy_section(twin->reported.payload, twin->reported.payload_len,
                 r->reported, sizeof(r->reported), &r->reported_len);
}

/* A HUB_NEXT fixture. The twin client's Hub-Next behavior depends on the
 * presence handshake having completed, because backend-initiated messages are
 * bound to the connection's birth nonce. */
typedef struct next_fixture
{
    fixture  base;
    uint8_t  nonce[16];
    uint8_t  encode_buf[256];
    bool     saw_twin_subscribe;
} next_fixture;

static int setup_next(void** state)
{
    next_fixture* nf = (next_fixture*)calloc(1, sizeof(*nf));
    assert_non_null(nf);

    az_iot_connection_client_options opts = {0};
    opts.host = "broker.example";
    opts.port = 8883;
    opts.client_id = "ut-device";
    opts.hub_protocol = AZ_IOT_HUB_PROTOCOL_NEXT;
    opts.twin_push.push_desired = true;
    opts.twin_push.push_reported = true;
    assert_int_equal(az_iot_connection_client_init(&nf->base.conn, &opts), AZ_IOT_OK);

    nf->base.factory = az_iot_mock_mqtt_factory_create(AZ_IOT_MQTT_VERSION_5);
    assert_non_null(nf->base.factory);

    assert_int_equal(az_iot_twin_client_init(&nf->base.twin, &nf->base.conn), AZ_IOT_OK);
    assert_int_equal(
        az_iot_twin_client_set_encode_buffer(&nf->base.twin, nf->encode_buf, sizeof(nf->encode_buf)),
        AZ_IOT_OK);

    *state = nf;
    return 0;
}

static int teardown_next(void** state)
{
    next_fixture* nf = (next_fixture*)*state;
    if (nf)
    {
        az_iot_twin_client_destroy(&nf->base.twin);
        az_iot_connection_client_destroy(&nf->base.conn);
        free(nf);
    }
    return 0;
}

static const az_iot_mock_call* last_call_of_kind(
    az_iot_mock_mqtt_client* m, az_iot_mock_call_kind kind)
{
    size_t n = az_iot_mock_mqtt_client_call_count(m);
    for (size_t i = n; i > 0; --i)
    {
        const az_iot_mock_call* c = az_iot_mock_mqtt_client_call_at(m, i - 1);
        if (c->kind == kind) return c;
    }
    return NULL;
}

/* Inject a service->device message with the given type, correlation data and
 * protobuf body, then pump. */
static void inject_next(next_fixture* nf, const char* type,
                        const uint8_t* corr, const uint8_t* body, size_t body_len)
{
    az_iot_mqtt_user_property type_prop = { "type", type };
    az_iot_mqtt_message msg;
    memset(&msg, 0, sizeof(msg));
    msg.topic = NEXT_DEV_TOPIC;
    msg.payload = body;
    msg.payload_len = body_len;
    msg.user_properties = &type_prop;
    msg.user_properties_count = 1;
    msg.correlation_data = corr;
    msg.correlation_data_len = 16;

    az_iot_mqtt_event evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = AZ_IOT_MQTT_EVT_MESSAGE;
    evt.message = &msg;
    assert_true(az_iot_mock_mqtt_client_inject_event(nf->base.mock, &evt));
    (void)az_iot_connection_client_do_work(&nf->base.conn, 0);
}

/* Drive CONNACK -> SUBACK -> birth -> birth-ack so the connection is CONNECTED
 * with a known birth nonce and known authoritative twin versions. */
static void open_to_connected_next(next_fixture* nf, uint64_t desired_v, uint64_t reported_v)
{
    fixture* fx = &nf->base;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory),
                     AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
    fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_non_null(fx->mock);

    az_iot_mqtt_event connack;
    memset(&connack, 0, sizeof(connack));
    connack.kind = AZ_IOT_MQTT_EVT_CONNECTED;
    connack.status = AZ_IOT_OK;
    assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &connack));
    (void)az_iot_connection_client_do_work(&fx->conn, 0);

    const az_iot_mock_call* sub = last_call_of_kind(fx->mock, AZ_IOT_MOCK_CALL_SUBSCRIBE);
    assert_non_null(sub);

    az_iot_mqtt_event suback;
    memset(&suback, 0, sizeof(suback));
    suback.kind = AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK;
    suback.status = AZ_IOT_OK;
    suback.packet_id = sub->packet_id;
    assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &suback));
    (void)az_iot_connection_client_do_work(&fx->conn, 0);

    const az_iot_mock_call* birth = last_call_of_kind(fx->mock, AZ_IOT_MOCK_CALL_PUBLISH);
    assert_non_null(birth);
    assert_int_equal(birth->correlation_data_len, 16);
    memcpy(nf->nonce, birth->correlation_data, 16);

    /* BirthAck { 10 desired_version, 11 reported_version } */
    uint8_t ack_body[24];
    size_t n = 0;
    if (desired_v) { ack_body[n++] = 0x50; ack_body[n++] = (uint8_t)desired_v; }
    if (reported_v) { ack_body[n++] = 0x58; ack_body[n++] = (uint8_t)reported_v; }

    az_iot_mqtt_user_property ack_type = { "type", "birth-ack:1" };
    az_iot_mqtt_message ack_msg;
    memset(&ack_msg, 0, sizeof(ack_msg));
    ack_msg.topic = "ih/ut-device/dev/presence";
    ack_msg.correlation_data = nf->nonce;
    ack_msg.correlation_data_len = 16;
    ack_msg.user_properties = &ack_type;
    ack_msg.user_properties_count = 1;
    ack_msg.payload = ack_body;
    ack_msg.payload_len = n;
    az_iot_mqtt_event ack;
    memset(&ack, 0, sizeof(ack));
    ack.kind = AZ_IOT_MQTT_EVT_MESSAGE;
    ack.message = &ack_msg;
    assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &ack));
    (void)az_iot_connection_client_do_work(&fx->conn, 0);

    /* Persistent subscriptions are issued when the connection is announced, so
     * capture the twin one before the history is cleared for the test proper. */
    nf->saw_twin_subscribe = history_has_subscribe(fx->mock, NEXT_DEV_TOPIC);

    az_iot_mock_mqtt_client_clear_calls(fx->mock);
}

/* The whole twin surface lives on one dev-bound topic; the message kind rides
 * the "type" user property (twin.md 2). */
static void next_subscribes_single_twin_topic(void** state)
{
    next_fixture* nf = (next_fixture*)*state;
    open_to_connected_next(nf, 0, 0);
    assert_true(nf->saw_twin_subscribe);
}

/* A GET is a TwinGet{sections=BOTH} published on srv/twin at QoS 0, typed and
 * correlated by a per-attempt UUID. */
static void next_get_publishes_conformant_request(void** state)
{
    next_fixture* nf = (next_fixture*)*state;
    open_to_connected_next(nf, 0, 0);

    get_record rec = {0};
    assert_int_equal(az_iot_twin_client_get(&nf->base.twin, on_get, &rec), AZ_IOT_OK);

    const az_iot_mock_call* pub = last_call_of_kind(nf->base.mock, AZ_IOT_MOCK_CALL_PUBLISH);
    assert_non_null(pub);
    assert_string_equal(pub->topic, NEXT_SRV_TOPIC);
    assert_int_equal(pub->qos, AZ_IOT_MQTT_QOS_0);
    assert_string_equal(pub->user_type, "get:1");
    assert_string_equal(pub->content_type, "application/protobuf");
    assert_int_equal(pub->correlation_data_len, 16);
    /* TwinGet { 1: sections = BOTH(3) } */
    const uint8_t expect[] = { 0x08, 0x03 };
    assert_int_equal(pub->payload_len, sizeof(expect));
    assert_memory_equal(pub->payload, expect, sizeof(expect));
}

/* The response is correlated by the request's UUID and decoded into the two
 * sections with their authoritative versions. */
static void next_get_response_decodes_both_sections(void** state)
{
    next_fixture* nf = (next_fixture*)*state;
    open_to_connected_next(nf, 0, 0);

    get_record rec = {0};
    assert_int_equal(az_iot_twin_client_get(&nf->base.twin, on_get, &rec), AZ_IOT_OK);
    const az_iot_mock_call* pub = last_call_of_kind(nf->base.mock, AZ_IOT_MOCK_CALL_PUBLISH);
    uint8_t corr[16];
    memcpy(corr, pub->correlation_data, 16);

    /* TwinGetResponse { 1: desired_version=7, 2: reported_version=9,
     *                   3: desired_payload="{\"d\":1}", 4: reported_payload="{\"r\":2}" } */
    const uint8_t body[] = {
        0x08, 0x07,
        0x10, 0x09,
        0x1A, 0x07, '{', '"', 'd', '"', ':', '1', '}',
        0x22, 0x07, '{', '"', 'r', '"', ':', '2', '}',
    };
    inject_next(nf, "get-response:1", corr, body, sizeof(body));

    assert_true(rec.fired);
    assert_int_equal(rec.status, AZ_IOT_OK);
    assert_int_equal(rec.desired_version, 7);
    assert_int_equal(rec.reported_version, 9);
    assert_string_equal(rec.desired, "{\"d\":1}");
    assert_string_equal(rec.reported, "{\"r\":2}");
}

/* A response whose correlation data belongs to no pending request is dropped. */
static void next_get_response_with_unknown_correlation_is_dropped(void** state)
{
    next_fixture* nf = (next_fixture*)*state;
    open_to_connected_next(nf, 0, 0);

    get_record rec = {0};
    assert_int_equal(az_iot_twin_client_get(&nf->base.twin, on_get, &rec), AZ_IOT_OK);

    uint8_t wrong[16];
    memset(wrong, 0xAB, sizeof(wrong));
    const uint8_t body[] = { 0x08, 0x07 };
    inject_next(nf, "get-response:1", wrong, body, sizeof(body));

    assert_false(rec.fired);
}

/* The reported patch is wrapped in ReportedPatch{if_match, payload}, with
 * if_match seeded from the birth-ack so optimistic concurrency works on the
 * first write after connecting. */
static void next_patch_sends_if_match_from_birth_ack(void** state)
{
    next_fixture* nf = (next_fixture*)*state;
    open_to_connected_next(nf, 0, 9);

    patch_record rec = {0};
    static const uint8_t patch[] = "{\"x\":1}";
    assert_int_equal(
        az_iot_twin_client_patch_reported(&nf->base.twin, patch, sizeof(patch) - 1, on_patch, &rec),
        AZ_IOT_OK);

    const az_iot_mock_call* pub = last_call_of_kind(nf->base.mock, AZ_IOT_MOCK_CALL_PUBLISH);
    assert_non_null(pub);
    assert_string_equal(pub->topic, NEXT_SRV_TOPIC);
    assert_int_equal(pub->qos, AZ_IOT_MQTT_QOS_0);
    assert_string_equal(pub->user_type, "reported-patch:1");
    assert_string_equal(pub->content_type, "application/protobuf");
    /* ReportedPatch { 1: if_match=9, 2: payload="{\"x\":1}" } */
    const uint8_t expect[] = { 0x08, 0x09, 0x12, 0x07, '{', '"', 'x', '"', ':', '1', '}' };
    assert_int_equal(pub->payload_len, sizeof(expect));
    assert_memory_equal(pub->payload, expect, sizeof(expect));
}

/* VERSION_MISMATCH must reach the application: it is the signal to re-read and
 * retry rather than a transport failure. The version it carries is adopted so
 * the retry's if_match is correct. */
static void next_patch_reports_version_mismatch(void** state)
{
    next_fixture* nf = (next_fixture*)*state;
    open_to_connected_next(nf, 0, 9);

    patch_record rec = {0};
    static const uint8_t patch[] = "{\"x\":1}";
    assert_int_equal(
        az_iot_twin_client_patch_reported(&nf->base.twin, patch, sizeof(patch) - 1, on_patch, &rec),
        AZ_IOT_OK);
    const az_iot_mock_call* pub = last_call_of_kind(nf->base.mock, AZ_IOT_MOCK_CALL_PUBLISH);
    uint8_t corr[16];
    memcpy(corr, pub->correlation_data, 16);

    /* ReportedPatchResponse { 1: result=VERSION_MISMATCH(2), 2: version=12 } */
    const uint8_t body[] = { 0x08, 0x02, 0x10, 0x0C };
    inject_next(nf, "reported-patch-response:1", corr, body, sizeof(body));

    assert_true(rec.fired);
    assert_int_equal(rec.status, AZ_IOT_OK);
    assert_int_equal(rec.patch_status, AZ_IOT_TWIN_PATCH_VERSION_MISMATCH);
    assert_int_equal(rec.version, 12);

    /* The next attempt must carry the version the service just reported. */
    az_iot_mock_mqtt_client_clear_calls(nf->base.mock);
    patch_record rec2 = {0};
    assert_int_equal(
        az_iot_twin_client_patch_reported(&nf->base.twin, patch, sizeof(patch) - 1, on_patch, &rec2),
        AZ_IOT_OK);
    const az_iot_mock_call* retry = last_call_of_kind(nf->base.mock, AZ_IOT_MOCK_CALL_PUBLISH);
    assert_non_null(retry);
    assert_int_equal(retry->payload[0], 0x08);
    assert_int_equal(retry->payload[1], 12);
}

/* A twin-push tagged with the current connection's birth nonce is decoded and
 * handed to the application. */
static void next_twin_push_dispatched(void** state)
{
    next_fixture* nf = (next_fixture*)*state;
    open_to_connected_next(nf, 0, 0);

    push_record rec = {0};
    assert_int_equal(az_iot_twin_client_set_push_callback(&nf->base.twin, on_push, &rec), AZ_IOT_OK);

    /* TwinPush { 1: Section{1: version=5, 2: payload="{\"d\":1}"} } */
    const uint8_t body[] = {
        0x0A, 0x0B,
              0x08, 0x05,
              0x12, 0x07, '{', '"', 'd', '"', ':', '1', '}',
    };
    inject_next(nf, "twin-push:1", nf->nonce, body, sizeof(body));

    assert_true(rec.fired);
    assert_int_equal(rec.desired_version, 5);
    assert_string_equal(rec.desired, "{\"d\":1}");
    assert_int_equal(rec.reported_len, 0);
}

/* A twin-push left over from a defunct connection carries a different nonce and
 * must be ignored (twin.md 2.1). */
static void next_twin_push_from_stale_connection_ignored(void** state)
{
    next_fixture* nf = (next_fixture*)*state;
    open_to_connected_next(nf, 0, 0);

    push_record rec = {0};
    assert_int_equal(az_iot_twin_client_set_push_callback(&nf->base.twin, on_push, &rec), AZ_IOT_OK);

    uint8_t stale[16];
    memset(stale, 0x5A, sizeof(stale));
    const uint8_t body[] = { 0x0A, 0x04, 0x08, 0x05, 0x12, 0x00 };
    inject_next(nf, "twin-push:1", stale, body, sizeof(body));

    assert_false(rec.fired);
}

/* A desired patch reaches the existing desired subscribers, carrying the
 * version from the protobuf body rather than a topic query string. */
static void next_desired_patch_dispatched_with_version(void** state)
{
    next_fixture* nf = (next_fixture*)*state;
    open_to_connected_next(nf, 0, 0);

    desired_record rec = {0};
    assert_int_equal(
        az_iot_twin_client_subscribe_desired(&nf->base.twin, on_desired, &rec), AZ_IOT_OK);

    /* DesiredPatch { 1: version=4, 2: payload="{\"a\":1}" } */
    const uint8_t body[] = {
        0x08, 0x04,
        0x12, 0x07, '{', '"', 'a', '"', ':', '1', '}',
    };
    inject_next(nf, "desired-patch:1", nf->nonce, body, sizeof(body));

    assert_true(rec.fired);
    assert_int_equal(rec.version, 4);
    assert_string_equal(rec.payload, "{\"a\":1}");
}

/* A desired-patch with no payload is a version probe: it advances the tracked
 * version but is not an application-visible patch (twin.md 3.5, 7.1). */
static void next_desired_patch_probe_not_dispatched(void** state)
{
    next_fixture* nf = (next_fixture*)*state;
    open_to_connected_next(nf, 0, 0);

    desired_record rec = {0};
    assert_int_equal(
        az_iot_twin_client_subscribe_desired(&nf->base.twin, on_desired, &rec), AZ_IOT_OK);

    /* DesiredPatch { 1: version=4 } -- payload absent */
    const uint8_t body[] = { 0x08, 0x04 };
    inject_next(nf, "desired-patch:1", nf->nonce, body, sizeof(body));

    assert_false(rec.fired);
}

/* Without an encode buffer the SDK has nowhere to frame the patch, and says so
 * rather than publishing something malformed. */
static void next_patch_without_encode_buffer_reports_no_space(void** state)
{
    next_fixture* nf = (next_fixture*)*state;
    open_to_connected_next(nf, 0, 0);

    assert_int_equal(az_iot_twin_client_set_encode_buffer(&nf->base.twin, NULL, 0), AZ_IOT_OK);

    patch_record rec = {0};
    static const uint8_t patch[] = "{\"x\":1}";
    assert_int_equal(
        az_iot_twin_client_patch_reported(&nf->base.twin, patch, sizeof(patch) - 1, on_patch, &rec),
        AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(create_subscribes_response_and_desired, setup, teardown),
        cmocka_unit_test_setup_teardown(get_publishes_and_response_fires_callback, setup, teardown),
        cmocka_unit_test_setup_teardown(patch_publishes_and_204_response_fires_callback, setup, teardown),
        cmocka_unit_test_setup_teardown(desired_message_dispatched_to_callback, setup, teardown),
        cmocka_unit_test_setup_teardown(feature_subscribers_notified_before_app, setup, teardown),
        cmocka_unit_test_setup_teardown(app_pool_full_returns_not_supported, setup, teardown),
        cmocka_unit_test_setup_teardown(resubscribe_same_pair_is_idempotent, setup, teardown),
        cmocka_unit_test_setup_teardown(unsubscribe_stops_delivery, setup, teardown),
        cmocka_unit_test_setup_teardown(mutating_registry_during_dispatch_is_busy, setup, teardown),
        cmocka_unit_test_setup_teardown(unknown_rid_drops_response, setup, teardown),

        cmocka_unit_test_setup_teardown(next_subscribes_single_twin_topic, setup_next, teardown_next),
        cmocka_unit_test_setup_teardown(next_get_publishes_conformant_request, setup_next, teardown_next),
        cmocka_unit_test_setup_teardown(next_get_response_decodes_both_sections, setup_next, teardown_next),
        cmocka_unit_test_setup_teardown(next_get_response_with_unknown_correlation_is_dropped, setup_next, teardown_next),
        cmocka_unit_test_setup_teardown(next_patch_sends_if_match_from_birth_ack, setup_next, teardown_next),
        cmocka_unit_test_setup_teardown(next_patch_reports_version_mismatch, setup_next, teardown_next),
        cmocka_unit_test_setup_teardown(next_twin_push_dispatched, setup_next, teardown_next),
        cmocka_unit_test_setup_teardown(next_twin_push_from_stale_connection_ignored, setup_next, teardown_next),
        cmocka_unit_test_setup_teardown(next_desired_patch_dispatched_with_version, setup_next, teardown_next),
        cmocka_unit_test_setup_teardown(next_desired_patch_probe_not_dispatched, setup_next, teardown_next),
        cmocka_unit_test_setup_teardown(next_patch_without_encode_buffer_reports_no_space, setup_next, teardown_next),
    };
    return cmocka_run_group_tests_name("twin_client", tests, NULL, NULL);
}
