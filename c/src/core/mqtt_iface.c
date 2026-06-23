// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#include "azure/iot/az_iot_mqtt_iface.h"

const char* az_iot_mqtt_version_to_string(az_iot_mqtt_version_t v)
{
    switch (v)
    {
        case AZ_IOT_MQTT_VERSION_3_1_1: return "MQTTv3.1.1";
        case AZ_IOT_MQTT_VERSION_5:     return "MQTTv5";
        default:                     return "MQTT?";
    }
}
