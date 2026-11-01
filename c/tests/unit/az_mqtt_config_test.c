// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_mqtt_config_test.c
 * @brief Checks, when compiled, how az_iot_az_mqtt_config.h resolves the adapter limits. One
 * case per build: AZ_IOT_AZ_MQTT_CONFIG_TEST_<case>. Definitions made in code, before the
 * header, as an application compiling the adapter would.
 */

// Each case sets its own inputs: drop any from the build (e.g. CMAKE_C_FLAGS).
#undef AZ_IOT_AZ_MQTT_CONFIG_FILE
#undef AZ_IOT_AZ_MQTT_FOOTPRINT
#undef AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX

#if defined(AZ_IOT_AZ_MQTT_CONFIG_TEST_DEFAULT)
#define EXPECTED_PROPERTIES 16
#elif defined(AZ_IOT_AZ_MQTT_CONFIG_TEST_CONSTRAINED)
#define AZ_IOT_AZ_MQTT_FOOTPRINT AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED
#define EXPECTED_PROPERTIES 4
#elif defined(AZ_IOT_AZ_MQTT_CONFIG_TEST_OVERRIDE)
#define AZ_IOT_AZ_MQTT_FOOTPRINT AZ_IOT_AZ_MQTT_FOOTPRINT_CONSTRAINED
#define AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX 6
#define EXPECTED_PROPERTIES 6
#elif defined(AZ_IOT_AZ_MQTT_CONFIG_TEST_CONFIG_FILE)
// The file selects CONSTRAINED and sets USER_PROPERTIES_MAX.
#define AZ_IOT_AZ_MQTT_CONFIG_FILE "az_mqtt_config_test_user.h"
#define EXPECTED_PROPERTIES 2
#else
#error "Define AZ_IOT_AZ_MQTT_CONFIG_TEST_<case>"
#endif

#include "az_iot_az_mqtt_config.h"

#if AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX != EXPECTED_PROPERTIES
#error "AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX"
#endif

int main(void) { return 0; }
