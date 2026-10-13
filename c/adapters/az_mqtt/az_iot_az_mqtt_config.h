// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_az_mqtt_config.h
 * @brief Per-client sizes of the az_mqtt adapter, fixed when the adapter is compiled.
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
    "AZ_IOT_AZ_MQTT_BUFFER_SIZE was replaced by AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE and AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE"
#endif

/* Nonzero: an undefined name in #if is 0, so a misspelled footprint fails below. */
/** @brief Footprint for the IoT Hub limits: 256 KB device-to-cloud messages. */
#define AZ_IOT_AZ_MQTT_FOOTPRINT_DEFAULT 1
/** @brief Smaller footprint: outgoing packets up to 8 KiB, incoming up to 132 KiB (direct
 * method payloads of 128 KB), 8 requests in flight, 4 user properties. */
#define AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED 2

/** @brief Selected footprint: #AZ_IOT_AZ_MQTT_FOOTPRINT_DEFAULT or
 * #AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED. */
#ifndef AZ_IOT_AZ_MQTT_FOOTPRINT
#define AZ_IOT_AZ_MQTT_FOOTPRINT AZ_IOT_AZ_MQTT_FOOTPRINT_DEFAULT
#endif

#if AZ_IOT_AZ_MQTT_FOOTPRINT == AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED
#ifndef AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE
#define AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE (8 * 1024)
#endif
#ifndef AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE
#define AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE (132 * 1024)
#endif
#ifndef AZ_IOT_AZ_MQTT_INFLIGHT_MAX
#define AZ_IOT_AZ_MQTT_INFLIGHT_MAX 8
#endif
#ifndef AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX
#define AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX 4
#endif
#elif AZ_IOT_AZ_MQTT_FOOTPRINT != AZ_IOT_AZ_MQTT_FOOTPRINT_DEFAULT
#error \
    "AZ_IOT_AZ_MQTT_FOOTPRINT must be AZ_IOT_AZ_MQTT_FOOTPRINT_DEFAULT or AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED"
#endif

/** @brief Send buffer size: the largest outgoing packet. A larger publish or connect fails. */
#ifndef AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE
#define AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE (264 * 1024)
#endif

/** @brief Receive buffer size: the largest incoming packet (MQTT 5: the Maximum Packet Size
 * advertised; the server drops larger ones. MQTT 3.1.1: a larger one ends the session). */
#ifndef AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE
#define AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE (264 * 1024)
#endif

/** @brief QoS 1/2 exchanges in flight at once, both directions together (MQTT 5: also the
 * Receive Maximum advertised). */
#ifndef AZ_IOT_AZ_MQTT_INFLIGHT_MAX
#define AZ_IOT_AZ_MQTT_INFLIGHT_MAX 64
#endif

/** @brief MQTT 5 user properties per packet, each direction. A larger outgoing count is refused;
 * received ones beyond it are dropped. */
#ifndef AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX
#define AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX 16
#endif

#if AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE < 1 || AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE > 268435455
#error "AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE must be 1 to 268435455"
#endif
#if AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE < 1 || AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE > 268435455
#error "AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE must be 1 to 268435455 (MQTT 5 Maximum Packet Size)"
#endif
// MQTT 5 uses twice as many entries, and az_mqtt uses at most 65,535.
#if AZ_IOT_AZ_MQTT_INFLIGHT_MAX < 1 || AZ_IOT_AZ_MQTT_INFLIGHT_MAX > 32767
#error "AZ_IOT_AZ_MQTT_INFLIGHT_MAX must be 1 to 32767"
#endif
#if AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX < 1
#error "AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX must be at least 1"
#endif

#endif // AZ_IOT_AZ_MQTT_CONFIG_H
