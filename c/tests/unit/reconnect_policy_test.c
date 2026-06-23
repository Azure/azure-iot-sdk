// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
/* Phase 2.2 - reconnect-policy unit tests for the pure delay calculator. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "internal/reconnect.h"

static void disabled_when_initial_delay_is_zero(void** state)
{
    (void)state;
    az_iot_reconnect_policy_t p = {0};
    uint64_t rng = 1;
    assert_int_equal(az_iot_reconnect_delay_ms(&p, 1, &rng), 0);
    assert_int_equal(az_iot_reconnect_delay_ms(&p, 5, &rng), 0);
}

static void no_jitter_doubles_until_cap(void** state)
{
    (void)state;
    az_iot_reconnect_policy_t p = {0};
    p.initial_delay_ms = 100;
    p.max_delay_ms     = 1000;
    p.jitter_pct       = 0;

    uint64_t rng = 42;
    assert_int_equal(az_iot_reconnect_delay_ms(&p, 1, &rng), 100);
    assert_int_equal(az_iot_reconnect_delay_ms(&p, 2, &rng), 200);
    assert_int_equal(az_iot_reconnect_delay_ms(&p, 3, &rng), 400);
    assert_int_equal(az_iot_reconnect_delay_ms(&p, 4, &rng), 800);
    /* 100 << 4 = 1600 > cap; clamped. */
    assert_int_equal(az_iot_reconnect_delay_ms(&p, 5, &rng), 1000);
    assert_int_equal(az_iot_reconnect_delay_ms(&p, 100, &rng), 1000);
}

static void jitter_stays_within_band(void** state)
{
    (void)state;
    az_iot_reconnect_policy_t p = {0};
    p.initial_delay_ms = 1000;
    p.max_delay_ms     = 1000;
    p.jitter_pct       = 20;     /* +/- 20% of base */

    uint64_t rng = 0xDEADBEEFCAFEBABEull;
    for (int i = 0; i < 200; ++i)
    {
        uint32_t d = az_iot_reconnect_delay_ms(&p, 1, &rng);
        /* base = 1000, jitter +/- 200, clamped to [1, max=1000]. So [800, 1000]. */
        assert_true(d >= 800);
        assert_true(d <= 1000);
    }
}

static void zero_max_delay_means_initial_is_the_cap(void** state)
{
    (void)state;
    az_iot_reconnect_policy_t p = {0};
    p.initial_delay_ms = 250;
    p.max_delay_ms     = 0;     /* unset; should be treated as = initial_delay */
    p.jitter_pct       = 0;

    uint64_t rng = 7;
    assert_int_equal(az_iot_reconnect_delay_ms(&p, 1, &rng), 250);
    assert_int_equal(az_iot_reconnect_delay_ms(&p, 10, &rng), 250);
}

static void attempt_zero_treated_as_one(void** state)
{
    (void)state;
    az_iot_reconnect_policy_t p = {0};
    p.initial_delay_ms = 100;
    p.max_delay_ms     = 1000;

    uint64_t rng = 1;
    assert_int_equal(az_iot_reconnect_delay_ms(&p, 0, &rng), 100);
}

static void shift_saturates_no_ub(void** state)
{
    (void)state;
    az_iot_reconnect_policy_t p = {0};
    p.initial_delay_ms = 1;
    p.max_delay_ms     = 60000;

    uint64_t rng = 1;
    /* Big attempts shouldn't UB or wrap; result must be the cap. */
    assert_int_equal(az_iot_reconnect_delay_ms(&p, 1000, &rng), 60000);
    assert_int_equal(az_iot_reconnect_delay_ms(&p, UINT32_MAX, &rng), 60000);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(disabled_when_initial_delay_is_zero),
        cmocka_unit_test(no_jitter_doubles_until_cap),
        cmocka_unit_test(jitter_stays_within_band),
        cmocka_unit_test(zero_max_delay_means_initial_is_the_cap),
        cmocka_unit_test(attempt_zero_treated_as_one),
        cmocka_unit_test(shift_saturates_no_ub),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
