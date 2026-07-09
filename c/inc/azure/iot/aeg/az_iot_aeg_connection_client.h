// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef az_iot_CONNECTION_CLIENT_H
#define az_iot_CONNECTION_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

#include "az_iot_result.h"
#include "az_iot_log.h"
#include "az_iot_mqtt_iface.h"
#include "az_iot_certificate_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Note: the hub flavor (Classic vs Next) is NOT a caller-facing knob.
 * It is learned from DPS at provisioning time and the SDK selects the
 * appropriate MQTT version (v3.1.1 for Classic, v5 for Next) internally. */

typedef struct az_iot_reconnect_policy
{
    uint32_t initial_delay_ms;
    uint32_t max_delay_ms;
    uint32_t max_attempts;             /* 0 = infinite */
    uint8_t  jitter_pct;               /* 0..100 */
} az_iot_reconnect_policy;

typedef struct az_iot_connection_client_options
{
    const char* host;                  /* hub host (or NULL when using DPS) */
    uint16_t    port;                  /* default 8883 */
    const char* client_id;             /* device id */
    az_iot_certificate_provider* certificate_provider; /* required for X.509 auth */
    az_iot_reconnect_policy reconnect;
    az_iot_log_sink log;

    /* DPS provisioning options.  When host is NULL and id_scope is set, the
     * connection client internally provisions via DPS before connecting to the
     * assigned hub. */
    struct
    {
        const char* global_endpoint;   /* NULL => "global.azure-devices-provisioning.net" */
        const char* id_scope;
        const char* registration_id;
    } dps;
} az_iot_connection_client_options;

typedef struct az_iot_connection_client az_iot_connection_client;

typedef enum az_iot_connection_state
{
    az_iot_CONN_STATE_IDLE = 0,
    az_iot_CONN_STATE_CONNECTING,
    az_iot_CONN_STATE_CONNECTED,
    az_iot_CONN_STATE_RECONNECTING,
    az_iot_CONN_STATE_DISCONNECTING,
    az_iot_CONN_STATE_FAULTED
} az_iot_connection_state;

typedef void (*az_iot_connection_state_callback)(az_iot_connection_state state, az_iot_result reason, void* user_ctx);

const char* az_iot_connection_state_to_string(az_iot_connection_state s);

/* Returns a default options struct pre-filled for DPS provisioning with X.509.
 * Sets port=8883, host=NULL, reconnect defaults, and the DPS fields. */
az_iot_connection_client_options az_iot_connection_client_options_get_default(
    const char* id_scope,
    const char* registration_id,
    az_iot_certificate_provider* certificate_provider);

az_iot_result az_iot_connection_client_init(
    az_iot_connection_client* client,
    const az_iot_connection_client_options* opts);

void az_iot_connection_client_destroy(az_iot_connection_client* client);

/* Register an MQTT factory in the client's adapter registry. The client may hold
 * multiple factories; at session-open time it picks the one whose
 * (version, supported_roles_mask) matches the required (version, role) for that
 * session. Adapters for DPS+Classic must be v3.1.1; adapters for Next must be v5. */
az_iot_result az_iot_connection_client_register_mqtt_factory(
    az_iot_connection_client* client,
    const az_iot_mqtt_factory* factory);

az_iot_result az_iot_connection_client_set_state_callback(
    az_iot_connection_client* client,
    az_iot_connection_state_callback cb,
    void* user_ctx);

/* Open a session to the configured host. Non-blocking; observe state via callback
 * and drive progress with do_work(). */
az_iot_result az_iot_connection_client_open(az_iot_connection_client* client);

az_iot_result az_iot_connection_client_close(az_iot_connection_client* client);

/* Pump network I/O and dispatch callbacks. Single-threaded contract: all user
 * callbacks fire synchronously from inside this call. */
az_iot_result az_iot_connection_client_do_work(az_iot_connection_client* client, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* az_iot_CONNECTION_CLIENT_H */
