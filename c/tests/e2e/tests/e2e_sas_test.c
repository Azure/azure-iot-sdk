// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* SAS end-to-end: a device in a DPS symmetric-key enrollment group registers
 * and connects to the assigned mqttv3 hub with SAS tokens the SDK signs from
 * the group key, then sends telemetry.
 *
 * Environment: AZ_IOT_DPS_ID_SCOPE,
 * AZ_IOT_DPS_SAS_GROUP_KEY, AZ_IOT_DPS_SAS_REGISTRATION_ID, AZ_IOT_TRUSTED_CA;
 * optional AZ_IOT_DPS_GLOBAL_ENDPOINT. */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include "e2e_log.h"
#include "e2e_sas_device.h"

static void test_dps_and_hub_with_a_group_sas_key(void** state)
{
  (void)state;
  e2e_sas_config cfg;
  e2e_sas_config_load(&cfg);

  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  e2e_sas_apply_dps(&cfg, &copts);
  /* The hub identity DPS creates has the keys derived from the group key. */
  copts.hub_auth = copts.dps_auth;

  e2e_sas_run run;
  e2e_sas_connect_and_send(&run, &copts, "e2e_sas");
  assert_int_equal(run.dps_source, AZ_IOT_AUTH_SOURCE_PRIMARY_KEY);
  assert_int_equal(run.hub_source, AZ_IOT_AUTH_SOURCE_PRIMARY_KEY);

  e2e_sas_config_free(&cfg);
}

int main(void)
{
  e2e_install_log_sink();
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_dps_and_hub_with_a_group_sas_key),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
