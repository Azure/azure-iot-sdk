// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Contract tests for az_iot_mqtt_iface, exercised through the in-memory mock.
 * These tests only depend on the public iface header + the mock; no real broker. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_mqtt_iface.h"

#include "../support/mock_mqtt_iface.h"

typedef struct collected_events_tag
{
    size_t count;
    az_iot_mqtt_event_kind_t kinds[8];
    az_iot_result_t          statuses[8];
    char                         topics[8][AZ_IOT_MOCK_TOPIC_MAX];
    uint8_t                      payloads[8][AZ_IOT_MOCK_PAYLOAD_MAX];
    size_t                       payload_lens[8];
} collected_events_t;

static void on_event(const az_iot_mqtt_event_t* evt, void* ctx)
{
    collected_events_t* col = (collected_events_t*)ctx;
    if (col->count >= 8) return;
    size_t i = col->count++;
    col->kinds[i] = evt->kind;
    col->statuses[i] = evt->status;
    if (evt->message)
    {
        if (evt->message->topic)
        {
            size_t n = strlen(evt->message->topic);
            if (n >= AZ_IOT_MOCK_TOPIC_MAX) n = AZ_IOT_MOCK_TOPIC_MAX - 1;
            memcpy(col->topics[i], evt->message->topic, n);
            col->topics[i][n] = '\0';
        }
        size_t plen = evt->message->payload_len;
        if (plen > AZ_IOT_MOCK_PAYLOAD_MAX) plen = AZ_IOT_MOCK_PAYLOAD_MAX;
        if (plen) memcpy(col->payloads[i], evt->message->payload, plen);
        col->payload_lens[i] = plen;
    }
}

/* ------------------------------------------------------------------------- */

static void factory_advertises_version(void** state)
{
    (void)state;
    az_iot_mqtt_factory_t* f = az_iot_mock_mqtt_factory_create(
        AZ_IOT_MQTT_VERSION_3_1_1);
    assert_non_null(f);
    assert_int_equal(f->version, AZ_IOT_MQTT_VERSION_3_1_1);
    az_iot_mock_mqtt_factory_destroy(f);
}

static void client_carries_iface_pointer_with_version(void** state)
{
    (void)state;
    az_iot_mqtt_factory_t* f = az_iot_mock_mqtt_factory_create(
        AZ_IOT_MQTT_VERSION_5);
    az_iot_mqtt_client_t* c = f->create(f->factory_ctx);
    assert_non_null(c);
    assert_non_null(c->iface);
    assert_int_equal(c->iface->version, AZ_IOT_MQTT_VERSION_5);
    /* Every vtable slot must be populated. */
    assert_non_null(c->iface->connect);
    assert_non_null(c->iface->disconnect);
    assert_non_null(c->iface->subscribe);
    assert_non_null(c->iface->unsubscribe);
    assert_non_null(c->iface->publish);
    assert_non_null(c->iface->process_loop);
    assert_non_null(c->iface->set_inbound_cb);
    assert_non_null(c->iface->destroy);
    az_iot_mock_mqtt_factory_destroy(f);
}

static void publish_records_topic_payload_and_assigns_packet_id(void** state)
{
    (void)state;
    az_iot_mqtt_factory_t* f = az_iot_mock_mqtt_factory_create(
        AZ_IOT_MQTT_VERSION_3_1_1);
    az_iot_mqtt_client_t* c = f->create(f->factory_ctx);

    az_iot_mqtt_message_t msg = {0};
    msg.topic = "devices/dev1/messages/events/";
    static const uint8_t body[] = {'h', 'i'};
    msg.payload = body;
    msg.payload_len = sizeof(body);
    msg.qos = AZ_IOT_MQTT_QOS_1;

    uint16_t pid = 0;
    assert_int_equal(c->iface->publish(c, &msg, &pid), AZ_IOT_OK);
    assert_int_not_equal(pid, 0);

    az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_client_from(c);
    assert_int_equal(az_iot_mock_mqtt_client_call_count(m), 1);
    const az_iot_mock_call_t* call = az_iot_mock_mqtt_client_call_at(m, 0);
    assert_int_equal(call->kind, AZ_IOT_MOCK_CALL_PUBLISH);
    assert_string_equal(call->topic, "devices/dev1/messages/events/");
    assert_int_equal(call->payload_len, sizeof(body));
    assert_memory_equal(call->payload, body, sizeof(body));
    assert_int_equal(call->qos, AZ_IOT_MQTT_QOS_1);
    assert_int_equal(call->packet_id, pid);

    az_iot_mock_mqtt_factory_destroy(f);
}

static void scripted_failure_propagates_to_caller(void** state)
{
    (void)state;
    az_iot_mqtt_factory_t* f = az_iot_mock_mqtt_factory_create(
        AZ_IOT_MQTT_VERSION_3_1_1);
    az_iot_mqtt_client_t* c = f->create(f->factory_ctx);
    az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_client_from(c);

    az_iot_mock_mqtt_client_set_next_result(m, AZ_IOT_MOCK_CALL_CONNECT, AZ_IOT_ERR_MQTT);
    az_iot_mqtt_connect_options_t copts = {0};
    copts.host = "example.invalid";
    assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_ERR_MQTT);
    /* Override is one-shot; second call returns AZ_IOT_OK again. */
    assert_int_equal(c->iface->connect(c, &copts), AZ_IOT_OK);

    az_iot_mock_mqtt_factory_destroy(f);
}

static void process_loop_drains_one_event_per_call(void** state)
{
    (void)state;
    az_iot_mqtt_factory_t* f = az_iot_mock_mqtt_factory_create(
        AZ_IOT_MQTT_VERSION_3_1_1);
    az_iot_mqtt_client_t* c = f->create(f->factory_ctx);
    az_iot_mock_mqtt_client_t* m = az_iot_mock_mqtt_client_from(c);

    collected_events_t col = {0};
    c->iface->set_inbound_cb(c, on_event, &col);

    assert_true(az_iot_mock_mqtt_client_inject_connected(m, AZ_IOT_OK));
    static const uint8_t body[] = {'p', 'a', 'y'};
    assert_true(az_iot_mock_mqtt_client_inject_message(
        m, "devices/dev1/methods/POST/reboot/?$rid=42", body, sizeof(body), AZ_IOT_MQTT_QOS_0));

    assert_int_equal(col.count, 0);
    assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);
    assert_int_equal(col.count, 1);
    assert_int_equal(col.kinds[0], AZ_IOT_MQTT_EVT_CONNECTED);
    assert_int_equal(col.statuses[0], AZ_IOT_OK);

    assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);
    assert_int_equal(col.count, 2);
    assert_int_equal(col.kinds[1], AZ_IOT_MQTT_EVT_MESSAGE);
    assert_string_equal(col.topics[1], "devices/dev1/methods/POST/reboot/?$rid=42");
    assert_int_equal(col.payload_lens[1], sizeof(body));
    assert_memory_equal(col.payloads[1], body, sizeof(body));

    /* No more events queued; process_loop is still safe to call. */
    assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);
    assert_int_equal(col.count, 2);

    az_iot_mock_mqtt_factory_destroy(f);
}

static void destroy_via_iface_is_recorded(void** state)
{
    (void)state;
    az_iot_mqtt_factory_t* f = az_iot_mock_mqtt_factory_create(
        AZ_IOT_MQTT_VERSION_3_1_1);
    az_iot_mqtt_client_t* c = f->create(f->factory_ctx);
    /* Calling destroy through the iface frees the client; the factory's
     * last_client back-pointer is cleared. */
    c->iface->destroy(c);
    assert_null(az_iot_mock_mqtt_factory_last_client(f));
    az_iot_mock_mqtt_factory_destroy(f);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(factory_advertises_version),
        cmocka_unit_test(client_carries_iface_pointer_with_version),
        cmocka_unit_test(publish_records_topic_payload_and_assigns_packet_id),
        cmocka_unit_test(scripted_failure_propagates_to_caller),
        cmocka_unit_test(process_loop_drains_one_event_per_call),
        cmocka_unit_test(destroy_via_iface_is_recorded),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
