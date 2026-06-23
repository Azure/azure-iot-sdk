// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Shared implementation of the azure-iot-sdk MQTT iface conformance suite.
 *
 * This translation unit knows nothing about any specific MQTT adapter. It
 * exercises the public iface vtable end-to-end against a real broker and is
 * therefore the basis for both:
 *   - CI verification of the bundled Paho adapter
 *   - Customer self-validation of any MQTT client+adapter they want to plug in
 */
#include "az_iot_conformance.h"

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#if defined(_WIN32)
#  include <windows.h>
static void conf_sleep_ms(unsigned ms) { Sleep(ms); }
static unsigned long conf_now_ms(void) { return (unsigned long)GetTickCount64(); }
#else
#  include <time.h>
static void conf_sleep_ms(unsigned ms) {
    struct timespec ts; ts.tv_sec = ms / 1000; ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
static unsigned long conf_now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)(ts.tv_sec * 1000UL + ts.tv_nsec / 1000000UL);
}
#endif

/* ------------------------------------------------------------------------- */
/* shared state for the suite (set by az_iot_conformance_run)            */
/* ------------------------------------------------------------------------- */

static az_iot_mqtt_factory_t* g_factory = NULL;
static const char*                 g_host   = "localhost";
static uint16_t                    g_port   = 1883;
static const unsigned              k_step_timeout_ms = 5000;

/* ------------------------------------------------------------------------- */
/* event recorder used by every test                                         */
/* ------------------------------------------------------------------------- */

#define CONF_TOPIC_MAX 256
#define CONF_PAYLOAD_MAX 1024
#define CONF_EVENTS_MAX 16

typedef struct conf_recorder_tag
{
    size_t count;
    az_iot_mqtt_event_kind_t kinds[CONF_EVENTS_MAX];
    az_iot_result_t          statuses[CONF_EVENTS_MAX];
    uint16_t                     packet_ids[CONF_EVENTS_MAX];
    char                         topics[CONF_EVENTS_MAX][CONF_TOPIC_MAX];
    uint8_t                      payloads[CONF_EVENTS_MAX][CONF_PAYLOAD_MAX];
    size_t                       payload_lens[CONF_EVENTS_MAX];
} conf_recorder_t;

static void on_event(const az_iot_mqtt_event_t* evt, void* ctx)
{
    conf_recorder_t* r = (conf_recorder_t*)ctx;
    if (r->count >= CONF_EVENTS_MAX) return;
    size_t i = r->count++;
    r->kinds[i]      = evt->kind;
    r->statuses[i]   = evt->status;
    r->packet_ids[i] = evt->packet_id;
    if (evt->message)
    {
        if (evt->message->topic)
        {
            size_t n = strlen(evt->message->topic);
            if (n >= CONF_TOPIC_MAX) n = CONF_TOPIC_MAX - 1;
            memcpy(r->topics[i], evt->message->topic, n);
            r->topics[i][n] = '\0';
        }
        size_t plen = evt->message->payload_len;
        if (plen > CONF_PAYLOAD_MAX) plen = CONF_PAYLOAD_MAX;
        if (plen) memcpy(r->payloads[i], evt->message->payload, plen);
        r->payload_lens[i] = plen;
    }
}

/* Pump process_loop() until either `predicate(recorder)` is true or the
 * timeout elapses. Returns true if the predicate became true. */
typedef int (*conf_predicate_fn)(const conf_recorder_t*);

static int wait_until(az_iot_mqtt_client_t* c, const conf_recorder_t* r, conf_predicate_fn p, unsigned timeout_ms)
{
    unsigned long deadline = conf_now_ms() + timeout_ms;
    while (conf_now_ms() < deadline)
    {
        c->iface->process_loop(c, 50);
        if (p(r)) return 1;
        conf_sleep_ms(10);
    }
    return p(r);
}

static int saw_connected_ok(const conf_recorder_t* r)
{
    for (size_t i = 0; i < r->count; ++i)
    {
        if (r->kinds[i] == AZ_IOT_MQTT_EVT_CONNECTED && r->statuses[i] == AZ_IOT_OK) return 1;
    }
    return 0;
}

static int saw_subscribe_ack_ok(const conf_recorder_t* r)
{
    for (size_t i = 0; i < r->count; ++i)
    {
        if (r->kinds[i] == AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK && r->statuses[i] == AZ_IOT_OK) return 1;
    }
    return 0;
}

static int saw_message(const conf_recorder_t* r)
{
    for (size_t i = 0; i < r->count; ++i)
    {
        if (r->kinds[i] == AZ_IOT_MQTT_EVT_MESSAGE) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */
/* ------------------------------------------------------------------------- */

static void unique_client_id(char* buf, size_t cap, const char* prefix)
{
    /* Cheap uniqueness: prefix + monotonic ticks + pid-ish. */
    snprintf(buf, cap, "%s-%lu", prefix, conf_now_ms());
}

static az_iot_mqtt_client_t* make_client(void)
{
    az_iot_mqtt_client_t* c = g_factory->create(g_factory->factory_ctx);
    assert_non_null(c);
    assert_non_null(c->iface);
    return c;
}

static void destroy_client(az_iot_mqtt_client_t* c)
{
    if (c && c->iface && c->iface->destroy) c->iface->destroy(c);
}

static void connect_client(az_iot_mqtt_client_t* c, conf_recorder_t* rec, const char* client_id)
{
    c->iface->set_inbound_cb(c, on_event, rec);
    az_iot_mqtt_connect_options_t copts = {0};
    copts.host = g_host;
    copts.port = g_port;
    copts.client_id = client_id;
    copts.keep_alive_seconds = 30;
    copts.connect_timeout_ms = k_step_timeout_ms;
    assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);
    assert_true(wait_until(c, rec, saw_connected_ok, k_step_timeout_ms));
}

/* ------------------------------------------------------------------------- */
/* test cases                                                                 */
/* ------------------------------------------------------------------------- */

static void connect_disconnect_roundtrip(void** state)
{
    (void)state;
    char cid[64]; unique_client_id(cid, sizeof(cid), "az-iot-conf-conn");
    conf_recorder_t rec = {0};
    az_iot_mqtt_client_t* c = make_client();
    connect_client(c, &rec, cid);

    assert_int_equal(c->iface->disconnect(c), AZ_IOT_OK);
    /* Disconnect ack arrives as DISCONNECTED event; allow up to timeout but
     * don't fail if the broker tears down silently. */
    (void)wait_until(c, &rec, saw_message /* unrelated; just pumps */, 200);
    destroy_client(c);
}

static void publish_subscribe_roundtrip(void** state)
{
    (void)state;
    char cid[64]; unique_client_id(cid, sizeof(cid), "az-iot-conf-pubsub");
    char topic[128];
    snprintf(topic, sizeof(topic), "az_iot/conformance/%s", cid);

    conf_recorder_t rec = {0};
    az_iot_mqtt_client_t* c = make_client();
    connect_client(c, &rec, cid);

    /* subscribe + wait for SUBSCRIBE_ACK */
    uint16_t sub_pid = 0;
    assert_int_equal(c->iface->subscribe(c, topic, AZ_IOT_MQTT_QOS_1, &sub_pid), AZ_IOT_OK);
    assert_int_not_equal(sub_pid, 0);
    assert_true(wait_until(c, &rec, saw_subscribe_ack_ok, k_step_timeout_ms));

    /* publish */
    static const uint8_t body[] = {'p', 'i', 'n', 'g'};
    az_iot_mqtt_message_t msg = {0};
    msg.topic = topic;
    msg.payload = body;
    msg.payload_len = sizeof(body);
    msg.qos = AZ_IOT_MQTT_QOS_1;
    uint16_t pub_pid = 0;
    assert_int_equal(c->iface->publish(c, &msg, &pub_pid), AZ_IOT_OK);
    assert_int_not_equal(pub_pid, 0);

    /* expect the message to come back through our own subscription */
    assert_true(wait_until(c, &rec, saw_message, k_step_timeout_ms));

    /* find the matching message and verify topic+payload */
    int found = 0;
    for (size_t i = 0; i < rec.count; ++i)
    {
        if (rec.kinds[i] == AZ_IOT_MQTT_EVT_MESSAGE
            && strcmp(rec.topics[i], topic) == 0
            && rec.payload_lens[i] == sizeof(body)
            && memcmp(rec.payloads[i], body, sizeof(body)) == 0)
        {
            found = 1;
            break;
        }
    }
    assert_true(found);

    /* tidy up */
    uint16_t unsub_pid = 0;
    assert_int_equal(c->iface->unsubscribe(c, topic, &unsub_pid), AZ_IOT_OK);
    (void)c->iface->disconnect(c);
    destroy_client(c);
}

static void disconnect_without_connect_is_rejected(void** state)
{
    (void)state;
    az_iot_mqtt_client_t* c = make_client();
    az_iot_result_t r = c->iface->disconnect(c);
    /* Either NOT_CONNECTED (preferred) or some adapter-specific MQTT error,
     * but never AZ_IOT_OK on a never-connected client. */
    assert_int_not_equal(r, AZ_IOT_OK);
    destroy_client(c);
}

/* ------------------------------------------------------------------------- */
/* entry point                                                                */
/* ------------------------------------------------------------------------- */

static int env_truthy(const char* v)
{
    if (!v || !*v) return 0;
    return (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y');
}

int az_iot_conformance_run(
    az_iot_conformance_suite_t suite_kind,
    az_iot_mqtt_factory_t* factory)
{
    if (!factory) return 1;

    /* Validate the factory's advertised version matches the requested suite. */
    az_iot_mqtt_version_t want = (suite_kind == AZ_IOT_CONFORMANCE_SUITE_V5)
        ? AZ_IOT_MQTT_VERSION_5 : AZ_IOT_MQTT_VERSION_3_1_1;
    if (factory->version != want)
    {
        fprintf(stderr, "conformance: factory version mismatch (got %d, want %d)\n",
                (int)factory->version, (int)want);
        return 1;
    }

    /* Resolve broker config from env. */
    const char* host = getenv("AZ_IOT_MQTT_BROKER_HOST");
    const char* port = getenv("AZ_IOT_MQTT_BROKER_PORT");
    const char* skip = getenv("AZ_IOT_MQTT_BROKER_SKIP");

    if (env_truthy(skip) || !host || !*host)
    {
        fprintf(stderr,
            "conformance: skipped (set AZ_IOT_MQTT_BROKER_HOST to a reachable broker; "
            "current: host=%s skip=%s)\n",
            host ? host : "(unset)", skip ? skip : "(unset)");
        return 77; /* CTest SKIP_RETURN_CODE */
    }

    g_factory = factory;
    g_host    = host;
    g_port    = port ? (uint16_t)atoi(port) : (uint16_t)1883;

    fprintf(stderr,
        "conformance: running %s suite against %s:%u\n",
        (suite_kind == AZ_IOT_CONFORMANCE_SUITE_V5) ? "MQTTv5" : "MQTTv3.1.1",
        g_host, (unsigned)g_port);

    const struct CMUnitTest tests[] = {
        cmocka_unit_test(connect_disconnect_roundtrip),
        cmocka_unit_test(publish_subscribe_roundtrip),
        cmocka_unit_test(disconnect_without_connect_is_rejected),
    };
    int failed = cmocka_run_group_tests(tests, NULL, NULL);
    return (failed == 0) ? 0 : 1;
}
