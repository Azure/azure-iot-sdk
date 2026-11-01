// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_az_mqtt_config.h
 * @brief Compile-time limits of the az_mqtt adapter. Buffer sizes and requests in flight are
 * run-time values: az_iot_connection_client_options::mqtt_limits.
 *
 * Each value, in order of precedence:
 * 1. A definition from the build (-D) or from the file named by #AZ_IOT_AZ_MQTT_CONFIG_FILE.
 * 2. The value of the selected #AZ_IOT_AZ_MQTT_FOOTPRINT.
 * 3. The default footprint's value.
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

/* Nonzero: an undefined name in #if is 0, so a misspelled footprint fails below. */
/** @brief Footprint for the IoT Hub limits: 16 MQTT 5 user properties on CONNECT. */
#define AZ_IOT_AZ_MQTT_FOOTPRINT_DEFAULT 1
/** @brief Smaller footprint: 4 MQTT 5 user properties on CONNECT. */
#define AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED 2

/** @brief Selected footprint: #AZ_IOT_AZ_MQTT_FOOTPRINT_DEFAULT or
 * #AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED. */
#ifndef AZ_IOT_AZ_MQTT_FOOTPRINT
#define AZ_IOT_AZ_MQTT_FOOTPRINT AZ_IOT_AZ_MQTT_FOOTPRINT_DEFAULT
#endif

#if AZ_IOT_AZ_MQTT_FOOTPRINT == AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED
#ifndef AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX
#define AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX 4
#endif
#elif AZ_IOT_AZ_MQTT_FOOTPRINT != AZ_IOT_AZ_MQTT_FOOTPRINT_DEFAULT
#error \
    "AZ_IOT_AZ_MQTT_FOOTPRINT must be AZ_IOT_AZ_MQTT_FOOTPRINT_DEFAULT or AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED"
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
