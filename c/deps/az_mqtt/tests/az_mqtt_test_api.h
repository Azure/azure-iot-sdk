// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_test_api.h
 * @brief The client API a version-dependent test runs against.
 *
 * Such tests are built with AZ_MQTT_TEST_VERSION=3 (az_mqttv3) or 5
 * (az_mqttv5) and name the API through AZ_MQTT_T(), e.g.
 * AZ_MQTT_T(client_connect) is az_mqtt3_client_connect or az_mqtt5_client_connect.
 */
#ifndef AZ_MQTT_TEST_API_H
#define AZ_MQTT_TEST_API_H

#if AZ_MQTT_TEST_VERSION == 5
#include <az_mqtt5/az_mqtt5_client.h>
#include <az_mqtt5/az_mqtt5_codec.h>
/** @brief The az_mqtt5_ API element @p name. */
#define AZ_MQTT_T(name) az_mqtt5_##name
/** @brief Disconnect normally. */
#define AZ_MQTT_TEST_DISCONNECT(client) \
  az_mqtt5_client_disconnect((client), AZ_MQTT5_REASON_NORMAL_DISCONNECTION)
/** @brief CONNACK reason / return code. */
#define AZ_MQTT_TEST_CONNACK_CODE(connack) ((int)(connack)->reason_code)
/** @brief CONNACK code for an accepted connection. */
#define AZ_MQTT_TEST_CONNACK_ACCEPTED ((int)AZ_MQTT5_REASON_SUCCESS)
/** @brief Reason code of a PUBACK/PUBCOMP; MQTT 3.1.1 acks carry none and mean success (0). */
#define AZ_MQTT_TEST_ACK_REASON(ack) ((int)(ack)->reason_code)
/** @brief connect_options clean start / clean session flag. */
#define AZ_MQTT_TEST_CLEAN clean_start
#elif AZ_MQTT_TEST_VERSION == 3
#include <az_mqtt3/az_mqtt3_client.h>
#include <az_mqtt3/az_mqtt3_codec.h>
#define AZ_MQTT_T(name) az_mqtt3_##name
#define AZ_MQTT_TEST_DISCONNECT(client) az_mqtt3_client_disconnect(client)
#define AZ_MQTT_TEST_CONNACK_CODE(connack) ((int)(connack)->return_code)
#define AZ_MQTT_TEST_CONNACK_ACCEPTED ((int)AZ_MQTT3_CONNACK_ACCEPTED)
#define AZ_MQTT_TEST_ACK_REASON(ack) ((void)(ack), 0)
#define AZ_MQTT_TEST_CLEAN clean_session
#else
#error "Define AZ_MQTT_TEST_VERSION as 3 or 5."
#endif

#endif // AZ_MQTT_TEST_API_H
