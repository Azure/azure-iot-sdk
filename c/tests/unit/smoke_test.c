// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_connection_client.h"
#include "internal/connection_client_internal.h"

static void mqtt_role_to_required_version(void** state)
{
  (void)state;
  assert_int_equal(
      az_iot_mqtt_required_version_for_role(AZ_IOT_MQTT_ROLE_DPS), AZ_IOT_MQTT_VERSION_3_1_1);
  assert_int_equal(
      az_iot_mqtt_required_version_for_role(AZ_IOT_MQTT_ROLE_HUB_MQTT_V3),
      AZ_IOT_MQTT_VERSION_3_1_1);
  assert_int_equal(
      az_iot_mqtt_required_version_for_role(AZ_IOT_MQTT_ROLE_HUB_MQTT_V5), AZ_IOT_MQTT_VERSION_5);
}

static void result_to_string_known_codes(void** state)
{
  (void)state;
  assert_string_equal(az_iot_result_to_string(AZ_IOT_OK), "AZ_IOT_OK");
  assert_string_equal(az_iot_result_to_string(AZ_IOT_ERR_INVALID_ARG), "AZ_IOT_ERR_INVALID_ARG");
  assert_string_equal(
      az_iot_result_to_string(AZ_IOT_ERR_NOT_SUPPORTED), "AZ_IOT_ERR_NOT_SUPPORTED");
}

static void connection_client_init_rejects_null(void** state)
{
  (void)state;
  az_iot_connection_client c;
  az_iot_connection_client_options opts = { 0 };
  assert_int_equal(az_iot_connection_client_init(NULL, &opts), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_connection_client_init(&c, NULL), AZ_IOT_ERR_INVALID_ARG);
}

static void connection_client_init_deinit_roundtrip(void** state)
{
  (void)state;
  az_iot_connection_client_options opts = { 0 };
  opts.client_id = "ut-device";
  az_iot_connection_client c;
  assert_int_equal(az_iot_connection_client_init(&c, &opts), AZ_IOT_OK);
  az_iot_connection_client_deinit(&c);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(mqtt_role_to_required_version),
    cmocka_unit_test(result_to_string_known_codes),
    cmocka_unit_test(connection_client_init_rejects_null),
    cmocka_unit_test(connection_client_init_deinit_roundtrip),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
