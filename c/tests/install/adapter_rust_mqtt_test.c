// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* azure::iot::adapter_rust_mqtt: the adapter and its FFI contract header are
 * installed; with no FFI table installed the factory is absent. */
#include <stddef.h>

#include "az_iot_mqtt_rust_ffi.h"
#include "azure/iot/adapters/az_iot_adapter_rust_mqtt.h"

#include "install_test.h"

int main(void)
{
  CHECK(sizeof(az_iot_rust_mqtt_ffi) > 0);
  CHECK(az_iot_rust_mqtt_install(NULL) == AZ_IOT_OK);
  CHECK(az_iot_rust_mqtt_factory_create_v5() == NULL);
  return 0;
}
