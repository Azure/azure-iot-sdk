// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_az_mqtt_config.h
 * @brief Compile-time limits of the az_mqtt adapter. Buffer sizes and requests in flight are
 * run-time values: az_iot_connection_client_options::mqtt_limits.
 *
 * A value defined by the build (-D) or by the file named by #AZ_IOT_AZ_MQTT_CONFIG_FILE overrides
 * the default below.
 */
#ifndef AZ_IOT_AZ_MQTT_CONFIG_H
#define AZ_IOT_AZ_MQTT_CONFIG_H

/**
 * @def AZ_IOT_AZ_MQTT_CONFIG_FILE
 * @brief Optional header, as written in a quoted #include, read first: it may define any
 * value below. Example: -DAZ_IOT_AZ_MQTT_CONFIG_FILE='"my_az_mqtt_config.h"'.
 */
#ifdef AZ_IOT_AZ_MQTT_CONFIG_FILE
#include AZ_IOT_AZ_MQTT_CONFIG_FILE
#endif

#ifdef AZ_IOT_AZ_MQTT_BUFFER_SIZE
#error \
    "AZ_IOT_AZ_MQTT_BUFFER_SIZE was removed: set send_buffer_size and receive_buffer_size in az_iot_connection_client_options::mqtt_limits"
#endif
#if defined(AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE) || defined(AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE) \
    || defined(AZ_IOT_AZ_MQTT_INFLIGHT_MAX) || defined(AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE)
#error \
    "AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE, AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE, AZ_IOT_AZ_MQTT_INFLIGHT_MAX and AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE were removed: use az_iot_connection_client_options::mqtt_limits"
#endif

#ifdef AZ_IOT_AZ_MQTT_FOOTPRINT
#error \
    "AZ_IOT_AZ_MQTT_FOOTPRINT was removed: define AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX instead (DEFAULT was 16, CONSTRAINED 4)"
#endif

/** @brief MQTT 5 user properties on CONNECT. A larger count is refused. PUBLISH user properties,
 * either direction, are not limited by it. */
#ifndef AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX
#define AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX 16
#endif

#if AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX < 1
#error "AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX must be at least 1"
#endif

#endif // AZ_IOT_AZ_MQTT_CONFIG_H
