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
    char username[256]; /* CONNECT username, "" if none */
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

  /* Recorded call history. Newest call is at index (count - 1). */
  size_t az_iot_mock_mqtt_client_call_count(const az_iot_mock_mqtt_client* m);
  const az_iot_mock_call* az_iot_mock_mqtt_client_call_at(
      const az_iot_mock_mqtt_client* m,
      size_t i);
  void az_iot_mock_mqtt_client_clear_calls(az_iot_mock_mqtt_client* m);

  /* Script the next return code for the given call kind. By default every call
   * returns AZ_IOT_OK. The override is one-shot: it applies to the next call of
   * the kind, then reverts to AZ_IOT_OK. */
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
