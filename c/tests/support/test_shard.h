// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_TEST_SHARD_H
#define AZ_IOT_TEST_SHARD_H

/**
 * @file
 * @brief Splits a test executable's cmocka tests across processes that run in parallel.
 *
 * AZ_IOT_TEST_SHARD="<index>/<count>" runs only the tests whose position, counted across every
 * group run through az_iot_test_shard_run_group() in one translation unit, leaves remainder
 * <index> when divided by <count>. Shards 0 to <count>-1 together run every test exactly once.
 * Unset or empty: every test runs. Malformed: the group fails without running.
 *
 * Include after <cmocka.h>.
 */

#include "test_env.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * @brief Parses a shard specification.
 *
 * @param[in] spec "<index>/<count>", both decimal.
 * @param[out] index Shard index, set on success.
 * @param[out] count Number of shards, set on success.
 * @return true when @p spec is well formed and 0 <= index < count.
 */
static inline bool az_iot_test_shard_parse(const char* spec, unsigned* index, unsigned* count)
{
  if (spec == NULL || !isdigit((unsigned char)spec[0]))
  {
    return false;
  }
  char* end = NULL;
  const unsigned long parsed_index = strtoul(spec, &end, 10);
  if (*end != '/' || !isdigit((unsigned char)end[1]))
  {
    return false;
  }
  const char* count_text = end + 1;
  const unsigned long parsed_count = strtoul(count_text, &end, 10);
  if (*end != '\0' || parsed_count == 0 || parsed_count > 1000 || parsed_index >= parsed_count)
  {
    return false;
  }
  *index = (unsigned)parsed_index;
  *count = (unsigned)parsed_count;
  return true;
}

/**
 * @brief Whether the test at @p position belongs to shard @p index of @p count.
 */
static inline bool az_iot_test_shard_selects(size_t position, unsigned index, unsigned count)
{
  return position % count == index;
}

/**
 * @brief Runs the tests of a group that belong to this process's shard (AZ_IOT_TEST_SHARD).
 *
 * Same contract as cmocka's _cmocka_run_group_tests(). A group with no test in this shard is
 * not run, so its setup and teardown are not called.
 *
 * @return Number of failed tests; 1 when AZ_IOT_TEST_SHARD is malformed or memory runs out.
 */
static inline int az_iot_test_shard_run_group(
    const char* group_name,
    const struct CMUnitTest* tests,
    size_t test_count,
    CMFixtureFunction group_setup,
    CMFixtureFunction group_teardown)
{
  /* Positions continue across groups, so single-test groups spread over the shards too. */
  static size_t next_position = 0;
  const size_t first_position = next_position;
  next_position += test_count;

  char* spec = az_iot_test_env_dup("AZ_IOT_TEST_SHARD");
  if (spec == NULL)
  {
    return _cmocka_run_group_tests(group_name, tests, test_count, group_setup, group_teardown);
  }
  unsigned index = 0;
  unsigned count = 0;
  const bool parsed = az_iot_test_shard_parse(spec, &index, &count);
  if (!parsed)
  {
    fprintf(stderr, "AZ_IOT_TEST_SHARD='%s' is not <index>/<count> with index < count\n", spec);
  }
  free(spec);
  if (!parsed)
  {
    return 1;
  }

  struct CMUnitTest* selected
      = (struct CMUnitTest*)malloc(sizeof(*selected) * (test_count > 0 ? test_count : 1));
  if (selected == NULL)
  {
    fprintf(stderr, "%s: out of memory selecting shard %u/%u\n", group_name, index, count);
    return 1;
  }
  size_t selected_count = 0;
  for (size_t i = 0; i < test_count; ++i)
  {
    if (az_iot_test_shard_selects(first_position + i, index, count))
    {
      selected[selected_count++] = tests[i];
    }
  }

  printf(
      "[shard %u/%u] %s: %u of %u test(s)\n",
      index,
      count,
      group_name,
      (unsigned)selected_count,
      (unsigned)test_count);
  int failed = 0;
  if (selected_count > 0)
  {
    failed = _cmocka_run_group_tests(
        group_name, selected, selected_count, group_setup, group_teardown);
  }
  free(selected);
  return failed;
}

/** @brief az_iot_test_shard_run_group() named after the @p group_tests array, as
 * cmocka_run_group_tests() names it. */
#define az_iot_test_shard_run_group_tests(group_tests, group_setup, group_teardown) \
  az_iot_test_shard_run_group(                                                      \
      #group_tests,                                                                 \
      group_tests,                                                                  \
      sizeof(group_tests) / sizeof((group_tests)[0]),                               \
      group_setup,                                                                  \
      group_teardown)

/** @brief az_iot_test_shard_run_group() with an explicit name, as cmocka_run_group_tests_name(). */
#define az_iot_test_shard_run_group_tests_name(           \
    group_name, group_tests, group_setup, group_teardown) \
  az_iot_test_shard_run_group(                            \
      group_name,                                         \
      group_tests,                                        \
      sizeof(group_tests) / sizeof((group_tests)[0]),     \
      group_setup,                                        \
      group_teardown)

#endif // AZ_IOT_TEST_SHARD_H
