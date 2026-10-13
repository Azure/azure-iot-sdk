// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_mqtt_static_config.h
 * @brief az_mqtt adapter configuration of az_mqtt_adapter_static_test.c: static clients. Included
 * first; replaces any adapter configuration from the build. AZ_MQTT_STATIC_TINY_TRANSPORT: a
 * transport area smaller than any transport.
 */
#ifndef AZ_MQTT_STATIC_CONFIG_H
#define AZ_MQTT_STATIC_CONFIG_H

#undef AZ_IOT_AZ_MQTT_CONFIG_FILE
#undef AZ_IOT_AZ_MQTT_FOOTPRINT
#undef AZ_IOT_AZ_MQTT_BUFFER_SIZE
#undef AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE
#undef AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE
#undef AZ_IOT_AZ_MQTT_INFLIGHT_MAX
#undef AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX
#undef AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE
#undef AZ_IOT_AZ_MQTT_STATIC_CLIENTS
#undef AZ_IOT_AZ_MQTT_TRANSPORT_SIZE
#undef AZ_IOT_AZ_MQTT_CONNECT_STRINGS_SIZE
#define AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE 1024
#define AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE 1024
#define AZ_IOT_AZ_MQTT_STATIC_CLIENTS 2
#define AZ_IOT_AZ_MQTT_CONNECT_STRINGS_SIZE 64
#define AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE 0
#ifdef AZ_MQTT_STATIC_TINY_TRANSPORT
#define AZ_IOT_AZ_MQTT_TRANSPORT_SIZE 16
#endif

#endif // AZ_MQTT_STATIC_CONFIG_H
