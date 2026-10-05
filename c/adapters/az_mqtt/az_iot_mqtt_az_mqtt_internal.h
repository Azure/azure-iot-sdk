// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_mqtt_az_mqtt_internal.h
 * @brief Helpers shared by the MQTT 3.1.1 and MQTT 5 builds of the az_mqtt adapter.
 */
#ifndef AZ_IOT_MQTT_AZ_MQTT_INTERNAL_H
#define AZ_IOT_MQTT_AZ_MQTT_INTERNAL_H

#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief MQTT 5 user properties per packet, each direction. A larger outgoing count is refused;
 * received ones beyond it are dropped. */
#ifndef AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX
#define AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX 16
#endif

/** @brief Send and receive buffer size, each: the largest packet either way. */
#ifndef AZ_IOT_AZ_MQTT_BUFFER_SIZE
#define AZ_IOT_AZ_MQTT_BUFFER_SIZE (264 * 1024)
#endif

/** @brief QoS 1/2 exchanges in flight at once, both directions together. */
#ifndef AZ_IOT_AZ_MQTT_INFLIGHT_MAX
#define AZ_IOT_AZ_MQTT_INFLIGHT_MAX 64
#endif

/** @brief Events raised outside process_loop() and held until it runs. */
#define AZ_IOT_AZ_MQTT_PENDING_EVENTS_MAX 4

/** @brief Copy of @p s (NUL-terminated), or NULL when out of memory. NULL in, NULL out. */
char* az_iot_az_mqtt_strdup(const char* s);

/** @brief Whether @p s is non-NULL and not empty. */
bool az_iot_az_mqtt_has_text(const char* s);

/**
 * @brief The result an iface call returns for an az_mqtt failure @p rc of a request
 * (subscribe, unsubscribe, publish).
 */
az_iot_result az_iot_az_mqtt_request_result(az_result rc);

/** @brief The status an event reports for a connect or session that failed with @p rc. */
az_iot_result az_iot_az_mqtt_session_result(az_result rc);

/**
 * @brief Make the OpenSSL 3 provider @p name available: loaded once for the process and never
 * unloaded; the default provider stays available.
 * @retval AZ_IOT_OK Available.
 * @retval AZ_IOT_ERR_NOT_SUPPORTED Not an OpenSSL build, or the provider cannot be loaded.
 */
az_iot_result az_iot_az_mqtt_load_key_provider(const char* name);

/**
 * @brief Writes NUL-terminated copies of received spans into one buffer, sized so that every
 * string of one received packet fits.
 */
typedef struct
{
  char* next;
  char* end;
} az_iot_az_mqtt_string_writer;

/** @brief Copy @p s and a NUL; NULL if it does not fit. */
const char* az_iot_az_mqtt_string_writer_add(az_iot_az_mqtt_string_writer* w, az_span s);

#endif /* AZ_IOT_MQTT_AZ_MQTT_INTERNAL_H */
