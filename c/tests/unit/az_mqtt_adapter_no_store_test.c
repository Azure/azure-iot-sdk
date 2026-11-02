// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter with no message store (az_iot_mqtt_limits.message_store_size 0): a connect whose
 * session outlives the connection is refused, and so is such a session usage at init; any other
 * connect is accepted. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <cmocka.h>

#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include "azure/iot/az_iot_mqtt_iface.h"

static az_iot_result connect_once(az_iot_mqtt_version version, bool clean_start)
{
  az_iot_mqtt_limits const limits = {
    .send_buffer_size = 1024,
    .receive_buffer_size = 1024,
    .max_in_flight_messages = 1,
    .message_store_size = 0,
    .connect_strings_size = 256,
  };
  size_t const size = az_iot_az_mqtt_get_workspace_size(&limits);
  uint8_t* workspace = (uint8_t*)malloc(size);
  assert_non_null(workspace);
  az_iot_mqtt_client* c = NULL;
  assert_int_equal(
      az_iot_az_mqtt_get_adapter()->init(version, &limits, workspace, size, &c), AZ_IOT_OK);
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "h";
  o.client_id = "c";
  o.clean_start = clean_start;
  az_iot_result rc = c->iface->connect(c, &o);
  (void)c->iface->disconnect(c);
  c->iface->destroy(c);
  free(workspace);
  return rc;
}

static void v3_only_a_clean_session_is_accepted(void** state)
{
  (void)state;
  assert_int_equal(connect_once(AZ_IOT_MQTT_VERSION_3_1_1, true), AZ_IOT_OK);
  assert_int_equal(connect_once(AZ_IOT_MQTT_VERSION_3_1_1, false), AZ_IOT_ERR_NOT_SUPPORTED);
}

/* The init-time check refuses exactly the sessions connect() refuses: those that outlive their
 * connection, and only without a store. */
static void the_session_usage_check_matches_connect(void** state)
{
  (void)state;
  const az_iot_mqtt_adapter* adapter = az_iot_az_mqtt_get_adapter();
  assert_non_null(adapter->validate_limits_for_session_usage);
  az_iot_mqtt_limits limits = {
    .send_buffer_size = 1024,
    .receive_buffer_size = 1024,
    .max_in_flight_messages = 1,
    .message_store_size = 0,
    .connect_strings_size = 256,
  };
  az_iot_mqtt_session_usage const clean = { .session_outlives_connection = false };
  az_iot_mqtt_session_usage const outliving = { .session_outlives_connection = true };
  assert_int_equal(adapter->validate_limits_for_session_usage(&limits, &clean), AZ_IOT_OK);
  assert_int_equal(
      adapter->validate_limits_for_session_usage(&limits, &outliving), AZ_IOT_ERR_INVALID_ARG);
  limits.message_store_size = 1024 + 18;
  assert_int_equal(adapter->validate_limits_for_session_usage(&limits, &outliving), AZ_IOT_OK);
  assert_int_equal(
      adapter->validate_limits_for_session_usage(NULL, &clean), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      adapter->validate_limits_for_session_usage(&limits, NULL), AZ_IOT_ERR_INVALID_ARG);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(v3_only_a_clean_session_is_accepted),
    cmocka_unit_test(the_session_usage_check_matches_connect),
  };
  return cmocka_run_group_tests_name("az_mqtt_adapter_no_store", tests, NULL, NULL);
}
