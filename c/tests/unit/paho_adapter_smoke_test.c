// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Construction-level smoke test for the Paho adapter. Does not touch the
 * network; verifies that the factory advertises the right version and that a
 * created client carries a fully-populated iface vtable. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/adapters/az_iot_adapter_paho.h"

static void v3_factory_creates_v3_client(void** state)
{
    (void)state;
    az_iot_mqtt_factory* f = az_iot_paho_factory_create_v3_1_1();
    assert_non_null(f);
    assert_int_equal(f->version, AZ_IOT_MQTT_VERSION_3_1_1);

    az_iot_mqtt_client* c = f->create(f->factory_ctx);
    assert_non_null(c);
    assert_non_null(c->iface);
    assert_int_equal(c->iface->version, AZ_IOT_MQTT_VERSION_3_1_1);
    assert_non_null(c->iface->connect);
    assert_non_null(c->iface->disconnect);
    assert_non_null(c->iface->subscribe);
    assert_non_null(c->iface->unsubscribe);
    assert_non_null(c->iface->publish);
    assert_non_null(c->iface->process_loop);
    assert_non_null(c->iface->set_inbound_cb);
    assert_non_null(c->iface->destroy);

    /* Operations on a not-yet-connected client are rejected gracefully. */
    assert_int_equal(c->iface->disconnect(c), AZ_IOT_ERR_NOT_CONNECTED);
    /* process_loop on an empty queue is a no-op. */
    assert_int_equal(c->iface->process_loop(c, 0), AZ_IOT_OK);

    c->iface->destroy(c);
    az_iot_paho_factory_destroy(f);
}

static void v5_factory_creates_v5_client(void** state)
{
    (void)state;
    az_iot_mqtt_factory* f = az_iot_paho_factory_create_v5();
    assert_non_null(f);
    assert_int_equal(f->version, AZ_IOT_MQTT_VERSION_5);

    az_iot_mqtt_client* c = f->create(f->factory_ctx);
    assert_non_null(c);
    assert_int_equal(c->iface->version, AZ_IOT_MQTT_VERSION_5);
    c->iface->destroy(c);
    az_iot_paho_factory_destroy(f);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(v3_factory_creates_v3_client),
        cmocka_unit_test(v5_factory_creates_v5_client),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
