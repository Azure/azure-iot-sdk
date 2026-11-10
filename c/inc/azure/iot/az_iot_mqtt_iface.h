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

/** @brief Bytes an adapter keeps of a CONNACK or server DISCONNECT Reason String: all the SDK
 * retains (AZ_IOT_CONN_ERROR_MESSAGE_MAX). */
#define AZ_IOT_MQTT_SESSION_REASON_STRING_MAX 128

#ifdef __cplusplus
extern "C"
{
#endif

  /* MQTT protocol versions supported by adapters. DPS and the MQTTv3 hub require v3.1.1. */
  typedef enum az_iot_mqtt_version
  {
    AZ_IOT_MQTT_VERSION_3_1_1 = 0
  } az_iot_mqtt_version;

  typedef enum az_iot_mqtt_qos
  {
    AZ_IOT_MQTT_QOS_0 = 0,
    AZ_IOT_MQTT_QOS_1 = 1,
    AZ_IOT_MQTT_QOS_2 = 2
  } az_iot_mqtt_qos;

  typedef struct az_iot_mqtt_message
  {
    const char* topic;
    const uint8_t* payload;
    size_t payload_len;
    az_iot_mqtt_qos qos;
    bool retain;
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
   * Called only from inside the adapter's connect() or process_loop(), so on the
   * thread that drives the client. An adapter whose TLS handshake runs on its own
   * thread must not accept this route.
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

  /* Default WebSocket resource path for DPS and the mqttv3 IoT Hub. Used when
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
   * Populated by the SDK and read by the adapter. Every string, PEM buffer and
   * path is valid only while connect() runs: the SDK then releases it to the
   * certificate provider, which may wipe the private key and its password. An
   * adapter copies what it needs during the call. sign and sign_ctx are the
   * exception: they stay valid until the adapter client is destroyed, so the
   * handshake may call sign() after connect() returns. A new field must mean "not
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
     * than from disk (e.g. az_mqtt on an ESP32, which has no filesystem) use these;
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
    /* PSA Crypto key id of a non-extractable client key (mbedTLS); 0: none. A
     * third custody route: an adapter that cannot use it fails the connect with
     * AZ_IOT_ERR_NOT_SUPPORTED. The key stays usable until the adapter client is
     * destroyed, so the handshake may use it after connect() returns. */
    uint32_t client_key_psa_id;
  } az_iot_mqtt_tls_options;

  /* Options for az_iot_mqtt_iface::connect(). This struct and every pointer in
   * it (strings, TLS material, Will) are valid only while
   * connect() runs; the adapter copies what it uses afterwards, e.g. from
   * process_loop(). The SDK wipes the SAS password and releases TLS material to
   * the certificate provider, which may wipe the private key, once it returns.
   * Exception: tls.sign and tls.sign_ctx (see az_iot_mqtt_tls_options). */
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
    bool clean_start; /* maps to the v3.1.1 Clean Session flag */
    /* Last Will and Testament. */
    struct
    {
      const char* topic; /* NULL or "" = no LWT */
      const uint8_t* payload;
      size_t payload_len;
      az_iot_mqtt_qos qos;
      bool retain;
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
     * selects AZ_IOT_MQTT_DEFAULT_WEBSOCKET_PATH. Must start with '/'. */
    const char* websocket_path;
    az_iot_mqtt_proxy_options proxy;
  } az_iot_mqtt_connect_options;

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
    /* For ACK / ERROR events. For AZ_IOT_MQTT_EVT_DISCONNECTED: AZ_IOT_OK only
     * when a disconnect() the client asked for completed; an error otherwise. */
    az_iot_result status;
    const az_iot_mqtt_message* message; /* for AZ_IOT_MQTT_EVT_MESSAGE only */
    /* For AZ_IOT_MQTT_EVT_CONNECTED: the Session Present flag the broker
     * returned in the CONNACK. Adapters populate it because a session
     * connected with Clean Session 0 has no other way to learn whether the
     * broker resumed it or quietly started a fresh one. False for every other
     * event kind. */
    bool session_present;
    /* The code that came off the wire, verbatim, for the ack this event carries
     * (a CONNACK return code, a SUBACK return code). A non-zero
     * value is always that code, including the granted QoS on a SUBACK that
     * succeeded -- a grant is diagnostic too. Exception: for a publish the client
     * dropped unacknowledged, no PUBACK arrived and the code is the client's own
     * (az_mqtt: 0x80). Only 0 is ambiguous: it means
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
    /* The Reason String of the packet behind this event, verbatim. UTF-8, not NUL-terminated,
     * valid only during the callback. NULL: the packet carried none; non-NULL with length 0:
     * present and empty. Diagnostics: never branch on it. MQTT 3.1.1 packets carry none, so MQTT
     * 3.1.1 adapters leave it NULL. For CONNECTED and DISCONNECTED only, an adapter that copies it
     * may keep only its first AZ_IOT_MQTT_SESSION_REASON_STRING_MAX bytes; other kinds carry it
     * whole. */
    const char* reason_string;
    size_t reason_string_len;
  } az_iot_mqtt_event;

  typedef void (*az_iot_mqtt_event_callback)(const az_iot_mqtt_event* evt, void* user_ctx);

  typedef struct az_iot_mqtt_client az_iot_mqtt_client;

  /* Adapter vtable. Each instance carries its own version. All calls are non-blocking;
   * actual I/O happens inside process_loop(). */
  typedef struct az_iot_mqtt_iface
  {
    az_iot_mqtt_version version;

    /* @p opts and every pointer it holds are valid only during this call, except
     * tls.sign and tls.sign_ctx (see az_iot_mqtt_tls_options); copy what is
     * needed later. */
    az_iot_result (*connect)(az_iot_mqtt_client* self, const az_iot_mqtt_connect_options* opts);
    /* Orderly close of the session. */
    az_iot_result (*disconnect)(az_iot_mqtt_client* self);
    az_iot_result (*subscribe)(
        az_iot_mqtt_client* self,
        const char* topic_filter,
        az_iot_mqtt_qos qos,
        uint16_t* out_packet_id);
    /* An accepted unsubscribe ends in one AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK carrying
     * *out_packet_id, unless the session ends first. */
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

  /* Concrete client object built by an adapter's init(). The first member MUST be a pointer
   * to the iface so the core can dispatch generically. */
  struct az_iot_mqtt_client
  {
    const az_iot_mqtt_iface* iface;
    /* adapter-specific state follows */
  };

  /**
   * @brief Sizes of one MQTT client, chosen by the application. An adapter turns them into the
   * workspace it needs (az_iot_mqtt_adapter::get_workspace_size).
   */
  typedef struct az_iot_mqtt_limits
  {
    /** @brief Largest outgoing packet, in bytes. */
    size_t send_buffer_size;
    /** @brief Largest incoming packet, in bytes. */
    size_t receive_buffer_size;
    /** @brief QoS 1 exchanges at once (the SDK uses no QoS 2), 1-32767. */
    uint16_t max_in_flight_messages;
    /**
     * @brief Bytes for the copies of unacknowledged QoS 1 PUBLISH an adapter keeps while the
     * session outlives the connection. 0: clean sessions only (with az_mqtt,
     * az_iot_connection_client_init() fails with AZ_IOT_ERR_INVALID_ARG unless session_continuity
     * is AZ_IOT_SESSION_CONTINUITY_CLEAN or dps.provision_only is set, and a connect whose session
     * outlives the connection fails with AZ_IOT_ERR_NOT_SUPPORTED); otherwise at least one packet
     * of send_buffer_size plus the adapter's per-message overhead. az_mqtt keeps each copy (encoded
     * size + 18 bytes) until its PUBACK; a publish that does not fit returns AZ_IOT_ERR_BUSY.
     *
     * The SDK never asks an adapter to resend these copies: it destroys the client at every
     * session end and creates a new one for the next session, and the layer above retries.
     */
    size_t message_store_size;
    /**
     * @brief Bytes for the copies a connect makes of its strings (host, credentials, TLS paths or
     * PEM, proxy, will), each with a NUL. A connect whose copies do not fit
     * returns AZ_IOT_ERR_NOT_ENOUGH_SPACE.
     */
    size_t connect_strings_size;
  } az_iot_mqtt_limits;

  /**
   * @brief How the connection client will use the sessions of the clients an adapter builds,
   * known at az_iot_connection_client_init(). Zero: every session is clean.
   */
  typedef struct az_iot_mqtt_session_usage
  {
    /**
     * @brief Whether a connect may set az_iot_mqtt_connect_options::clean_start false, so the
     * session outlives its connection.
     */
    bool session_outlives_connection;
  } az_iot_mqtt_session_usage;

  /**
   * @brief An MQTT implementation that builds its clients in application memory.
   *
   * A client built by init() is released with its iface destroy(): that wipes what the client
   * copied (credentials included) and frees nothing; the application then owns the workspace again.
   */
  typedef struct az_iot_mqtt_adapter
  {
    /**
     * @brief Workspace bytes for the largest client this adapter builds with @p limits. 0 when
     * @p limits is invalid.
     */
    size_t (*get_workspace_size)(const az_iot_mqtt_limits* limits);
    /**
     * @brief Build a client of @p mqtt_version in @p workspace.
     *
     * @param[in] workspace At least get_workspace_size(limits) bytes, valid until the client is
     * destroyed. Any alignment.
     * @param[out] out_client The client, which lives in @p workspace.
     * @retval AZ_IOT_OK Built.
     * @retval AZ_IOT_ERR_INVALID_ARG NULL argument or invalid @p limits.
     * @retval AZ_IOT_ERR_NOT_SUPPORTED @p mqtt_version not compiled in.
     * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE @p workspace_size is smaller than needed.
     */
    az_iot_result (*init)(
        az_iot_mqtt_version mqtt_version,
        const az_iot_mqtt_limits* limits,
        uint8_t* workspace,
        size_t workspace_size,
        az_iot_mqtt_client** out_client);
    /**
     * @brief Optional (NULL: every @p limits get_workspace_size() accepts suits every session).
     * Whether @p limits suit the sessions described by @p usage, so a misconfiguration fails
     * az_iot_connection_client_init() instead of every connect. Logs the reason when they do not.
     *
     * @retval AZ_IOT_OK They suit.
     * @retval AZ_IOT_ERR_INVALID_ARG NULL argument, or @p limits do not suit @p usage.
     */
    az_iot_result (*validate_limits_for_session_usage)(
        const az_iot_mqtt_limits* limits,
        const az_iot_mqtt_session_usage* usage);
  } az_iot_mqtt_adapter;

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
   * code (1..5). 0 means success. A negative value is treated as an
   * adapter-internal failure (socket, TLS, library error) rather than a code
   * that came off the wire, and maps to AZ_IOT_ERR_MQTT.
   *
   * `version` selects the code scheme. A version this function does not
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
   * The distinction that matters here is permanent-versus-transient. A v3.1.1
   * refusal (0x80) becomes AZ_IOT_ERR_SUBSCRIPTION_REFUSED: an MQTTv3 device
   * subscribes to a topic set fixed at compile time, so re-issuing the filter
   * can only be refused again. Any other failure stays AZ_IOT_ERR_MQTT and is
   * retried.
   *
   * `suback_code` is the value carried in the SUBACK: a granted QoS (0..2) or a
   * v3.1.1 failure (0x80). **A granted QoS lower than the one requested is a
   * success, not a refusal** -- the subscription exists and MQTT delivers at
   * min(publish QoS, granted QoS). A negative value is treated as an
   * adapter-internal failure (socket, TLS, library error): it never reached a
   * broker, so it says nothing about the filter and is retried.
   *
   * `version` selects the code scheme. A version this function does not
   * recognize yields AZ_IOT_ERR_MQTT for any non-grant code -- guessing a
   * scheme would be guessing whether to fail the session, and retrying is the
   * safe half of that split. */
  AZ_NODISCARD az_iot_result
  az_iot_mqtt_suback_result(az_iot_mqtt_version version, int suback_code);

  /**
   * @brief Map a PUBACK code onto the status an adapter reports with
   * AZ_IOT_MQTT_EVT_PUBLISH_ACK. Adapters must route every PUBACK through it.
   *
   * @param version Selects the code scheme.
   * @param puback_code 0: an MQTT 3.1.1 PUBACK carries no code. Non-zero: the adapter's own
   * failure.
   *
   * @retval AZ_IOT_OK 0.
   * @retval AZ_IOT_ERR_MQTT Any other code.
   */
  AZ_NODISCARD az_iot_result
  az_iot_mqtt_puback_result(az_iot_mqtt_version version, int puback_code);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTT_IFACE_H */
