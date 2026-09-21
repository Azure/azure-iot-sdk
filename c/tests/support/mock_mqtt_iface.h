// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* In-memory mock implementation of az_iot_mqtt_iface for unit tests.
 *
 * The mock records every vtable call (with a copy of relevant arguments) into a
 * bounded ring of "events", and allows the test to:
 *   - script per-call return codes (default AZ_IOT_OK)
 *   - inject inbound MQTT events (CONNECTED / MESSAGE / *_ACK / DISCONNECTED / ERROR)
 *     that get delivered to the registered callback when process_loop() runs
 *
 * Lifetime: az_iot_mock_mqtt_factory_create() returns a factory whose create()
 * function produces az_iot_mqtt_client instances backed by this mock. The factory
 * keeps a back-pointer to the most-recently-created client so a test can introspect
 * it. When the test is done, call az_iot_mock_mqtt_factory_destroy() to free
 * everything (any client still alive is destroyed first).
 *
 * Not thread-safe; tests are single-threaded.
 */
#ifndef AZ_IOT_MOCK_MQTT_IFACE_H
#define AZ_IOT_MOCK_MQTT_IFACE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#ifdef __cplusplus
extern "C"
{
#endif

  typedef enum az_iot_mock_call_kind
  {
    AZ_IOT_MOCK_CALL_CONNECT = 0,
    AZ_IOT_MOCK_CALL_DISCONNECT,
    AZ_IOT_MOCK_CALL_SUBSCRIBE,
    AZ_IOT_MOCK_CALL_UNSUBSCRIBE,
    AZ_IOT_MOCK_CALL_PUBLISH,
    AZ_IOT_MOCK_CALL_PROCESS_LOOP,
    AZ_IOT_MOCK_CALL_DESTROY
  } az_iot_mock_call_kind;

#define AZ_IOT_MOCK_TOPIC_MAX 256
/* Must be at least the largest number any feature client can send on one
 * PUBLISH (telemetry forwards up to AZ_IOT_TELEMETRY_MAX_USER_PROPS = 16),
 * otherwise the mock silently truncates and a test asserting on the tail
 * stops checking anything. */
#define AZ_IOT_MOCK_MAX_USER_PROPS 20
#define AZ_IOT_MOCK_PAYLOAD_MAX 4096
#define AZ_IOT_MOCK_HISTORY_MAX 64

  typedef struct az_iot_mock_call
  {
    az_iot_mock_call_kind kind;
    char topic[AZ_IOT_MOCK_TOPIC_MAX];
    uint8_t payload[AZ_IOT_MOCK_PAYLOAD_MAX];
    size_t payload_len;
    az_iot_mqtt_qos qos;
    bool retain;
    uint16_t packet_id; /* the packet_id this call returned (0 if N/A) */
    uint32_t timeout_ms; /* for process_loop */
    /* MQTT v5 PUBLISH extras, captured for presence/birth tests. */
    uint8_t correlation_data[64];
    size_t correlation_data_len;
    char user_type[64]; /* value of the "type" User Property, "" if none */
    /* Every User Property in order, so a test can assert what a feature client
     * actually forwarded rather than only the "type" marker. Properties past
     * the bound are dropped; assert on the count if that matters. */
    struct
    {
      char key[64];
      char value[128];
    } user_properties[AZ_IOT_MOCK_MAX_USER_PROPS];
    size_t user_properties_count;
    char content_type[64]; /* PUBLISH Content Type, "" if none */
    uint32_t message_expiry_seconds; /* PUBLISH Message Expiry Interval, 0 if none */
    char username[256]; /* CONNECT username, "" if none */
    /* CONNECT options, captured so tests can assert which endpoint/identity the
     * core targeted. `topic` also carries the host for backwards compatibility. */
    struct
    {
      char host[AZ_IOT_MOCK_TOPIC_MAX];
      char client_id[128];
      uint16_t port;
      uint16_t keep_alive_seconds;
      uint32_t connect_timeout_seconds;
      bool clean_start;
      bool use_tls;
      char trusted_ca_path[256];
      char client_cert_path[256];
      char client_key_path[256];
      bool has_trusted_ca_pem;
      bool has_client_cert_pem;
      bool has_client_key_pem;
      /* Non-extractable key custody (D8), captured so a test can assert the
       * core forwarded the key reference and the sign() hook rather than
       * dropping them on the way to the adapter. */
      char client_key_uri[256];
      char crypto_engine_id[64];
      bool has_sign;
      void* sign_ctx;
      az_iot_mqtt_sign_callback sign;
      /* Transport + proxy, captured so a test can assert the core forwarded the
       * caller's egress configuration to the adapter on BOTH connects (DPS and
       * hub) instead of quietly connecting direct. */
      az_iot_mqtt_transport transport;
      char websocket_path[128];
      char proxy_host[256];
      uint16_t proxy_port;
      char proxy_username[128];
      char proxy_password[128];
      /* Session semantics, captured so a test can assert the options the core
       * chose for the role it was connecting in rather than only that it
       * connected. */
      uint32_t session_expiry_seconds;
      uint8_t disconnect_reason_code;
      /* Last Will, as handed to the adapter. `lwt_topic` is empty when no Will
       * was requested, which is the assertion most cases need. */
      char lwt_topic[AZ_IOT_MOCK_TOPIC_MAX];
      uint8_t lwt_payload[256];
      size_t lwt_payload_len;
      az_iot_mqtt_qos lwt_qos;
      bool lwt_retain;
      uint32_t lwt_will_delay_seconds;
    } connect;
  } az_iot_mock_call;

  typedef struct az_iot_mock_mqtt_client az_iot_mock_mqtt_client;

  /* Construct a factory that produces mock MQTT clients of the given version.
   * Caller owns and must destroy. */
  az_iot_mqtt_factory* az_iot_mock_mqtt_factory_create(az_iot_mqtt_version version);

  void az_iot_mock_mqtt_factory_destroy(az_iot_mqtt_factory* factory);

  /* The most-recently-created client from this factory, or NULL if none. */
  az_iot_mock_mqtt_client* az_iot_mock_mqtt_factory_last_client(const az_iot_mqtt_factory* factory);

  /* Cast an az_iot_mqtt_client to its mock backing if it came from this factory. */
  az_iot_mock_mqtt_client* az_iot_mock_mqtt_client_from(az_iot_mqtt_client* c);

  /* Value of the User Property named `key` on a recorded call, or NULL if the
   * call did not carry one. */
  const char* az_iot_mock_call_user_property(const az_iot_mock_call* c, const char* key);

  /* Recorded call history. Newest call is at index (count - 1). */
  size_t az_iot_mock_mqtt_client_call_count(const az_iot_mock_mqtt_client* m);
  const az_iot_mock_call* az_iot_mock_mqtt_client_call_at(
      const az_iot_mock_mqtt_client* m,
      size_t i);
  void az_iot_mock_mqtt_client_clear_calls(az_iot_mock_mqtt_client* m);

  /* Script the next return code for the given call kind. By default every call
   * returns AZ_IOT_OK. The override is one-shot: it applies to the next call of
   * the kind, then reverts to AZ_IOT_OK. */
  /* Arm a one-shot CONNECT failure on the NEXT client this factory creates.
   * For code paths that build their own adapter inside the call under test,
   * where there is no client object to script beforehand. */
  void az_iot_mock_mqtt_factory_fail_next_connect(
      az_iot_mqtt_factory* factory,
      az_iot_result result);

  void az_iot_mock_mqtt_client_set_next_result(
      az_iot_mock_mqtt_client* m,
      az_iot_mock_call_kind kind,
      az_iot_result result);

  /* Queue an inbound event to be delivered on the next process_loop() call. May be
   * called multiple times; events are delivered in FIFO order, one per process_loop
   * invocation (so the test controls timing). Returns true on success, false if the
   * queue is full. */
  bool az_iot_mock_mqtt_client_inject_event(
      az_iot_mock_mqtt_client* m,
      const az_iot_mqtt_event* evt);

  /* Convenience: queue a CONNECTED event with the given status. */
  bool az_iot_mock_mqtt_client_inject_connected(az_iot_mock_mqtt_client* m, az_iot_result status);

  /* Convenience: queue a DISCONNECTED event (peer- or transport-initiated). */
  bool az_iot_mock_mqtt_client_inject_disconnected(az_iot_mock_mqtt_client* m);

  /* Convenience: queue an adapter ERROR event (socket/TLS/library failure). */
  bool az_iot_mock_mqtt_client_inject_error(az_iot_mock_mqtt_client* m, az_iot_result status);

  /* Convenience: queue a PUBLISH_ACK / SUBSCRIBE_ACK for the given packet id. */
  bool az_iot_mock_mqtt_client_inject_puback(
      az_iot_mock_mqtt_client* m,
      uint16_t packet_id,
      az_iot_result status);
  bool az_iot_mock_mqtt_client_inject_suback(
      az_iot_mock_mqtt_client* m,
      uint16_t packet_id,
      az_iot_result status);

  /* Number of calls of the given kind currently in the history. */
  size_t az_iot_mock_mqtt_client_count_of(
      const az_iot_mock_mqtt_client* m,
      az_iot_mock_call_kind k);

  /* The most recent call of the given kind, or NULL if there is none. */
  const az_iot_mock_call* az_iot_mock_mqtt_client_last_of(
      const az_iot_mock_mqtt_client* m,
      az_iot_mock_call_kind k);

  /* Convenience: queue an inbound MESSAGE event. The mock copies topic+payload into
   * its own storage; safe to free the inputs immediately. */
  bool az_iot_mock_mqtt_client_inject_message(
      az_iot_mock_mqtt_client* m,
      const char* topic,
      const uint8_t* payload,
      size_t payload_len,
      az_iot_mqtt_qos qos);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_MOCK_MQTT_IFACE_H */
