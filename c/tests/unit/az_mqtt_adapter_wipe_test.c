// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter: credentials copied by connect() are wiped before their memory is freed, at the
 * next connect and at destroy(), and so is the send buffer (it holds the CONNECT). Includes the
 * MQTT 3.1.1 client, with az_mqtt_wipe_hooks.h, to reach its send buffer. */
#include "az_mqtt_wipe_hooks.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "az_iot_mqtt_az_mqtt_v3.c"

static az_iot_result connect_with(az_iot_mqtt_client* c, char const* password)
{
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "h";
  o.client_id = "c";
  o.username = "u";
  o.password = password;
  o.clean_start = true;
  return c->iface->connect(c, &o);
}

static void reset_hooks(char const* secret)
{
  az_mqtt_wipe_test_secret = secret;
  az_mqtt_wipe_test_frees = 0;
  az_mqtt_wipe_test_leaks = 0;
}

static void copies_are_wiped_at_next_connect(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_az_mqtt_factory_create_v3_1_1();
  assert_non_null(f);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);

  assert_int_equal(connect_with(c, "FIRST-SECRET"), AZ_IOT_OK);
  (void)c->iface->disconnect(c);

  reset_hooks("FIRST-SECRET");
  assert_int_equal(connect_with(c, "2nd"), AZ_IOT_OK);
  assert_true(az_mqtt_wipe_test_frees >= 4); // host, client id, username, password
  assert_int_equal(az_mqtt_wipe_test_leaks, 0);

  (void)c->iface->disconnect(c);
  c->iface->destroy(c);
  az_iot_az_mqtt_factory_destroy(f);
  reset_hooks(NULL);
}

static void copies_and_send_buffer_are_wiped_at_destroy(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_az_mqtt_factory_create_v3_1_1();
  assert_non_null(f);
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);

  assert_int_equal(connect_with(c, "LAST-SECRET"), AZ_IOT_OK);
  (void)c->iface->disconnect(c);
  // As the CONNECT encoded there would.
  memcpy(_azm_self(c)->send_buffer, "LAST-SECRET", sizeof("LAST-SECRET"));

  reset_hooks("LAST-SECRET");
  c->iface->destroy(c);
  assert_true(az_mqtt_wipe_test_frees >= 6); // 4 copies, the buffers' block, the client
  assert_int_equal(az_mqtt_wipe_test_leaks, 0);

  az_iot_az_mqtt_factory_destroy(f);
  reset_hooks(NULL);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(copies_are_wiped_at_next_connect),
    cmocka_unit_test(copies_and_send_buffer_are_wiped_at_destroy),
  };
  return cmocka_run_group_tests_name("az_mqtt_adapter_wipe", tests, NULL, NULL);
}
