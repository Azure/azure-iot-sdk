// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Smoke test for the Rust MQTT adapter shell.
 *
 * The shell ships without a Rust runtime, so we simulate one by installing a
 * fake FFI table and confirming that the adapter:
 *   - returns NULL from the factory before any table is installed,
 *   - rejects partial / NULL FFI tables at install time,
 *   - on a valid install, produces a client whose iface vtable is wired to
 *     the FFI calls and forwards lifecycle correctly,
 *   - returns NULL again after `_install(NULL)` uninstalls the table.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "azure/iot/adapters/az_iot_adapter_rust_mqtt.h"
#include "../../adapters/rust_mqtt/az_iot_mqtt_rust_ffi.h"

/* ------------------------------------------------------------------------- */
/* Fake Rust client + FFI implementation. */

typedef struct fake_client_tag
{
    int  connected;
    int  destroyed;
    int  process_loop_calls;
    az_iot_mqtt_event_cb cb;
    void* cb_ctx;
} fake_client_t;

static fake_client_t* g_last_client;
static int            g_create_calls;
static int            g_destroy_calls;

static az_iot_rust_mqtt_client* fake_create(az_iot_mqtt_version_t v, az_iot_mqtt_role_t r)
{
    assert_int_equal(v, AZ_IOT_MQTT_VERSION_5);
    assert_int_equal(r, AZ_IOT_MQTT_ROLE_HUB_NEXT);
    g_create_calls++;
    fake_client_t* fc = (fake_client_t*)test_calloc(1, sizeof(*fc));
    g_last_client = fc;
    return (az_iot_rust_mqtt_client*)fc;
}

static void fake_destroy(az_iot_rust_mqtt_client* c)
{
    fake_client_t* fc = (fake_client_t*)c;
    fc->destroyed = 1;
    g_destroy_calls++;
    test_free(fc);
}

static az_iot_result_t fake_connect(az_iot_rust_mqtt_client* c,
                                        const az_iot_mqtt_connect_options_t* opts)
{
    (void)opts;
    ((fake_client_t*)c)->connected = 1;
    return AZ_IOT_OK;
}
static az_iot_result_t fake_disconnect(az_iot_rust_mqtt_client* c)
{
    ((fake_client_t*)c)->connected = 0;
    return AZ_IOT_OK;
}
static az_iot_result_t fake_subscribe(az_iot_rust_mqtt_client* c, const char* t,
                                          az_iot_mqtt_qos_t q, uint16_t* p)
{ (void)c; (void)t; (void)q; if (p) *p = 7; return AZ_IOT_OK; }
static az_iot_result_t fake_unsubscribe(az_iot_rust_mqtt_client* c, const char* t,
                                            uint16_t* p)
{ (void)c; (void)t; if (p) *p = 8; return AZ_IOT_OK; }
static az_iot_result_t fake_publish(az_iot_rust_mqtt_client* c,
                                        const az_iot_mqtt_message_t* m, uint16_t* p)
{ (void)c; (void)m; if (p) *p = 9; return AZ_IOT_OK; }
static az_iot_result_t fake_process_loop(az_iot_rust_mqtt_client* c, uint32_t t)
{ (void)t; ((fake_client_t*)c)->process_loop_calls++; return AZ_IOT_OK; }
static void fake_set_inbound_cb(az_iot_rust_mqtt_client* c, az_iot_mqtt_event_cb cb, void* u)
{ ((fake_client_t*)c)->cb = cb; ((fake_client_t*)c)->cb_ctx = u; }

static const az_iot_rust_mqtt_ffi_t k_fake_ffi = {
    .create         = fake_create,
    .destroy        = fake_destroy,
    .connect        = fake_connect,
    .disconnect     = fake_disconnect,
    .subscribe      = fake_subscribe,
    .unsubscribe    = fake_unsubscribe,
    .publish        = fake_publish,
    .process_loop   = fake_process_loop,
    .set_inbound_cb = fake_set_inbound_cb,
};

/* ------------------------------------------------------------------------- */

static int reset_fixture(void** s)
{
    (void)s;
    /* Always start from a clean (uninstalled) state. */
    (void)az_iot_rust_mqtt_install(NULL);
    g_last_client = NULL;
    g_create_calls = 0;
    g_destroy_calls = 0;
    return 0;
}

static void test_factory_returns_null_when_uninstalled(void** s)
{
    (void)s;
    az_iot_mqtt_factory_t* f = az_iot_rust_mqtt_factory_create_v5();
    assert_null(f);
}

static void test_install_rejects_partial_table(void** s)
{
    (void)s;
    az_iot_rust_mqtt_ffi_t bad = k_fake_ffi;
    bad.publish = NULL;
    assert_int_equal(AZ_IOT_ERR_INVALID_ARG, az_iot_rust_mqtt_install(&bad));
    /* Factory still NULL after a rejected install. */
    assert_null(az_iot_rust_mqtt_factory_create_v5());
}

static void test_install_then_factory_then_dispatch(void** s)
{
    (void)s;
    assert_int_equal(AZ_IOT_OK, az_iot_rust_mqtt_install(&k_fake_ffi));

    az_iot_mqtt_factory_t* f = az_iot_rust_mqtt_factory_create_v5();
    assert_non_null(f);
    assert_int_equal(f->version, AZ_IOT_MQTT_VERSION_5);
    assert_true((f->supported_roles_mask & (1u << AZ_IOT_MQTT_ROLE_HUB_NEXT)) != 0);

    /* Wrong role -> NULL. */
    assert_null(f->create(f->factory_ctx, AZ_IOT_MQTT_ROLE_DPS));
    assert_int_equal(g_create_calls, 0);

    az_iot_mqtt_client_t* c = f->create(f->factory_ctx, AZ_IOT_MQTT_ROLE_HUB_NEXT);
    assert_non_null(c);
    assert_int_equal(g_create_calls, 1);
    assert_non_null(g_last_client);

    az_iot_mqtt_connect_options_t opts = { .host = "h", .port = 8883, .client_id = "id" };
    assert_int_equal(AZ_IOT_OK, c->iface->connect(c, &opts));
    assert_int_equal(g_last_client->connected, 1);

    uint16_t pid = 0;
    assert_int_equal(AZ_IOT_OK, c->iface->subscribe(c, "t/#", AZ_IOT_MQTT_QOS_1, &pid));
    assert_int_equal(pid, 7);

    az_iot_mqtt_message_t m = { .topic = "t/x", .payload = (const uint8_t*)"x", .payload_len = 1,
                                    .qos = AZ_IOT_MQTT_QOS_0 };
    pid = 0;
    assert_int_equal(AZ_IOT_OK, c->iface->publish(c, &m, &pid));
    assert_int_equal(pid, 9);

    assert_int_equal(AZ_IOT_OK, c->iface->process_loop(c, 5));
    assert_int_equal(g_last_client->process_loop_calls, 1);

    assert_int_equal(AZ_IOT_OK, c->iface->disconnect(c));
    assert_int_equal(g_last_client->connected, 0);

    /* Capture the underlying handle BEFORE destroy frees both layers. */
    c->iface->destroy(c);
    assert_int_equal(g_destroy_calls, 1);

    az_iot_rust_mqtt_factory_destroy(f);
}

static void test_uninstall_disables_factory(void** s)
{
    (void)s;
    assert_int_equal(AZ_IOT_OK, az_iot_rust_mqtt_install(&k_fake_ffi));
    assert_non_null(az_iot_rust_mqtt_factory_create_v5());
    assert_int_equal(AZ_IOT_OK, az_iot_rust_mqtt_install(NULL));
    assert_null(az_iot_rust_mqtt_factory_create_v5());
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup(test_factory_returns_null_when_uninstalled, reset_fixture),
        cmocka_unit_test_setup(test_install_rejects_partial_table,         reset_fixture),
        cmocka_unit_test_setup(test_install_then_factory_then_dispatch,    reset_fixture),
        cmocka_unit_test_setup(test_uninstall_disables_factory,            reset_fixture),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
