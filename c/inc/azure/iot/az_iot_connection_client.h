// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_CONNECTION_CLIENT_H
#define AZ_IOT_CONNECTION_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

#include "az_iot_result.h"
#include "az_iot_log.h"
#include "az_iot_mqtt_iface.h"
#include "az_iot_certificate_provider.h"
#include "az_iot_dispatch.h"

#include <azure/iot/az_iot_hub_client.h>
#include <azure/iot/az_iot_provisioning_client.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Note: the hub flavor (Classic vs Next) is NOT a caller-facing knob.
 * It is learned from DPS at provisioning time and the SDK selects the
 * appropriate MQTT version (v3.1.1 for Classic, v5 for Next) internally. */

typedef struct az_iot_reconnect_policy_tag
{
    uint32_t initial_delay_ms;
    uint32_t max_delay_ms;
    uint32_t max_attempts;             /* 0 = infinite */
    uint8_t  jitter_pct;               /* 0..100 */
} az_iot_reconnect_policy_t;

typedef struct az_iot_connection_client_options_tag
{
    const char* host;                  /* hub host (or NULL when using DPS) */
    uint16_t    port;                  /* default 8883 */
    const char* client_id;             /* device id */
    const char* model_id;              /* IoT Plug and Play model id announced at
                                        * connection (NULL = none). Required for
                                        * Device Update (ADU) to discover the
                                        * device; e.g.
                                        * "dtmi:azure:iot:deviceUpdateContractModel;2". */
    az_iot_certificate_provider_t* certificate_provider; /* required for X.509 auth */
    az_iot_reconnect_policy_t reconnect;
    az_iot_log_sink_t log;

    /* DPS provisioning options.  When host is NULL and id_scope is set, the
     * connection client internally provisions via DPS before connecting to the
     * assigned hub. */
    struct
    {
        const char* global_endpoint;   /* NULL => "global.azure-devices-provisioning.net" */
        const char* id_scope;
        const char* registration_id;
    } dps;
} az_iot_connection_client_options_t;

typedef enum az_iot_connection_state_tag
{
    AZ_IOT_CONN_STATE_IDLE = 0,
    AZ_IOT_CONN_STATE_CONNECTING,
    AZ_IOT_CONN_STATE_CONNECTED,
    AZ_IOT_CONN_STATE_RECONNECTING,
    AZ_IOT_CONN_STATE_DISCONNECTING,
    AZ_IOT_CONN_STATE_FAULTED
} az_iot_connection_state_t;

typedef void (*az_iot_connection_state_cb)(az_iot_connection_state_t state, az_iot_result_t reason, void* user_ctx);

typedef void (*az_iot_publish_ack_cb)(az_iot_result_t status, void* user_ctx);

/* ------------------------------------------------------------------------- */
/* Internal struct constants                                                 */
/* ------------------------------------------------------------------------- */

#define AZ_IOT_MAX_MQTT_FACTORIES       4
#define AZ_IOT_MAX_PENDING_PUBACKS      16
#define AZ_IOT_MAX_PERSISTENT_SUBS      8
#define AZ_IOT_PERSISTENT_SUB_TOPIC_MAX 128
#define AZ_IOT_DPS_TOPIC_BUF           256
#define AZ_IOT_DPS_OPERATION_ID_MAX     64
#define AZ_IOT_DPS_HOST_BUF            128
#define AZ_IOT_DPS_DEVICE_ID_BUF       128
#define AZ_IOT_MQTT_USERNAME_BUF        256

/* ------------------------------------------------------------------------- */
/* struct az_iot_connection_client_t (caller-owned, init/deinit lifecycle)    */
/* Fields below are INTERNAL — do not access directly from user code.        */
/* ------------------------------------------------------------------------- */

enum { AZ_IOT_CONN_DEFER_NONE = 0, AZ_IOT_CONN_DEFER_FAULT, AZ_IOT_CONN_DEFER_RECONNECT, AZ_IOT_CONN_DEFER_IDLE };
enum { AZ_IOT_DPS_PHASE_NONE = 0, AZ_IOT_DPS_PHASE_CONNECTING, AZ_IOT_DPS_PHASE_SUBSCRIBING,
       AZ_IOT_DPS_PHASE_REGISTERING, AZ_IOT_DPS_PHASE_POLLING, AZ_IOT_DPS_PHASE_DONE };

/* Session role: determines which Azure service the connection targets and
 * which MQTT version is required. This is an SDK-internal concept — adapters
 * do not need to know about roles; they only advertise MQTT version. */
typedef enum az_iot_mqtt_role_tag
{
    AZ_IOT_MQTT_ROLE_DPS          = 0, /* requires MQTT v3.1.1 */
    AZ_IOT_MQTT_ROLE_HUB_CLASSIC  = 1, /* requires MQTT v3.1.1 */
    AZ_IOT_MQTT_ROLE_HUB_NEXT     = 2  /* requires MQTT v5     */
} az_iot_mqtt_role_t;

struct az_iot_connection_client_tag
{
    az_iot_connection_client_options_t opts;

    az_iot_mqtt_factory_t factories[AZ_IOT_MAX_MQTT_FACTORIES];
    size_t factory_count;

    az_iot_mqtt_role_t session_role;
    az_iot_mqtt_client_t* active_client;

    az_iot_connection_state_t state;
    az_iot_connection_state_cb state_cb;
    void* state_cb_ctx;

    bool user_close;

    int deferred;
    az_iot_result_t deferred_reason;

    uint32_t reconnect_attempt;
    uint64_t reconnect_due_ms;
    uint64_t rng_state;

    az_iot_dispatch_table_t dispatch;

    struct {
        uint16_t packet_id;
        az_iot_publish_ack_cb cb;
        void* user_ctx;
        bool in_use;
    } pending_pubacks[AZ_IOT_MAX_PENDING_PUBACKS];

    struct {
        char  topic_filter[AZ_IOT_PERSISTENT_SUB_TOPIC_MAX];
        az_iot_mqtt_qos_t qos;
        bool  in_use;
    } persistent_subs[AZ_IOT_MAX_PERSISTENT_SUBS];

    char* owned_host;
    char* owned_client_id;

    int dps_phase;
    az_iot_provisioning_client dps_prov;
    az_iot_mqtt_client_t* dps_mqtt;
    char  dps_operation_id[AZ_IOT_DPS_OPERATION_ID_MAX];
    size_t dps_operation_id_len;
    uint64_t dps_poll_due_ms;
    char  dps_assigned_hub[AZ_IOT_DPS_HOST_BUF];
    char  dps_assigned_device_id[AZ_IOT_DPS_DEVICE_ID_BUF];
    bool  dps_pending_finalize;
    bool  dps_pending_have_assignment;
    az_iot_result_t dps_pending_status;

    az_iot_hub_client hub_client;
    bool hub_client_initialized;
    char hub_username[AZ_IOT_MQTT_USERNAME_BUF];
};

typedef struct az_iot_connection_client_tag az_iot_connection_client_t;

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

const char* az_iot_connection_state_to_string(az_iot_connection_state_t s);

/* Returns a default options struct pre-filled for DPS provisioning with X.509.
 * Sets port=8883, host=NULL, reconnect defaults, and the DPS fields. */
az_iot_connection_client_options_t az_iot_connection_client_options_get_default(
    const char* id_scope,
    const char* registration_id,
    az_iot_certificate_provider_t* certificate_provider);

az_iot_result_t az_iot_connection_client_init(
    az_iot_connection_client_t* client,
    const az_iot_connection_client_options_t* opts);

void az_iot_connection_client_deinit(az_iot_connection_client_t* client);

/* Register an MQTT factory in the client's adapter registry. The client may hold
 * multiple factories; at session-open time it picks the one whose
 * (version, supported_roles_mask) matches the required (version, role) for that
 * session. Adapters for DPS+Classic must be v3.1.1; adapters for Next must be v5. */
az_iot_result_t az_iot_connection_client_register_mqtt_factory(
    az_iot_connection_client_t* client,
    const az_iot_mqtt_factory_t* factory);

az_iot_result_t az_iot_connection_client_set_state_callback(
    az_iot_connection_client_t* client,
    az_iot_connection_state_cb cb,
    void* user_ctx);

/* Open a session to the configured host. Non-blocking; observe state via callback
 * and drive progress with do_work(). */
az_iot_result_t az_iot_connection_client_open(az_iot_connection_client_t* client);

az_iot_result_t az_iot_connection_client_close(az_iot_connection_client_t* client);

/* Pump network I/O and dispatch callbacks. Single-threaded contract: all user
 * callbacks fire synchronously from inside this call. */
az_iot_result_t az_iot_connection_client_do_work(az_iot_connection_client_t* client, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CONNECTION_CLIENT_H */
