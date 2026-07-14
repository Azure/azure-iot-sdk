// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Phase 2.3 - protocol_profile + dispatch unit tests. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include "internal/dispatch.h"
#include "internal/protocol_profile.h"

/* ------------------------------------------------------------------------- */
/* protocol_profile                                                          */
/* ------------------------------------------------------------------------- */

static void profile_for_classic_role_is_v3(void** state)
{
    (void)state;
    const az_iot_protocol_profile* p =
        az_iot_protocol_profile_for_role(AZ_IOT_MQTT_ROLE_HUB_CLASSIC);
    assert_non_null(p);
    assert_int_equal(p->flavor, AZ_IOT_HUB_FLAVOR_CLASSIC);
    assert_int_equal(p->mqtt_version, AZ_IOT_MQTT_VERSION_3_1_1);
    assert_non_null(p->twin_response_topic_prefix);
    assert_non_null(p->twin_desired_topic_prefix);
    assert_non_null(p->methods_request_topic_prefix);
    assert_non_null(p->d2c_publish_topic_template);
    assert_true(p->default_request_response_timeout_ms > 0);
}

static void profile_for_dps_role_is_classic(void** state)
{
    (void)state;
    const az_iot_protocol_profile* p =
        az_iot_protocol_profile_for_role(AZ_IOT_MQTT_ROLE_DPS);
    assert_non_null(p);
    assert_int_equal(p->flavor, AZ_IOT_HUB_FLAVOR_CLASSIC);
}

static void profile_for_next_role_is_stub_null(void** state)
{
    (void)state;
    /* Next profile is now implemented. Validate it returns a valid v5 profile. */
    const az_iot_protocol_profile* p =
        az_iot_protocol_profile_for_role(AZ_IOT_MQTT_ROLE_HUB_NEXT);
    assert_non_null(p);
    assert_int_equal(p->flavor, AZ_IOT_HUB_FLAVOR_NEXT);
    assert_true(p->uses_mqtt5_properties);
}

/* ------------------------------------------------------------------------- */
/* dispatch                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct hit_record
{
    size_t hits;
    char   last_topic[128];
    void*  last_user_ctx;
} hit_record;

static void hit_handler(void* user_ctx, const az_iot_mqtt_message* msg)
{
    hit_record* h = (hit_record*)user_ctx;
    h->hits++;
    h->last_user_ctx = user_ctx;
    if (msg && msg->topic)
    {
        size_t n = strlen(msg->topic);
        if (n >= sizeof(h->last_topic)) n = sizeof(h->last_topic) - 1;
        memcpy(h->last_topic, msg->topic, n);
        h->last_topic[n] = '\0';
    }
}

static void dispatch_route_returns_false_when_no_match(void** state)
{
    (void)state;
    az_iot_dispatch_table t;
    az_iot_dispatch_init(&t);

    az_iot_mqtt_message msg = {0};
    msg.topic = "$iothub/twin/res/200/?$rid=1";
    assert_false(az_iot_dispatch_route(&t, &msg));
}

static void dispatch_routes_to_matching_prefix(void** state)
{
    (void)state;
    az_iot_dispatch_table t;
    az_iot_dispatch_init(&t);
    hit_record twin = {0};
    hit_record methods = {0};

    assert_int_equal(
        az_iot_dispatch_register_prefix(&t, "$iothub/twin/res/", hit_handler, &twin),
        AZ_IOT_OK);
    assert_int_equal(
        az_iot_dispatch_register_prefix(&t, "$iothub/methods/POST/", hit_handler, &methods),
        AZ_IOT_OK);

    az_iot_mqtt_message m_twin = {0};
    m_twin.topic = "$iothub/twin/res/200/?$rid=42";
    assert_true(az_iot_dispatch_route(&t, &m_twin));
    assert_int_equal(twin.hits, 1);
    assert_int_equal(methods.hits, 0);

    az_iot_mqtt_message m_meth = {0};
    m_meth.topic = "$iothub/methods/POST/reboot/?$rid=7";
    assert_true(az_iot_dispatch_route(&t, &m_meth));
    assert_int_equal(twin.hits, 1);
    assert_int_equal(methods.hits, 1);
}

static void dispatch_longest_prefix_wins(void** state)
{
    (void)state;
    az_iot_dispatch_table t;
    az_iot_dispatch_init(&t);
    hit_record generic = {0};
    hit_record specific = {0};

    /* Register generic first; specific should still win because its prefix is
     * longer. */
    assert_int_equal(
        az_iot_dispatch_register_prefix(&t, "$iothub/twin/", hit_handler, &generic),
        AZ_IOT_OK);
    assert_int_equal(
        az_iot_dispatch_register_prefix(&t, "$iothub/twin/res/", hit_handler, &specific),
        AZ_IOT_OK);

    az_iot_mqtt_message msg = {0};
    msg.topic = "$iothub/twin/res/200/?$rid=1";
    assert_true(az_iot_dispatch_route(&t, &msg));
    assert_int_equal(specific.hits, 1);
    assert_int_equal(generic.hits, 0);

    /* A topic that only matches the generic prefix still works. */
    msg.topic = "$iothub/twin/PATCH/properties/desired/?$version=3";
    assert_true(az_iot_dispatch_route(&t, &msg));
    assert_int_equal(specific.hits, 1);
    assert_int_equal(generic.hits, 1);
}

static void dispatch_unregister_by_ctx_removes_all_owned(void** state)
{
    (void)state;
    az_iot_dispatch_table t;
    az_iot_dispatch_init(&t);
    hit_record ctx_a = {0};
    hit_record ctx_b = {0};

    assert_int_equal(az_iot_dispatch_register_prefix(&t, "a/", hit_handler, &ctx_a), AZ_IOT_OK);
    assert_int_equal(az_iot_dispatch_register_prefix(&t, "aa/", hit_handler, &ctx_a), AZ_IOT_OK);
    assert_int_equal(az_iot_dispatch_register_prefix(&t, "b/", hit_handler, &ctx_b), AZ_IOT_OK);
    assert_int_equal(az_iot_dispatch_count(&t), 3);

    assert_int_equal(az_iot_dispatch_unregister_by_ctx(&t, &ctx_a), 2);
    assert_int_equal(az_iot_dispatch_count(&t), 1);

    az_iot_mqtt_message msg = {0};
    msg.topic = "a/x";
    assert_false(az_iot_dispatch_route(&t, &msg));
    msg.topic = "b/x";
    assert_true(az_iot_dispatch_route(&t, &msg));
    assert_int_equal(ctx_b.hits, 1);
}

static void dispatch_register_rejects_when_full(void** state)
{
    (void)state;
    az_iot_dispatch_table t;
    az_iot_dispatch_init(&t);
    hit_record ctx = {0};

    char buf[16];
    for (int i = 0; i < AZ_IOT_MAX_INBOUND_HANDLERS; ++i)
    {
        snprintf(buf, sizeof(buf), "p%02d/", i);
        assert_int_equal(az_iot_dispatch_register_prefix(&t, buf, hit_handler, &ctx), AZ_IOT_OK);
    }
    /* One more should be rejected. */
    assert_int_equal(
        az_iot_dispatch_register_prefix(&t, "overflow/", hit_handler, &ctx),
        AZ_IOT_ERR_NOT_SUPPORTED);
}

static void dispatch_register_validates_args(void** state)
{
    (void)state;
    az_iot_dispatch_table t;
    az_iot_dispatch_init(&t);
    hit_record ctx = {0};
    assert_int_equal(az_iot_dispatch_register_prefix(NULL, "p/", hit_handler, &ctx),
                     AZ_IOT_ERR_INVALID_ARG);
    assert_int_equal(az_iot_dispatch_register_prefix(&t, NULL, hit_handler, &ctx),
                     AZ_IOT_ERR_INVALID_ARG);
    assert_int_equal(az_iot_dispatch_register_prefix(&t, "p/", NULL, &ctx),
                     AZ_IOT_ERR_INVALID_ARG);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(profile_for_classic_role_is_v3),
        cmocka_unit_test(profile_for_dps_role_is_classic),
        cmocka_unit_test(profile_for_next_role_is_stub_null),
        cmocka_unit_test(dispatch_route_returns_false_when_no_match),
        cmocka_unit_test(dispatch_routes_to_matching_prefix),
        cmocka_unit_test(dispatch_longest_prefix_wins),
        cmocka_unit_test(dispatch_unregister_by_ctx_removes_all_owned),
        cmocka_unit_test(dispatch_register_rejects_when_full),
        cmocka_unit_test(dispatch_register_validates_args),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
