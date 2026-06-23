// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Phase 3.1 - TelemetryClient unit tests, driven through the public API and
 * the in-memory mock_mqtt_iface. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_telemetry_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "support/mock_mqtt_iface.h"

/* ------------------------------------------------------------------------- */
/* fixtures                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct send_record_tag
{
    bool                fired;
    az_iot_result_t status;
} send_record_t;

static void on_send(az_iot_result_t status, void* user_ctx)
{
    send_record_t* r = (send_record_t*)user_ctx;
    r->fired = true;
    r->status = status;
}

typedef struct fixture_tag
{
    az_iot_connection_client_t    conn;
    az_iot_telemetry_client_t   tc;
    az_iot_mqtt_factory_t*      factory;
    az_iot_mock_mqtt_client_t*  mock; /* convenience alias */
} fixture_t;

static int setup(void** state)
{
    fixture_t* fx = (fixture_t*)calloc(1, sizeof(*fx));
    assert_non_null(fx);

    az_iot_connection_client_options_t opts = {0};
    opts.host = "broker.example";
    opts.port = 8883;
    opts.client_id = "ut-device";
    assert_int_equal(az_iot_connection_client_init(&fx->conn, &opts), AZ_IOT_OK);

    fx->factory = az_iot_mock_mqtt_factory_create(
        AZ_IOT_MQTT_VERSION_3_1_1);
    assert_non_null(fx->factory);

    assert_int_equal(az_iot_telemetry_client_init(&fx->tc, &fx->conn), AZ_IOT_OK);

    *state = fx;
    return 0;
}

static int teardown(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    if (fx)
    {
        az_iot_telemetry_client_deinit(&fx->tc);
        az_iot_connection_client_deinit(&fx->conn);
        free(fx);
    }
    return 0;
}

/* Drive the connection through CONNECTING -> CONNECTED on the mock. */
static void open_to_connected(fixture_t* fx)
{
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory),
                     AZ_IOT_OK);
    assert_int_equal(az_iot_connection_client_open(&fx->conn), AZ_IOT_OK);
    fx->mock = az_iot_mock_mqtt_factory_last_client(fx->factory);
    assert_non_null(fx->mock);
    /* Drop the CONNECT call from the history so PUBLISH lands at index 0. */
    az_iot_mock_mqtt_client_clear_calls(fx->mock);

    assert_true(az_iot_mock_mqtt_client_inject_connected(fx->mock, AZ_IOT_OK));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);
    /* process_loop is called from inside do_work() to deliver the injected
     * event; that adds a PROCESS_LOOP call to the mock history. Clear again so
     * the test can index PUBLISH at 0. */
    az_iot_mock_mqtt_client_clear_calls(fx->mock);
}

/* ------------------------------------------------------------------------- */
/* tests                                                                     */
/* ------------------------------------------------------------------------- */

static void init_rejects_nulls(void** state)
{
    (void)state;
    az_iot_telemetry_client_t tc;
    az_iot_connection_client_t* dummy = (az_iot_connection_client_t*)(uintptr_t)1;
    assert_int_equal(az_iot_telemetry_client_init(NULL, dummy), AZ_IOT_ERR_INVALID_ARG);
    assert_int_equal(az_iot_telemetry_client_init(&tc, NULL), AZ_IOT_ERR_INVALID_ARG);
    /* deinit(NULL) is a no-op. */
    az_iot_telemetry_client_deinit(NULL);
}

static void send_before_connect_returns_not_connected(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    assert_int_equal(az_iot_connection_client_register_mqtt_factory(&fx->conn, fx->factory),
                     AZ_IOT_OK);
    /* Not opened yet - publish must refuse. */
    static const uint8_t payload[] = "hello";
    az_iot_telemetry_message_t msg = {0};
    msg.payload = payload;
    msg.payload_len = sizeof(payload) - 1;
    assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL),
                     AZ_IOT_ERR_NOT_CONNECTED);
}

static void send_publishes_qos1(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);

    static const uint8_t payload[] = "hello";
    az_iot_telemetry_message_t msg = {0};
    msg.payload = payload;
    msg.payload_len = sizeof(payload) - 1;

    send_record_t r = {0};
    assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, on_send, &r), AZ_IOT_OK);

    /* Always QoS 1 -> publish() called once with the expected D2C topic. */
    assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
    const az_iot_mock_call_t* c0 = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
    assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_PUBLISH);
    assert_string_equal(c0->topic, "devices/ut-device/messages/events/");
    assert_int_equal(c0->payload_len, 5);
    assert_memory_equal(c0->payload, "hello", 5);
    assert_int_equal(c0->qos, AZ_IOT_MQTT_QOS_1);

    /* QoS 1 -> ack callback NOT yet fired (deferred until PUBACK). */
    assert_false(r.fired);
}

static void send_qos1_defers_cb_until_puback(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);

    static const uint8_t payload[] = "{\"t\":1}";
    az_iot_telemetry_message_t msg = {0};
    msg.payload = payload;
    msg.payload_len = sizeof(payload) - 1;

    send_record_t r = {0};
    assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, on_send, &r), AZ_IOT_OK);

    /* publish() was called and a packet_id was assigned. */
    assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
    const az_iot_mock_call_t* c0 = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
    assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_PUBLISH);
    assert_int_equal(c0->qos, AZ_IOT_MQTT_QOS_1);
    uint16_t pid = c0->packet_id;
    assert_true(pid != 0);

    /* QoS 1 -> ack callback NOT yet fired. */
    assert_false(r.fired);

    /* Inject the matching PUBLISH_ACK. */
    az_iot_mqtt_event_t evt = {0};
    evt.kind      = AZ_IOT_MQTT_EVT_PUBLISH_ACK;
    evt.packet_id = pid;
    evt.status    = AZ_IOT_OK;
    assert_true(az_iot_mock_mqtt_client_inject_event(fx->mock, &evt));
    assert_int_equal(az_iot_connection_client_do_work(&fx->conn, 0), AZ_IOT_OK);

    assert_true(r.fired);
    assert_int_equal(r.status, AZ_IOT_OK);
}

static void send_propagates_content_type_and_properties(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);

    az_iot_telemetry_property_t props[] = {
        { AZ_IOT_MSG_PROP_CONTENT_TYPE, "application/json" },
        { AZ_IOT_MSG_PROP_CONTENT_ENCODING, "utf-8" },
        { "k1", "v1" },
        { "k2", "v2" }
    };
    static const uint8_t payload[] = "p";
    az_iot_telemetry_message_t msg = {0};
    msg.payload          = payload;
    msg.payload_len      = 1;
    msg.properties       = props;
    msg.properties_count = 4;

    assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL), AZ_IOT_OK);

    assert_int_equal(az_iot_mock_mqtt_client_call_count(fx->mock), 1);
    const az_iot_mock_call_t* c0 = az_iot_mock_mqtt_client_call_at(fx->mock, 0);
    assert_int_equal(c0->kind, AZ_IOT_MOCK_CALL_PUBLISH);
    /* IoT Hub Classic topic with system + application properties appended. */
    assert_string_equal(
        c0->topic,
        "devices/ut-device/messages/events/$.ct=application/json&$.ce=utf-8&k1=v1&k2=v2");
}

static void send_rejects_invalid_args(void** state)
{
    fixture_t* fx = (fixture_t*)*state;
    open_to_connected(fx);

    az_iot_telemetry_message_t msg = {0};
    msg.payload_len = 4; /* but payload is NULL */
    assert_int_equal(az_iot_telemetry_client_send(&fx->tc, &msg, NULL, NULL),
                     AZ_IOT_ERR_INVALID_ARG);
    assert_int_equal(az_iot_telemetry_client_send(NULL, &msg, NULL, NULL),
                     AZ_IOT_ERR_INVALID_ARG);
    assert_int_equal(az_iot_telemetry_client_send(&fx->tc, NULL, NULL, NULL),
                     AZ_IOT_ERR_INVALID_ARG);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(init_rejects_nulls),
        cmocka_unit_test_setup_teardown(send_before_connect_returns_not_connected, setup, teardown),
        cmocka_unit_test_setup_teardown(send_publishes_qos1, setup, teardown),
        cmocka_unit_test_setup_teardown(send_qos1_defers_cb_until_puback, setup, teardown),
        cmocka_unit_test_setup_teardown(send_propagates_content_type_and_properties, setup, teardown),
        cmocka_unit_test_setup_teardown(send_rejects_invalid_args, setup, teardown),
    };
    return cmocka_run_group_tests_name("telemetry_client", tests, NULL, NULL);
}
