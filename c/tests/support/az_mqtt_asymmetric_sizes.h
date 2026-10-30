// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_mqtt_asymmetric_sizes.h
 * @brief az_mqtt adapter sizes of az_mqtt_adapter_sizes_test.c: receive larger than send.
 * Included first; replaces any size from the build.
 */
#ifndef AZ_MQTT_ASYMMETRIC_SIZES_H
#define AZ_MQTT_ASYMMETRIC_SIZES_H

#undef AZ_IOT_AZ_MQTT_CONFIG_FILE
#undef AZ_IOT_AZ_MQTT_FOOTPRINT
#undef AZ_IOT_AZ_MQTT_BUFFER_SIZE
#undef AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE
#undef AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE
#undef AZ_IOT_AZ_MQTT_INFLIGHT_MAX
#undef AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX
#undef AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE
#define AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE 1024
#define AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE 8192

#endif // AZ_MQTT_ASYMMETRIC_SIZES_H
