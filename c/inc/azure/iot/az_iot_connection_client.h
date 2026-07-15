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

typedef struct az_iot_reconnection_policy
{
    uint32_t initial_delay_ms;
    uint32_t max_delay_ms;
    uint32_t max_attempts;             /* 0 = infinite */
    uint8_t  jitter_pct;               /* 0..100 */
} az_iot_reconnection_policy;

/* Returns a reasonable default reconnection policy: 1s initial delay, 30s max
 * backoff, infinite attempts, 20% jitter. Assign it to
 * az_iot_connection_client_options.reconnection_policy, then override as needed. */
az_iot_reconnection_policy az_iot_reconnection_policy_default(void);

/* Recommended minimum size (bytes) for opts.csr_payload_buffer: enough to build
 * the largest CSR request body {"id":...,"csr":<base64>,"replace":...} for the
 * service CSR size limit. Apps that only use small keys (EC / RSA-2048) may size
 * it smaller. */
#define AZ_IOT_CSR_PAYLOAD_BUFFER_MIN 8448

/* Declares a correctly-sized CSR payload buffer named `name`; wrap it in an
 * az_span for opts.csr_payload_buffer:
 *   AZ_IOT_CSR_PAYLOAD_STORAGE(csr_buf);
 *   copts.csr_payload_buffer = az_span_create(csr_buf, sizeof(csr_buf)); */
#define AZ_IOT_CSR_PAYLOAD_STORAGE(name) uint8_t name[AZ_IOT_CSR_PAYLOAD_BUFFER_MIN]

typedef struct az_iot_connection_client_options
{
    const char* host;                  /* hub host (or NULL when using DPS) */
    uint16_t    port;                  /* default 8883 */
    const char* client_id;             /* device id */
    const char* model_id;              /* IoT Plug and Play model id announced at
                                        * connection (NULL = none). Required for
                                        * Device Update (ADU) to discover the
                                        * device; e.g.
                                        * "dtmi:azure:iot:deviceUpdateContractModel;2". */
    az_iot_certificate_provider* certificate_provider; /* required for X.509 auth */
    az_iot_reconnection_policy reconnection_policy;
    az_iot_log_sink log;

    /* Caller-provided scratch buffer used to BUILD the outbound CSR request
     * payload - the DPS registration body when dps.request_operational_certificate
     * is set, and the hub renewal body for az_iot_connection_client_send_csr().
     * The SDK never allocates or declares a payload buffer of its own; provide
     * one here (>= AZ_IOT_CSR_PAYLOAD_BUFFER_MIN to cover the service CSR size
     * limit) when using either CSR feature. Leave AZ_SPAN_EMPTY otherwise. */
    az_span csr_payload_buffer;

    /* DPS provisioning options.  When host is NULL and id_scope is set, the
     * connection client internally provisions via DPS before connecting to the
     * assigned hub. */
    struct
    {
        const char* global_endpoint;   /* NULL => "global.azure-devices-provisioning.net" */
        const char* id_scope;
        const char* registration_id;
        bool        request_operational_certificate;  /* CSR-based enrollment (D2): send a CSR
                                                        * from the certificate_provider during DPS
                                                        * registration and connect to the assigned
                                                        * hub with the issued operational cert.
                                                        * Requires a provider whose vtable exposes
                                                        * get_csr (version >= 2). */
    } dps;
} az_iot_connection_client_options;

typedef enum az_iot_connection_state
{
    AZ_IOT_CONN_STATE_IDLE = 0,
    AZ_IOT_CONN_STATE_CONNECTING,
    AZ_IOT_CONN_STATE_CONNECTED,
    AZ_IOT_CONN_STATE_RECONNECTING,
    AZ_IOT_CONN_STATE_DISCONNECTING,
    AZ_IOT_CONN_STATE_FAULTED
} az_iot_connection_state;

typedef void (*az_iot_connection_state_callback)(az_iot_connection_state state, az_iot_result reason, void* user_ctx);

typedef void (*az_iot_publish_ack_callback)(az_iot_result status, void* user_ctx);

/* ---- Runtime Hub-side certificate renewal (Classic hub) ------------------ */
/* Device-initiated CSR to the connected hub. Two-phase: ACCEPTED (202) then
 * ISSUED (200) with the new chain, or FAILED. See docs/eng/certificate-management.md. */
typedef enum az_iot_csr_event_kind
{
    AZ_IOT_CSR_ACCEPTED = 0,   /* 202: hub accepted; signing in progress      */
    AZ_IOT_CSR_ISSUED,         /* 200: issued chain delivered (evt->issued)   */
    AZ_IOT_CSR_FAILED          /* rejected/failed (evt->status, service_code)  */
} az_iot_csr_event_kind;

typedef struct az_iot_csr_event
{
    az_iot_csr_event_kind kind;
    az_iot_result status;        /* AZ_IOT_OK unless FAILED                  */
    int32_t  service_code;         /* hub errorCode on FAILED; 0 otherwise     */
    uint32_t retry_after_s;        /* suggested retry delay; 0 if none         */
    const az_iot_issued_certificate* issued;  /* non-NULL on ISSUED          */
} az_iot_csr_event;

typedef void (*az_iot_csr_callback)(const az_iot_csr_event* evt, void* user_ctx);

/* Fired when the connection client obtains a DPS/provider-issued operational
 * certificate during provisioning (D4). Optional; use for app-side persistence
 * or to react (e.g. inventory). The chain is valid only during the callback. */
typedef void (*az_iot_operational_cert_callback)(
    const az_iot_issued_certificate* issued, void* user_ctx);

/* ------------------------------------------------------------------------- */
/* Internal struct constants                                                 */
/*                                                                           */
/* Compile-time footprint knobs sizing the caller-allocated client's in-     */
/* struct buffers/pools. #define any of them (or pass -D...) before including */
/* this header to tune memory use; the defaults suit a typical device.       */
/* ------------------------------------------------------------------------- */

#ifndef AZ_IOT_MAX_MQTT_FACTORIES
#define AZ_IOT_MAX_MQTT_FACTORIES       4
#endif
#ifndef AZ_IOT_MAX_PENDING_PUBACKS
#define AZ_IOT_MAX_PENDING_PUBACKS      16
#endif
#ifndef AZ_IOT_MAX_PERSISTENT_SUBS
#define AZ_IOT_MAX_PERSISTENT_SUBS      8
#endif
#ifndef AZ_IOT_PERSISTENT_SUB_TOPIC_MAX
#define AZ_IOT_PERSISTENT_SUB_TOPIC_MAX 128
#endif
#ifndef AZ_IOT_DPS_TOPIC_BUF
#define AZ_IOT_DPS_TOPIC_BUF           256
#endif
#ifndef AZ_IOT_DPS_OPERATION_ID_MAX
#define AZ_IOT_DPS_OPERATION_ID_MAX     64
#endif
#ifndef AZ_IOT_DPS_HOST_BUF
#define AZ_IOT_DPS_HOST_BUF            128
#endif
#ifndef AZ_IOT_DPS_DEVICE_ID_BUF
#define AZ_IOT_DPS_DEVICE_ID_BUF       128
#endif
#ifndef AZ_IOT_MQTT_USERNAME_BUF
#define AZ_IOT_MQTT_USERNAME_BUF        256
#endif

/* ------------------------------------------------------------------------- */
/* struct az_iot_connection_client (caller-owned, init/deinit lifecycle)    */
/* Fields below are INTERNAL — do not access directly from user code.        */
/* ------------------------------------------------------------------------- */

enum { AZ_IOT_CONN_DEFER_NONE = 0, AZ_IOT_CONN_DEFER_FAULT, AZ_IOT_CONN_DEFER_RECONNECT, AZ_IOT_CONN_DEFER_IDLE };
enum { AZ_IOT_DPS_PHASE_NONE = 0, AZ_IOT_DPS_PHASE_CONNECTING, AZ_IOT_DPS_PHASE_SUBSCRIBING,
       AZ_IOT_DPS_PHASE_REGISTERING, AZ_IOT_DPS_PHASE_POLLING, AZ_IOT_DPS_PHASE_DONE };

/* Session role: determines which Azure service the connection targets and
 * which MQTT version is required. This is an SDK-internal concept — adapters
 * do not need to know about roles; they only advertise MQTT version. */
typedef enum az_iot_mqtt_role
{
    AZ_IOT_MQTT_ROLE_DPS          = 0, /* requires MQTT v3.1.1 */
    AZ_IOT_MQTT_ROLE_HUB_CLASSIC  = 1, /* requires MQTT v3.1.1 */
    AZ_IOT_MQTT_ROLE_HUB_NEXT     = 2  /* requires MQTT v5     */
} az_iot_mqtt_role;

struct az_iot_connection_client
{
    az_iot_connection_client_options opts;

    az_iot_mqtt_factory factories[AZ_IOT_MAX_MQTT_FACTORIES];
    size_t factory_count;

    az_iot_mqtt_role session_role;
    az_iot_mqtt_client* active_client;

    az_iot_connection_state state;
    az_iot_connection_state_callback state_cb;
    void* state_cb_ctx;
    az_iot_operational_cert_callback op_cert_cb;
    void* op_cert_cb_ctx;

    bool user_close;

    int deferred;
    az_iot_result deferred_reason;

    uint32_t reconnect_attempt;
    uint64_t reconnect_due_ms;
    uint64_t rng_state;

    az_iot_dispatch_table dispatch;

    struct {
        uint16_t packet_id;
        az_iot_publish_ack_callback cb;
        void* user_ctx;
        bool in_use;
    } pending_pubacks[AZ_IOT_MAX_PENDING_PUBACKS];

    struct {
        char  topic_filter[AZ_IOT_PERSISTENT_SUB_TOPIC_MAX];
        az_iot_mqtt_qos qos;
        bool  in_use;
    } persistent_subs[AZ_IOT_MAX_PERSISTENT_SUBS];

    /* Effective hub hostname / device id when not caller-provided (DPS-assigned,
     * mock, or runtime-set). Inline fixed buffers in this caller-allocated struct
     * -- no heap. */
    char  provisioned_iot_hub_hostname[AZ_IOT_DPS_HOST_BUF];
    char  provisioned_device_id[AZ_IOT_DPS_DEVICE_ID_BUF];

    int dps_phase;
    az_iot_provisioning_client dps_prov;
    az_iot_mqtt_client* dps_mqtt;
    char  dps_operation_id[AZ_IOT_DPS_OPERATION_ID_MAX];
    size_t dps_operation_id_len;
    uint64_t dps_poll_due_ms;
    char  dps_assigned_hub[AZ_IOT_DPS_HOST_BUF];
    char  dps_assigned_device_id[AZ_IOT_DPS_DEVICE_ID_BUF];
    bool  dps_pending_finalize;
    bool  dps_pending_have_assignment;
    az_iot_result dps_pending_status;
    bool  dps_enrolling;           /* CSR-based enrollment active for this DPS session */
    bool  dps_have_issued_cert;    /* an operational cert was issued by DPS/Hub and stored */

    az_iot_hub_client hub_client;
    bool hub_client_initialized;
    char hub_username[AZ_IOT_MQTT_USERNAME_BUF];

    /* Runtime Hub-side CSR renewal: one in-flight operation, matched by rid. */
    struct {
        bool  in_use;
        bool  subscribed;
        char  request_id[64];
        az_iot_csr_callback cb;
        void* user_ctx;
        uint64_t deadline_ms;      /* abandon the op if no terminal response by here */
    } csr_op;
};

typedef struct az_iot_connection_client az_iot_connection_client;

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

const char* az_iot_connection_state_to_string(az_iot_connection_state s);

/* Returns an options struct with optional fields defaulted (port=8883, no
 * reconnect, no log sink). Set the required fields for your auth/provisioning
 * mode on the returned struct before az_iot_connection_client_init():
 *   - DPS + X.509 (host==NULL): dps.id_scope, dps.registration_id,
 *     certificate_provider.
 *   - Direct hub connect: host, client_id, certificate_provider. */
AZ_NODISCARD az_iot_connection_client_options az_iot_connection_client_options_default(void);

AZ_NODISCARD az_iot_result az_iot_connection_client_init(
    az_iot_connection_client* client,
    const az_iot_connection_client_options* opts);

void az_iot_connection_client_destroy(az_iot_connection_client* client);

/* Register an MQTT factory in the client's adapter registry. The client may hold
 * multiple factories; at session-open time it picks the one whose
 * (version, supported_roles_mask) matches the required (version, role) for that
 * session. Adapters for DPS+Classic must be v3.1.1; adapters for Next must be v5. */
AZ_NODISCARD az_iot_result az_iot_connection_client_register_mqtt_factory(
    az_iot_connection_client* client,
    const az_iot_mqtt_factory* factory);

/* Not AZ_NODISCARD: configuration setters that fail only on invalid arguments
 * (a programming error), so callers routinely fire-and-forget them. */
az_iot_result az_iot_connection_client_set_state_callback(
    az_iot_connection_client* client,
    az_iot_connection_state_callback cb,
    void* user_ctx);

/* Register a callback fired when a DPS/provider-issued operational certificate
 * is obtained during provisioning (D4). Optional. */
az_iot_result az_iot_connection_client_set_operational_cert_callback(
    az_iot_connection_client* client,
    az_iot_operational_cert_callback cb,
    void* user_ctx);

/* Open a session to the configured host. Non-blocking; observe state via callback
 * and drive progress with do_work(). */
AZ_NODISCARD az_iot_result az_iot_connection_client_open(az_iot_connection_client* client);

/* Not AZ_NODISCARD: teardown/lifecycle op commonly called fire-and-forget. */
az_iot_result az_iot_connection_client_close(az_iot_connection_client* client);

/* Pump network I/O and dispatch callbacks. Single-threaded contract: all user
 * callbacks fire synchronously from inside this call. Not AZ_NODISCARD: this is a
 * pump, commonly called in a loop where the per-call result is observed via the
 * state callback rather than the return value. */
az_iot_result az_iot_connection_client_do_work(az_iot_connection_client* client, uint32_t timeout_ms);

/* Request a renewed operational certificate from the connected (Classic) hub by
 * sending a CSR. Two-phase: the callback fires with AZ_IOT_CSR_ACCEPTED (202),
 * then AZ_IOT_CSR_ISSUED (200) carrying the new chain, or AZ_IOT_CSR_FAILED.
 *   request_id: NULL => the SDK generates one; pass a prior id to resubmit.
 *   replace:    NULL, or "*" / a request id to supersede an active hub-side op.
 * The request's device id is taken from the connected client_id. Only one CSR
 * operation may be in flight; returns AZ_IOT_ERR_BUSY otherwise. The issued
 * chain in AZ_IOT_CSR_ISSUED is valid only for the duration of the callback.
 * If no terminal (200/error) response arrives within an internal timeout, the
 * callback fires once with AZ_IOT_CSR_FAILED / AZ_IOT_ERR_TIMEOUT and the slot
 * is released, so a lost response can never wedge renewal permanently. */
AZ_NODISCARD az_iot_result az_iot_connection_client_send_csr(
    az_iot_connection_client* client,
    const az_iot_certificate_signing_request* csr,
    const char* request_id,
    const char* replace,
    az_iot_csr_callback cb,
    void* user_ctx);

/* Abandon the in-flight CSR renewal (if any) without waiting for the timeout,
 * freeing the one-operation slot for a new az_iot_connection_client_send_csr().
 * No callback fires. Returns AZ_IOT_ERR_NOT_FOUND when no operation is active. */
AZ_NODISCARD az_iot_result az_iot_connection_client_cancel_csr(az_iot_connection_client* client);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CONNECTION_CLIENT_H */
