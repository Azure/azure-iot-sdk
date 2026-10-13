// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* Built with an AZ_IOT_SU_REQUEST_BUFFER_SIZE (set by CMake) that differs from
 * the SDK library's, as an application overriding it only in its own build
 * would. az_iot_su_client_init() must refuse the client without writing it. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <cmocka.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_su.h"

#ifndef AZ_IOT_TEST_SU_MISMATCHED_REQUEST_BUFFER_SIZE
#error "Build with AZ_IOT_TEST_SU_MISMATCHED_REQUEST_BUFFER_SIZE (see tests/CMakeLists.txt)."
#endif

static char last_error[256];

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
    snprintf(last_error, sizeof(last_error), "%s", msg);
  }
}

static void a_mismatched_request_buffer_size_is_refused(void** state)
{
  (void)state;
  assert_int_equal(AZ_IOT_SU_REQUEST_BUFFER_SIZE, AZ_IOT_TEST_SU_MISMATCHED_REQUEST_BUFFER_SIZE);

  az_iot_log_sink sink = { capture_error, NULL, AZ_IOT_LOG_LEVEL_ERROR };
  az_iot_log_set_global_sink(&sink);

  /* Not initialized: the check precedes any use of the connection or options. */
  static az_iot_connection_client conn;
  az_iot_su_client_config_options options = az_iot_su_client_config_options_default();

  static az_iot_su_client client;
  static uint8_t untouched[sizeof(az_iot_su_client)];
  memset(&client, 0x5A, sizeof(client));
  memset(untouched, 0x5A, sizeof(untouched));

  assert_int_equal(az_iot_su_client_init(&client, &conn, &options), AZ_IOT_ERR_INVALID_ARG);
  assert_memory_equal(&client, untouched, sizeof(client));

  char expected[96];
  snprintf(
      expected,
      sizeof(expected),
      "AZ_IOT_SU_REQUEST_BUFFER_SIZE is %lu in the application",
      (unsigned long)AZ_IOT_SU_REQUEST_BUFFER_SIZE);
  assert_non_null(strstr(last_error, expected));

  az_iot_log_set_global_sink(NULL);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_mismatched_request_buffer_size_is_refused),
  };
  return cmocka_run_group_tests_name("su_client_layout_mismatch", tests, NULL, NULL);
}
