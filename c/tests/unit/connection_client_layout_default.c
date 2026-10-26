// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* Built with the SDK's capacity macros: sizes and layout as the SDK library sees them. */

#include <stddef.h>

#include "azure/iot/az_iot_connection_client.h"

size_t az_iot_test_connection_client_default_size(void);

size_t az_iot_test_connection_client_default_size(void) { return sizeof(az_iot_connection_client); }

void az_iot_test_connection_client_default_layout(az_iot_connection_client_layout* out);

void az_iot_test_connection_client_default_layout(az_iot_connection_client_layout* out)
{
  az_iot_connection_client_layout const layout = {
    .layout_size = sizeof(az_iot_connection_client_layout),
    .client_size = sizeof(az_iot_connection_client),
    .options_size = sizeof(az_iot_connection_client_options),
    .max_mqtt_factories = AZ_IOT_MAX_MQTT_FACTORIES,
    .max_pending_pubacks = AZ_IOT_MAX_PENDING_PUBACKS,
    .max_puback_reservations = AZ_IOT_MAX_PUBACK_RESERVATIONS,
    .max_persistent_subs = AZ_IOT_MAX_PERSISTENT_SUBS,
    .max_session_handlers = AZ_IOT_MAX_SESSION_HANDLERS,
    .max_feature_state_observers = AZ_IOT_MAX_FEATURE_STATE_OBSERVERS,
    .max_app_state_observers = AZ_IOT_MAX_APP_STATE_OBSERVERS,
    .persistent_sub_topic_max = AZ_IOT_PERSISTENT_SUB_TOPIC_MAX,
    .dps_operation_id_max = AZ_IOT_DPS_OPERATION_ID_MAX,
    .dps_host_buffer_size = AZ_IOT_DPS_HOST_BUF,
    .dps_device_id_buffer_size = AZ_IOT_DPS_DEVICE_ID_BUF,
    .connection_profile_raw_buffer_size = AZ_IOT_CONNECTION_PROFILE_RAW_BUF,
    .max_feature_client_binds = AZ_IOT_MAX_FEATURE_CLIENT_BINDS,
    .mqtt_username_buffer_size = AZ_IOT_MQTT_USERNAME_BUF,
    .max_inbound_handlers = AZ_IOT_MAX_INBOUND_HANDLERS,
    .dispatch_prefix_max = AZ_IOT_DISPATCH_PREFIX_MAX,
  };
  *out = layout;
}
