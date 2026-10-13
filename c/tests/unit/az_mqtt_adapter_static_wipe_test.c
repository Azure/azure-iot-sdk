// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter with static clients (az_mqtt_static_config.h): credentials copied by connect()
 * are wiped from the static slot when released (the next connect, even refused) and when the
 * client is destroyed. Includes the MQTT 5 client to inspect its slots. */
#include "az_mqtt_static_config.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "az_iot_mqtt_az_mqtt_v5.c"

/** @brief Whether @p size bytes at @p data hold @p text. */
static bool holds(void const* data, size_t size, char const* text)
{
  uint8_t const* p = (uint8_t const*)data;
  size_t const n = strlen(text);
  for (size_t i = 0; i + n <= size; i++)
  {
    if (memcmp(p + i, text, n) == 0)
    {
      return true;
    }
  }
  return false;
}

static az_iot_result connect_with(az_iot_mqtt_client* c, char const* password, bool clean_start)
{
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "h";
  o.client_id = "c";
  o.username = "u";
  o.password = password;
  o.clean_start = clean_start;
  o.session_expiry_seconds = clean_start ? 0 : 60;
  return c->iface->connect(c, &o);
}

static void credentials_are_wiped_when_released_and_destroyed(void** state)
{
  (void)state;
  az_iot_mqtt_factory* f = az_iot_az_mqtt_factory_create_v5();
  az_iot_mqtt_client* c = f->create(f->factory_ctx);
  assert_non_null(c);
  _azm_slot const* slot = (_azm_slot const*)(void const*)c;
  size_t const used = offsetof(_azm_slot, in_use);

  assert_int_equal(connect_with(c, "FIRST-SECRET", true), AZ_IOT_OK);
  assert_true(holds(slot, used, "FIRST-SECRET"));
  (void)c->iface->disconnect(c);

  // The next connect releases the previous copies; a shorter password does not overwrite them.
  assert_int_equal(connect_with(c, "2nd", true), AZ_IOT_OK);
  assert_false(holds(slot, used, "-SECRET"));
  assert_true(holds(slot, used, "2nd"));
  (void)c->iface->disconnect(c);

  // A refused connect (no message store for a resumable session) releases them too.
  assert_int_equal(connect_with(c, "3rd", false), AZ_IOT_ERR_NOT_SUPPORTED);
  assert_false(holds(slot, used, "2nd"));

  // destroy() leaves nothing of the client in the slot.
  c->iface->destroy(c);
  uint8_t const* bytes = (uint8_t const*)slot;
  for (size_t i = 0; i < used; i++)
  {
    assert_int_equal(bytes[i], 0);
  }
  assert_false(slot->in_use);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(credentials_are_wiped_when_released_and_destroyed),
  };
  return cmocka_run_group_tests_name("az_mqtt_adapter_static_wipe", tests, NULL, NULL);
}
