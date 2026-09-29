// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Unit tests for az_iot_env_read(). The variable is set by this executable and
 * read by the library, which in a Windows shared build is a DLL with its own
 * C runtime: that crossing is what the helper exists for. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "internal/env.h"

#define TEST_ENV "AZ_IOT_ENV_READ_TEST"

/* NULL unsets. */
static void set_env(const char* value)
{
#ifdef _WIN32
  assert_int_equal(_putenv_s(TEST_ENV, value ? value : ""), 0);
#else
  if (value)
  {
    assert_int_equal(setenv(TEST_ENV, value, 1), 0);
  }
  else
  {
    assert_int_equal(unsetenv(TEST_ENV), 0);
  }
#endif
}

static int teardown(void** state)
{
  (void)state;
  set_env(NULL);
  return 0;
}

static void an_unset_variable_reads_as_empty(void** state)
{
  (void)state;
  char buf[8];
  memset(buf, 'x', sizeof(buf));
  set_env(NULL);
  assert_int_equal(az_iot_env_read(TEST_ENV, buf, sizeof(buf)), AZ_IOT_OK);
  assert_string_equal(buf, "");
}

static void a_value_set_by_the_application_is_read(void** state)
{
  (void)state;
  char buf[8];
  set_env("abc");
  assert_int_equal(az_iot_env_read(TEST_ENV, buf, sizeof(buf)), AZ_IOT_OK);
  assert_string_equal(buf, "abc");
}

static void a_value_of_cap_minus_one_fits(void** state)
{
  (void)state;
  char buf[8];
  set_env("1234567");
  assert_int_equal(az_iot_env_read(TEST_ENV, buf, sizeof(buf)), AZ_IOT_OK);
  assert_string_equal(buf, "1234567");
}

static void a_value_of_cap_does_not_fit_and_reads_as_empty(void** state)
{
  (void)state;
  char buf[8];
  memset(buf, 'x', sizeof(buf));
  set_env("12345678");
  assert_int_equal(az_iot_env_read(TEST_ENV, buf, sizeof(buf)), AZ_IOT_ERR_NOT_ENOUGH_SPACE);
  assert_string_equal(buf, "");
}

static void a_changed_value_is_read_again(void** state)
{
  (void)state;
  char buf[8];
  set_env("first");
  assert_int_equal(az_iot_env_read(TEST_ENV, buf, sizeof(buf)), AZ_IOT_OK);
  set_env("second");
  assert_int_equal(az_iot_env_read(TEST_ENV, buf, sizeof(buf)), AZ_IOT_OK);
  assert_string_equal(buf, "second");
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_teardown(an_unset_variable_reads_as_empty, teardown),
    cmocka_unit_test_teardown(a_value_set_by_the_application_is_read, teardown),
    cmocka_unit_test_teardown(a_value_of_cap_minus_one_fits, teardown),
    cmocka_unit_test_teardown(a_value_of_cap_does_not_fit_and_reads_as_empty, teardown),
    cmocka_unit_test_teardown(a_changed_value_is_read_again, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
