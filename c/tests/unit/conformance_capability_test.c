/* Copyright (c) Microsoft Corporation. All rights reserved. */
/* SPDX-License-Identifier: MIT */

/* The opt-out policy for a declared-but-unexercised capability.
 *
 * This is the suite's own exit-code contract, and nothing else in CI can catch
 * a regression in it: every job either supplies a real token (so the capability
 * is exercised) or sets the opt-out (so the failure path never runs). A change
 * that made the gate always return 0 would leave every pipeline green while
 * silently restoring the behaviour this gate exists to remove -- an adapter
 * claiming a capability the suite never checked.
 *
 * The return value is what the run adds to its failure count, so 1 means the
 * suite exits non-zero and 0 means it does not. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdlib.h>

#include <cmocka.h>

#include "az_iot_conformance.h"

static int report(void)
{
  return az_iot_conformance_report_unproven_capability("AZ_IOT_CONFORMANCE_CAP_TEST", "under test");
}

static void set_allow(const char* value)
{
  if (value == NULL)
  {
    assert_int_equal(unsetenv("AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN"), 0);
  }
  else
  {
    assert_int_equal(setenv("AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN", value, 1), 0);
  }
}

/* The default. An unexercised claim is a failure, so a pass always means the
 * claim was checked. */
static void an_unexercised_capability_fails_by_default(void** state)
{
  (void)state;
  set_allow(NULL);
  assert_int_equal(report(), 1);
}

static void the_exact_value_one_downgrades_it_to_a_notice(void** state)
{
  (void)state;
  set_allow("1");
  assert_int_equal(report(), 0);
  set_allow(NULL);
}

/* Anything other than "1" is not an opt-out. Values that look affirmative are
 * the dangerous ones: a job that sets "true" or "yes" and is quietly let
 * through would believe it had opted out while proving nothing. */
static void only_the_exact_value_one_opts_out(void** state)
{
  (void)state;
  static const char* const not_an_opt_out[]
      = { "0", "true", "TRUE", "yes", "on", "", " 1", "1 ", "10", "01", "11" };

  for (size_t i = 0; i < sizeof(not_an_opt_out) / sizeof(not_an_opt_out[0]); ++i)
  {
    set_allow(not_an_opt_out[i]);
    assert_int_equal(report(), 1);
  }
  set_allow(NULL);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(an_unexercised_capability_fails_by_default),
    cmocka_unit_test(the_exact_value_one_downgrades_it_to_a_notice),
    cmocka_unit_test(only_the_exact_value_one_opts_out),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
