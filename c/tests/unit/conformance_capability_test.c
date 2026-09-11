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

/* The three end-to-end custody fields are all-or-none, judged on all three.
 *
 * An earlier version tested key_uri alone, so engine and certificate supplied
 * without it were ignored in silence -- and since the run then reported "no key
 * was supplied", the opt-out above downgraded it to a notice. A regression that
 * restores the key_uri-only test brings that back, so every combination is
 * pinned here. */
static void custody_material_is_all_or_none(void** state)
{
  (void)state;
  const char* const u = "pkcs11:object=k;type=private";
  const char* const e = "pkcs11";
  const char* const c = "/tmp/cert.pem";

  assert_int_equal(
      az_iot_conformance_custody_material_state(NULL, NULL, NULL), AZ_IOT_CONFORMANCE_CUSTODY_NONE);
  assert_int_equal(
      az_iot_conformance_custody_material_state(u, e, c), AZ_IOT_CONFORMANCE_CUSTODY_COMPLETE);

  /* One of three. The engine-only and certificate-only cases are the ones the
   * key_uri-only test used to wave through. */
  assert_int_equal(
      az_iot_conformance_custody_material_state(u, NULL, NULL), AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL);
  assert_int_equal(
      az_iot_conformance_custody_material_state(NULL, e, NULL), AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL);
  assert_int_equal(
      az_iot_conformance_custody_material_state(NULL, NULL, c), AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL);

  /* An empty string is missing, not supplied. The harnesses already map an
   * empty environment variable to NULL, and "" as a key URI would otherwise
   * pass the completeness gate and reach the TLS case. */
  assert_int_equal(
      az_iot_conformance_custody_material_state("", e, c), AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL);
  assert_int_equal(
      az_iot_conformance_custody_material_state(u, "", c), AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL);
  assert_int_equal(
      az_iot_conformance_custody_material_state(u, e, ""), AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL);
  assert_int_equal(
      az_iot_conformance_custody_material_state("", "", ""), AZ_IOT_CONFORMANCE_CUSTODY_NONE);
  assert_int_equal(
      az_iot_conformance_custody_material_state("", NULL, NULL), AZ_IOT_CONFORMANCE_CUSTODY_NONE);

  /* Two of three. */
  assert_int_equal(
      az_iot_conformance_custody_material_state(u, e, NULL), AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL);
  assert_int_equal(
      az_iot_conformance_custody_material_state(u, NULL, c), AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL);
  assert_int_equal(
      az_iot_conformance_custody_material_state(NULL, e, c), AZ_IOT_CONFORMANCE_CUSTODY_PARTIAL);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(an_unexercised_capability_fails_by_default),
    cmocka_unit_test(the_exact_value_one_downgrades_it_to_a_notice),
    cmocka_unit_test(only_the_exact_value_one_opts_out),
    cmocka_unit_test(custody_material_is_all_or_none),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
