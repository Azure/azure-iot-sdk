// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* az_mqtt adapter: the descriptor that builds MQTT 3.1.1 and MQTT 5 clients in application memory.
 */

#include "az_iot_mqtt_az_mqtt_internal.h"

#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_log_components.h"

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

/**
 * @brief az_iot_mqtt_adapter::validate_limits_for_session_usage: a session that outlives its
 * connection needs a message store, as connect() checks.
 */
static az_iot_result az_mqtt_adapter_validate_limits_for_session_usage(
    const az_iot_mqtt_limits* limits,
    const az_iot_mqtt_session_usage* usage)
{
  if (limits == NULL || usage == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (usage->session_outlives_connection && limits->message_store_size == 0)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_AZ_MQTT,
        "message_store_size 0 allows only clean sessions: set it, or set session_continuity "
        "to AZ_IOT_SESSION_CONTINUITY_CLEAN");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return AZ_IOT_OK;
}

static const az_iot_mqtt_adapter az_mqtt_adapter = {
  .get_workspace_size = az_iot_az_mqtt_get_workspace_size,
  .init = az_mqtt_adapter_init,
  .validate_limits_for_session_usage = az_mqtt_adapter_validate_limits_for_session_usage,
};

const az_iot_mqtt_adapter* az_iot_az_mqtt_get_adapter(void) { return &az_mqtt_adapter; }
