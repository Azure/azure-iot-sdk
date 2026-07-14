// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#include "internal/protocol_profile.h"

#include <stddef.h>

/* Classic (MQTT v3.1.1) profile: topic prefixes per the Azure IoT Hub MQTT
 * contract. These constants are deliberately local rather than re-exported
 * from azure-sdk-for-c so this profile module stays a leaf with no upstream
 * coupling. */
static const az_iot_protocol_profile s_profile_classic = {
    AZ_IOT_HUB_FLAVOR_CLASSIC,
    AZ_IOT_MQTT_VERSION_3_1_1,
    /* Classic topic prefixes */
    "$iothub/twin/res/",
    "$iothub/twin/PATCH/properties/desired/",
    "$iothub/methods/POST/",
    "devices/{device_id}/messages/events/",
    /* Next-specific fields (unused for Classic) */
    NULL,   /* next_topic_base_template */
    false,  /* uses_mqtt5_properties */
    /* Common */
    30000u
};

/* Next (MQTT v5) profile: flat topics with metadata in User Properties.
 * Classic topic fields are NULL — feature clients must use the Next-specific
 * path (next_topic_base_template + User Properties). */
static const az_iot_protocol_profile s_profile_next = {
    AZ_IOT_HUB_FLAVOR_NEXT,
    AZ_IOT_MQTT_VERSION_5,
    /* Classic topic prefixes (unused for Next) */
    NULL,  /* twin_response_topic_prefix */
    NULL,  /* twin_desired_topic_prefix */
    NULL,  /* methods_request_topic_prefix */
    NULL,  /* d2c_publish_topic_template */
    /* Next-specific fields */
    "ih/{device_id}",  /* next_topic_base_template */
    true,              /* uses_mqtt5_properties */
    /* Common */
    30000u
};

const az_iot_protocol_profile* az_iot_protocol_profile_for_role(
    az_iot_mqtt_role role)
{
    switch (role)
    {
        case AZ_IOT_MQTT_ROLE_DPS:
            /* DPS sessions don't expose hub-feature topics; we still hand back
             * the classic profile so its mqtt_version/timeout defaults can be
             * consulted, but feature clients should not subscribe to the
             * hub-twin/methods prefixes during a DPS session. */
            return &s_profile_classic;
        case AZ_IOT_MQTT_ROLE_HUB_CLASSIC:
            return &s_profile_classic;
        case AZ_IOT_MQTT_ROLE_HUB_NEXT:
            return &s_profile_next;
        default:
            return NULL;
    }
}

const char* az_iot_hub_flavor_to_string(az_iot_hub_flavor f)
{
    switch (f)
    {
        case AZ_IOT_HUB_FLAVOR_CLASSIC: return "AZ_IOT_HUB_FLAVOR_CLASSIC";
        case AZ_IOT_HUB_FLAVOR_NEXT:    return "AZ_IOT_HUB_FLAVOR_NEXT";
        default:                            return "AZ_IOT_HUB_FLAVOR_UNKNOWN";
    }
}
