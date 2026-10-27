// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter, MQTT 5 client, compiled in: a connect() refused after the busy check releases
 * the copies of the previous connect's strings (credentials). */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "az_iot_mqtt_az_mqtt_v5.c"

#include <cmocka.h>

static az_iot_mqtt_client* connected_once(az_iot_mqtt_factory** out_factory)
{
  az_iot_mqtt_factory* f = az_iot_az_mqtt_factory_create_v5();
  assert_non_null(f);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "h";
  o.client_id = "c";
  o.password = "secret";
  assert_int_equal(c->iface->connect(c, &o), AZ_IOT_OK);
  assert_true(_azm_self(c)->owned_count > 0);
  (void)c->iface->disconnect(c);
  *out_factory = f;
  return c;
}

static void done(az_iot_mqtt_factory* f, az_iot_mqtt_client* c)
{
  c->iface->destroy(c);
  az_iot_az_mqtt_factory_destroy(f);
}

static void a_connect_with_invalid_options_releases_previous_copies(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f;
  az_iot_mqtt_client* c = connected_once(&f);
  az_iot_mqtt_connect_options o = { 0 };
  o.client_id = "c"; // No host.
  assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(_azm_self(c)->owned_count, 0);
  done(f, c);
}

static void a_connect_with_unsupported_options_releases_previous_copies(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f;
  az_iot_mqtt_client* c = connected_once(&f);
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "h";
  o.client_id = "c";
  o.transport = (az_iot_mqtt_transport)99;
  assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_NOT_SUPPORTED);
  assert_int_equal(_azm_self(c)->owned_count, 0);
  done(f, c);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_connect_with_invalid_options_releases_previous_copies),
    cmocka_unit_test(a_connect_with_unsupported_options_releases_previous_copies),
  };
  return cmocka_run_group_tests_name("az_mqtt_adapter_release", tests, NULL, NULL);
}
