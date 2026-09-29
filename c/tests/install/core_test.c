// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* azure::iot::core: public headers (including the bundled azure-sdk-for-c
 * ones) resolve, and the library and its transitive libraries link and run. */
#include <stddef.h>
#include <string.h>

#include <azure/core/az_span.h>

#include "azure/iot/az_iot.h"
#include "azure/iot/az_iot_su.h"
#include "azure/iot/az_iot_version.h"

#include "install_test.h"

static az_iot_connection_client g_conn;

int main(void)
{
  CHECK(strcmp(az_iot_version_string(), AZ_IOT_VERSION_STRING) == 0);
  CHECK(az_iot_result_to_string(AZ_IOT_OK) != NULL);
  CHECK(az_span_size(az_span_create_from_str("abc")) == 3);

  az_iot_connection_client_options options = az_iot_connection_client_options_default();
  options.host = "hub.example";
  options.client_id = "install-test";
  CHECK(az_iot_connection_client_init(&g_conn, &options) == AZ_IOT_OK);
  CHECK(
      az_iot_connection_client_get_state(&g_conn, AZ_IOT_CONN_SCOPE_HUB) == AZ_IOT_CONN_STATE_IDLE);
  az_iot_connection_client_deinit(&g_conn);

  size_t count = 0;
  CHECK(az_iot_su_microsoft_root_keys(&count) != NULL);
  CHECK(count > 0);
  return 0;
}
