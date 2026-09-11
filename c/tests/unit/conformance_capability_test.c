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

#include <cmocka.h>

#include "az_iot_conformance.h"

/* `allow_value` is the value of AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN, NULL when
 * unset. Passing it in rather than setting the environment keeps this runnable
 * on every leg: setenv() is POSIX and does not exist on MSVC, and a contract
 * this central should not be checked on some platforms only. */
static int report(const char* allow_value)
{
  return az_iot_conformance_report_unproven_capability(
      "AZ_IOT_CONFORMANCE_CAP_TEST", "under test", allow_value);
}

/* The default. An unexercised claim is a failure, so a pass always means the
 * claim was checked. */
static void an_unexercised_capability_fails_by_default(void** state)
{
  (void)state;
  assert_int_equal(report(NULL), 1);
}

static void the_exact_value_one_downgrades_it_to_a_notice(void** state)
{
  (void)state;
  assert_int_equal(report("1"), 0);
}

/* Anything other than "1" is not an opt-out. The affirmative-looking values are
 * the dangerous ones: a job that sets "true" and is quietly let through would
 * believe it had opted out while proving nothing. */
static void only_the_exact_value_one_opts_out(void** state)
{
  (void)state;
  static const char* const not_an_opt_out[]
      = { "0", "true", "TRUE", "yes", "on", "", " 1", "1 ", "10", "01", "11" };

  for (size_t i = 0; i < sizeof(not_an_opt_out) / sizeof(not_an_opt_out[0]); ++i)
  {
    assert_int_equal(report(not_an_opt_out[i]), 1);
  }
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
