// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* azure::iot::certificate_provider_managed: the provider links and validates
 * its options. No key or certificate files are touched. */
#include <stddef.h>

#include "az_iot_certificate_provider_managed.h"

#include "install_test.h"

int main(void)
{
  az_iot_certificate_provider_managed provider;
  az_iot_certificate_provider_managed_options options = { 0 };
  CHECK(az_iot_certificate_provider_managed_init(&provider, NULL) == AZ_IOT_ERR_INVALID_ARG);
  CHECK(az_iot_certificate_provider_managed_init(&provider, &options) == AZ_IOT_ERR_INVALID_ARG);
  return 0;
}
