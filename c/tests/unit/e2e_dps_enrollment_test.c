// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* e2e/service/e2e_dps_enrollment.h: which enrollment, read back after a resent enrollment PUT got
 * 409, counts as the one requested. No network is used. */

#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "e2e_dps_enrollment.h"

static void the_top_level_device_id_is_matched(void** state)
{
  (void)state;
  const char* json = "{\"registrationId\":\"r1\",\"deviceId\":\"d1\",\"initialTwin\":{\"tags\":{"
                     "\"deviceId\":\"x\"}}}";
  assert_true(e2e_dps_enrollment_assigns_device(json, "r1", "d1"));
  assert_false(e2e_dps_enrollment_assigns_device(json, "r1", "x"));
  assert_false(e2e_dps_enrollment_assigns_device(json, "r1", NULL));
}

static void a_nested_device_id_is_not_the_assignment(void** state)
{
  (void)state;
  const char* json = "{\"registrationId\":\"r1\",\"initialTwin\":{\"tags\":{\"deviceId\":\"d1\"}}}";
  assert_false(e2e_dps_enrollment_assigns_device(json, "r1", "d1"));
  assert_true(e2e_dps_enrollment_assigns_device(json, "r1", NULL));
}

static void an_absent_or_null_device_id_assigns_the_registration_id(void** state)
{
  (void)state;
  assert_true(e2e_dps_enrollment_assigns_device("{\"registrationId\":\"r1\"}", "r1", NULL));
  assert_true(e2e_dps_enrollment_assigns_device("{\"registrationId\":\"r1\"}", "r1", "r1"));
  assert_true(e2e_dps_enrollment_assigns_device(
      "{\"registrationId\":\"r1\",\"deviceId\":null}", "r1", NULL));
  assert_false(e2e_dps_enrollment_assigns_device(
      "{\"registrationId\":\"r1\",\"deviceId\":\"other\"}", "r1", NULL));
}

static void json_whitespace_is_accepted(void** state)
{
  (void)state;
  assert_true(e2e_dps_enrollment_assigns_device(
      "{ \"registrationId\" : \"r1\" ,\n \"deviceId\" : \"d1\" }\n", "r1", "d1"));
}

static void another_registration_id_is_refused(void** state)
{
  (void)state;
  assert_false(e2e_dps_enrollment_assigns_device(
      "{\"registrationId\":\"r2\",\"deviceId\":\"d1\"}", "r1", "d1"));
  assert_false(e2e_dps_enrollment_assigns_device("{\"deviceId\":\"d1\"}", "r1", "d1"));
}

static void a_truncated_or_malformed_enrollment_is_refused(void** state)
{
  (void)state;
  /* A response cut before a deviceId naming another device must not fall back to the default. */
  assert_false(e2e_dps_enrollment_assigns_device(
      "{\"registrationId\":\"r1\",\"initialTwin\":{}", "r1", NULL));
  assert_false(e2e_dps_enrollment_assigns_device(
      "{\"registrationId\":\"r1\",\"deviceId\":\"d1\"", "r1", "d1"));
  assert_false(e2e_dps_enrollment_assigns_device(
      "{\"registrationId\":\"r1\"} {\"deviceId\":\"other\"}", "r1", NULL));
  assert_false(e2e_dps_enrollment_assigns_device("not json", "r1", NULL));
  assert_false(e2e_dps_enrollment_assigns_device("", "r1", NULL));
  assert_false(
      e2e_dps_enrollment_assigns_device("{\"registrationId\":\"r1\",\"deviceId\":7}", "r1", NULL));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(the_top_level_device_id_is_matched),
    cmocka_unit_test(a_nested_device_id_is_not_the_assignment),
    cmocka_unit_test(an_absent_or_null_device_id_assigns_the_registration_id),
    cmocka_unit_test(json_whitespace_is_accepted),
    cmocka_unit_test(another_registration_id_is_refused),
    cmocka_unit_test(a_truncated_or_malformed_enrollment_is_refused),
  };
  return cmocka_run_group_tests_name("e2e_dps_enrollment", tests, NULL, NULL);
}
