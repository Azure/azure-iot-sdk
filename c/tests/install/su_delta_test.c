// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* azure::iot::su_delta: the handler links and validates its options. No file is touched and the
 * delta processor is not loaded. */
#include <stddef.h>
#include <string.h>

#include "az_iot_su_delta.h"

#include "install_test.h"

int main(void)
{
  az_iot_su_delta_handler handler;
  az_iot_su_delta_handler_options options = az_iot_su_delta_handler_options_default();
  CHECK(az_iot_su_delta_handler_init(&handler, NULL) == AZ_IOT_ERR_INVALID_ARG);
  CHECK(az_iot_su_delta_handler_init(&handler, &options) == AZ_IOT_ERR_INVALID_ARG);
  CHECK(
      strcmp(az_iot_su_delta_handler_get_download_handler(&handler).id, "microsoft/delta:1") == 0);
  return 0;
}
