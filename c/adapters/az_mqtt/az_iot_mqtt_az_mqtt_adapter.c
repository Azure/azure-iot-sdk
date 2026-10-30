// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter: the descriptor that builds MQTT 3.1.1 and MQTT 5 clients in application memory.
 */

#include "az_iot_mqtt_az_mqtt_internal.h"

#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"

size_t az_iot_az_mqtt_get_workspace_size(const az_iot_mqtt_limits* limits)
{
  size_t const size_3_1_1 = az_iot_az_mqtt__get_workspace_size_v3_1_1(limits);
  size_t const size_5 = az_iot_az_mqtt__get_workspace_size_v5(limits);
  if (size_3_1_1 == 0 || size_5 == 0)
  {
    return 0;
  }
  return size_3_1_1 > size_5 ? size_3_1_1 : size_5;
}

/** @brief az_iot_mqtt_adapter::init for both MQTT versions. */
static az_iot_result az_mqtt_adapter_init(
    az_iot_mqtt_version mqtt_version,
    const az_iot_mqtt_limits* limits,
    uint8_t* workspace,
    size_t workspace_size,
    az_iot_mqtt_client** out_client)
{
  switch (mqtt_version)
  {
    case AZ_IOT_MQTT_VERSION_3_1_1:
      return az_iot_az_mqtt__init_v3_1_1(limits, workspace, workspace_size, out_client);
    case AZ_IOT_MQTT_VERSION_5:
      return az_iot_az_mqtt__init_v5(limits, workspace, workspace_size, out_client);
    default:
      return AZ_IOT_ERR_NOT_SUPPORTED;
  }
}

static const az_iot_mqtt_adapter az_mqtt_adapter = {
  .get_workspace_size = az_iot_az_mqtt_get_workspace_size,
  .init = az_mqtt_adapter_init,
};

const az_iot_mqtt_adapter* az_iot_az_mqtt_get_adapter(void) { return &az_mqtt_adapter; }
