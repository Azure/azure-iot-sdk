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
void az_iot_test_connection_client_default_layout(az_iot_connection_client_layout* out);

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

/* Capacities that match but a struct size that differs: the size checks alone refuse it (layout
 * changes no listed macro explains). */
static void refuse_struct_size(size_t client_delta, size_t options_delta, const char* expected_name)
{
  start_capture();
  static az_iot_connection_client client;
  static uint8_t untouched[sizeof(az_iot_connection_client)];
  memset(&client, 0x5A, sizeof(client));
  memset(untouched, 0x5A, sizeof(untouched));
  az_iot_connection_client_options options = az_iot_connection_client_options_default();
  az_iot_connection_client_layout layout;
  az_iot_test_connection_client_default_layout(&layout);
  layout.client_size += client_delta;
  layout.options_size += options_delta;

  assert_int_equal(
      az_iot_connection_client_init_checked(&client, &options, &layout), AZ_IOT_ERR_INVALID_ARG);
  assert_memory_equal(&client, untouched, sizeof(client));
  assert_non_null(strstr(logged_errors, expected_name));
  assert_null(strstr(logged_errors, "AZ_IOT_")); /* every capacity matched */

  az_iot_log_set_global_sink(NULL);
}

static void a_client_of_another_size_is_refused(void** state)
{
  (void)state;
  refuse_struct_size(8, 0, "sizeof(az_iot_connection_client) is ");
  refuse_struct_size((size_t)-8, 0, "sizeof(az_iot_connection_client) is ");
}

static void options_of_another_size_are_refused(void** state)
{
  (void)state;
  refuse_struct_size(0, 8, "sizeof(az_iot_connection_client_options) is ");
  refuse_struct_size(0, (size_t)-8, "sizeof(az_iot_connection_client_options) is ");
}

/* Each capacity, one at a time, above and below the SDK value, sizes unchanged: refused by that
 * capacity's own check, named in the only diagnostic, client untouched. */
static void each_capacity_alone_is_refused(void** state)
{
  (void)state;
  static const struct
  {
    size_t offset;
    const char* name;
  } capacities[] = {
    { offsetof(az_iot_connection_client_layout, max_mqtt_factories), "AZ_IOT_MAX_MQTT_FACTORIES" },
    { offsetof(az_iot_connection_client_layout, max_pending_pubacks),
      "AZ_IOT_MAX_PENDING_PUBACKS" },
    { offsetof(az_iot_connection_client_layout, max_puback_reservations),
      "AZ_IOT_MAX_PUBACK_RESERVATIONS" },
    { offsetof(az_iot_connection_client_layout, max_persistent_subs),
      "AZ_IOT_MAX_PERSISTENT_SUBS" },
    { offsetof(az_iot_connection_client_layout, max_session_handlers),
      "AZ_IOT_MAX_SESSION_HANDLERS" },
    { offsetof(az_iot_connection_client_layout, max_feature_state_observers),
      "AZ_IOT_MAX_FEATURE_STATE_OBSERVERS" },
    { offsetof(az_iot_connection_client_layout, max_app_state_observers),
      "AZ_IOT_MAX_APP_STATE_OBSERVERS" },
    { offsetof(az_iot_connection_client_layout, persistent_sub_topic_max),
      "AZ_IOT_PERSISTENT_SUB_TOPIC_MAX" },
    { offsetof(az_iot_connection_client_layout, dps_operation_id_max),
      "AZ_IOT_DPS_OPERATION_ID_MAX" },
    { offsetof(az_iot_connection_client_layout, dps_host_buffer_size), "AZ_IOT_DPS_HOST_BUF" },
    { offsetof(az_iot_connection_client_layout, dps_device_id_buffer_size),
      "AZ_IOT_DPS_DEVICE_ID_BUF" },
    { offsetof(az_iot_connection_client_layout, connection_profile_raw_buffer_size),
      "AZ_IOT_CONNECTION_PROFILE_RAW_BUF" },
    { offsetof(az_iot_connection_client_layout, max_feature_client_binds),
      "AZ_IOT_MAX_FEATURE_CLIENT_BINDS" },
    { offsetof(az_iot_connection_client_layout, mqtt_username_buffer_size),
      "AZ_IOT_MQTT_USERNAME_BUF" },
    { offsetof(az_iot_connection_client_layout, max_inbound_handlers),
      "AZ_IOT_MAX_INBOUND_HANDLERS" },
    { offsetof(az_iot_connection_client_layout, dispatch_prefix_max),
      "AZ_IOT_DISPATCH_PREFIX_MAX" },
  };
  static const size_t deltas[] = { 1, (size_t)-1 };
  static az_iot_connection_client client;
  static uint8_t untouched[sizeof(az_iot_connection_client)];
  memset(untouched, 0x5A, sizeof(untouched));
  for (size_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); i++)
  {
    for (size_t d = 0; d < sizeof(deltas) / sizeof(deltas[0]); d++)
    {
      start_capture();
      memset(&client, 0x5A, sizeof(client));
      az_iot_connection_client_options options = az_iot_connection_client_options_default();
      az_iot_connection_client_layout layout;
      az_iot_test_connection_client_default_layout(&layout);
      size_t* field = (size_t*)(void*)((uint8_t*)&layout + capacities[i].offset);
      *field += deltas[d];

      assert_int_equal(
          az_iot_connection_client_init_checked(&client, &options, &layout),
          AZ_IOT_ERR_INVALID_ARG);
      assert_memory_equal(&client, untouched, sizeof(client));
      char expected[64];
      (void)snprintf(expected, sizeof(expected), "init: %s is ", capacities[i].name);
      const char* found = strstr(logged_errors, expected);
      assert_non_null(found);
      assert_ptr_equal(found, logged_errors); /* the only diagnostic */
      assert_int_equal(strchr(logged_errors, '\n')[1], '\0');

      az_iot_log_set_global_sink(NULL);
    }
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_mismatched_layout_is_refused_and_the_client_untouched),
    cmocka_unit_test(a_missing_layout_is_refused),
    cmocka_unit_test(a_layout_of_another_size_is_refused_before_its_fields_are_read),
    cmocka_unit_test(a_client_of_another_size_is_refused),
    cmocka_unit_test(options_of_another_size_are_refused),
    cmocka_unit_test(each_capacity_alone_is_refused),
  };
  return cmocka_run_group_tests_name("connection_client_layout", tests, NULL, NULL);
}
