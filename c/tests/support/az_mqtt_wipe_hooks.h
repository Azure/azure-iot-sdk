// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_mqtt_wipe_hooks.h
 * @brief Routes the az_mqtt adapter's malloc, calloc and free through recording hooks, so
 * az_mqtt_adapter_wipe_test.c can check freed memory. Included before the adapter's sources.
 */
#ifndef AZ_MQTT_WIPE_HOOKS_H
#define AZ_MQTT_WIPE_HOOKS_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/** @brief Text no block may hold when freed; NULL: none. */
extern char const* az_mqtt_wipe_test_secret;
/** @brief Recorded blocks freed. */
extern int az_mqtt_wipe_test_frees;
/** @brief Recorded blocks freed while holding az_mqtt_wipe_test_secret. */
extern int az_mqtt_wipe_test_leaks;

void* az_mqtt_wipe_test_malloc(size_t size);
void* az_mqtt_wipe_test_calloc(size_t count, size_t size);
void az_mqtt_wipe_test_free(void* p);

#define malloc az_mqtt_wipe_test_malloc
#define calloc az_mqtt_wipe_test_calloc
#define free az_mqtt_wipe_test_free

#endif // AZ_MQTT_WIPE_HOOKS_H
