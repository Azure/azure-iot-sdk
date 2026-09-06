// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

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
extern "C"
{
#endif

  /* ---- The connection profile ---------------------------------------------- */
  /* Which generation of hub this connection speaks to, and therefore which MQTT
   * version and topic shapes it uses. One type serves both directions:
   *   - DPS connect (host == NULL): LEARNED, from the `connectionProfile`
   *     property of the ASSIGNED payload. opts.connection_profile is ignored.
   *     Until that DPS api-version ships, local development can synthesize an
   *     absent/null value with AZ_IOT_DPS_CONNECTION_PROFILE_OVERRIDE; an
   *     explicit service value always wins.
   *   - Direct connect (host set, no DPS): DECLARED by the caller through
   *     opts.connection_profile, because there is nobody to ask. Defaults to
   *     CLASSIC (MQTT v3.1.1); set MQTT_V5 for an IoT Hub Next / Event Grid
   *     (AEG) endpoint.
   * Either way az_iot_connection_client_get_hub_profile() reports the result.
   *
   * On the wire `connectionProfile` is a STRING and an extensible union -- the
   * service contract says future hub capabilities pass through without a
   * breaking change. A closed C enum cannot represent that, which is why the
   * verbatim string is carried alongside it. See docs/eng/client-separation.md. */
  typedef enum az_iot_connection_profile
  {
    AZ_IOT_CONNECTION_PROFILE_CLASSIC = 0, /* "classic" -- also the absent/null default */
    AZ_IOT_CONNECTION_PROFILE_MQTT_V5 = 1, /* "mqttV5"                                  */
    /* A value newer than this SDK. Only ever produced by the service; passing it
     * to az_iot_connection_client_init() is rejected, since the caller cannot
     * meaningfully declare a profile the SDK does not know how to speak. */
    AZ_IOT_CONNECTION_PROFILE_UNKNOWN = -1
  } az_iot_connection_profile;

  /* Caller-allocated and expected to grow, so it carries a size stamp per
   * docs/struct_versioning.md: a caller compiled against an older header is
   * defaulted rather than misread. MUST be initialized with
   * AZ_IOT_HUB_PROFILE_INIT -- a raw `= {0}` stamps size 0 and is rejected. */
  typedef struct az_iot_hub_profile
  {
    uint32_t _internal_size;
    az_iot_connection_profile connection_profile;
    /* The effective profile text, never NULL. When DPS supplies a string it is
     * verbatim, which keeps the extensible union from becoming lossy at the C
     * boundary: a profile this SDK has never heard of still reports UNKNOWN
     * *and* the text the service sent. For absent/null it is the resolved
     * contract default ("classic"), or the exact development override value.
     * Points into the connection client and stays valid until destroy().
     *
     * Bounded by AZ_IOT_CONNECTION_PROFILE_RAW_BUF, so it is the value verbatim
     * only when connection_profile_raw_truncated is false. Callers that report
     * this value onward MUST check that flag rather than assume the text is
     * complete. */
    const char* connection_profile_raw;
    /* The service sent a longer value than connection_profile_raw can hold, so
     * the text above is a prefix. Such a profile is always UNKNOWN -- every
     * profile this SDK recognises is short, so an overlong one cannot be one of
     * them -- and therefore fails the connection. Raise
     * AZ_IOT_CONNECTION_PROFILE_RAW_BUF if a real profile ever needs the room. */
    bool connection_profile_raw_truncated;
  } az_iot_hub_profile;

#define AZ_IOT_HUB_PROFILE_INIT                                                              \
  {                                                                                          \
    ._internal_size = sizeof(az_iot_hub_profile),                                            \
    .connection_profile = AZ_IOT_CONNECTION_PROFILE_CLASSIC, .connection_profile_raw = NULL, \
    .connection_profile_raw_truncated = false,                                               \
  }

  typedef struct az_iot_reconnection_policy
  {
    uint32_t initial_delay_ms;
    uint32_t max_delay_ms;
    uint32_t max_attempts; /* 0 = infinite */
    uint8_t jitter_pct; /* 0..100 */
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
    const char* host; /* hub host (or NULL when using DPS) */
    uint16_t port; /* default 8883 */

    /* MQTT keep-alive, in seconds. 0 selects AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS.
     *
     * IoT Hub's server-side timeout is 1.5x this value, capped at 1767 s, so
     * the largest useful setting is 1177 s; anything above that is clamped by
     * the service rather than by the SDK. Any traffic resets the timer.
     *
     * Shorten it to notice a dead link sooner, at the cost of more PINGREQs;
     * lengthen it on a metered or battery-powered device, at the cost of the
     * service taking longer to notice the device is gone.
     *
     * Applies to the DPS bootstrap connect as well as the hub connect. */
    uint16_t keep_alive_seconds;

    /* How long to wait for the transport to connect, in seconds. 0 selects
     * AZ_IOT_DEFAULT_CONNECT_TIMEOUT_SECONDS.
     *
     * In seconds like keep_alive_seconds above, so every timing option on this
     * struct reads on one scale. Sub-second connect timeouts are not useful
     * over TLS on a cellular or satellite link. */
    uint32_t connect_timeout_seconds;

    /* How long CONNECTED may be withheld waiting for the SUBACKs of the
     * persistent filters issued on connect, in seconds. 0 selects
     * AZ_IOT_DEFAULT_SUBSCRIPTION_ACK_TIMEOUT_SECONDS.
     *
     * Nothing else bounds that wait: connect_timeout_seconds covers the CONNACK,
     * the presence handshake has its own timeout, and keep-alive cannot help
     * because the link is alive -- a broker that accepts the connection and then
     * never answers the SUBSCRIBE would otherwise hold the client in CONNECTING
     * indefinitely.
     *
     * The clock starts when the last SUBSCRIBE of the batch reaches the adapter
     * and is never extended by an arriving SUBACK: a per-ack reset would let a
     * broker answering one filter just inside each window hold CONNECTED open
     * forever, which is the failure this bounds. Expiry is treated as transient
     * -- silence is not a refusal -- so it reconnects under the policy. */
    uint32_t subscription_ack_timeout_seconds;
    const char* client_id; /* device id */
    az_iot_connection_profile connection_profile; /* direct-connect generation (host set,
                                                   * no DPS): CLASSIC (v3.1.1, default) or
                                                   * MQTT_V5 (AEG). Ignored when using DPS,
                                                   * where it is learned instead. */
    const char* model_id; /* IoT Plug and Play model id announced at
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
      const char* global_endpoint; /* NULL => "global.azure-devices-provisioning.net" */
      const char* id_scope;
      const char* registration_id;
      bool request_operational_certificate; /* CSR-based enrollment (D2): send a CSR
                                             * from the certificate_provider during DPS
                                             * registration and connect to the assigned
                                             * hub with the issued operational cert.
                                             * Requires a provider whose vtable exposes
                                             * get_csr (version >= 2). */

      /* Consecutive failed hub connect attempts after which the assignment is
       * treated as stale and re-provisioning is forced. Defaults to
       * AZ_IOT_DEFAULT_MAX_HUB_CONNECT_ATTEMPTS_BEFORE_REPROVISION; 0 disables
       * it.
       *
       * Re-provisioning is otherwise only triggered by a CONNACK that rejects
       * the identity. A hub that has been vacated service-side may simply stop
       * answering instead, and the cached assignment would then be retried until
       * the reconnection policy gives up -- never asking DPS where the device
       * actually lives now. This bounds that.
       *
       * Under the default policy (1s initial, 30s cap, +/-20% jitter) the delays
       * run 1, 2, 4, 8, 16 then 30s, so attempt N >= 6 falls at roughly
       * 31 + 30*(N-5) seconds: the default 50 is about 23 minutes. Long enough
       * that an ordinary network outage does not send a whole fleet to DPS at
       * once, short enough that a device left behind by a migration recovers
       * without an operator. */
      uint32_t max_hub_connect_attempts_before_reprovision;
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

  /* SDK-produced, callback-lifetime view of a connection-state transition.
   * The SDK stamps _internal_size; callers never initialize this struct. Future
   * SDKs may append fields, so callbacks must check _internal_size before
   * reading a field added after the version they were compiled against.
   *
   * profile is non-NULL when state == AZ_IOT_CONN_STATE_CONNECTED, and also on
   * a failure whose reason is AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH or
   * AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED -- an application needs the
   * assigned generation there in order to rebuild its feature clients. It
   * and the event itself remain valid only until the callback returns; copy any
   * value that must be retained. */
  typedef struct az_iot_connection_state_event
  {
    uint32_t _internal_size;
    az_iot_connection_state state;
    az_iot_result reason;
    const az_iot_hub_profile* profile;
  } az_iot_connection_state_event;

  typedef void (*az_iot_connection_state_callback)(
      const az_iot_connection_state_event* event,
      void* user_ctx);

  typedef void (*az_iot_publish_ack_callback)(az_iot_result status, void* user_ctx);

  /* Notified when an MQTT session ends -- peer disconnect, transport error, or a
   * user close -- so a feature client can complete whatever it had correlated
   * against that session instead of waiting forever for a response that can no
   * longer arrive. Registered through the internal header; applications use
   * az_iot_connection_client_set_state_callback() instead.
   *
   * Deliberately NOT invoked from destroy(), for the same reason pending QoS-1
   * acknowledgements are not completed there: the application is tearing the
   * client down and the context the callback closes over may already be gone. */
  typedef void (*az_iot_session_end_callback)(void* user_ctx);

  /* ---- Runtime Hub-side certificate renewal (Classic hub) ------------------ */
  /* Device-initiated CSR to the connected hub. Two-phase: ACCEPTED (202) then
   * ISSUED (200) with the new chain, or FAILED. See docs/eng/certificate-management.md. */
  typedef enum az_iot_csr_event_kind
  {
    AZ_IOT_CSR_ACCEPTED = 0, /* 202: hub accepted; signing in progress      */
    AZ_IOT_CSR_ISSUED, /* 200: issued chain delivered (evt->issued)   */
    AZ_IOT_CSR_FAILED /* rejected/failed (evt->status, service_code)  */
  } az_iot_csr_event_kind;

  typedef struct az_iot_csr_event
  {
    az_iot_csr_event_kind kind;
    az_iot_result status; /* AZ_IOT_OK unless FAILED                  */
    int32_t service_code; /* hub errorCode on FAILED; 0 otherwise     */
    uint32_t retry_after_s; /* suggested retry delay; 0 if none         */
    const az_iot_issued_certificate* issued; /* non-NULL on ISSUED          */
  } az_iot_csr_event;

  typedef void (*az_iot_csr_callback)(const az_iot_csr_event* evt, void* user_ctx);

  /* Fired when the connection client obtains a DPS/provider-issued operational
   * certificate during provisioning (D4). Optional; use for app-side persistence
   * or to react (e.g. inventory). The chain is valid only during the callback. */
  typedef void (
      *az_iot_operational_cert_callback)(const az_iot_issued_certificate* issued, void* user_ctx);

  /* What a refused persistent subscription costs. Both values name a FAILURE:
   * the difference is blast radius, not whether the subscription mattered.
   * See docs/eng/client-separation.md section 9. */
  typedef enum az_iot_subscription_failure_scope
  {
    /* The registering client cannot work without this filter, so a refusal ends
     * the connection. Every feature client's own filter is registered this way. */
    AZ_IOT_SUBSCRIPTION_FAILS_SESSION = 0,
    /* A refusal is reported to the owner and the entry dropped; the connection
     * survives. For application-supplied topics, where one declined filter must
     * not take telemetry and every other feature down with it. */
    AZ_IOT_SUBSCRIPTION_FAILS_SELF
  } az_iot_subscription_failure_scope;

  /* Reports that a FAILS_SELF subscription did not come up. The entry is already
   * out of the registry when this runs, so the callback may re-register.
   * `protocol_code` is the verbatim wire code, 0 when there was none.
   * `topic_filter` is valid only for the duration of the call. */
  typedef void (*az_iot_subscription_failed_callback)(
      const char* topic_filter,
      az_iot_result reason,
      int32_t protocol_code,
      const void* owner);

  /* ------------------------------------------------------------------------- */
  /* Internal struct constants                                                 */
  /*                                                                           */
  /* Compile-time footprint knobs sizing the caller-allocated client's in-     */
  /* struct buffers/pools. #define any of them (or pass -D...) before including */
  /* this header to tune memory use; the defaults suit a typical device.       */
  /* ------------------------------------------------------------------------- */

#ifndef AZ_IOT_MAX_MQTT_FACTORIES
#define AZ_IOT_MAX_MQTT_FACTORIES 4
#endif
#ifndef AZ_IOT_MAX_PENDING_PUBACKS
#define AZ_IOT_MAX_PENDING_PUBACKS 16
#endif
/* Topic filters the connection re-subscribes on every session. Feature clients
 * take one slot per filter they need, so the default leaves headroom over what
 * a fully loaded device asks for: five on Classic (C2D, direct methods, twin
 * response, twin desired, certificate renewal) and six on Hub-Next. Registering
 * past the array fails with AZ_IOT_ERR_NOT_ENOUGH_SPACE and names the filter
 * that did not fit -- raise this if an application needs more slots than the
 * default holds. */
#ifndef AZ_IOT_MAX_PERSISTENT_SUBS
#define AZ_IOT_MAX_PERSISTENT_SUBS 8
#endif
/* Feature clients that correlate a request against a session (twin GET/PATCH,
 * hub certificate renewal) register here to be told when that session ends. */
#ifndef AZ_IOT_MAX_SESSION_HANDLERS
#define AZ_IOT_MAX_SESSION_HANDLERS 4
#endif

/* Defaults applied when the corresponding option is left at 0. */
#ifndef AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS
#define AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS 30
#endif
#ifndef AZ_IOT_DEFAULT_CONNECT_TIMEOUT_SECONDS
#define AZ_IOT_DEFAULT_CONNECT_TIMEOUT_SECONDS 30
#endif
/* Matches the presence birth-ack timeout: both bound "the broker accepted the
 * connection and then went quiet", and having two different windows for that on
 * one connect path would be arbitrary. */
#ifndef AZ_IOT_DEFAULT_SUBSCRIPTION_ACK_TIMEOUT_SECONDS
#define AZ_IOT_DEFAULT_SUBSCRIPTION_ACK_TIMEOUT_SECONDS 60
#endif
#ifndef AZ_IOT_PERSISTENT_SUB_TOPIC_MAX
#define AZ_IOT_PERSISTENT_SUB_TOPIC_MAX 128
#endif
#ifndef AZ_IOT_DPS_TOPIC_BUF
#define AZ_IOT_DPS_TOPIC_BUF 256
#endif
#ifndef AZ_IOT_DPS_OPERATION_ID_MAX
#define AZ_IOT_DPS_OPERATION_ID_MAX 64
#endif
#ifndef AZ_IOT_DPS_HOST_BUF
#define AZ_IOT_DPS_HOST_BUF 128
#endif
#ifndef AZ_IOT_DPS_DEVICE_ID_BUF
#define AZ_IOT_DPS_DEVICE_ID_BUF 128
#endif
/* Holds the verbatim `connectionProfile` string. Sized for a value far longer
 * than the ones defined today ("classic", "mqttV5") because the property is an
 * extensible union: the whole point is to report values this SDK has never seen.
 * A profile longer than this is reported truncated rather than dropped. */
#ifndef AZ_IOT_CONNECTION_PROFILE_RAW_BUF
#define AZ_IOT_CONNECTION_PROFILE_RAW_BUF 64
#endif
/* See opts.dps.max_hub_connect_attempts_before_reprovision. */
#ifndef AZ_IOT_DEFAULT_MAX_HUB_CONNECT_ATTEMPTS_BEFORE_REPROVISION
#define AZ_IOT_DEFAULT_MAX_HUB_CONNECT_ATTEMPTS_BEFORE_REPROVISION 50u
#endif
/* Feature clients that ask to build their topics at connect time. One per
 * attached feature client, so this tracks the persistent-subscription bound. */
#ifndef AZ_IOT_MAX_FEATURE_CLIENT_BINDS
#define AZ_IOT_MAX_FEATURE_CLIENT_BINDS 8
#endif
#ifndef AZ_IOT_MQTT_USERNAME_BUF
#define AZ_IOT_MQTT_USERNAME_BUF 256
#endif
/* Buffer sizing the ih/{deviceId}/srv|dev/presence topics built for the
 * AEG/Hub-Next birth handshake. */
#ifndef AZ_IOT_PRESENCE_TOPIC_BUF
#define AZ_IOT_PRESENCE_TOPIC_BUF 256
#endif
/* How long to wait for the SUBACK + birth-ack that complete the AEG/Hub-Next
 * presence handshake before abandoning the attempt (mirrors the .NET SDK's
 * 60s defensive birth-ack timeout). */
#ifndef AZ_IOT_PRESENCE_BIRTH_ACK_TIMEOUT_MS
#define AZ_IOT_PRESENCE_BIRTH_ACK_TIMEOUT_MS 60000u
#endif

  /* ------------------------------------------------------------------------- */
  /* struct az_iot_connection_client (caller-owned, init/deinit lifecycle)    */
  /* Fields below are INTERNAL — do not access directly from user code.        */
  /* ------------------------------------------------------------------------- */

  enum
  {
    AZ_IOT_CONN_DEFER_NONE = 0,
    AZ_IOT_CONN_DEFER_FAULT,
    AZ_IOT_CONN_DEFER_RECONNECT,
    AZ_IOT_CONN_DEFER_IDLE
  };
  enum
  {
    AZ_IOT_DPS_PHASE_NONE = 0,
    AZ_IOT_DPS_PHASE_CONNECTING,
    AZ_IOT_DPS_PHASE_SUBSCRIBING,
    AZ_IOT_DPS_PHASE_REGISTERING,
    AZ_IOT_DPS_PHASE_POLLING,
    AZ_IOT_DPS_PHASE_DONE
  };
  /* AEG/Hub-Next presence (birth) handshake phases. Classic/DPS sessions never
   * leave AZ_IOT_PRESENCE_PHASE_NONE. */
  enum
  {
    AZ_IOT_PRESENCE_PHASE_NONE = 0,
    AZ_IOT_PRESENCE_PHASE_SUBSCRIBING,
    AZ_IOT_PRESENCE_PHASE_BIRTH,
    AZ_IOT_PRESENCE_PHASE_DONE
  };

  /* Session role: determines which Azure service the connection targets and
   * which MQTT version is required. This is an SDK-internal concept — adapters
   * do not need to know about roles; they only advertise MQTT version. */
  typedef enum az_iot_mqtt_role
  {
    AZ_IOT_MQTT_ROLE_DPS = 0, /* requires MQTT v3.1.1 */
    AZ_IOT_MQTT_ROLE_HUB_CLASSIC = 1, /* requires MQTT v3.1.1 */
    AZ_IOT_MQTT_ROLE_HUB_NEXT = 2 /* requires MQTT v5     */
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

    /* Set when the next reconnect attempt must re-provision through DPS rather
     * than reconnect to the cached assignment -- because the hub refused this
     * identity, or because hub attempts crossed the configured threshold. Kept
     * beside user_close so it lands in the padding that already precedes
     * `deferred` rather than adding its own. */
    bool needs_reprovision;

    int deferred;
    az_iot_result deferred_reason;

    uint32_t reconnect_attempt;
    uint64_t reconnect_due_ms;
    uint64_t rng_state;

    az_iot_dispatch_table dispatch;

    struct
    {
      uint16_t packet_id;
      az_iot_publish_ack_callback cb;
      void* user_ctx;
      bool in_use;
    } pending_pubacks[AZ_IOT_MAX_PENDING_PUBACKS];

    struct
    {
      char topic_filter[AZ_IOT_PERSISTENT_SUB_TOPIC_MAX];
      az_iot_mqtt_qos qos;
      /* The feature client that registered this filter, so its destroy() can
       * withdraw exactly its own entries without rebuilding the strings. */
      const void* owner;
      /* The generation this filter was built for. A reconnect that resolves a
       * different profile drops it instead of re-issuing a filter the new hub
       * will not recognise -- see docs/eng/client-separation.md section 9. */
      az_iot_connection_profile profile;
      /* Whether a refusal ends the session or only this subscription. */
      az_iot_subscription_failure_scope failure_scope;
      /* Told when a FAILS_SELF filter fails. NULL on a FAILS_SESSION entry,
       * which reports through the connection state instead. */
      az_iot_subscription_failed_callback on_failed;
      bool in_use;
    } persistent_subs[AZ_IOT_MAX_PERSISTENT_SUBS];

    struct
    {
      az_iot_session_end_callback cb;
      void* user_ctx;
      bool in_use;
    } session_handlers[AZ_IOT_MAX_SESSION_HANDLERS];

    /* Effective hub hostname / device id when not caller-provided (DPS-assigned,
     * mock, or runtime-set). Inline fixed buffers in this caller-allocated struct
     * -- no heap. */
    char provisioned_iot_hub_hostname[AZ_IOT_DPS_HOST_BUF];
    char provisioned_device_id[AZ_IOT_DPS_DEVICE_ID_BUF];

    int dps_phase;
    az_iot_provisioning_client dps_prov;
    az_iot_mqtt_client* dps_mqtt;
    char dps_operation_id[AZ_IOT_DPS_OPERATION_ID_MAX];
    size_t dps_operation_id_len;
    uint64_t dps_poll_due_ms;
    char dps_assigned_hub[AZ_IOT_DPS_HOST_BUF];
    char dps_assigned_device_id[AZ_IOT_DPS_DEVICE_ID_BUF];
    bool dps_pending_finalize;
    bool dps_pending_have_assignment;
    az_iot_result dps_pending_status;
    bool dps_enrolling; /* CSR-based enrollment active for this DPS session */
    bool dps_have_issued_cert; /* an operational cert was issued by DPS/Hub and stored */

    /* What this connection is to. Seeded from opts.connection_profile at init so
     * a direct connect is always answerable, then overwritten on the DPS path by
     * whatever `connectionProfile` the ASSIGNED payload carried. The raw string
     * is kept verbatim so an unrecognised profile is still reportable. */
    az_iot_connection_profile connection_profile;
    char connection_profile_raw[AZ_IOT_CONNECTION_PROFILE_RAW_BUF];
    bool connection_profile_raw_truncated;
    /* True once connection_profile is authoritative rather than the value
     * seeded at init: immediately for a direct connect, where opts declares it,
     * and when ASSIGNED is applied on the DPS path -- including an ASSIGNED that
     * carries no connectionProfile, since absent resolves to classic. */
    bool connection_profile_resolved;

    /* The generation the attached feature clients require, refcounted by them.
     * Checked against connection_profile the moment that becomes authoritative,
     * so a reassignment to the other generation fails the connection instead of
     * coming up underneath clients built for the old one. */
    az_iot_connection_profile required_profile;
    uint32_t required_profile_refs;

    /* Feature clients whose topics can only be built once the device id is
     * settled; re-run before every connect attempt. */
    struct
    {
      void* owner;
      az_iot_result (*on_bind)(void* owner, struct az_iot_connection_client* client);
    } feature_client_binds[AZ_IOT_MAX_FEATURE_CLIENT_BINDS];

    /* Failed HUB connect attempts since the last success. DPS attempts are not
     * counted: they are what this threshold escalates TO. */
    uint32_t consecutive_hub_connect_failures;

    az_iot_hub_client hub_client;
    bool hub_client_initialized;
    char hub_username[AZ_IOT_MQTT_USERNAME_BUF];

    /* Runtime Hub-side CSR renewal: one in-flight operation, matched by rid. */
    struct
    {
      bool in_use;
      bool subscribed;
      char request_id[64];
      az_iot_csr_callback cb;
      void* user_ctx;
      uint64_t deadline_ms; /* abandon the op if no terminal response by here */
    } csr_op;

    /* SUBACKs still outstanding for the persistent filters issued on this
     * session. CONNECTED is announced once every FAILS_SESSION filter has been
     * granted, so a feature client rebuilt from that callback never publishes a
     * request before the subscription carrying its response exists. FAILS_SELF
     * filters ride the same batch but never hold the transition: their outcome
     * cannot change whether the session is honest about being live. An empty
     * gated set announces immediately. */
    struct
    {
      struct
      {
        uint16_t packet_id;
        uint8_t sub_index; /* into persistent_subs[] */
        bool gated;
      } pending[AZ_IOT_MAX_PERSISTENT_SUBS];
      size_t pending_count;
      size_t gated_outstanding;
      uint64_t deadline_ms;
      bool active;
    } subscription_gate;

    /* AEG/Hub-Next presence (birth) handshake. After CONNACK on a HUB_NEXT (v5)
     * session the client SUBSCRIBEs to ih/{deviceId}/dev/#, PUBLISHes a birth
     * message to ih/{deviceId}/srv/presence, and only announces CONNECTED once
     * it receives a birth-ack -- matched by the exact ih/{deviceId}/dev/presence
     * topic -- whose correlation data matches `nonce`.
     * Classic/DPS sessions leave phase == AZ_IOT_PRESENCE_PHASE_NONE. */
    struct
    {
      int phase;
      bool session_present; /* observed in CONNACK; reported in birth */
      uint16_t sub_packet_id; /* SUBACK correlation for the dev/# sub */
      uint8_t nonce[16]; /* connection nonce echoed by birth-ack */
      uint64_t deadline_ms; /* handshake timeout (monotonic ms) */
    } presence;
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
   *   - Direct hub connect: host, client_id, certificate_provider; also set
   *     connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5 for an IoT Hub
   *     Next / AEG (v5) endpoint (defaults to Classic v3.1.1). */
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
   * (a programming error), so callers routinely fire-and-forget them. The state
   * callback receives an SDK-owned event valid only for the duration of the call. */
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
  az_iot_result az_iot_connection_client_do_work(
      az_iot_connection_client* client,
      uint32_t timeout_ms);

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

  /* Returns the effective IoT Hub address (FQDN) this client is bound to: the
   * host supplied in options for a direct-hub connection, or the DPS-assigned hub
   * once provisioning completes. Returns NULL when no host has been established
   * yet (e.g. a DPS-only client that has not finished provisioning). The returned
   * pointer is owned by the client and stays valid until destroy(). Useful for
   * protocol-independent, HTTPS-only features such as file upload that must reach
   * the hub's REST endpoint directly rather than over the MQTT session. */
  const char* az_iot_connection_client_get_iothub_address(const az_iot_connection_client* client);

  /* Report what this client is connected to, so an application can branch on the
   * hub generation without inferring it from its own configuration.
   *
   * Valid only once the connection has reached CONNECTED; before that it returns
   * AZ_IOT_ERR_NOT_CONNECTED, because on the DPS path the profile is not known
   * until provisioning completes. The one deliberate exception is a connection
   * that failed with AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED: the profile
   * remains readable there precisely so the offending value can be logged or
   * reported.
   *
   * out_profile MUST have been initialized with AZ_IOT_HUB_PROFILE_INIT;
   * an unstamped struct returns AZ_IOT_ERR_INVALID_ARG. */
  AZ_NODISCARD az_iot_result az_iot_connection_client_get_hub_profile(
      const az_iot_connection_client* client,
      az_iot_hub_profile* out_profile);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CONNECTION_CLIENT_H */
