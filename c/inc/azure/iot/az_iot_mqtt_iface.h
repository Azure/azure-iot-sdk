// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#ifndef AZ_IOT_MQTT_IFACE_H
#define AZ_IOT_MQTT_IFACE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* MQTT protocol versions supported by adapters. DPS + MQTTv3 hub require v3.1.1;
   * MQTTv5 hub requires v5. A single adapter binary may register factories for both,
   * but each instance speaks exactly one version. */
  typedef enum az_iot_mqtt_version
  {
    AZ_IOT_MQTT_VERSION_3_1_1 = 0,
    AZ_IOT_MQTT_VERSION_5 = 1
  } az_iot_mqtt_version;

  typedef enum az_iot_mqtt_qos
  {
    AZ_IOT_MQTT_QOS_0 = 0,
    AZ_IOT_MQTT_QOS_1 = 1,
    AZ_IOT_MQTT_QOS_2 = 2
  } az_iot_mqtt_qos;

  /* MQTT v5 User Property (key-value pair). Carried on PUBLISH and CONNECT.
   * Ignored by v3.1.1 adapters. */
  typedef struct az_iot_mqtt_user_property
  {
    const char* key;
    const char* value;
  } az_iot_mqtt_user_property;

  typedef struct az_iot_mqtt_message
  {
    const char* topic;
    const uint8_t* payload;
    size_t payload_len;
    az_iot_mqtt_qos qos;
    bool retain;
    /* MQTTv5-only fields; ignored by v3.1.1 adapters. */
    const az_iot_mqtt_user_property* user_properties; /* typed array */
    size_t user_properties_count; /* number of entries */
    uint16_t topic_alias; /* 0 = none */
    uint32_t message_expiry_seconds; /* 0 = none */
    const char* content_type;
    const char* response_topic;
    const uint8_t* correlation_data;
    size_t correlation_data_len;
  } az_iot_mqtt_message;

  /* Signs one digest with a private key the caller never sees (D8). This is the
   * escape hatch for stacks that have no engine/provider abstraction: the key
   * lives in an HSM / TPM / secure element and the only operation exposed is
   * "sign these bytes".
   *
   * `ctx` is the opaque context supplied alongside the callback on
   * az_iot_mqtt_tls_options. `digest`/`digest_len` are the bytes to sign;
   * the signature is written to `out_sig` (at most `out_sig_cap` bytes) and its
   * length reported through `out_sig_len`. Returns AZ_IOT_ERR_NOT_ENOUGH_SPACE
   * when the buffer is too small, and leaves *out_sig_len untouched on failure.
   *
   * Deliberately a plain function pointer rather than the certificate-provider
   * type: this header is the standalone BYO-MQTT-client seam (see
   * docs/how_to_byo_mqtt_client.md) and must not depend on the certificate
   * provider ABI. */
  typedef az_iot_result (*az_iot_mqtt_sign_callback)(
      void* ctx,
      const uint8_t* digest,
      size_t digest_len,
      uint8_t* out_sig,
      size_t out_sig_cap,
      size_t* out_sig_len);

  /* Which transport carries the MQTT session.
   *
   * Zero is TCP, so a zero-initialized connect options struct keeps the
   * behaviour every caller had before this field existed. */
  typedef enum az_iot_mqtt_transport
  {
    /* MQTT directly over TCP (over TLS when TLS is selected). Default port
     * 8883 with TLS, 1883 without. */
    AZ_IOT_MQTT_TRANSPORT_TCP = 0,
    /* MQTT over WebSockets, for networks that only allow HTTP(S) ports.
     * Default port 443 with TLS, 80 without. */
    AZ_IOT_MQTT_TRANSPORT_WEBSOCKET = 1
  } az_iot_mqtt_transport;

  /* Default WebSocket resource path for Azure IoT Hub and DPS. Used when
   * `websocket_path` is NULL. */
#define AZ_IOT_MQTT_DEFAULT_WEBSOCKET_PATH "/$iothub/websocket"

/* Default broker ports, by transport and by whether the session is TLS. Named
 * rather than spelled out at each use: the same four numbers are needed by the
 * core when it derives a port and by every adapter when it builds a URI, and a
 * bare 8883 in one of those places is indistinguishable from a typo. */
#define AZ_IOT_MQTT_DEFAULT_PORT_TCP_TLS 8883u
#define AZ_IOT_MQTT_DEFAULT_PORT_TCP_PLAIN 1883u
#define AZ_IOT_MQTT_DEFAULT_PORT_WEBSOCKET_TLS 443u
#define AZ_IOT_MQTT_DEFAULT_PORT_WEBSOCKET_PLAIN 80u

/* Default port of an HTTP proxy when az_iot_mqtt_proxy_options.port is 0. */
#define AZ_IOT_MQTT_DEFAULT_PROXY_PORT 8080u

  /* HTTP proxy to tunnel the MQTT connection through, via HTTP CONNECT.
   *
   * Applies to every transport, not only WebSockets: a device on a filtered
   * network commonly has to tunnel plain MQTT over 8883 as well.
   *
   * The proxy is a transport detail only. The TLS session is end-to-end with
   * the broker: it is established INSIDE the tunnel, so the proxy sees no
   * plaintext and server-certificate and hostname validation are unchanged.
   *
   * `host` NULL means "no proxy". Credentials are optional; when `username` is
   * set they are sent as HTTP Basic, which is why a proxy that requires
   * authentication should itself be reached over a trusted network segment. */
  typedef struct az_iot_mqtt_proxy_options
  {
    const char* host; /* proxy host name or IP; NULL = no proxy */
    uint16_t port; /* 0 selects AZ_IOT_MQTT_DEFAULT_PROXY_PORT */
    const char* username; /* may be NULL */
    const char* password; /* may be NULL */
  } az_iot_mqtt_proxy_options;

  /* TLS credentials handed to an adapter on connect.
   *
   * Populated by the SDK and read by the adapter. A new field must mean "not
   * requested" when zero, so an adapter written before it keeps working once
   * rebuilt (see docs/struct_versioning.md). */
  typedef struct az_iot_mqtt_tls_options
  {
    const char* trusted_ca_path; /* file or NULL for system store */
    const char* client_cert_path; /* PEM */
    const char* client_key_path; /* PEM */
    const char* client_key_password; /* may be NULL */
    /* Selects TLS.
     *
     * There is deliberately NO option to disable server certificate
     * validation. Whenever an adapter establishes a TLS session it validates
     * the chain AND the hostname, unconditionally; this SDK connects to Azure
     * endpoints, and an unverified session authenticates nothing.
     *
     * This slot previously held `verify_server`, which could switch validation
     * off and, being false in a zero-initialized struct, did so for any caller
     * who simply forgot it. It now only selects TLS, which is what the SDK ever
     * used it for. Set it for a connection that carries no other TLS material,
     * such as server-authentication-only; connections carrying a certificate,
     * a CA or a key reference select TLS on that alone. */
    bool use_tls;
    /* In-memory PEM material. Adapters that load credentials from memory rather
     * than from disk (e.g. esp-mqtt on a device with no filesystem) use these;
     * file-path adapters (Paho + OpenSSL) ignore them. Any field may be NULL. */
    const char* trusted_ca_pem; /* CA chain PEM, or NULL */
    const char* client_cert_pem; /* client certificate PEM, or NULL */
    const char* client_key_pem; /* client private key PEM, or NULL */
    /* Non-extractable key custody (D8). Set when the private key cannot be read
     * -- it stays inside an HSM, TPM or secure element -- so the adapter has to
     * sign THROUGH it instead of loading it.
     *
     * Two mutually independent routes; an adapter may support either, both, or
     * neither, and must fail the connect with AZ_IOT_ERR_NOT_SUPPORTED rather
     * than connect without a client key when asked for one it cannot honour:
     *
     *  - client_key_uri + crypto_engine_id: the stack has an engine/provider
     *    abstraction (OpenSSL ENGINE or OpenSSL 3.x provider). The adapter
     *    loads the named engine/provider and resolves the URI through it.
     *  - sign + sign_ctx: no such abstraction exists; the adapter drives the
     *    handshake signature through the callback.
     *
     * All four are NULL when the key is an ordinary PEM or file. */
    const char* client_key_uri; /* e.g. "pkcs11:token=...;object=..." */
    const char* crypto_engine_id; /* OpenSSL ENGINE/provider id: "pkcs11", "tpm2" */
    az_iot_mqtt_sign_callback sign; /* may be NULL */
    void* sign_ctx; /* opaque, passed back to sign() */
  } az_iot_mqtt_tls_options;

  typedef struct az_iot_mqtt_connect_options
  {
    const char* host;
    /* 0 selects the default for the transport and TLS selection: 8883/1883 for
     * TCP, 443/80 for WebSockets. */
    uint16_t port;
    const char* client_id;
    const char* username; /* may be NULL */
    const char* password; /* may be NULL */
    uint16_t keep_alive_seconds;
    uint32_t connect_timeout_seconds;
    az_iot_mqtt_tls_options tls;
    /* MQTTv5-only fields. */
    bool clean_start; /* v5 Clean Start flag (v3.1.1: maps to cleanSession) */
    uint32_t session_expiry_seconds;
    const az_iot_mqtt_user_property* user_properties; /* typed array */
    size_t user_properties_count;
    /* Last Will and Testament (v3.1.1 + v5). */
    struct
    {
      const char* topic; /* NULL or "" = no LWT */
      const uint8_t* payload;
      size_t payload_len;
      az_iot_mqtt_qos qos;
      bool retain;
      uint32_t will_delay_seconds; /* v5 only; 0 = immediate */
    } lwt;
    /* Transport selection and HTTP proxy. Appended, so zero means TCP with no
     * proxy -- the behaviour that predates these fields.
     *
     * An adapter that cannot honour a non-default value here MUST fail the
     * connect with AZ_IOT_ERR_NOT_SUPPORTED. Silently connecting directly when
     * a proxy was asked for would bypass the very egress control the caller
     * selected, and silently connecting over TCP when WebSockets were asked for
     * would be blocked by the firewall the caller was working around. */
    az_iot_mqtt_transport transport;
    /* WebSocket resource path, used only when transport is WEBSOCKET. NULL
     * selects AZ_IOT_MQTT_DEFAULT_WEBSOCKET_PATH, which is what Azure IoT Hub
     * and DPS expect. Must start with '/'. */
    const char* websocket_path;
    az_iot_mqtt_proxy_options proxy;
    /* Reason code the adapter puts in the MQTT 5 DISCONNECT packet it sends
     * when disconnect() is called on THIS session.
     *
     * It belongs to the connect options rather than to disconnect() because the
     * vtable is a published seam: adding a parameter to disconnect(), or a slot
     * to az_iot_mqtt_iface, changes every bring-your-own adapter's source, while
     * an adapter written before this field never reads it, and a zero leaves
     * the behaviour that predates it (see docs/struct_versioning.md).
     *
     * 0 is AZ_IOT_MQTT_DISCONNECT_NORMAL, which is also what an MQTT 5
     * DISCONNECT with no reason code means, so zero-initialization keeps the
     * orderly close every caller had before. The only other value this SDK
     * uses is AZ_IOT_MQTT_DISCONNECT_WITH_WILL_MESSAGE.
     *
     * MQTT 3.1.1 has no DISCONNECT reason codes at all: a v3.1.1 adapter MUST
     * ignore this field rather than invent a byte for it. */
    uint8_t disconnect_reason_code;
  } az_iot_mqtt_connect_options;

/* MQTT 5 DISCONNECT reason codes used by az_iot_mqtt_connect_options.
 * disconnect_reason_code. Named rather than spelled out because a bare 0x04 at
 * a call site is indistinguishable from a typo.
 *
 * NORMAL (0x00) closes the session and DISCARDS any Will. WITH_WILL_MESSAGE
 * (0x04) closes it and asks the broker to publish the Will anyway, which is the
 * only way an orderly close can still announce the departure. */
#define AZ_IOT_MQTT_DISCONNECT_NORMAL 0x00u
#define AZ_IOT_MQTT_DISCONNECT_WITH_WILL_MESSAGE 0x04u

  /* Inbound event types delivered through the single adapter callback. */
  typedef enum az_iot_mqtt_event_kind
  {
    AZ_IOT_MQTT_EVT_CONNECTED = 0,
    AZ_IOT_MQTT_EVT_DISCONNECTED,
    AZ_IOT_MQTT_EVT_MESSAGE,
    AZ_IOT_MQTT_EVT_PUBLISH_ACK,
    AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK,
    AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK,
    AZ_IOT_MQTT_EVT_ERROR
  } az_iot_mqtt_event_kind;

  typedef struct az_iot_mqtt_event
  {
    az_iot_mqtt_event_kind kind;
    uint16_t packet_id; /* for *_ACK events */
    az_iot_result status; /* for ACK / ERROR events */
    const az_iot_mqtt_message* message; /* for AZ_IOT_MQTT_EVT_MESSAGE only */
    /* For AZ_IOT_MQTT_EVT_CONNECTED: the Session Present flag the broker
     * returned in the CONNACK. Carried by BOTH protocol versions -- MQTT 3.1.1
     * has the flag too -- and adapters populate it for both, because a session
     * connected with Clean Session 0 has no other way to learn whether the
     * broker resumed it or quietly started a fresh one. False for every other
     * event kind. */
    bool session_present;
    /* The code that came off the wire, verbatim, for the ack this event carries
     * (a CONNACK return/reason code, a SUBACK return/reason code, a PUBACK reason code). A non-zero
     * value is always that code, including the granted QoS on a SUBACK that
     * succeeded -- a grant is diagnostic too. Exception: for a publish the client
     * dropped unacknowledged, no PUBACK arrived and the code is the client's own
     * (az_mqtt: 0x80, or 0x95 over the server's Maximum Packet Size). Only 0 is ambiguous: it means
     * either "no code applies here" or "a producer that does not populate this
     * field", and nothing can tell those apart, which is the reason no decision
     * may rest on it.
     *
     * `status` is the classification the SDK acts on; this is the evidence for
     * it. Both travel because a classification cannot describe a code this SDK
     * has never seen, and a log that prints only "AZ_IOT_ERR_MQTT" cannot
     * either. Diagnostics and telemetry only -- never branch on it. */
    int32_t protocol_code;
    /* The adapter's own error code for a failure BELOW MQTT: TLS handshake,
     * socket refused, DNS. Adapter-defined and not comparable across adapters,
     * which is why it is separate from protocol_code rather than sharing it --
     * a value here has no wire meaning.
     *
     * 0 means "none", with the same ambiguity as protocol_code. Diagnostics
     * only; an adapter that has nothing to report leaves it 0 and stays
     * conformant. */
    int32_t transport_code;
  } az_iot_mqtt_event;

  typedef void (*az_iot_mqtt_event_callback)(const az_iot_mqtt_event* evt, void* user_ctx);

  typedef struct az_iot_mqtt_client az_iot_mqtt_client;

  /* Adapter vtable. Each instance carries its own version. All calls are non-blocking;
   * actual I/O happens inside process_loop(). */
  typedef struct az_iot_mqtt_iface
  {
    az_iot_mqtt_version version;

    az_iot_result (*connect)(az_iot_mqtt_client* self, const az_iot_mqtt_connect_options* opts);
    /* Orderly close of the session. On MQTT 5 the DISCONNECT carries the reason
     * code the session was connected with
     * (az_iot_mqtt_connect_options.disconnect_reason_code); on v3.1.1 there is
     * no reason code to carry. */
    az_iot_result (*disconnect)(az_iot_mqtt_client* self);
    az_iot_result (*subscribe)(
        az_iot_mqtt_client* self,
        const char* topic_filter,
        az_iot_mqtt_qos qos,
        uint16_t* out_packet_id);
    az_iot_result (
        *unsubscribe)(az_iot_mqtt_client* self, const char* topic_filter, uint16_t* out_packet_id);
    az_iot_result (*publish)(
        az_iot_mqtt_client* self,
        const az_iot_mqtt_message* msg,
        uint16_t* out_packet_id);
    az_iot_result (*process_loop)(az_iot_mqtt_client* self, uint32_t timeout_ms);
    void (*set_inbound_cb)(az_iot_mqtt_client* self, az_iot_mqtt_event_callback cb, void* user_ctx);
    void (*destroy)(az_iot_mqtt_client* self);
  } az_iot_mqtt_iface;

  /* Concrete client object returned by a factory. The first member MUST be a pointer
   * to the iface so the core can dispatch generically. */
  struct az_iot_mqtt_client
  {
    const az_iot_mqtt_iface* iface;
    /* adapter-specific state follows */
  };

  /* Factory: produces a client that speaks a specific MQTT version. The SDK
   * selects a factory by version at connection time based on the service being
   * targeted (DPS/MQTTv3 require v3.1.1, MQTTv5 requires v5). */
  typedef struct az_iot_mqtt_factory
  {
    az_iot_mqtt_version version;
    az_iot_mqtt_client* (*create)(void* factory_ctx);
    void* factory_ctx;
    /* Called by az_iot_connection_client_deinit() to free factory resources.
     * NULL means no cleanup needed (e.g. stack-allocated factory). */
    void (*destroy)(void* factory_ctx);
  } az_iot_mqtt_factory;

  const char* az_iot_mqtt_version_to_string(az_iot_mqtt_version v);

  /* Map a CONNACK code from the wire onto the status an adapter reports with
   * AZ_IOT_MQTT_EVT_CONNECTED. Adapters should route every CONNACK rejection
   * through this so the core sees one consistent vocabulary regardless of which
   * MQTT client is underneath.
   *
   * The distinction that matters to the core is identity-versus-transport: codes
   * that mean "this client id / credential / authorization is not acceptable"
   * become AZ_IOT_ERR_IDENTITY_REJECTED, which the SDK recovers from on
   * opts.identity_recovery instead of on the ordinary reconnection policy.
   * Everything else stays AZ_IOT_ERR_MQTT and is retried.
   *
   * `connack_code` is the value carried in the CONNACK packet: a v3.1.1 return
   * code (1..5) or a v5 reason code (>= 0x80). 0 means success. A negative value
   * is treated as an adapter-internal failure (socket, TLS, library error) rather
   * than a code that came off the wire, and maps to AZ_IOT_ERR_MQTT.
   *
   * `version` selects which of the two code schemes applies; they overlap
   * numerically, so it is not optional. A version this function does not
   * recognize yields AZ_IOT_ERR_MQTT for any non-zero code -- guessing a scheme
   * would be guessing whether to re-provision, and the retryable answer is the
   * safe one. */
  AZ_NODISCARD az_iot_result
  az_iot_mqtt_connack_result(az_iot_mqtt_version version, int connack_code);

  /* Map a SUBACK code from the wire onto the status an adapter reports with
   * AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK. The SUBACK counterpart of
   * az_iot_mqtt_connack_result(), and adapters should route every SUBACK
   * through it for the same reason: the core needs one vocabulary regardless of
   * which MQTT client is underneath.
   *
   * The distinction that matters here is permanent-versus-transient. A filter
   * the broker will never accept (not authorized, invalid filter) becomes
   * AZ_IOT_ERR_SUBSCRIPTION_REFUSED, because re-issuing it can only be refused
   * again; a quota or unspecified error stays AZ_IOT_ERR_MQTT and is retried,
   * which is how a transient service-side fault presents.
   *
   * `suback_code` is the value carried in the SUBACK: a granted QoS (0..2), a
   * v3.1.1 failure (0x80), or a v5 reason code (>= 0x80). **A granted QoS lower
   * than the one requested is a success, not a refusal** -- the subscription
   * exists and MQTT delivers at min(publish QoS, granted QoS). A negative value
   * is treated as an adapter-internal failure (socket, TLS, library error): it
   * never reached a broker, so it says nothing about the filter and is retried.
   *
   * `version` selects which code scheme applies. A version this function does
   * not recognize yields AZ_IOT_ERR_MQTT for any non-grant code -- guessing a
   * scheme would be guessing whether to fail the session, and retrying is the
   * safe half of that split. */
  AZ_NODISCARD az_iot_result
  az_iot_mqtt_suback_result(az_iot_mqtt_version version, int suback_code);

  /* Classify a server-sent MQTT 5 DISCONNECT reason code.
   *
   * A server DISCONNECT is not automatically a clean close: 0x00 is, and
   * everything from 0x80 up is the server saying why it terminated the
   * session. Reporting those as AZ_IOT_OK made "the hub closed us for quota
   * exceeded" indistinguishable from an ordinary peer close, and with retries
   * disabled it settled the session at IDLE with nothing to explain it.
   *
   * v3.1.1 has no DISCONNECT reason code; pass 0. */
  AZ_NODISCARD az_iot_result
  az_iot_mqtt_disconnect_result(az_iot_mqtt_version version, int disconnect_code);

  /**
   * @brief Map a PUBACK code onto the status an adapter reports with
   * AZ_IOT_MQTT_EVT_PUBLISH_ACK. Adapters should route every PUBACK through it; of the bundled
   * adapters, only az_mqtt does.
   *
   * @param version Selects the code scheme.
   * @param puback_code MQTT 5 reason code; MQTT 3.1.1: 0. Negative: the adapter's own failure.
   *
   * @retval AZ_IOT_OK 0; MQTT 5: any code below 0x80 (0x10 included).
   * @retval AZ_IOT_ERR_PUBLISH_REFUSED MQTT 5: 0x87, 0x90, 0x99, which the broker repeats; 0x95,
   *         which a client reports for a publish over the server's Maximum Packet Size (not a
   *         PUBACK code on the wire).
   * @retval AZ_IOT_ERR_BUSY MQTT 5: 0x97 (quota exceeded).
   * @retval AZ_IOT_ERR_MQTT Any other code, or a non-zero code with an unknown @p version.
   */
  AZ_NODISCARD az_iot_result
  az_iot_mqtt_puback_result(az_iot_mqtt_version version, int puback_code);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTT_IFACE_H */
