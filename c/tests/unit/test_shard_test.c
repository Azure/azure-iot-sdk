// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* support/test_shard.h: AZ_IOT_TEST_SHARD parsing and test selection. */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "support/test_shard.h"

static void set_shard_env(const char* value)
{
#if defined(_WIN32)
  (void)_putenv_s("AZ_IOT_TEST_SHARD", value != NULL ? value : "");
#else
  if (value != NULL)
  {
    (void)setenv("AZ_IOT_TEST_SHARD", value, 1);
  }
  else
  {
    (void)unsetenv("AZ_IOT_TEST_SHARD");
  }
#endif
}

static void well_formed_specs_parse(void** state)
{
  (void)state;
  unsigned index = 99;
  unsigned count = 99;
  assert_true(az_iot_test_shard_parse("0/1", &index, &count));
  assert_int_equal(index, 0);
  assert_int_equal(count, 1);
  assert_true(az_iot_test_shard_parse("3/4", &index, &count));
  assert_int_equal(index, 3);
  assert_int_equal(count, 4);
}

static void malformed_specs_are_rejected(void** state)
{
  (void)state;
  const char* const bad[] = { "",     "1",   "/2",  "1/",  "-1/2", "1/-2",  " 1/2",  "1/ 2",
                              "1/2 ", "2/2", "5/2", "0/0", "a/2",  "1/2/3", "0x1/2", "1/1001" };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
  {
    unsigned index = 7;
    unsigned count = 7;
    if (az_iot_test_shard_parse(bad[i], &index, &count))
    {
      fail_msg("'%s' was accepted", bad[i]);
    }
    assert_int_equal(index, 7);
    assert_int_equal(count, 7);
  }
  unsigned index = 0;
  unsigned count = 0;
  assert_false(az_iot_test_shard_parse(NULL, &index, &count));
}

static void every_position_is_in_exactly_one_shard(void** state)
{
  (void)state;
  for (unsigned count = 1; count <= 5; ++count)
  {
    for (size_t position = 0; position < 23; ++position)
    {
      unsigned owners = 0;
      for (unsigned index = 0; index < count; ++index)
      {
        owners += az_iot_test_shard_selects(position, index, count) ? 1u : 0u;
      }
      assert_int_equal(owners, 1);
    }
  }
}

/* Positions of the recorder tests that ran, in the sharded groups main() runs below. */
static int g_ran[16];

#define RECORDER(n)                      \
  static void recorder_##n(void** state) \
  {                                      \
    (void)state;                         \
    g_ran[n]++;                          \
  }
RECORDER(0)
RECORDER(1)
RECORDER(2)
RECORDER(3)
RECORDER(4)
RECORDER(5)
RECORDER(6)
RECORDER(7)
RECORDER(8)
RECORDER(9)

/* Runs groups of 7 and 3 tests under shard 1/3 and checks positions 1, 4 and 7 ran: the second
 * group's positions continue from the first's. A malformed spec fails without running a test. */
static int sharded_groups_run_only_their_shard(void)
{
  const struct CMUnitTest first_group[] = {
    cmocka_unit_test(recorder_0), cmocka_unit_test(recorder_1), cmocka_unit_test(recorder_2),
    cmocka_unit_test(recorder_3), cmocka_unit_test(recorder_4), cmocka_unit_test(recorder_5),
    cmocka_unit_test(recorder_6),
  };
  const struct CMUnitTest second_group[] = {
    cmocka_unit_test(recorder_7),
    cmocka_unit_test(recorder_8),
    cmocka_unit_test(recorder_9),
  };

  set_shard_env("1/3");
  int failed = az_iot_test_shard_run_group_tests(first_group, NULL, NULL);
  failed += az_iot_test_shard_run_group_tests_name("second_group", second_group, NULL, NULL);

  set_shard_env("3/3");
  const int malformed = az_iot_test_shard_run_group_tests(first_group, NULL, NULL);
  set_shard_env(NULL);

  int errors = failed != 0 ? 1 : 0;
  for (int position = 0; position < 10; ++position)
  {
    const int want = (position % 3 == 1) ? 1 : 0;
    if (g_ran[position] != want)
    {
      fprintf(stderr, "position %d ran %d time(s), want %d\n", position, g_ran[position], want);
      errors++;
    }
  }
  if (malformed != 1)
  {
    fprintf(stderr, "a malformed AZ_IOT_TEST_SHARD returned %d, want 1\n", malformed);
    errors++;
  }
  return errors;
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(well_formed_specs_parse),
    cmocka_unit_test(malformed_specs_are_rejected),
    cmocka_unit_test(every_position_is_in_exactly_one_shard),
  };
  set_shard_env(NULL);
  int failed = cmocka_run_group_tests(tests, NULL, NULL);
  failed += sharded_groups_run_only_their_shard();
  return failed == 0 ? 0 : 1;
}
