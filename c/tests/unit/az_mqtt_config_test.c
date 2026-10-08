// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_mqtt_config_test.c
 * @brief Checks, when compiled, how az_iot_az_mqtt_config.h resolves the adapter sizes. One
 * case per build: AZ_IOT_AZ_MQTT_CONFIG_TEST_<case>. Definitions made in code, before the
 * header, as an application compiling the adapter would.
 */

// Each case sets its own inputs: drop any from the build (e.g. CMAKE_C_FLAGS).
#undef AZ_IOT_AZ_MQTT_CONFIG_FILE
#undef AZ_IOT_AZ_MQTT_FOOTPRINT
#undef AZ_IOT_AZ_MQTT_BUFFER_SIZE
#undef AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE
#undef AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE
#undef AZ_IOT_AZ_MQTT_INFLIGHT_MAX
#undef AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX

#if defined(AZ_IOT_AZ_MQTT_CONFIG_TEST_DEFAULT)
#define EXPECTED_SEND (264 * 1024)
#define EXPECTED_RECEIVE (264 * 1024)
#define EXPECTED_INFLIGHT 64
#define EXPECTED_PROPERTIES 16
#elif defined(AZ_IOT_AZ_MQTT_CONFIG_TEST_CONSTRAINED)
#define AZ_IOT_AZ_MQTT_FOOTPRINT AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED
#define EXPECTED_SEND (8 * 1024)
#define EXPECTED_RECEIVE (132 * 1024)
#define EXPECTED_INFLIGHT 8
#define EXPECTED_PROPERTIES 4
#elif defined(AZ_IOT_AZ_MQTT_CONFIG_TEST_OVERRIDE)
#define AZ_IOT_AZ_MQTT_FOOTPRINT AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED
#define AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE 36864
#define EXPECTED_SEND (8 * 1024)
#define EXPECTED_RECEIVE 36864
#define EXPECTED_INFLIGHT 8
#define EXPECTED_PROPERTIES 4
#elif defined(AZ_IOT_AZ_MQTT_CONFIG_TEST_CONFIG_FILE)
// The file selects CONSTRAINED and sets INFLIGHT_MAX; a definition here still wins.
#define AZ_IOT_AZ_MQTT_CONFIG_FILE "az_mqtt_config_test_user.h"
#define AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX 2
#define EXPECTED_SEND (8 * 1024)
#define EXPECTED_RECEIVE (132 * 1024)
#define EXPECTED_INFLIGHT 2
#define EXPECTED_PROPERTIES 2
#else
#error "Define AZ_IOT_AZ_MQTT_CONFIG_TEST_<case>"
#endif

#include "az_iot_az_mqtt_config.h"

#if AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE != EXPECTED_SEND
#error "AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE"
#endif
#if AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE != EXPECTED_RECEIVE
#error "AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE"
#endif
#if AZ_IOT_AZ_MQTT_INFLIGHT_MAX != EXPECTED_INFLIGHT
#error "AZ_IOT_AZ_MQTT_INFLIGHT_MAX"
#endif
#if AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX != EXPECTED_PROPERTIES
#error "AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX"
#endif

int main(void) { return 0; }
