// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter with AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE 0 (az_mqtt_no_store_config.h): a
 * connect whose session outlives the connection is refused; any other connect is accepted. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

#include <cmocka.h>

#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include "azure/iot/az_iot_mqtt_iface.h"

static az_iot_result connect_once(
    az_iot_mqtt_factory* (*create_factory)(void),
    bool clean_start,
    uint32_t session_expiry_seconds)
{
  az_iot_mqtt_factory* f = create_factory();
  assert_non_null(f);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "h";
  o.client_id = "c";
  o.clean_start = clean_start;
  o.session_expiry_seconds = session_expiry_seconds;
  az_iot_result rc = c->iface->connect(c, &o);
  (void)c->iface->disconnect(c);
  c->iface->destroy(c);
  az_iot_az_mqtt_factory_destroy(f);
  return rc;
}

static void v3_only_a_clean_session_is_accepted(void** state)
{
  (void)state;
  assert_int_equal(connect_once(az_iot_az_mqtt_factory_create_v3_1_1, true, 0), AZ_IOT_OK);
  assert_int_equal(
      connect_once(az_iot_az_mqtt_factory_create_v3_1_1, false, 0), AZ_IOT_ERR_NOT_SUPPORTED);
}

static void v5_only_a_session_with_expiry_and_no_clean_start_is_refused(void** state)
{
  (void)state;
  assert_int_equal(connect_once(az_iot_az_mqtt_factory_create_v5, true, 0), AZ_IOT_OK);
  assert_int_equal(connect_once(az_iot_az_mqtt_factory_create_v5, true, 60), AZ_IOT_OK);
  assert_int_equal(connect_once(az_iot_az_mqtt_factory_create_v5, false, 0), AZ_IOT_OK);
  assert_int_equal(
      connect_once(az_iot_az_mqtt_factory_create_v5, false, 60), AZ_IOT_ERR_NOT_SUPPORTED);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(v3_only_a_clean_session_is_accepted),
    cmocka_unit_test(v5_only_a_session_with_expiry_and_no_clean_start_is_refused),
  };
  return cmocka_run_group_tests_name("az_mqtt_adapter_no_store", tests, NULL, NULL);
}
