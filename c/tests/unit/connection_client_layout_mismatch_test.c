// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* Built with capacity macros (set by CMake) that differ from the SDK library's, as an application
 * overriding them only in its own build would. az_iot_connection_client_init() must refuse the
 * client without writing it, and name the value that differs. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_log.h"

#ifndef AZ_IOT_TEST_EXPECTED_MISMATCH
#error "Build with AZ_IOT_TEST_EXPECTED_MISMATCH (see tests/CMakeLists.txt)."
#endif
/* 1 if the overrides keep sizeof(az_iot_connection_client) unchanged on 64-bit targets. */
#ifndef AZ_IOT_TEST_SAME_SIZE_ON_64_BIT
#define AZ_IOT_TEST_SAME_SIZE_ON_64_BIT 0
#endif

size_t az_iot_test_connection_client_default_size(void);

/* Every error logged, so the test finds the one it expects among several. */
static char logged_errors[1024];

static void capture_error(
    void* user_ctx,
    az_iot_log_level level,
    const char* component,
    const char* file,
    int line,
    const char* msg)
{
  (void)user_ctx;
  (void)component;
  (void)file;
  (void)line;
  if (level == AZ_IOT_LOG_LEVEL_ERROR)
  {
    size_t used = strlen(logged_errors);
    (void)snprintf(logged_errors + used, sizeof(logged_errors) - used, "%s\n", msg);
  }
}

static void start_capture(void)
{
  logged_errors[0] = '\0';
  static az_iot_log_sink sink = { capture_error, NULL, AZ_IOT_LOG_LEVEL_ERROR };
  az_iot_log_set_global_sink(&sink);
}

static void a_mismatched_layout_is_refused_and_the_client_untouched(void** state)
{
  (void)state;
  if (AZ_IOT_TEST_SAME_SIZE_ON_64_BIT && sizeof(void*) == 8)
  {
    assert_int_equal(
        sizeof(az_iot_connection_client), az_iot_test_connection_client_default_size());
  }
  start_capture();

  static az_iot_connection_client client;
  static uint8_t untouched[sizeof(az_iot_connection_client)];
  memset(&client, 0x5A, sizeof(client));
  memset(untouched, 0x5A, sizeof(untouched));
  az_iot_connection_client_options options = az_iot_connection_client_options_default();

  assert_int_equal(az_iot_connection_client_init(&client, &options), AZ_IOT_ERR_INVALID_ARG);
  assert_memory_equal(&client, untouched, sizeof(client));
  assert_non_null(strstr(logged_errors, AZ_IOT_TEST_EXPECTED_MISMATCH " is "));

  az_iot_log_set_global_sink(NULL);
}

static void a_missing_layout_is_refused(void** state)
{
  (void)state;
  start_capture();
  static az_iot_connection_client client;
  az_iot_connection_client_options options = az_iot_connection_client_options_default();

  assert_int_equal(
      az_iot_connection_client_init_checked(&client, &options, NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_non_null(strstr(logged_errors, "no layout"));

  az_iot_log_set_global_sink(NULL);
}

static void a_layout_of_another_size_is_refused_before_its_fields_are_read(void** state)
{
  (void)state;
  start_capture();
  static az_iot_connection_client client;
  az_iot_connection_client_options options = az_iot_connection_client_options_default();
  az_iot_connection_client_layout layout;
  memset(&layout, 0, sizeof(layout));
  layout.layout_size = sizeof(layout) + 8;

  assert_int_equal(
      az_iot_connection_client_init_checked(&client, &options, &layout), AZ_IOT_ERR_INVALID_ARG);
  assert_non_null(strstr(logged_errors, "sizeof(az_iot_connection_client_layout) is "));
  assert_null(strstr(logged_errors, "AZ_IOT_MAX_")); /* fields not compared */

  az_iot_log_set_global_sink(NULL);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_mismatched_layout_is_refused_and_the_client_untouched),
    cmocka_unit_test(a_missing_layout_is_refused),
    cmocka_unit_test(a_layout_of_another_size_is_refused_before_its_fields_are_read),
  };
  return cmocka_run_group_tests_name("connection_client_layout", tests, NULL, NULL);
}
