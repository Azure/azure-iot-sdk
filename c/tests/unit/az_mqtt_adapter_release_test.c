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
#include <stdlib.h>
#include <string.h>

#include "az_iot_mqtt_az_mqtt_v5.c"

#include <cmocka.h>

/** @brief A client built by the adapter in a heap workspace, returned in @p out_workspace, after a
 * connect() that copied its strings. */
static az_iot_mqtt_client* connected_once(uint8_t** out_workspace)
{
  az_iot_mqtt_limits const limits = {
    .send_buffer_size = 1024,
    .receive_buffer_size = 1024,
    .max_in_flight_messages = 1,
    .message_store_size = 0,
    .connect_strings_size = 256,
  };
  size_t const size = az_iot_az_mqtt_get_workspace_size(&limits);
  *out_workspace = (uint8_t*)malloc(size);
  assert_non_null(*out_workspace);
  az_iot_mqtt_client* c = NULL;
  assert_int_equal(
      az_iot_az_mqtt_get_adapter()->init(AZ_IOT_MQTT_VERSION_5, &limits, *out_workspace, size, &c),
      AZ_IOT_OK);
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "h";
  o.client_id = "c";
  o.password = "secret";
  assert_int_equal(c->iface->connect(c, &o), AZ_IOT_OK);
  assert_true(_azm_self(c)->connect_strings_used > 0);
  (void)c->iface->disconnect(c);
  return c;
}

/** @brief Destroys @p c, then frees its @p workspace. */
static void done(uint8_t* workspace, az_iot_mqtt_client* c)
{
  c->iface->destroy(c);
  free(workspace);
}

static void a_connect_with_invalid_options_releases_previous_copies(void** state)
{
  (void)state;
  uint8_t* workspace;
  az_iot_mqtt_client* c = connected_once(&workspace);
  az_iot_mqtt_connect_options o = { 0 };
  o.client_id = "c"; // No host.
  assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(_azm_self(c)->connect_strings_used, 0);
  done(workspace, c);
}

static void a_connect_with_unsupported_options_releases_previous_copies(void** state)
{
  (void)state;
  uint8_t* workspace;
  az_iot_mqtt_client* c = connected_once(&workspace);
  az_iot_mqtt_connect_options o = { 0 };
  o.host = "h";
  o.client_id = "c";
  o.transport = (az_iot_mqtt_transport)99;
  assert_int_equal(c->iface->connect(c, &o), AZ_IOT_ERR_NOT_SUPPORTED);
  assert_int_equal(_azm_self(c)->connect_strings_used, 0);
  done(workspace, c);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_connect_with_invalid_options_releases_previous_copies),
    cmocka_unit_test(a_connect_with_unsupported_options_releases_previous_copies),
  };
  return cmocka_run_group_tests_name("az_mqtt_adapter_release", tests, NULL, NULL);
}
