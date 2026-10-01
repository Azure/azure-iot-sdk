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
   *     MQTT_V3 (MQTT v3.1.1); set MQTT_V5 for an MQTTv5 endpoint.
   * Either way az_iot_connection_client_get_hub_profile() reports the result.
   *
   * On the wire `connectionProfile` is a STRING and an extensible union -- the
   * service contract says future hub capabilities pass through without a
   * breaking change. A closed C enum cannot represent that, which is why the
   * verbatim string is carried alongside it. See docs/eng/connection-c.md section 4. */
  typedef enum az_iot_connection_profile
  {
    AZ_IOT_CONNECTION_PROFILE_MQTT_V3 = 0, /* "classic" -- also the absent/null default */
    AZ_IOT_CONNECTION_PROFILE_MQTT_V5 = 1, /* "mqttV5"                                  */
    /* A value newer than this SDK. Only ever produced by the service; passing it
     * to az_iot_connection_client_init() is rejected, since the caller cannot
     * meaningfully declare a profile the SDK does not know how to speak. */
    AZ_IOT_CONNECTION_PROFILE_UNKNOWN = -1
  } az_iot_connection_profile;

  /* Whether the hub session asks the broker to resume the device's previous
   * MQTT session, or to start a fresh one.
   *
   * Zero is DEFAULT so a zero-initialized options struct gets the SDK's
   * per-role choice, which is what almost every caller wants. */
  typedef enum az_iot_session_continuity
  {
    /* Resume where the role's default says to. Today that is: resume on both
     * hub generations, and always clean for DPS (which does not support
     * session persistence at all). */
    AZ_IOT_SESSION_CONTINUITY_DEFAULT = 0,
    /* Ask the broker to resume the previous session (clean_start = false, or
     * Clean Session = 0 on MQTT 3.1.1). */
    AZ_IOT_SESSION_CONTINUITY_RESUME = 1,
    /* Discard any previous session and start a new one (clean_start = true).
     * Everything the broker had queued for this device is dropped. */
    AZ_IOT_SESSION_CONTINUITY_CLEAN = 2
  } az_iot_session_continuity;

  /* Caller-allocated; az_iot_connection_client_get_hub_profile() fills every
   * field. */
  typedef struct az_iot_hub_profile
  {
    az_iot_connection_profile connection_profile;
    /* The effective profile text, never NULL. When DPS supplies a string it is
     * verbatim, which keeps the extensible union from becoming lossy at the C
     * boundary: a profile this SDK has never heard of still reports UNKNOWN
     * *and* the text the service sent. For absent/null it is the resolved
     * contract default ("classic"), or the exact development override value.
     * Points into the connection client and stays valid until deinit().
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

  /* How the client retries a failed connection. Four numbers describe every
   * schedule the SDK can produce; the getters below name the three shapes
   * worth naming, and an application that wants another writes a function of
   * the same shape itself.
   *
   * The retry ladders are PER SCOPE (provisioning and hub, see
   * az_iot_connection_scope), and these numbers apply to each of them
   * independently -- including max_attempts, which is a budget per ladder, not
   * one shared across both. */
  typedef struct az_iot_reconnection_policy
  {
    /* Delay before the first retry, doubled on each subsequent one up to
     * max_delay_ms.
     *
     * 0 DISABLES retrying: every failure becomes terminal. It is a sentinel,
     * not a duration, so a zero-initialized policy has retries off whether or
     * not that was intended -- prefer
     * az_iot_reconnection_policy_get_retry_disabled() to say it deliberately.
     * There is deliberately no way to express "retry immediately, forever":
     * that is a hot loop against a service that is already failing. */
    uint32_t initial_delay_ms;
    /* Ceiling on the BACKOFF -- how far the doubling may climb. Jitter is
     * applied around it, so an individual delay may exceed this by up to
     * jitter_pct; that is the point of jitter, and clamping it back here would
     * put half of all retries on exactly this value. 0 means "same as
     * initial_delay_ms".
     *
     * Setting this EQUAL to initial_delay_ms (or leaving it 0) pins the delay
     * at initial_delay_ms from the first retry, which is a FIXED-INTERVAL
     * schedule -- a supported configuration, not a degenerate one. See
     * az_iot_reconnection_policy_get_fixed_interval(). */
    uint32_t max_delay_ms;
    uint32_t max_attempts; /* 0 = infinite; applies PER ladder */
    uint8_t jitter_pct; /* 0..100, applied around the backoff */
  } az_iot_reconnection_policy;

  /* Exponential backoff with jitter: 1s initial delay, 60s cap, retry forever,
   * +/-20% jitter. What az_iot_connection_client_options_default() installs. */
  az_iot_reconnection_policy az_iot_reconnection_policy_get_default(void);

  /* Never retry: every dropped link, refused CONNACK, stalled handshake and
   * failed registration ends the session rather than being retried.
   *
   * Where the client ends up depends on how the session ended. A peer
   * DISCONNECT is a clean end of session and settles in
   * AZ_IOT_CONN_STATE_IDLE, ready for another
   * az_iot_connection_client_open(). A failure -- a refused CONNACK, a
   * transport error, a stalled handshake, a failed registration -- settles in
   * AZ_IOT_CONN_STATE_FAULTED, which carries the reason and waits for the
   * application to call close() and open again.
   *
   * This is the spelling for "no retries". It is what a zero-initialized
   * policy already means, but saying it through this getter states the intent
   * at the call site rather than leaving it to a zeroed field. */
  az_iot_reconnection_policy az_iot_reconnection_policy_get_retry_disabled(void);

  /* Retry at a constant interval rather than backing off: every
   * @p interval_ms, up to @p max_attempts tries per ladder (0 = forever).
   * Jitter is left at 0 -- add it on the returned struct if a fleet of these
   * devices should not retry in lockstep.
   *
   * @p interval_ms must be non-zero. 0 is the sentinel that DISABLES retrying
   * (see initial_delay_ms), so a zero interval cannot mean "retry with no
   * delay" -- it is clamped to 1 ms rather than silently returning the
   * opposite of what this function's name promises. Use
   * az_iot_reconnection_policy_get_retry_disabled() to disable retrying.
   *
   * For a device on a link that is either up or down, where doubling the delay
   * only delays recovery. */
  az_iot_reconnection_policy az_iot_reconnection_policy_get_fixed_interval(
      uint32_t interval_ms,
      uint32_t max_attempts);

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

/* Bytes the DPS registration body adds around a custom registration payload
 * (dps.registration_payload), on top of the payload itself. An upper bound
 * covering both shapes: the `{"payload":` + `}` wrapper (12) when the payload
 * is alone in the body, and the `,` + `"payload":` members (11) when it shares
 * the body with a CSR whose own braces AZ_IOT_CSR_PAYLOAD_BUFFER_MIN already
 * counts. */
#define AZ_IOT_DPS_REGISTRATION_PAYLOAD_OVERHEAD 12

/* Largest custom registration payload AZ_IOT_DPS_REGISTRATION_BODY_STORAGE()
 * leaves room for. Override before including if your allocation policy needs a
 * bigger one, or size a buffer yourself: the option takes an az_span, so this
 * constant binds only the convenience macro below. */
#ifndef AZ_IOT_DPS_REGISTRATION_PAYLOAD_MAX
#define AZ_IOT_DPS_REGISTRATION_PAYLOAD_MAX 512
#endif

/* Declares a registration-body build buffer named `name`, sized to hold a CSR
 * enrollment body AND a custom registration payload of up to
 * AZ_IOT_DPS_REGISTRATION_PAYLOAD_MAX bytes, for dps.registration_body_buffer:
 *   AZ_IOT_DPS_REGISTRATION_BODY_STORAGE(body_buf);
 *   copts.dps.registration_body_buffer = az_span_create(body_buf, sizeof(body_buf));
 * A device that sends a payload but does NOT request an operational certificate
 * needs only its payload plus AZ_IOT_DPS_REGISTRATION_PAYLOAD_OVERHEAD bytes
 * and can declare a smaller buffer itself. */
#define AZ_IOT_DPS_REGISTRATION_BODY_STORAGE(name)                              \
  uint8_t name                                                                  \
      [AZ_IOT_CSR_PAYLOAD_BUFFER_MIN + AZ_IOT_DPS_REGISTRATION_PAYLOAD_OVERHEAD \
       + AZ_IOT_DPS_REGISTRATION_PAYLOAD_MAX]

  typedef struct az_iot_connection_client_options
  {
    const char* host; /* hub host (or NULL when using DPS) */
    /* Port for the HUB connect. 0 selects the default for `transport`: 8883 for
     * MQTT over TCP, 443 for MQTT over WebSockets.
     *
     * The hub connect only, which is what this field has always meant: the DPS
     * bootstrap connect takes the transport default instead (it used to be a
     * hardcoded 8883). The provisioning gateway is a different host, so aiming
     * a hub port at it would leave the device unable to provision at all, and
     * there is no separate option for the DPS port. */
    uint16_t port;

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
                                                   * no DPS): MQTT_V3 (v3.1.1, default) or
                                                   * MQTT_V5 (MQTTv5). Ignored when using DPS,
                                                   * where it is learned instead. */
    const char* model_id; /* IoT Plug and Play model id announced at
                           * connection (NULL = none). Not used by device
                           * update, which matches a device on the
                           * compatibility properties it sends with each
                           * update request. */
    /**
     * @brief Source of every credential: trust anchors, plus per role (DPS,
     * hub) either an X.509 client identity or a SAS signer. Required.
     *
     * Every DPS and hub connection uses TLS: open() refuses a client without a
     * provider (AZ_IOT_ERR_CREDENTIAL_INCOMPLETE), and a connect attempt whose
     * load() fails fails with the provider's error instead of connecting
     * without TLS. load() selects X.509 or SAS per role through
     * az_iot_certificate_material::kind, so any onboarding credential can pair
     * with any operational one.
     */
    az_iot_certificate_provider* certificate_provider;

    /* How the client retries after a failure. See az_iot_reconnection_policy
     * for the fields and the getters that name the usual shapes.
     *
     * With retrying disabled (initial_delay_ms == 0, which is what a zeroed
     * options struct has) nothing is retried, and where the client settles
     * depends on how the session ended:
     *   - a peer DISCONNECT is a clean end of session, so the client goes to
     *     AZ_IOT_CONN_STATE_IDLE and is ready for another open();
     *   - a failure -- refused CONNACK, transport error, stalled handshake,
     *     failed registration -- goes to AZ_IOT_CONN_STATE_FAULTED, which
     *     carries the reason and waits until the application calls
     *     az_iot_connection_client_close() and opens again.
     *
     * That does NOT discard what the failure established. An identity
     * rejection still records that the device must re-provision, so the next
     * az_iot_connection_client_open() goes to DPS rather than back to the hub
     * that refused it. What disabling retries removes is the SDK acting on
     * its own; the verdict survives for the application's next open().
     *
     * One trigger does depend on retrying being enabled:
     * dps.max_hub_connect_attempts_before_reprovision counts consecutive
     * automatic attempts, and with retries off there is no such run to count.
     *
     * az_iot_connection_client_options_default() fills this with
     * az_iot_reconnection_policy_get_default(). Use
     * az_iot_reconnection_policy_get_retry_disabled() to opt out. */
    az_iot_reconnection_policy reconnection_policy;

    /* Caller-provided scratch buffer used to BUILD the outbound CSR request
     * payload - the DPS registration body when dps.request_operational_certificate
     * is set, and the hub renewal body for az_iot_connection_client_send_csr().
     * The SDK never allocates or declares a payload buffer of its own; provide
     * one here (>= AZ_IOT_CSR_PAYLOAD_BUFFER_MIN to cover the service CSR size
     * limit) when using either CSR feature. Leave AZ_SPAN_EMPTY otherwise.
     *
     * AZ_IOT_CSR_PAYLOAD_BUFFER_MIN covers the CSR body alone. When
     * dps.registration_payload is ALSO set, the registration body carries both
     * members and needs a further
     * AZ_IOT_DPS_REGISTRATION_PAYLOAD_OVERHEAD + payload bytes; give that room
     * either here or, preferably, in dps.registration_body_buffer. A body that
     * does not fit fails the registration with AZ_IOT_ERR_NOT_ENOUGH_SPACE and
     * is never truncated. */
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
                                             * get_csr (version >= 2). The onboarding
                                             * credential may be X.509 or SAS. */

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
       * Under the default policy (1s initial, 60s cap, +/-20% jitter) the delays
       * run 1, 2, 4, 8, 16, 32 then 60s, so attempt N >= 7 falls at roughly
       * 63 + 60*(N-6) seconds: the default 50 is about 45 minutes. Long enough
       * that an ordinary network outage does not send a whole fleet to DPS at
       * once, short enough that a device left behind by a migration recovers
       * without an operator. */
      uint32_t max_hub_connect_attempts_before_reprovision;

      /**
       * @brief This device has no IoT Hub: keep a provisioning session up, never
       * register, never connect to a hub. Settles at DPS:CONNECTED + HUB:IDLE.
       *
       * For devices whose operations all run pre-registration over the
       * provisioning session, which is therefore the connection itself. The
       * session is pumped and re-established as usual under
       * opts.reconnection_policy.
       *
       * Declared, not inferred: a hubless enrollment fails registration with
       * errorCode 401001, and so does a MISCONFIGURED one. Inferring success
       * from it would hide real misconfiguration; retrying it would loop.
       *
       * Requires dps.id_scope and dps.registration_id. open() returns
       * AZ_IOT_ERR_INVALID_ARG if opts.host or
       * dps.request_operational_certificate is also set.
       */
      bool provision_only;

      /* Custom registration payload: caller-supplied JSON sent with the
       * registration request as the `payload` member of the registration body.
       * DPS forwards it to a custom-allocation policy (webhook / Function), and
       * it is also how a device declares an IoT Plug and Play model id at
       * provisioning time:
       *
       *   static const char k_payload[] = "{\"modelId\":\"dtmi:com:example:Thermostat;1\"}";
       *   copts.dps.registration_payload
       *       = az_span_create((uint8_t*)k_payload, (int32_t)(sizeof(k_payload) - 1));
       *
       * Note that opts.model_id is NOT announced here: it feeds the MQTTv3
       * MQTT username only, and injecting it would have to merge with (or
       * silently override) a `modelId` this payload already carries. Put the
       * model id in this payload when provisioning should see it.
       *
       * Zero-copy and never retained: the span must stay valid and unchanged
       * from az_iot_connection_client_open() until provisioning completes,
       * since it is re-read on every registration attempt (including a
       * re-provision). It is copied INTO the body build buffer, so it MUST NOT
       * overlap registration_body_buffer (nor opts.csr_payload_buffer when that
       * is the one being used); open() rejects an overlap with
       * AZ_IOT_ERR_INVALID_ARG rather than letting the build overwrite its own
       * source.
       *
       * The SDK validates it, in az_iot_connection_client_open(), as exactly
       * one well-formed JSON object and nothing else, and rejects anything else
       * with AZ_IOT_ERR_INVALID_ARG rather than emitting a body the service
       * will refuse. The contents are otherwise opaque to the SDK.
       *
       * Leave AZ_SPAN_EMPTY to send no payload, which is what a
       * zero-initialized options struct does. */
      az_span registration_payload;

      /* Caller-provided scratch buffer used to BUILD the registration body when
       * registration_payload is set. The SDK declares no payload buffer of its
       * own.
       *
       * Must hold the whole body: the payload plus
       * AZ_IOT_DPS_REGISTRATION_PAYLOAD_OVERHEAD, plus the CSR body when
       * request_operational_certificate is also set --
       * AZ_IOT_DPS_REGISTRATION_BODY_STORAGE() sizes exactly that. A body that
       * does not fit fails the registration with AZ_IOT_ERR_NOT_ENOUGH_SPACE;
       * the SDK never truncates one.
       *
       * Left AZ_SPAN_EMPTY, the SDK builds into opts.csr_payload_buffer
       * instead, so a CSR-enrolling device that adds a small payload only has
       * to enlarge the buffer it already provides. Unused when
       * registration_payload is empty: the CSR-only body keeps using
       * opts.csr_payload_buffer exactly as before. */
      az_span registration_body_buffer;
    } dps;

    /* How long registration may be held for a pre-registration exchange on the
     * provisioning session, in milliseconds. 0 selects
     * AZ_IOT_DPS_HOLD_TIMEOUT_MS. The hold is advisory and this is its bound:
     * when it expires the device registers regardless, so a feature client can
     * delay provisioning but never prevent it.
     *
     * Appended deliberately: this struct is filled by callers, and inserting a
     * member would shift every one after it for positional aggregate
     * initializers. New options go at the end. */
    uint32_t dps_hold_timeout_ms;

    /* Transport that carries every MQTT session this client opens -- the DPS
     * bootstrap connect as well as the hub connect.
     *
     * AZ_IOT_MQTT_TRANSPORT_WEBSOCKET tunnels MQTT inside WebSockets over 443,
     * for devices on networks that only allow HTTP(S) ports.
     *
     * Appended, like dps_hold_timeout_ms above and for the same reason: this
     * struct is filled by callers, so a member inserted anywhere else would
     * shift every one after it for positional aggregate initializers. */
    az_iot_mqtt_transport transport;

    /* WebSocket resource path; used only when transport is WEBSOCKET. NULL
     * selects AZ_IOT_MQTT_DEFAULT_WEBSOCKET_PATH ("/$iothub/websocket"), which
     * is what IoT Hub and DPS expect; set it only for a gateway that terminates
     * WebSockets elsewhere. */
    const char* websocket_path;

    /* HTTP proxy for every MQTT session this client opens, via HTTP CONNECT.
     * Leave zeroed for a direct connection. Works with both transports, since a
     * filtered network usually requires the tunnel for plain MQTT too.
     *
     * TLS remains end-to-end with the broker: it is negotiated inside the
     * tunnel, so the proxy carries only ciphertext and certificate and hostname
     * validation are unaffected.
     *
     * Adapters that cannot honour it fail the connect with
     * AZ_IOT_ERR_NOT_SUPPORTED rather than connecting around the proxy.
     *
     * Note for the Paho adapter: when this is left unset, Paho itself still
     * falls back to the lowercase `http_proxy` / `https_proxy` environment
     * variables (the uppercase spellings are ignored). Set the proxy here to be
     * explicit and independent of the environment. */
    az_iot_mqtt_proxy_options proxy;

    /* MQTTv5 hub twin push advertisement. These two bits ride the birth
     * message on every connection and tell the service which twin traffic this
     * device wants dispatched to it. They reflect the application's
     * configuration at init and MUST stay constant for the lifetime of the
     * client: changing push mode at runtime would desynchronize the device's
     * expectation from the service's most recently recorded decision.
     *
     * Both default to false (pull-only). With push_desired, the service pushes
     * the desired snapshot on connect instead of the mqttv5 twin client fetching
     * it. With push_reported, a pushed reported section reaches
     * az_iot_mqttv5_twin_client_set_reported_handler(); without a handler it is
     * dropped. Ignored for MQTTv3 hubs and for DPS sessions.
     *
     * Appended, like the options above it and for the same reason: this struct
     * is filled by callers, so a member inserted anywhere else would shift
     * every one after it for positional aggregate initializers. */
    struct
    {
      bool push_desired; /* request push of the desired payload on connect,
                          * plus incremental desired patches while connected */
      bool push_reported; /* request one-shot rehydration of the reported
                           * payload on connect (for volatile devices) */
    } twin_push;

    /* Last Will and Testament for the HUB session, announced in CONNECT and
     * published by the broker if the device disappears without an orderly
     * close. Leave zeroed (topic NULL) for no Will, which is the default.
     *
     * Scope, deliberately:
     *  - It is applied to the hub session only -- MQTTv3 and MQTTv5 alike. The
     *    DPS session never carries it: provisioning is a short exchange that is
     *    fully torn down before the hub session exists, and a Will published
     *    from it would announce a departure that never happened.
     *  - `will_delay_seconds` is MQTT 5 only (ignored on an MQTTv3 hub, which
     *    speaks v3.1.1). On an MQTTv5 session the SDK also raises the session
     *    expiry to cover the delay, because MQTT 5 ends the delay at whichever
     *    comes first -- a delay longer than the session expiry is silently no
     *    delay at all.
     *  - On an MQTTv5 session, configuring a Will also makes the SDK close with
     *    DISCONNECT reason 0x04 (Disconnect with Will Message) so an orderly
     *    close announces the departure too, instead of discarding the Will.
     *
     * The SDK sets NO default Will: the topic and payload are an application
     * (or protocol) decision, and this client does not have one to make. See
     * docs/connecting.md.
     *
     * The buffers are borrowed, like every other pointer on this struct, and
     * must outlive the client.
     *
     * Appended, like the options above it and for the same reason: this struct
     * is filled by callers, so a member inserted anywhere else would shift
     * every one after it for positional aggregate initializers. */
    struct
    {
      const char* topic; /* NULL = no Will */
      const uint8_t* payload;
      size_t payload_len;
      az_iot_mqtt_qos qos;
      bool retain;
      uint32_t will_delay_seconds; /* MQTT 5 only; 0 = publish immediately */
    } lwt;

    /* MQTT session continuity for the HUB session.
     *
     * The session is broker-side state -- the device's subscriptions, the QoS 1
     * traffic queued for it while it was away, the in-flight quota -- and it is
     * a TRANSPORT EFFICIENCY choice, never a correctness one. Every feature
     * client in this SDK is correct if each connect started a fresh session;
     * resuming one only saves a re-subscribe and preserves messages the broker
     * would otherwise have to have redelivered from the service.
     *
     * Leave at the default unless there is a reason not to. A device on a flaky
     * link benefits from resuming; a device that has been away long enough to
     * accumulate a large queue may be better off starting clean than replaying
     * a backlog. Ignored for DPS, which does not support session persistence
     * and is always connected clean. */
    az_iot_session_continuity session_continuity;

    /* Session Expiry Interval requested on an MQTT v5 hub session, in seconds.
     * 0 selects AZ_IOT_DEFAULT_SESSION_EXPIRY_SECONDS.
     *
     * How long the broker keeps the session after the connection drops. It is
     * only meaningful alongside a resumed session: a session asked to expire
     * immediately is gone before any reconnect can resume it.
     *
     * MQTT 5 only, so it has no effect on an MQTTv3 hub. The Event Grid
     * namespace clamps this to its configured maximum (8 hours), and a
     * disconnected session is also bounded by its queue (100 messages / 1 MB) --
     * overflowing that destroys the session regardless of this value. */
    uint32_t session_expiry_seconds;

    /**
     * @brief Lifetime of each SAS token the SDK signs, in seconds. 0 selects
     * AZ_IOT_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS. Ignored for X.509 roles.
     *
     * The hub drops a session whose token expires, so the SDK reconnects the
     * hub session with a fresh token at 80% of this lifetime. The reconnect is
     * reported like any other (HUB: RECONNECTING, then CONNECTED) and resumes
     * the session per session_continuity. DPS tokens are signed per attempt.
     *
     * Applies to MQTTv3 and MQTTv5 hubs alike. Whether a hub accepts SAS is the
     * service's decision; a refusal fails with AZ_IOT_ERR_IDENTITY_REJECTED.
     */
    uint32_t sas_token_lifetime_seconds;
  } az_iot_connection_client_options;

  /* Which of the client's two lifecycles something refers to.
   *
   * A DPS-provisioned device runs two connections in sequence, and sometimes
   * side by side: the provisioning session, and the hub session it is assigned
   * to. They fail, retry and settle independently, so anything scoped to one
   * of them -- today the retry ladders below -- has to say which.
   *
   * DPS and HUB only. Which hub GENERATION a hub session speaks (MQTTv3 or
   * MQTT v5) is reported through az_iot_hub_profile, not here: it is one
   * logical connection either way, and splitting the scope by generation would
   * make a caller handle two values for it. */
  typedef enum az_iot_connection_scope
  {
    AZ_IOT_CONN_SCOPE_DPS = 0,
    AZ_IOT_CONN_SCOPE_HUB = 1
  } az_iot_connection_scope;

#define AZ_IOT_CONN_SCOPE_COUNT 2

  typedef enum az_iot_connection_state
  {
    AZ_IOT_CONN_STATE_IDLE = 0,
    AZ_IOT_CONN_STATE_CONNECTING,
    AZ_IOT_CONN_STATE_CONNECTED,
    AZ_IOT_CONN_STATE_RECONNECTING,
    AZ_IOT_CONN_STATE_DISCONNECTING,
    /* The connection gave up: either no reconnection policy is configured, or
     * its attempts were exhausted, or the failure is one a retry cannot fix
     * (AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH / _UNSUPPORTED).
     *
     * FAULTED is settled, not a dead end. The SDK never leaves it on its own --
     * do_work() does not retry from here -- but it is recoverable:
     * az_iot_connection_client_close() is legal from FAULTED and returns the
     * client to IDLE, from which az_iot_connection_client_open() starts a fresh
     * attempt. Attached feature clients keep working across that; they only
     * have to be rebuilt when the reason says the hub generation changed.
     *
     * The application decides whether and when to retry, which is the point of
     * the state: an unattended device can back off, ask for new credentials or
     * report the fault before trying again, instead of the SDK looping on a
     * failure it has already been told not to retry. */
    AZ_IOT_CONN_STATE_FAULTED
  } az_iot_connection_state;

  /* SDK-produced, callback-lifetime view of a connection-state transition.
   *
   * `scope` says WHICH connection the event is about, and `state` is
   * meaningless without it: a device that provisions through DPS runs two
   * independent lifecycles, and `CONNECTED` on the provisioning scope does not
   * mean the hub is usable. Always read the pair.
   *
   * profile is non-NULL on HUB:CONNECTED, and on a failure whose reason is
   * AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH or _UNSUPPORTED -- an application
   * needs the assigned generation there to rebuild its feature clients.
   *
   * NULL on DPS:CONNECTED: the generation comes from the assignment, which does
   * not exist at the SUBACK and never will on a provision_only device.
   *
   * It and the event itself are valid only until the callback returns; copy any
   * value that must be retained. */
/** @brief Bound on the service error text retained for an event. Longer
 * messages are truncated; a shortened diagnostic still names the cause. */
#define AZ_IOT_CONN_ERROR_MESSAGE_MAX 128

  /**
   * @brief Which codebook decodes az_iot_connection_error_detail.code.
   *
   * Names the DICTIONARY, not the connection -- which connection an event is
   * about is az_iot_connection_state_event.scope. A hub CONNACK and a DPS
   * CONNACK are both _MQTT; only the registration verdict is _DPS.
   */
  typedef enum az_iot_connection_error_source
  {
    AZ_IOT_CONN_ERR_SRC_NONE = 0,
    /** @brief Below MQTT: TLS, socket, DNS. `code` is adapter-defined. */
    AZ_IOT_CONN_ERR_SRC_TRANSPORT,
    /** @brief A code off the wire: CONNACK, SUBACK, or a server-sent MQTT 5
     * DISCONNECT reason. */
    AZ_IOT_CONN_ERR_SRC_MQTT,
    /** @brief The provisioning service's own verdict. `code` is
     * `extended_error_code` (e.g. 401001). */
    AZ_IOT_CONN_ERR_SRC_DPS
  } az_iot_connection_error_source;

  /**
   * @brief Diagnostic detail for a failure. Never branch on it: `reason` is the
   * classification the SDK acts on, this is the evidence behind it.
   */
  typedef struct az_iot_connection_error_detail
  {
    /** @brief What `code` means. _NONE when no code was available. */
    az_iot_connection_error_source source;
    /** @brief The code itself. 0 means "none supplied" and is ambiguous. */
    int32_t code;
    /** @brief Service-supplied text, empty when there is none. DPS supplies it;
     * MQTT does not. Callback lifetime -- copy to retain. */
    az_span message;
  } az_iot_connection_error_detail;

  typedef struct az_iot_connection_state_event
  {
    /* Which lifecycle this event is about. Placed beside `state` because the
     * two are only meaningful together. */
    az_iot_connection_scope scope;
    az_iot_connection_state state;
    az_iot_result reason;
    const az_iot_hub_profile* profile;
    /* Whether another attempt at this CAUSE could plausibly succeed. Says
     * nothing about whether the SDK will try: with retries disabled the
     * application owns the ladder, and this is what tells it a retry is worth
     * making. A best-effort classification -- `reason`, `error->source` and
     * `error->code` remain the authority for an application that wants to
     * decide for itself. */
    bool is_retriable;
    /* Diagnostic detail, or NULL when none is available. Valid only until the
     * callback returns. */
    const az_iot_connection_error_detail* error;
  } az_iot_connection_state_event;

  typedef void (*az_iot_connection_state_callback)(
      const az_iot_connection_state_event* event,
      void* user_ctx);

  typedef void (*az_iot_publish_ack_callback)(az_iot_result status, void* user_ctx);

  /* Notified when an MQTT session ends -- peer disconnect, transport error, or a
   * user close -- so a feature client can complete whatever it had correlated
   * against that session instead of waiting forever for a response that can no
   * longer arrive. Registered through the internal header; applications use
   * az_iot_connection_client_add_state_observer() instead.
   *
   * Deliberately NOT invoked from deinit(), for the same reason pending QoS-1
   * acknowledgements are not completed there: the application is tearing the
   * client down and the context the callback closes over may already be gone. */
  typedef void (*az_iot_session_end_callback)(void* user_ctx);

  /* ---- Runtime Hub-side certificate renewal (MQTTv3 hub) ------------------ */
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

  /* Fired when a DPS registration completes and the assignment carries a custom
   * payload -- `registrationState.payload`, the counterpart of
   * opts.dps.registration_payload and what a custom-allocation policy returns
   * to the device. Optional; not fired when the service sends no payload.
   *
   * `payload` is the verbatim JSON object, zero-copy: it points into the
   * inbound MQTT message, which is reused as soon as the callback returns.
   * Copy anything that must outlive the call. */
  typedef void (*az_iot_registration_payload_callback)(az_span payload, void* user_ctx);

  /* What a refused persistent subscription costs. Both values name a FAILURE:
   * the difference is blast radius, not whether the subscription mattered.
   * See docs/eng/connection-c.md section 9.3. */
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
 * a fully loaded device asks for: five on MQTTv3 (C2D, direct methods, twin
 * response, twin desired, certificate renewal), none on MQTTv5. Registering
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

/* Connection-state observers. Two pools, because the two kinds of subscriber
 * must not be able to starve each other: an application that registers four
 * observers must still leave every feature client able to attach, and a build
 * with every feature client attached must still leave the application able to
 * watch its own connection.
 *
 * The feature-client pool is sized for the clients that exist (telemetry, c2d,
 * direct method, twin, file upload, su). Raise either at compile time. */
#ifndef AZ_IOT_MAX_FEATURE_STATE_OBSERVERS
#define AZ_IOT_MAX_FEATURE_STATE_OBSERVERS 6
#endif
#ifndef AZ_IOT_MAX_APP_STATE_OBSERVERS
#define AZ_IOT_MAX_APP_STATE_OBSERVERS 4
#endif

/* Defaults applied when the corresponding option is left at 0. */
#ifndef AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS
#define AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS 30
#endif
#ifndef AZ_IOT_DEFAULT_CONNECT_TIMEOUT_SECONDS
#define AZ_IOT_DEFAULT_CONNECT_TIMEOUT_SECONDS 30
#endif
/* Session Expiry Interval requested on an MQTT v5 hub session, in seconds.
 *
 * The session is what carries a device's subscriptions and the QoS 1 traffic
 * queued for it across a transient drop, and an expiry of 0 ends it the moment
 * the network connection closes -- so a resumable session needs a non-zero
 * value here as much as it needs clean_start = false.
 *
 * One hour matches the Event Grid namespace default. The namespace maximum is
 * eight hours and the broker clamps anything larger, so this is always
 * accepted. It is deliberately not longer: a disconnected session also has a
 * bounded queue (100 messages / 1 MB), and a device that accumulates past that
 * loses the whole session anyway, so a very long expiry buys little while
 * holding broker state for every absent device. */
#ifndef AZ_IOT_DEFAULT_SESSION_EXPIRY_SECONDS
#define AZ_IOT_DEFAULT_SESSION_EXPIRY_SECONDS 3600
#endif
/** @brief Default SAS token lifetime, in seconds (opts.sas_token_lifetime_seconds). */
#ifndef AZ_IOT_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS
#define AZ_IOT_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS 3600
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
 * MQTTv5 birth handshake. */
#ifndef AZ_IOT_PRESENCE_TOPIC_BUF
#define AZ_IOT_PRESENCE_TOPIC_BUF 256
#endif
/* How long to wait for the SUBACK + birth-ack that complete the MQTTv5
 * presence handshake before abandoning the attempt (mirrors the .NET SDK's
 * 60s defensive birth-ack timeout). */
#ifndef AZ_IOT_PRESENCE_BIRTH_ACK_TIMEOUT_MS
#define AZ_IOT_PRESENCE_BIRTH_ACK_TIMEOUT_MS 60000u
#endif

/* How long registration may be held for a pre-registration exchange on the
 * provisioning session before it proceeds anyway. The hold is advisory: a
 * feature client that stalls, or one whose service call never answers, must
 * not leave the device unable to provision. */
#ifndef AZ_IOT_DPS_HOLD_TIMEOUT_MS
#define AZ_IOT_DPS_HOLD_TIMEOUT_MS 60000u
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
    AZ_IOT_DPS_PHASE_DONE,
    /* Between SUBSCRIBING and REGISTERING: the session is usable and
     * registration is deliberately held so a feature client can run a
     * pre-registration exchange on it. Appended rather than inserted in flow
     * order so the existing phase values do not shift. */
    AZ_IOT_DPS_PHASE_HOLD
  };
  /* MQTTv5 presence (birth) handshake phases. MQTTv3/DPS sessions never
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
    AZ_IOT_MQTT_ROLE_HUB_MQTT_V3 = 1, /* requires MQTT v3.1.1 */
    AZ_IOT_MQTT_ROLE_HUB_MQTT_V5 = 2 /* requires MQTT v5     */
  } az_iot_mqtt_role;

  /* Inbound provisioning-session messages that the provisioning flow does not
   * claim are offered to this observer. Returning true means it consumed the
   * message. Declared here because the client struct stores one; it is set
   * through an internal entry point and is not application-facing. */
  typedef bool (*az_iot_dps_message_observer)(
      const char* topic,
      const uint8_t* payload,
      size_t payload_len,
      void* user_ctx);

  struct az_iot_connection_client
  {
    az_iot_connection_client_options opts;

    az_iot_mqtt_factory factories[AZ_IOT_MAX_MQTT_FACTORIES];
    size_t factory_count;

    az_iot_mqtt_role session_role;
    az_iot_mqtt_client* active_client;

    /* One lifecycle per scope, indexed by az_iot_connection_scope. They move
     * independently: a provisioning session failing must not disturb a healthy
     * hub session, and a hub drop must not invalidate a provisioning session a
     * feature client is using. */
    az_iot_connection_state state[AZ_IOT_CONN_SCOPE_COUNT];
    /* Connection-state observers, dispatched feature clients first (see
     * az_iot_connection_client_add_state_observer). Two pools so neither kind
     * of subscriber can starve the other. */
    struct
    {
      az_iot_connection_state_callback cb;
      void* user_ctx;
    } feature_state_observers[AZ_IOT_MAX_FEATURE_STATE_OBSERVERS];
    struct
    {
      az_iot_connection_state_callback cb;
      void* user_ctx;
    } app_state_observers[AZ_IOT_MAX_APP_STATE_OBSERVERS];
    /* Set while a transition is being dispatched. Registering an observer from
     * inside a callback would hand it the transition in flight, so adding is
     * refused with AZ_IOT_ERR_BUSY while this is set. Removing is allowed: the
     * walk re-reads each slot and skips a NULL callback, and an owner being
     * torn down must be able to withdraw. */
    bool dispatching_state;
    az_iot_operational_cert_callback op_cert_cb;
    void* op_cert_cb_ctx;
    az_iot_registration_payload_callback reg_payload_cb;
    void* reg_payload_cb_ctx;

    bool user_close;

    /* Set when the next reconnect attempt must re-provision through DPS rather
     * than reconnect to the cached assignment -- because the hub refused this
     * identity, or because hub attempts crossed the configured threshold. Kept
     * beside user_close so it lands in the padding that already precedes
     * `deferred` rather than adding its own. */
    bool needs_reprovision;

    int deferred;
    az_iot_result deferred_reason;

    /* Retry ladder position, PER SCOPE. Two ladders, not one: provisioning and
     * hub connection fail for unrelated reasons, and a device that exhausts
     * one must not inherit the other's backoff or spend the other's budget.
     *
     * With a single counter a device that burned its hub attempts up to the
     * 60s cap and then re-provisioned made its DPS retries at the cap instead
     * of at initial_delay_ms, and reconnection_policy.max_attempts was one
     * budget shared across both -- so a long hub outage could leave zero
     * attempts for a registration that would have succeeded first try.
     *
     * max_attempts is therefore applied per ladder as well. Indexed by
     * az_iot_connection_scope. */
    uint32_t retry_attempt[AZ_IOT_CONN_SCOPE_COUNT];
    /* Only one retry is ever pending, so a single deadline serves both ladders.
     * Which ladder it belongs to is not stored: do_work() derives it from
     * needs_reprovision at the moment it acts, the same way schedule_reconnect()
     * derived it when it set the deadline. Keeping a copy would be a second
     * source of truth that nothing reads and a later change could desync. */
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
      /* The feature client that registered this filter, so its deinit() can
       * withdraw exactly its own entries without rebuilding the strings. */
      const void* owner;
      /* The generation this filter was built for. A reconnect that resolves a
       * different profile drops it instead of re-issuing a filter the new hub
       * will not recognise -- see docs/eng/connection-c.md section 5.3. */
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

    /* Observer for provisioning-session messages the provisioning flow itself
     * does not claim -- the device-update operations share this session. Stored
     * as a function pointer, not erased through void*: ISO C does not guarantee
     * that function and object pointers share a representation. */
    az_iot_dps_message_observer dps_message_observer;
    void* dps_message_observer_ctx;

    /* Set when the provisioning subscription is SUBACKed. The phase alone is
     * not enough: SUBSCRIBING is entered when the SUBSCRIBE is sent, so a
     * publish made on the phase could race ahead of the response route. */
    bool dps_subscription_confirmed;

    /* Pre-registration hold. While a holder is registered, registration waits
     * at AZ_IOT_DPS_PHASE_HOLD so a feature client can use the provisioning
     * session first. The deadline is what guarantees a feature client can
     * never stop the device from provisioning. */
    uint8_t dps_hold_count;
    bool dps_hold_active;
    uint64_t dps_hold_deadline_ms;

    /* Who needs the provisioning session. It exists exactly while this total is
     * non-zero: the connection client holds dps_registration_ref while the
     * device must register, and each session user holds a count.
     *
     * Registration is a task performed on the session, not a property of it --
     * it runs only while dps_registration_ref is held, which is what keeps a
     * session opened for its other users from registering and tearing down the
     * hub connection beside it. */
    bool dps_registration_ref;
    uint8_t dps_user_count;

    /* SUBACK seen; DPS:CONNECTED still to be announced. Announcing from the
     * adapter callback would let an observer's close() free that adapter
     * mid-dispatch, so the pump does it. */
    bool dps_pending_ready_announce;

    /* The client's own demand for a provisioning session, held for the whole
     * open()..close() life of a provision_only client. Distinct from
     * dps_registration_ref (a task) and dps_user_count (feature clients): it
     * says the session IS the connection, so the pump must neither collect it
     * nor leave it down. */
    bool dps_standing_ref;

    /* Pacing for re-opening a provisioning session that was held for its USERS
     * rather than for a registration.
     *
     * Such a session is not put through schedule_reconnect(): that begins with
     * teardown_active(), so a side channel dropping would destroy a healthy hub
     * connection. It settles the DPS scope at IDLE instead. But IDLE is exactly
     * the state dps_session_ensure() lets through, so without a deadline the
     * next pump tick opened another session -- a full TLS handshake, CONNECT
     * and SUBSCRIBE per tick, against a service that is already failing.
     *
     * A THIRD ladder, deliberately separate from retry_attempt[DPS]. That one
     * is the registration ladder; spending it on a user session's outage would
     * leave a later re-provisioning with no budget, which is the same argument
     * that made max_attempts per-ladder in the first place. It is internal, so
     * it adds no public vocabulary: the pacing, the jitter and the bound all
     * come from opts.reconnection_policy.
     *
     * dps_user_retry_due_ms is a consumed token, like reconnect_due_ms: firing
     * clears it. dps_user_retry_blocked latches when the policy disables
     * retries (initial_delay_ms == 0, where the computed delay would be 0 and
     * pace nothing) or when max_attempts is spent; it is cleared by a session
     * that comes up, by a successful registration, by open()/close(), and by
     * the last user releasing its ref -- a new holder is new demand. */
    uint32_t dps_user_retry_attempt;
    uint64_t dps_user_retry_due_ms;
    bool dps_user_retry_blocked;

    /* Set by dps_start() when an observer closed the client from inside the
     * synchronous DPS:CONNECTING announcement. That cancellation returns the
     * same result code as a genuine start failure, and the two need opposite
     * treatment: a failure should be paced, a close is the caller's documented
     * escape and has already reset the pacing. Cleared at the top of every
     * dps_start(). */
    bool dps_start_cancelled;

    char dps_operation_id[AZ_IOT_DPS_OPERATION_ID_MAX];
    size_t dps_operation_id_len;
    uint64_t dps_poll_due_ms;
    char dps_assigned_hub[AZ_IOT_DPS_HOST_BUF];
    char dps_assigned_device_id[AZ_IOT_DPS_DEVICE_ID_BUF];
    bool dps_pending_finalize;
    bool dps_pending_have_assignment;
    az_iot_result dps_pending_status;
    /* retry-after the provisioning service put on a FAILED response, in
     * seconds; 0 when it sent none. A throttle (429) or a server error carries
     * it, and it is the service telling the device when to come back -- so it
     * is a FLOOR on the next registration attempt, applied over the
     * reconnection policy's own backoff. Ignoring it would let a device retry
     * faster than the service asked, which is how a throttled fleet turns into
     * a blocked one. */
    uint32_t dps_pending_retry_after_secs;

    /* Diagnostic detail for the failure currently being reported on
     * `error_scope`.
     *
     * Staged rather than passed to set_state_to(): a failure is recorded in an
     * adapter callback and reported later from the pump (the `deferred` /
     * `dps_pending_finalize` queues), so the two are not the same call.
     *
     * NOT consumed by the first transition that carries it. One failure
     * produces a SEQUENCE -- DISCONNECTING, IDLE, then RECONNECTING or
     * FAULTED -- all reporting the same thing, so it rides every one of them.
     * Consuming it on the first left the terminal event, the one applications
     * act on, with nothing. It is discarded instead when the scope next
     * reaches CONNECTING or CONNECTED, which is when it stops describing
     * anything current. Scoped, so a DPS verdict cannot attach to a hub event
     * that runs in between.
     *
     * The message is COPIED, not referenced. It arrives as a span into the
     * adapter's inbound buffer, and that buffer is reused or freed as soon as
     * the adapter's callback returns -- BEFORE the pump dispatches the
     * transition that reports it. The public event still promises only
     * callback lifetime; this buffer is what makes that promise keepable.
     * Truncated rather than grown: a shortened diagnostic still names the
     * cause. */
    az_iot_connection_error_source error_source;
    az_iot_connection_scope error_scope;
    int32_t error_code;
    char error_message[AZ_IOT_CONN_ERROR_MESSAGE_MAX];
    size_t error_message_len;
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
     * carries no connectionProfile, since absent resolves to "classic". */
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

    /* MQTTv5 presence (birth) handshake. After CONNACK on a HUB_MQTT_V5 (v5)
     * session the client SUBSCRIBEs to ih/{deviceId}/dev/#, PUBLISHes a birth
     * message to ih/{deviceId}/srv/presence, and only announces CONNECTED once
     * it receives a birth-ack -- matched by the exact ih/{deviceId}/dev/presence
     * topic -- whose correlation data matches `nonce`.
     * MQTTv3/DPS sessions leave phase == AZ_IOT_PRESENCE_PHASE_NONE. */
    struct
    {
      int phase;
      bool session_present; /* observed in CONNACK; reported in birth */
      uint16_t sub_packet_id; /* SUBACK correlation for the dev/# sub */
      uint8_t nonce[16]; /* connection nonce echoed by birth-ack */
      uint64_t deadline_ms; /* handshake timeout (monotonic ms) */
      uint64_t desired_version; /* authoritative twin versions carried by the */
      uint64_t reported_version; /* last birth-ack (0 when the service omits them) */
    } presence;
  };

  typedef struct az_iot_connection_client az_iot_connection_client;

  /* ------------------------------------------------------------------------- */
  /* Public API                                                                */
  /* ------------------------------------------------------------------------- */

  const char* az_iot_connection_state_to_string(az_iot_connection_state s);

  /* Returns an options struct with optional fields defaulted (port derived from
   * the transport -- 8883 for TCP, 443 for WebSockets -- no proxy,
   * and the default reconnection policy from
   * az_iot_reconnection_policy_get_default(): 1s initial delay, 60s cap, retry
   * forever, +/-20% jitter). Set reconnection_policy.initial_delay_ms = 0 on
   * the returned struct to make every failure terminal instead.
   *
   * Note that a zero-initialized options struct is NOT the same thing: it has
   * reconnection disabled, since initial_delay_ms is then 0.
   *
   * Set the required fields for your auth/provisioning
   * mode on the returned struct before az_iot_connection_client_init():
   *   - DPS (host==NULL): dps.id_scope, dps.registration_id,
   *     certificate_provider. The provider chooses X.509 or SAS per role.
   *   - Direct hub connect: host, client_id, certificate_provider; also set
   *     connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5 for an IoT Hub
   *     MQTTv5 (v5) endpoint (defaults to MQTTv3 v3.1.1). */
  AZ_NODISCARD az_iot_connection_client_options az_iot_connection_client_options_default(void);

  AZ_NODISCARD az_iot_result az_iot_connection_client_init(
      az_iot_connection_client* client,
      const az_iot_connection_client_options* opts);

  void az_iot_connection_client_deinit(az_iot_connection_client* client);

  /* Register an MQTT factory in the client's adapter registry. The client may hold
   * multiple factories; at session-open time it picks the one whose
   * (version, supported_roles_mask) matches the required (version, role) for that
   * session. Adapters for DPS+MQTTv3 must be v3.1.1; adapters for MQTTv5 must be v5. */
  AZ_NODISCARD az_iot_result az_iot_connection_client_register_mqtt_factory(
      az_iot_connection_client* client,
      const az_iot_mqtt_factory* factory);

  /* Ask to be told about every connection-state transition.
   *
   * A REGISTRY, not a single slot: the application and every attached feature
   * client can watch the connection at the same time. Registering twice with
   * the same (cb, user_ctx) pair is idempotent and consumes one entry.
   *
   * Observers are dispatched in two passes -- every feature-client observer
   * first, in registration order, then every application observer. So by the
   * time an application observer runs, the feature clients have already reacted
   * to the transition (re-subscribed, re-armed, or given up), and the
   * application sees a connection whose parts agree with each other.
   *
   * The event is SDK-owned and valid only for the duration of the call; copy
   * anything that must outlive it.
   *
   * An observer MUST NOT ADD an observer: the registry is being walked, and a
   * subscriber added mid-pass would be handed a transition it was not watching
   * for, so it answers AZ_IOT_ERR_BUSY while a dispatch is in progress.
   * REMOVING from inside an observer IS supported -- a feature client torn down
   * in reaction to a transition has to be able to give its seat back. Calling
   * close() from an observer IS supported.
   *
   * Returns AZ_IOT_ERR_NOT_ENOUGH_SPACE when the application pool
   * (AZ_IOT_MAX_APP_STATE_OBSERVERS) is full. */
  az_iot_result az_iot_connection_client_add_state_observer(
      az_iot_connection_client* client,
      az_iot_connection_state_callback cb,
      void* user_ctx);

  /* Stop being told. Matches on the (cb, user_ctx) pair, so one callback
   * registered with two contexts can be withdrawn one at a time. Answers
   * AZ_IOT_ERR_NOT_FOUND when that pair is not registered.
   *
   * Legal from inside an observer, and that case is the reason it must be: an
   * owner destroyed in reaction to a transition releases the storage the entry
   * points at, so it has to be able to withdraw before it returns. */
  az_iot_result az_iot_connection_client_remove_state_observer(
      az_iot_connection_client* client,
      az_iot_connection_state_callback cb,
      void* user_ctx);

  /* The current state of one lifecycle. There is no unscoped state to ask for:
   * a DPS-provisioned device runs two, and they move independently.
   *
   * AZ_IOT_CONN_SCOPE_HUB is what "am I connected?" means for telemetry, twin,
   * c2d, direct methods and file upload. AZ_IOT_CONN_SCOPE_DPS is what the
   * device-update client rides, and it stays IDLE for the life of a client that
   * connects directly to a hub -- that is the answer, not an error.
   *
   * Returns AZ_IOT_CONN_STATE_IDLE for a NULL client or an unknown scope. */
  az_iot_connection_state az_iot_connection_client_get_state(
      const az_iot_connection_client* client,
      az_iot_connection_scope scope);

  /* Register a callback fired when a DPS/provider-issued operational certificate
   * is obtained during provisioning (D4). Optional. */
  az_iot_result az_iot_connection_client_set_operational_cert_callback(
      az_iot_connection_client* client,
      az_iot_operational_cert_callback cb,
      void* user_ctx);

  /* Register a callback fired when a DPS assignment carries a custom
   * registration payload (registrationState.payload). Optional. The payload
   * span handed to the callback is valid only for the duration of the call. */
  az_iot_result az_iot_connection_client_set_registration_payload_callback(
      az_iot_connection_client* client,
      az_iot_registration_payload_callback cb,
      void* user_ctx);

  /* Open a session to the configured host. Non-blocking; observe state via callback
   * and drive progress with do_work().
   *
   * Legal only from AZ_IOT_CONN_STATE_IDLE; any other state returns
   * AZ_IOT_ERR_ALREADY_INITIALIZED. After a fault, call
   * az_iot_connection_client_close() first: that returns the client to IDLE and
   * makes this a supported retry. */
  AZ_NODISCARD az_iot_result az_iot_connection_client_open(az_iot_connection_client* client);

  /* Close the session and return the client to AZ_IOT_CONN_STATE_IDLE.
   *
   * Legal from every state. It is idempotent from IDLE, cancels a pending retry
   * from RECONNECTING, cancels a provisioning exchange that has not reached a
   * hub yet, and acknowledges a fault from FAULTED -- in all of those IDLE is
   * reached before this call returns. From a state with a live hub session the
   * disconnect is asynchronous: IDLE is announced on the state callback once
   * the transport reports the session gone, so keep calling do_work().
   *
   * The client's configuration and its attached feature clients survive, so
   * close() + open() is the ordinary way to retry after a fault; deinit() is
   * only needed when the client itself is going away.
   *
   * Not AZ_NODISCARD: teardown/lifecycle op commonly called fire-and-forget. */
  az_iot_result az_iot_connection_client_close(az_iot_connection_client* client);

  /* Pump network I/O and dispatch callbacks. Single-threaded contract: all user
   * callbacks fire synchronously from inside this call. Not AZ_NODISCARD: this is a
   * pump, commonly called in a loop where the per-call result is observed via the
   * state callback rather than the return value. */
  az_iot_result az_iot_connection_client_do_work(
      az_iot_connection_client* client,
      uint32_t timeout_ms);

  /* Request a renewed operational certificate from the connected (MQTTv3) hub by
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
   * pointer is owned by the client and stays valid until deinit(). Useful for
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
   * reported. */
  AZ_NODISCARD az_iot_result az_iot_connection_client_get_hub_profile(
      const az_iot_connection_client* client,
      az_iot_hub_profile* out_profile);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_CONNECTION_CLIENT_H */
