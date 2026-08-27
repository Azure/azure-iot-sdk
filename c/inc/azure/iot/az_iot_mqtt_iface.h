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

  /* MQTT protocol versions supported by adapters. DPS + IoTHub-Classic require v3.1.1;
   * IoTHub-Next requires v5. A single adapter binary may register factories for both,
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

  typedef struct az_iot_mqtt_tls_options
  {
    const char* trusted_ca_path; /* file or NULL for system store */
    const char* client_cert_path; /* PEM */
    const char* client_key_path; /* PEM */
    const char* client_key_password; /* may be NULL */
    bool verify_server; /* default true */
    /* In-memory PEM material. Adapters that load credentials from memory rather
     * than from disk (e.g. esp-mqtt on a device with no filesystem) use these;
     * file-path adapters (Paho + OpenSSL) ignore them. Any field may be NULL. */
    const char* trusted_ca_pem; /* CA chain PEM, or NULL */
    const char* client_cert_pem; /* client certificate PEM, or NULL */
    const char* client_key_pem; /* client private key PEM, or NULL */
  } az_iot_mqtt_tls_options;

  typedef struct az_iot_mqtt_connect_options
  {
    const char* host;
    uint16_t port; /* typically 8883 */
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
      const char* topic; /* NULL = no LWT */
      const uint8_t* payload;
      size_t payload_len;
      az_iot_mqtt_qos qos;
      bool retain;
      uint32_t will_delay_seconds; /* v5 only; 0 = immediate */
    } lwt;
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
    az_iot_result status; /* for ACK / ERROR events */
    const az_iot_mqtt_message* message; /* for AZ_IOT_MQTT_EVT_MESSAGE only */
    bool session_present; /* for AZ_IOT_MQTT_EVT_CONNECTED (v5 CONNACK) */
    /* The code that came off the wire, verbatim, for the ack this event carries
     * (a CONNACK return/reason code, a SUBACK return/reason code). 0 when the
     * event carries none, which is also what a producer that never sets it
     * reports -- so it is only meaningful alongside a failing `status`.
     *
     * `status` is the classification the SDK acts on; this is the evidence for
     * it. Both travel because a classification cannot describe a code this SDK
     * has never seen, and a log that prints only "AZ_IOT_ERR_MQTT" cannot
     * either. Diagnostics and telemetry only -- never branch on it. */
    int32_t protocol_code;
  } az_iot_mqtt_event;

  typedef void (*az_iot_mqtt_event_callback)(const az_iot_mqtt_event* evt, void* user_ctx);

  typedef struct az_iot_mqtt_client az_iot_mqtt_client;

  /* Adapter vtable. Each instance carries its own version. All calls are non-blocking;
   * actual I/O happens inside process_loop(). */
  typedef struct az_iot_mqtt_iface
  {
    az_iot_mqtt_version version;

    az_iot_result (*connect)(az_iot_mqtt_client* self, const az_iot_mqtt_connect_options* opts);
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
   * targeted (DPS/Classic require v3.1.1, Hub-Next requires v5). */
  typedef struct az_iot_mqtt_factory
  {
    az_iot_mqtt_version version;
    az_iot_mqtt_client* (*create)(void* factory_ctx);
    void* factory_ctx;
    /* Called by connection_client_destroy() to free factory resources.
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
   * become AZ_IOT_ERR_IDENTITY_REJECTED, which is what makes the SDK
   * re-provision through DPS instead of retrying an identity the broker has
   * already refused. Everything else stays AZ_IOT_ERR_MQTT and is retried.
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

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MQTT_IFACE_H */
