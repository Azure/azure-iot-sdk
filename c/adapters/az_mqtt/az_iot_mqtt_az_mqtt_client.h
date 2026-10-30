// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_mqtt_az_mqtt_client.h
 * @brief The az_mqtt adapter client, for one MQTT version. Included once per version, by
 * az_iot_mqtt_az_mqtt_v3.c and az_iot_mqtt_az_mqtt_v5.c, with AZ_IOT_AZ_MQTT_V set to 3 or 5;
 * everything here is static except the factory constructor.
 *
 * Single-threaded: every az_mqtt callback runs inside process_loop() or disconnect(), on the
 * caller's thread. connect() only prepares: name resolution, the socket connect, the handshakes
 * and every receive run in process_loop(). Sends (publish, subscribe, unsubscribe, disconnect)
 * are written when called; they wait only while the socket send buffer is full, at most
 * AZ_MQTT_TRANSPORT_SEND_TIMEOUT_MS.
 *
 * Events are delivered from process_loop() only. An event raised elsewhere (inside disconnect(),
 * or while the application handles an event) is held and delivered first by the next
 * process_loop() step.
 */

#include "az_iot_mqtt_az_mqtt_internal.h"
#include "internal/crypto.h" // az_iot_crypto__wipe()
#include "internal/mqtt_limits.h"

#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_log_components.h"

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_websocket.h>

#include <azure/core/az_platform.h>

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
#include "esp_crt_bundle.h"
// ESP-IDF has no system trust store: with no CA given, trust its certificate bundle.
#define _AZM_ESP_CERTIFICATE_BUNDLE 1
#endif
#endif

#if AZ_IOT_AZ_MQTT_V == 5
#include <az_mqtt5/az_mqtt5_client.h>
#include <az_mqtt5/az_mqtt5_codec.h>
#define _AZM(name) az_mqtt5_##name
#define _AZM_VERSION AZ_IOT_MQTT_VERSION_5
#define _AZM_FACTORY_CREATE az_iot_az_mqtt_factory_create_v5
#define _AZM_GET_WORKSPACE_SIZE az_iot_az_mqtt__get_workspace_size_v5
#define _AZM_INIT az_iot_az_mqtt__init_v5
#elif AZ_IOT_AZ_MQTT_V == 3
#include <az_mqtt3/az_mqtt3_client.h>
#define _AZM(name) az_mqtt3_##name
#define _AZM_VERSION AZ_IOT_MQTT_VERSION_3_1_1
#define _AZM_FACTORY_CREATE az_iot_az_mqtt_factory_create_v3_1_1
#define _AZM_GET_WORKSPACE_SIZE az_iot_az_mqtt__get_workspace_size_v3_1_1
#define _AZM_INIT az_iot_az_mqtt__init_v3_1_1
#else
#error "Define AZ_IOT_AZ_MQTT_V as 3 or 5"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/** @brief Copies of unacknowledged QoS 1/2 PUBLISH; at least one of the largest. 0: none, and a
 * connect with clean_start false (MQTT 5: and session_expiry_seconds > 0) is refused. */
#ifndef AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE
#define AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE \
  (AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD)
#endif

#if AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE != 0 \
    && AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE   \
        < AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD
#error \
    "AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE must be 0 or hold one packet of AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE"
#endif
#if AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE > 2147483647
#error "AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE must be at most 2147483647 (an az_span size)"
#endif

/** @brief Connect strings owned until the next connect: host, credentials, TLS, proxy, will... */
#define _AZM_OWNED_MAX (20 + 2 * AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX)

/** @brief Span over the bytes of an array of any element type. */
#define _AZM_SPAN_OF(ARRAY) az_span_create((uint8_t*)(ARRAY), (int32_t)sizeof(ARRAY))

/** @brief Native errors kept for the current connect attempt. */
#define _AZM_NATIVE_MAX 8

/** @brief Default keep-alive (keep_alive_seconds 0) and connect timeout (0), as Paho. */
#define _AZM_DEFAULT_KEEP_ALIVE_SECONDS 60
#define _AZM_DEFAULT_CONNECT_TIMEOUT_SECONDS 30

typedef struct
{
  az_iot_mqtt_client base; /* First: the iface pointer. */
  _AZM(client) client;
  az_mqtt_transport* transport;
  az_mqtt_websocket websocket;
  az_mqtt_tls_options tls;
  az_mqtt_proxy_options proxy;
  _AZM(will_options) will;
  char* owned[_AZM_OWNED_MAX];
  size_t owned_size[_AZM_OWNED_MAX]; /* Bytes of each, wiped before it is freed. */
  int owned_count;
  uint8_t* send_buffer;
  size_t send_buffer_size;
  uint8_t* receive_buffer;
  size_t receive_buffer_size;
  char* strings; /* receive_buffer_size + 3 bytes. */
  /* Workspace: the area of message_store_size bytes. Factory: allocated on the first connect
   * whose session outlives it. */
  uint8_t* message_storage;
  size_t message_store_size;
  az_mqtt_inflight_entry* inflight;
  size_t inflight_count;
  uint16_t max_in_flight_messages;
  /* Workspace: copies of connect strings, in place of owned[]. */
  char* connect_strings;
  size_t connect_strings_size;
  size_t connect_strings_used;
  /* Factory client (az_iot_az_mqtt_factory_create_*): the workspace, connect strings and message
   * store are allocated; destroy() frees them. */
  bool heap_mode;
  uint8_t* workspace;
  size_t workspace_size;
#if AZ_IOT_AZ_MQTT_V == 5
  az_mqtt5_user_property connect_properties[AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX];
  int32_t decode_codes[4]; /* Subscription identifiers or reason codes. */
  int32_t server_disconnect_code; /* -1: none. */
  uint8_t disconnect_reason_code;
#endif
  az_mqtt_native_error native[_AZM_NATIVE_MAX];
  int native_count;
  /* Session: connecting until CONNECTED is reported; then connected until closed. */
  bool connecting;
  bool start_pending; /* connect() called: the next process_loop() starts it. */
  int32_t connect_timeout_ms;
  bool connected;
  bool closing; /* disconnect() called. */
  bool connack_received;
  int32_t connack_code;
  bool session_present;
  /* Event delivery (see the file comment). */
  bool in_loop;
  bool delivering;
  az_iot_mqtt_event pending[AZ_IOT_AZ_MQTT_PENDING_EVENTS_MAX];
  int pending_count;
  uint32_t pending_qos0_acks;
  /* Of those, held when this process_loop() started: the only ones it delivers. */
  int due_count;
  uint32_t due_qos0_acks;
  az_iot_mqtt_event_callback callback;
  void* callback_context;
} _azm_client;

/* The limits every adapter accepts assume this store overhead. */
typedef char _azm_store_overhead_is_AZ_IOT_MQTT__MESSAGE_STORE_ENTRY_OVERHEAD
    [AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD == AZ_IOT_MQTT__MESSAGE_STORE_ENTRY_OVERHEAD ? 1 : -1];

/* AZ_IOT_AZ_MQTT_WORKSPACE_SIZE() relies on these bounds. */
typedef char _azm_client_state_fits_AZ_IOT_AZ_MQTT_CLIENT_STATE_SIZE
    [AZ_IOT_AZ_MQTT_WORKSPACE_ALIGN(sizeof(_azm_client)) <= AZ_IOT_AZ_MQTT_CLIENT_STATE_SIZE ? 1
                                                                                             : -1];
typedef char _azm_in_flight_entry_fits_AZ_IOT_AZ_MQTT_IN_FLIGHT_ENTRY_SIZE
    [sizeof(az_mqtt_inflight_entry) <= AZ_IOT_AZ_MQTT_IN_FLIGHT_ENTRY_SIZE ? 1 : -1];

static _azm_client* _azm_self(az_iot_mqtt_client* c) { return (_azm_client*)c; }

static _azm_client* _azm_of(_AZM(client) const* client)
{
  return (_azm_client*)_AZM(client_get_user_context)(client);
}

// ──────────────────────── Events ─────────────────────────────

static void _azm_deliver(_azm_client* m, az_iot_mqtt_event const* event)
{
  if (m->callback != NULL)
  {
    m->delivering = true;
    m->callback(event, m->callback_context);
    m->delivering = false;
  }
}

/**
 * @brief Deliver the held events, oldest first, then the QoS 0 publish acknowledgements. Only
 * those held when this process_loop() started (due_*): what callbacks raise waits for the next.
 */
static bool _azm_deliver_pending(_azm_client* m)
{
  bool delivered = false;
  while (m->due_count > 0 || m->due_qos0_acks > 0)
  {
    az_iot_mqtt_event event;
    if (m->due_count > 0)
    {
      event = m->pending[0];
      m->pending_count--;
      m->due_count--;
      memmove(&m->pending[0], &m->pending[1], (size_t)m->pending_count * sizeof(m->pending[0]));
    }
    else
    {
      memset(&event, 0, sizeof(event));
      event.kind = AZ_IOT_MQTT_EVT_PUBLISH_ACK;
      event.status = AZ_IOT_OK;
      m->pending_qos0_acks--;
      m->due_qos0_acks--;
    }
    _azm_deliver(m, &event);
    delivered = true;
  }
  return delivered;
}

/** @brief Deliver @p event now if inside process_loop(), else hold it. Not for messages. */
static void _azm_emit(_azm_client* m, az_iot_mqtt_event const* event)
{
  if (m->in_loop && !m->delivering)
  {
    (void)_azm_deliver_pending(m);
    _azm_deliver(m, event);
  }
  else if (m->pending_count < AZ_IOT_AZ_MQTT_PENDING_EVENTS_MAX)
  {
    m->pending[m->pending_count] = *event;
    // The reason string points into the receive buffer, which a held event outlives.
    m->pending[m->pending_count].reason_string = NULL;
    m->pending[m->pending_count].reason_string_len = 0;
    m->pending_count++;
  }
  else
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_AZ_MQTT, "event %d dropped: too many held", (int)event->kind);
  }
}

static void _azm_emit_status(
    _azm_client* m,
    az_iot_mqtt_event_kind kind,
    az_iot_result status,
    int32_t protocol_code,
    int32_t transport_code)
{
  az_iot_mqtt_event event;
  memset(&event, 0, sizeof(event));
  event.kind = kind;
  event.status = status;
  event.protocol_code = protocol_code;
  event.transport_code = transport_code;
  _azm_emit(m, &event);
}

// ──────────────────────── az_mqtt callbacks ──────────────────

/** @brief The native error behind @p rc in this connect attempt, else 0. */
static int32_t _azm_transport_code(_azm_client const* m, az_result rc)
{
  for (int i = 0; i < m->native_count; i++)
  {
    if (m->native[i].result == rc)
    {
      return m->native[i].code;
    }
  }
  return 0;
}

static void _azm_on_transport_error(_AZM(client) * client, az_mqtt_native_error const* error)
{
  _azm_client* m = _azm_of(client);
  if (m->native_count > 0 && m->native[0].connect_attempt != error->connect_attempt)
  {
    m->native_count = 0;
  }
  if (m->native_count < _AZM_NATIVE_MAX)
  {
    m->native[m->native_count++] = *error;
  }
}

static void _azm_on_connack(_AZM(client) * client, _AZM(connack_data) const* connack)
{
  _azm_client* m = _azm_of(client);
  m->connack_received = true;
#if AZ_IOT_AZ_MQTT_V == 5
  m->connack_code = (int32_t)connack->reason_code;
#else
  m->connack_code = (int32_t)connack->return_code;
#endif
  m->session_present = connack->session_present;
  if (m->connack_code == 0)
  {
    // Before anything this CONNACK lets through (resends, then messages).
    m->connecting = false;
    m->connected = true;
    az_iot_mqtt_event event;
    memset(&event, 0, sizeof(event));
    event.kind = AZ_IOT_MQTT_EVT_CONNECTED;
    event.status = AZ_IOT_OK;
    event.session_present = connack->session_present;
    _azm_emit(m, &event);
  }
}

/** @brief Called once per session, whatever ended it; the transport is closed. */
static void _azm_on_connection_closed(_AZM(client) * client, az_result reason)
{
  _azm_client* m = _azm_of(client);
  bool const was_connecting = m->connecting;
  bool const was_connected = m->connected;
  m->connecting = false;
  m->connected = false;
  if (m->closing)
  {
    if (was_connecting || was_connected)
    {
      _azm_emit_status(m, AZ_IOT_MQTT_EVT_DISCONNECTED, AZ_IOT_OK, 0, 0);
    }
    return;
  }
  if (was_connecting)
  {
    if (m->connack_received && m->connack_code != 0)
    {
      _azm_emit_status(
          m,
          AZ_IOT_MQTT_EVT_CONNECTED,
          az_iot_mqtt_connack_result(_AZM_VERSION, (int)m->connack_code),
          m->connack_code,
          0);
    }
    else
    {
      AZ_IOT_LOG_WARNF(
          AZ_IOT_LOG_COMPONENT_AZ_MQTT, "connect failed: az_result 0x%08X", (unsigned)reason);
      _azm_emit_status(
          m,
          AZ_IOT_MQTT_EVT_CONNECTED,
          az_iot_az_mqtt_session_result(reason),
          0,
          _azm_transport_code(m, reason));
    }
  }
  else if (was_connected)
  {
#if AZ_IOT_AZ_MQTT_V == 5
    if (m->server_disconnect_code >= 0)
    {
      _azm_emit_status(
          m,
          AZ_IOT_MQTT_EVT_DISCONNECTED,
          az_iot_mqtt_disconnect_result(_AZM_VERSION, (int)m->server_disconnect_code),
          m->server_disconnect_code,
          0);
      return;
    }
#endif
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_AZ_MQTT, "session ended: az_result 0x%08X", (unsigned)reason);
    _azm_emit_status(
        m,
        AZ_IOT_MQTT_EVT_DISCONNECTED,
        az_iot_az_mqtt_session_result(reason),
        0,
        _azm_transport_code(m, reason));
  }
}

#if AZ_IOT_AZ_MQTT_V == 5
static void _azm_on_disconnect(az_mqtt5_client* client, az_mqtt5_disconnect_data const* disconnect)
{
  // The session ends right after: _azm_on_connection_closed reports it with this code.
  _azm_of(client)->server_disconnect_code = (int32_t)disconnect->reason_code;
}
#endif

#if AZ_IOT_AZ_MQTT_V == 5
/** @brief az_iot_mqtt_user_properties_next_fn over the raw Properties of a received PUBLISH. */
static az_iot_result _azm_user_property_next(
    const void* source,
    size_t source_len,
    size_t* cursor,
    az_iot_mqtt_user_property_view* out)
{
  if (*cursor > source_len || source_len > INT32_MAX)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_span const properties = az_span_create((uint8_t*)(uintptr_t)source, (int32_t)source_len);
  int32_t offset = (int32_t)*cursor;
  az_mqtt5_user_property p;
  az_result const rc = az_mqtt5_codec_next_user_property(properties, &offset, &p);
  if (rc == AZ_ERROR_ITEM_NOT_FOUND)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  if (az_result_failed(rc))
  {
    return AZ_IOT_ERR_PROTOCOL;
  }
  *cursor = (size_t)offset;
  out->key = (const char*)az_span_ptr(p.key);
  out->key_len = (size_t)az_span_size(p.key);
  out->value = (const char*)az_span_ptr(p.value);
  out->value_len = (size_t)az_span_size(p.value);
  return AZ_IOT_OK;
}

/** @brief Context of _azm_publish_user_property_next(): the properties and their own cursor. */
typedef struct
{
  const az_iot_mqtt_user_properties* properties;
  size_t cursor; /**< SDK cursor (any size_t); the codec cursor only counts properties. */
} _azm_publish_user_properties;

/**
 * @brief az_mqtt5_publish_options.user_property_next over an az_iot_mqtt_user_properties.
 *
 * The codec cursor counts properties read and restarts at 0 for each encoding pass, which
 * restarts the SDK cursor.
 */
static az_result _azm_publish_user_property_next(
    void* context,
    int32_t* cursor,
    az_mqtt5_user_property* out)
{
  _azm_publish_user_properties* const ctx = (_azm_publish_user_properties*)context;
  if (*cursor < 0 || *cursor == INT32_MAX)
  {
    return AZ_ERROR_ARG;
  }
  if (*cursor == 0)
  {
    ctx->cursor = 0;
  }
  az_iot_mqtt_user_property_view p;
  az_iot_result const r = az_iot_mqtt_user_properties_next(ctx->properties, &ctx->cursor, &p);
  if (r == AZ_IOT_ERR_NOT_FOUND)
  {
    return AZ_ERROR_ITEM_NOT_FOUND;
  }
  if (r != AZ_IOT_OK || p.key_len > UINT16_MAX || p.value_len > UINT16_MAX)
  {
    return AZ_ERROR_ARG;
  }
  (*cursor)++;
  out->key = az_span_create((uint8_t*)(uintptr_t)p.key, (int32_t)p.key_len);
  out->value = az_span_create((uint8_t*)(uintptr_t)p.value, (int32_t)p.value_len);
  return AZ_OK;
}
#endif

static void _azm_on_publish(_AZM(client) * client, _AZM(publish_data) const* publish)
{
  _azm_client* m = _azm_of(client);
  az_iot_az_mqtt_string_writer strings = { m->strings, m->strings + m->receive_buffer_size + 3u };
  az_iot_mqtt_message message;
  memset(&message, 0, sizeof(message));
  message.topic = az_iot_az_mqtt_string_writer_add(&strings, publish->topic);
  message.payload = az_span_ptr(publish->payload);
  message.payload_len = (size_t)az_span_size(publish->payload);
  message.qos = (az_iot_mqtt_qos)publish->qos;
  message.retain = publish->retain;
#if AZ_IOT_AZ_MQTT_V == 5
  // A view into the receive buffer, valid while the event is delivered below.
  if (az_span_size(publish->properties) > 0)
  {
    message.user_properties.next = _azm_user_property_next;
    message.user_properties.source = az_span_ptr(publish->properties);
    message.user_properties.source_len = (size_t)az_span_size(publish->properties);
  }
  message.topic_alias = publish->topic_alias;
  message.message_expiry_seconds = publish->message_expiry_interval;
  if (az_span_size(publish->content_type) > 0)
  {
    message.content_type = az_iot_az_mqtt_string_writer_add(&strings, publish->content_type);
  }
  if (az_span_size(publish->response_topic) > 0)
  {
    message.response_topic = az_iot_az_mqtt_string_writer_add(&strings, publish->response_topic);
  }
  if (az_span_size(publish->correlation_data) > 0)
  {
    message.correlation_data = az_span_ptr(publish->correlation_data);
    message.correlation_data_len = (size_t)az_span_size(publish->correlation_data);
  }
#endif
  az_iot_mqtt_event event;
  memset(&event, 0, sizeof(event));
  event.kind = AZ_IOT_MQTT_EVT_MESSAGE;
  event.message = &message;
  // Only from process_loop, outside any event delivery: never held (message points to the stack).
  (void)_azm_deliver_pending(m);
  _azm_deliver(m, &event);
}

static void _azm_emit_publish_ack(_azm_client* m, _AZM(ack_data) const* ack)
{
#if AZ_IOT_AZ_MQTT_V == 5
  int32_t const code = (int32_t)ack->reason_code;
#else
  // MQTT 3.1.1 acknowledgements carry no code: 0x80 for one dropped unacknowledged.
  int32_t const code = az_result_failed(ack->status) ? 0x80 : 0;
#endif
  az_iot_mqtt_event event;
  memset(&event, 0, sizeof(event));
  event.kind = AZ_IOT_MQTT_EVT_PUBLISH_ACK;
  event.packet_id = ack->packet_id;
  event.protocol_code = code;
  event.status = az_iot_mqtt_puback_result(_AZM_VERSION, (int)code);
  if (event.status == AZ_IOT_OK && az_result_failed(ack->status))
  {
    event.status = AZ_IOT_ERR_MQTT;
  }
#if AZ_IOT_AZ_MQTT_V == 5
  // NULL: no Reason String. Non-NULL with length 0: present and empty.
  if (az_span_ptr(ack->reason_string) != NULL)
  {
    event.reason_string = (char const*)az_span_ptr(ack->reason_string);
    event.reason_string_len = (size_t)az_span_size(ack->reason_string);
  }
#endif
  _azm_emit(m, &event);
}

static void _azm_on_puback(_AZM(client) * client, _AZM(ack_data) const* ack)
{
  _azm_emit_publish_ack(_azm_of(client), ack);
}

static void _azm_on_pubcomp(_AZM(client) * client, _AZM(ack_data) const* ack)
{
  _azm_client* m = _azm_of(client);
  // An incoming QoS 2 exchange completed (PUBREL received): no event.
  if (!ack->incoming)
  {
    _azm_emit_publish_ack(m, ack);
  }
}

static void _azm_on_suback(_AZM(client) * client, _AZM(suback_data) const* suback)
{
  _azm_client* m = _azm_of(client);
#if AZ_IOT_AZ_MQTT_V == 5
  int32_t const code = suback->reason_code_count > 0 ? (int32_t)suback->reason_codes[0] : 0x80;
#else
  int32_t const code = az_span_size(suback->return_codes) > 0
      ? (int32_t)az_span_ptr(suback->return_codes)[0]
      : 0x80;
#endif
  az_iot_mqtt_event event;
  memset(&event, 0, sizeof(event));
  event.kind = AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK;
  event.packet_id = suback->packet_id;
  event.protocol_code = code;
  event.status = az_iot_mqtt_suback_result(_AZM_VERSION, (int)code);
  _azm_emit(m, &event);
}

#if AZ_IOT_AZ_MQTT_V == 5
static void _azm_on_unsuback(az_mqtt5_client* client, az_mqtt5_suback_data const* unsuback)
{
  int32_t const code = unsuback->reason_code_count > 0 ? (int32_t)unsuback->reason_codes[0] : 0;
  az_iot_mqtt_event event;
  memset(&event, 0, sizeof(event));
  event.kind = AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK;
  event.packet_id = unsuback->packet_id;
  event.protocol_code = code;
  event.status = code >= 0x80 ? AZ_IOT_ERR_MQTT : AZ_IOT_OK;
  _azm_emit(_azm_of(client), &event);
}
#else
static void _azm_on_unsuback(az_mqtt3_client* client, az_mqtt3_ack_data const* unsuback)
{
  az_iot_mqtt_event event;
  memset(&event, 0, sizeof(event));
  event.kind = AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK;
  event.packet_id = unsuback->packet_id;
  event.status = AZ_IOT_OK;
  _azm_emit(_azm_of(client), &event);
}
#endif

// ──────────────────────── Connect ────────────────────────────

/** @brief Wipes the connect strings (credentials) and releases them. */
static void _azm_release_owned(_azm_client* m)
{
  if (!m->heap_mode)
  {
    az_iot_crypto__wipe(m->connect_strings, m->connect_strings_used);
    m->connect_strings_used = 0;
    return;
  }
  for (int i = 0; i < m->owned_count; i++)
  {
    az_iot_crypto__wipe(m->owned[i], m->owned_size[i]);
    free(m->owned[i]);
  }
  m->owned_count = 0;
}

/**
 * @brief A copy of @p size bytes of @p data, plus a NUL when @p nul, owned until the next connect:
 * in the connect strings area (workspace) or allocated (factory). NULL when it does not fit.
 */
static char* _azm_copy(_azm_client* m, void const* data, size_t size, bool nul)
{
  size_t const total = size + (nul ? 1u : 0u);
  char* copy;
  if (!m->heap_mode)
  {
    if (total > m->connect_strings_size - m->connect_strings_used)
    {
      return NULL;
    }
    copy = m->connect_strings + m->connect_strings_used;
    m->connect_strings_used += total;
  }
  else
  {
    if (m->owned_count >= _AZM_OWNED_MAX)
    {
      return NULL;
    }
    copy = (char*)malloc(total);
    if (copy == NULL)
    {
      return NULL;
    }
    m->owned_size[m->owned_count] = total;
    m->owned[m->owned_count++] = copy;
  }
  memcpy(copy, data, size);
  if (nul)
  {
    copy[size] = '\0';
  }
  return copy;
}

/** @brief A copy of @p s owned until the next connect, as a span; empty for NULL or "". */
static bool _azm_own(_azm_client* m, const char* s, az_span* out)
{
  *out = AZ_SPAN_EMPTY;
  if (!az_iot_az_mqtt_has_text(s))
  {
    return true;
  }
  size_t const size = strlen(s);
  char* copy = size < (size_t)INT32_MAX ? _azm_copy(m, s, size, true) : NULL;
  if (copy == NULL)
  {
    return false;
  }
  *out = az_span_create((uint8_t*)copy, (int32_t)size);
  return true;
}

static bool _azm_own_bytes(_azm_client* m, uint8_t const* data, size_t size, az_span* out)
{
  *out = AZ_SPAN_EMPTY;
  if (data == NULL || size == 0)
  {
    return true;
  }
  char* copy = size <= (size_t)INT32_MAX ? _azm_copy(m, data, size, false) : NULL;
  if (copy == NULL)
  {
    return false;
  }
  *out = az_span_create((uint8_t*)copy, (int32_t)size);
  return true;
}

/** @brief Whether the session outlives the connection, so az_mqtt keeps QoS 1/2 copies. */
static bool _azm_session_persists(az_iot_mqtt_connect_options const* o)
{
#if AZ_IOT_AZ_MQTT_V == 5
  return !o->clean_start && o->session_expiry_seconds > 0;
#else
  return !o->clean_start;
#endif
}

/** @brief Whether @p qos is 0, 1 or 2. */
static bool _azm_qos_valid(az_iot_mqtt_qos qos) { return (unsigned)qos <= AZ_IOT_MQTT_QOS_2; }

static az_iot_result _azm_check_options(az_iot_mqtt_connect_options const* o)
{
  // Fail closed: a transport, proxy or credential this adapter cannot honour is refused.
  if (o->transport != AZ_IOT_MQTT_TRANSPORT_TCP && o->transport != AZ_IOT_MQTT_TRANSPORT_WEBSOCKET)
  {
    AZ_IOT_LOG_ERRORF(AZ_IOT_LOG_COMPONENT_AZ_MQTT, "unsupported transport %d", (int)o->transport);
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  if (o->tls.sign != NULL || o->tls.client_key_password != NULL)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_AZ_MQTT, "sign callback and key password are not supported");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  if (o->tls.client_key_psa_id != 0)
  {
#if defined(AZ_IOT_AZ_MQTT_MBEDTLS)
    // A PSA key replaces the key PEM, path and URI, and signs for a client certificate.
    if (o->tls.client_key_path != NULL || o->tls.client_key_pem != NULL
        || o->tls.client_key_uri != NULL
        || (o->tls.client_cert_path == NULL && o->tls.client_cert_pem == NULL))
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
#else
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_AZ_MQTT, "PSA client keys need the mbedTLS backend");
    return AZ_IOT_ERR_NOT_SUPPORTED;
#endif
  }
  if (o->tls.client_key_uri != NULL || o->tls.crypto_engine_id != NULL)
  {
    if (!az_iot_az_mqtt_has_text(o->tls.client_key_uri)
        || !az_iot_az_mqtt_has_text(o->tls.crypto_engine_id) || o->tls.client_key_path != NULL
        || o->tls.client_key_pem != NULL)
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
    az_iot_result const rc = az_iot_az_mqtt_load_key_provider(o->tls.crypto_engine_id);
    if (rc != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERRORF(
          AZ_IOT_LOG_COMPONENT_AZ_MQTT, "key provider '%s' not available", o->tls.crypto_engine_id);
      return rc;
    }
  }
  // Pointer and count pairs (MQTT 3.1.1 ignores user properties).
  if ((o->lwt.payload == NULL && o->lwt.payload_len > 0) || o->lwt.payload_len > (size_t)INT32_MAX)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  // A Will Topic is a Topic Name: no wildcards.
  if (az_iot_az_mqtt_has_text(o->lwt.topic)
      && (!_azm_qos_valid(o->lwt.qos) || strpbrk(o->lwt.topic, "+#") != NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
#if AZ_IOT_AZ_MQTT_V == 3
  // MQTT 3.1.1 3.1.2.9: no password without a user name (MQTT 5 allows it).
  if (az_iot_az_mqtt_has_text(o->password) && !az_iot_az_mqtt_has_text(o->username))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
#endif
#if AZ_IOT_AZ_MQTT_V == 5
  if (o->user_properties == NULL && o->user_properties_count > 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (o->user_properties_count > AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
#endif
  return AZ_IOT_OK;
}

#if defined(_AZM_ESP_CERTIFICATE_BUNDLE)
/** @brief az_mqtt_tls_configure_fn: trust the ESP-IDF certificate bundle. */
static az_result _azm_attach_esp_certificate_bundle(void* native_config, void* context)
{
  (void)context;
  return esp_crt_bundle_attach(native_config) == ESP_OK ? AZ_OK : AZ_ERROR_NOT_SUPPORTED;
}
#endif

/** @brief Copy what az_mqtt reads during the session into m (TLS, proxy, will, CONNECT). */
static bool _azm_copy_options(
    _azm_client* m,
    az_iot_mqtt_connect_options const* o,
    _AZM(client_options) * options)
{
  bool ok = true;
  az_iot_mqtt_tls_options const* t = &o->tls;
  m->tls = az_mqtt_tls_options_default();
  ok = ok && _azm_own(m, t->trusted_ca_path, &m->tls.ca_cert_path);
  ok = ok && _azm_own(m, t->client_cert_path, &m->tls.client_cert_path);
  ok = ok && _azm_own(m, t->client_key_path, &m->tls.client_key_path);
  ok = ok && _azm_own(m, t->client_key_uri, &m->tls.client_key_uri);
  // A path wins over in-memory PEM for the same item.
  ok = ok && _azm_own(m, t->trusted_ca_path ? NULL : t->trusted_ca_pem, &m->tls.ca_cert_pem);
  ok = ok && _azm_own(m, t->client_cert_path ? NULL : t->client_cert_pem, &m->tls.client_cert_pem);
  ok = ok && _azm_own(m, t->client_key_path ? NULL : t->client_key_pem, &m->tls.client_key_pem);
  m->tls.client_key_psa_id = t->client_key_psa_id;
  bool const tls = t->use_tls || t->trusted_ca_path || t->trusted_ca_pem || t->client_cert_path
      || t->client_cert_pem || t->client_key_path || t->client_key_pem || t->client_key_uri
      || t->client_key_psa_id != 0;
#if defined(_AZM_ESP_CERTIFICATE_BUNDLE)
  // Without the bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=n), az_mqtt refuses TLS with no CA.
  if (tls && az_span_size(m->tls.ca_cert_path) == 0 && az_span_size(m->tls.ca_cert_pem) == 0)
  {
    m->tls.configure = _azm_attach_esp_certificate_bundle;
  }
#endif
  options->tls_options = tls ? &m->tls : NULL;

  bool const websocket = o->transport == AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;
  if (o->port != 0)
  {
    options->port = o->port;
  }
  else if (websocket)
  {
    options->port = (uint16_t)(tls ? AZ_IOT_MQTT_DEFAULT_PORT_WEBSOCKET_TLS
                                   : AZ_IOT_MQTT_DEFAULT_PORT_WEBSOCKET_PLAIN);
  }
  else
  {
    options->port
        = (uint16_t)(tls ? AZ_IOT_MQTT_DEFAULT_PORT_TCP_TLS : AZ_IOT_MQTT_DEFAULT_PORT_TCP_PLAIN);
  }

  ok = ok && _azm_own(m, o->host, &options->hostname);

  memset(&m->proxy, 0, sizeof(m->proxy));
  if (az_iot_az_mqtt_has_text(o->proxy.host))
  {
    ok = ok && _azm_own(m, o->proxy.host, &m->proxy.host);
    ok = ok && _azm_own(m, o->proxy.username, &m->proxy.username);
    ok = ok && _azm_own(m, o->proxy.password, &m->proxy.password);
    m->proxy.port = o->proxy.port != 0 ? o->proxy.port : (uint16_t)AZ_IOT_MQTT_DEFAULT_PROXY_PORT;
    options->proxy_options = &m->proxy;
  }

  _AZM(connect_options)* c = &options->connect_options;
  *c = _AZM(connect_options_default)();
  ok = ok && _azm_own(m, o->client_id, &c->client_id);
  ok = ok && _azm_own(m, o->username, &c->username);
  ok = ok && _azm_own(m, o->password, &c->password);
  c->keep_alive_seconds
      = o->keep_alive_seconds != 0 ? o->keep_alive_seconds : _AZM_DEFAULT_KEEP_ALIVE_SECONDS;
#if AZ_IOT_AZ_MQTT_V == 5
  c->clean_start = o->clean_start;
  c->session_expiry_interval = o->session_expiry_seconds;
  // Below the entry count, so az_mqtt keeps this many entries for inbound QoS 2.
  c->receive_maximum = m->max_in_flight_messages;
  // Omitted, it would mean any size; larger packets do not fit the receive buffer.
  c->maximum_packet_size = (uint32_t)m->receive_buffer_size;
  int32_t count = 0;
  for (size_t i = 0; i < o->user_properties_count; i++)
  {
    if (o->user_properties[i].key == NULL)
    {
      continue;
    }
    ok = ok && _azm_own(m, o->user_properties[i].key, &m->connect_properties[count].key);
    ok = ok && _azm_own(m, o->user_properties[i].value, &m->connect_properties[count].value);
    count++;
  }
  c->user_properties = count > 0 ? m->connect_properties : NULL;
  c->user_property_count = count;
#else
  c->clean_session = o->clean_start;
#endif

  if (az_iot_az_mqtt_has_text(o->lwt.topic))
  {
    memset(&m->will, 0, sizeof(m->will));
    ok = ok && _azm_own(m, o->lwt.topic, &m->will.topic);
    ok = ok && _azm_own_bytes(m, o->lwt.payload, o->lwt.payload_len, &m->will.payload);
    m->will.qos = (az_mqtt_qos)o->lwt.qos;
    m->will.retain = o->lwt.retain;
#if AZ_IOT_AZ_MQTT_V == 5
    m->will.will_delay_interval = o->lwt.will_delay_seconds;
#endif
    c->will = &m->will;
  }
  return ok;
}

static az_iot_result _azm_connect(az_iot_mqtt_client* self, az_iot_mqtt_connect_options const* o)
{
  if (self == NULL || o == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  _azm_client* m = _azm_self(self);
  if (m->connecting || m->connected)
  {
    return AZ_IOT_ERR_BUSY;
  }
  // The previous connect's copies are unused from here: released even if this one is refused.
  _azm_release_owned(m);
  if (o->host == NULL || o->client_id == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_result rc = _azm_check_options(o);
  if (rc != AZ_IOT_OK)
  {
    return rc;
  }

  bool const persists = _azm_session_persists(o);
  if (persists && m->message_store_size == 0)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_AZ_MQTT,
        "connect: clean_start false (MQTT 5: with session_expiry_seconds > 0) needs a message "
        "store (az_iot_mqtt_limits.message_store_size; factory: "
        "AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE)");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  if (persists && m->message_storage == NULL) /* Factory: allocated on first need. */
  {
    m->message_storage = (uint8_t*)malloc(m->message_store_size);
    if (m->message_storage == NULL)
    {
      return AZ_IOT_ERR_OUT_OF_MEMORY;
    }
  }

  _AZM(client_options) options;
  memset(&options, 0, sizeof(options));
  if (!_azm_copy_options(m, o, &options))
  {
    _azm_release_owned(m);
    return m->heap_mode ? AZ_IOT_ERR_OUT_OF_MEMORY : AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  options.send_buffer = az_span_create(m->send_buffer, (int32_t)m->send_buffer_size);
  options.receive_buffer = az_span_create(m->receive_buffer, (int32_t)m->receive_buffer_size);
  options.inflight_control_buffer = az_span_create(
      (uint8_t*)m->inflight, (int32_t)(m->inflight_count * sizeof(az_mqtt_inflight_entry)));
  options.inflight_message_buffer = persists
      ? az_span_create(m->message_storage, (int32_t)m->message_store_size)
      : AZ_SPAN_EMPTY;
  options.user_context = m;
  options.on_connack = _azm_on_connack;
  options.on_publish = _azm_on_publish;
  options.on_suback = _azm_on_suback;
  options.on_unsuback = _azm_on_unsuback;
  options.on_puback = _azm_on_puback;
  options.on_pubcomp = _azm_on_pubcomp;
  options.on_connection_closed = _azm_on_connection_closed;
  options.on_transport_error = _azm_on_transport_error;
#if AZ_IOT_AZ_MQTT_V == 5
  options.on_disconnect = _azm_on_disconnect;
  // User Properties are read from the raw Properties of a PUBLISH; those of other packets are not
  // used.
  options.decode_user_properties = AZ_SPAN_EMPTY;
  options.decode_codes = _AZM_SPAN_OF(m->decode_codes);
  m->server_disconnect_code = -1;
  m->disconnect_reason_code = o->disconnect_reason_code;
#endif

  az_result result = az_mqtt_transport_init(m->transport);
  options.transport = m->transport;
  if (az_result_succeeded(result) && o->transport == AZ_IOT_MQTT_TRANSPORT_WEBSOCKET)
  {
    az_mqtt_websocket_options websocket = az_mqtt_websocket_options_default();
    if (!_azm_own(
            m,
            o->websocket_path != NULL ? o->websocket_path : AZ_IOT_MQTT_DEFAULT_WEBSOCKET_PATH,
            &websocket.path))
    {
      _azm_release_owned(m);
      return m->heap_mode ? AZ_IOT_ERR_OUT_OF_MEMORY : AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    result = az_mqtt_websocket_init(&m->websocket, m->transport, &websocket);
    options.transport = az_mqtt_websocket_get_transport(&m->websocket);
  }
  if (az_result_succeeded(result))
  {
    result = _AZM(client_init)(&m->client, &options);
  }
  if (az_result_failed(result))
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_AZ_MQTT, "client setup failed: az_result 0x%08X", (unsigned)result);
    _azm_release_owned(m);
    return result == AZ_MQTT_ERROR_INVALID_CONFIG ? AZ_IOT_ERR_INVALID_ARG
                                                  : az_iot_az_mqtt_request_result(result);
  }

  AZ_IOT_LOG_INFOF(
      AZ_IOT_LOG_COMPONENT_AZ_MQTT,
      "connecting to %s:%u as '%s' (%s%s, proxy=%s)",
      o->host,
      (unsigned)options.port,
      o->client_id,
      o->transport == AZ_IOT_MQTT_TRANSPORT_WEBSOCKET ? "websocket" : "tcp",
      options.tls_options != NULL ? "+tls" : "",
      az_iot_az_mqtt_has_text(o->proxy.host) ? o->proxy.host : "none");

  m->closing = false;
  m->connack_received = false;
  m->connack_code = 0;
  m->session_present = false;
  m->native_count = 0;
  uint32_t const timeout_seconds = o->connect_timeout_seconds != 0
      ? o->connect_timeout_seconds
      : _AZM_DEFAULT_CONNECT_TIMEOUT_SECONDS;
  m->connect_timeout_ms = timeout_seconds > (uint32_t)(INT32_MAX / 1000)
      ? INT32_MAX
      : (int32_t)(timeout_seconds * 1000u);
  // Name resolution and the socket connect run in process_loop(), not here.
  m->connecting = true;
  m->start_pending = true;
  return AZ_IOT_OK;
}

// ──────────────────────── Requests ───────────────────────────

static az_iot_result _azm_disconnect(az_iot_mqtt_client* self)
{
  if (self == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  _azm_client* m = _azm_self(self);
  if (!m->connecting && !m->connected)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  m->closing = true;
  m->start_pending = false;
#if AZ_IOT_AZ_MQTT_V == 5
  az_result const rc
      = az_mqtt5_client_disconnect(&m->client, (az_mqtt5_reason_code)m->disconnect_reason_code);
#else
  az_result const rc = az_mqtt3_client_disconnect(&m->client);
#endif
  if (az_result_failed(rc))
  {
    AZ_IOT_LOG_WARNF(AZ_IOT_LOG_COMPONENT_AZ_MQTT, "disconnect: az_result 0x%08X", (unsigned)rc);
  }
  if (m->connecting || m->connected)
  {
    // Not reported by on_connection_closed: report it here.
    m->connecting = false;
    m->connected = false;
    _azm_emit_status(m, AZ_IOT_MQTT_EVT_DISCONNECTED, AZ_IOT_OK, 0, 0);
  }
  return AZ_IOT_OK;
}

static az_iot_result _azm_subscribe(
    az_iot_mqtt_client* self,
    const char* topic_filter,
    az_iot_mqtt_qos qos,
    uint16_t* out_packet_id)
{
  if (self == NULL || topic_filter == NULL || topic_filter[0] == '\0' || !_azm_qos_valid(qos))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  _azm_client* m = _azm_self(self);
  if (!m->connected)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  _AZM(subscription) subscription;
  memset(&subscription, 0, sizeof(subscription));
  subscription.topic_filter = az_span_create_from_str((char*)(uintptr_t)topic_filter);
  subscription.qos = (az_mqtt_qos)qos;
  uint16_t packet_id = 0;
  az_iot_result const rc = az_iot_az_mqtt_request_result(
      _AZM(client_subscribe)(&m->client, &subscription, 1, &packet_id));
  if (rc == AZ_IOT_OK && out_packet_id != NULL)
  {
    *out_packet_id = packet_id;
  }
  return rc;
}

static az_iot_result _azm_unsubscribe(
    az_iot_mqtt_client* self,
    const char* topic_filter,
    uint16_t* out_packet_id)
{
  if (self == NULL || topic_filter == NULL || topic_filter[0] == '\0')
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  _azm_client* m = _azm_self(self);
  if (!m->connected)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  az_span filter = az_span_create_from_str((char*)(uintptr_t)topic_filter);
  uint16_t packet_id = 0;
  az_iot_result const rc
      = az_iot_az_mqtt_request_result(_AZM(client_unsubscribe)(&m->client, &filter, 1, &packet_id));
  if (rc == AZ_IOT_OK && out_packet_id != NULL)
  {
    *out_packet_id = packet_id;
  }
  return rc;
}

static az_iot_result _azm_publish(
    az_iot_mqtt_client* self,
    az_iot_mqtt_message const* msg,
    uint16_t* out_packet_id)
{
  if (self == NULL || msg == NULL || msg->topic == NULL || !_azm_qos_valid(msg->qos)
      || (msg->payload == NULL && msg->payload_len > 0) || msg->payload_len > (size_t)INT32_MAX)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  _azm_client* m = _azm_self(self);
  if (!m->connected)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  _AZM(publish_options) publish = _AZM(publish_options_default)();
#if AZ_IOT_AZ_MQTT_V == 5
  _azm_publish_user_properties user_properties = { &msg->user_properties, 0 };
#endif
  publish.topic = az_span_create_from_str((char*)(uintptr_t)msg->topic);
  publish.payload = az_span_create((uint8_t*)(uintptr_t)msg->payload, (int32_t)msg->payload_len);
  publish.qos = (az_mqtt_qos)msg->qos;
  publish.retain = msg->retain;
#if AZ_IOT_AZ_MQTT_V == 5
  // A Response Topic is a non-empty Topic Name.
  if ((msg->correlation_data == NULL && msg->correlation_data_len > 0)
      || (msg->response_topic != NULL
          && (msg->response_topic[0] == '\0' || strpbrk(msg->response_topic, "+#") != NULL)))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (msg->correlation_data_len > (size_t)INT32_MAX)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  // Read while encoding: no copy, no limit but the send buffer.
  if (msg->user_properties.next != NULL)
  {
    publish.user_property_next = _azm_publish_user_property_next;
    publish.user_property_context = &user_properties;
  }
  publish.topic_alias = msg->topic_alias;
  publish.message_expiry_interval = msg->message_expiry_seconds;
  if (msg->content_type != NULL)
  {
    publish.content_type = az_span_create_from_str((char*)(uintptr_t)msg->content_type);
  }
  if (msg->response_topic != NULL)
  {
    publish.response_topic = az_span_create_from_str((char*)(uintptr_t)msg->response_topic);
  }
  if (msg->correlation_data != NULL && msg->correlation_data_len > 0)
  {
    publish.correlation_data = az_span_create(
        (uint8_t*)(uintptr_t)msg->correlation_data, (int32_t)msg->correlation_data_len);
  }
#endif
  uint16_t packet_id = 0;
  az_iot_result const rc
      = az_iot_az_mqtt_request_result(_AZM(client_publish)(&m->client, &publish, &packet_id));
  if (rc != AZ_IOT_OK)
  {
    return rc;
  }
  if (msg->qos == AZ_IOT_MQTT_QOS_0)
  {
    m->pending_qos0_acks++; // Sent: acknowledged by the next process_loop().
  }
  if (out_packet_id != NULL)
  {
    *out_packet_id = packet_id;
  }
  return AZ_IOT_OK;
}

static az_iot_result _azm_process_loop(az_iot_mqtt_client* self, uint32_t timeout_ms)
{
  if (self == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  _azm_client* m = _azm_self(self);
  int32_t const timeout = timeout_ms > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)timeout_ms;
  m->in_loop = true;
  m->due_count = m->pending_count;
  m->due_qos0_acks = m->pending_qos0_acks;
  bool delivered = _azm_deliver_pending(m);
  if (m->start_pending)
  {
    m->start_pending = false;
    az_result const rc = _AZM(client_connect_start)(&m->client, m->connect_timeout_ms);
    if (az_result_failed(rc) && m->connecting)
    {
      // Refused before the session started (e.g. CONNECT larger than the send buffer), so the
      // close callback did not run.
      _azm_on_connection_closed(&m->client, rc);
    }
  }
  if (m->connecting || m->connected)
  {
    // A failure ends the session; _azm_on_connection_closed reports it.
    az_result const rc = _AZM(client_process_loop)(&m->client, timeout);
    (void)rc;
    delivered = _azm_deliver_pending(m) || delivered;
  }
  else if (!delivered && timeout > 0)
  {
    az_result const rc = az_platform_sleep_msec(timeout);
    (void)rc;
  }
  m->in_loop = false;
  return AZ_IOT_OK;
}

static void _azm_set_inbound_cb(
    az_iot_mqtt_client* self,
    az_iot_mqtt_event_callback callback,
    void* user_context)
{
  if (self != NULL)
  {
    _azm_self(self)->callback = callback;
    _azm_self(self)->callback_context = user_context;
  }
}

static void _azm_destroy(az_iot_mqtt_client* self)
{
  if (self == NULL)
  {
    return;
  }
  _azm_client* m = _azm_self(self);
  if (m->connecting || m->connected)
  {
    m->callback = NULL;
    (void)_azm_disconnect(self);
  }
  _azm_release_owned(m);
  /* The client lives in the workspace: read what is needed before wiping it. */
  bool const heap_mode = m->heap_mode;
  uint8_t* const workspace = m->workspace;
  size_t const workspace_size = m->workspace_size;
  if (heap_mode)
  {
    free(m->message_storage);
  }
  /* Client state, the CONNECT (credentials), received data, connect string copies. */
  az_iot_crypto__wipe(workspace, workspace_size);
  if (heap_mode)
  {
    free(workspace);
  }
}

static az_iot_mqtt_iface const _azm_iface = {
  .version = _AZM_VERSION,
  .connect = _azm_connect,
  .disconnect = _azm_disconnect,
  .subscribe = _azm_subscribe,
  .unsubscribe = _azm_unsubscribe,
  .publish = _azm_publish,
  .process_loop = _azm_process_loop,
  .set_inbound_cb = _azm_set_inbound_cb,
  .destroy = _azm_destroy,
};

/** @brief Offsets of the parts of a workspace, from its 16-byte aligned start. */
typedef struct
{
  size_t inflight;
  size_t inflight_count;
  size_t send;
  size_t receive;
  size_t strings;
  size_t transport;
  size_t connect_strings;
  size_t store;
  size_t total; /**< Workspace bytes, alignment slack included. */
} _azm_layout;

/** @brief Adds @p size, rounded up to 16, to @p offset; false on overflow. */
static bool _azm_layout_add(size_t* offset, size_t size)
{
  size_t const rounded = size + 15u;
  if (rounded < size || *offset > SIZE_MAX - (rounded & ~(size_t)15u))
  {
    return false;
  }
  *offset += rounded & ~(size_t)15u;
  return true;
}

/** @brief The workspace parts for @p limits; false when they are invalid. */
static bool _azm_layout_of(az_iot_mqtt_limits const* limits, _azm_layout* out)
{
  if (!az_iot_mqtt__limits_are_valid(limits))
  {
    return false;
  }
  int32_t const transport_size = az_mqtt_transport_sizeof();
  if (transport_size <= 0)
  {
    return false;
  }
#if AZ_IOT_AZ_MQTT_V == 5
  /* As many again for inbound QoS 2, which the server may send up to Receive Maximum of. */
  out->inflight_count = 2u * (size_t)limits->max_in_flight_messages;
#else
  out->inflight_count = (size_t)limits->max_in_flight_messages;
#endif
  size_t offset = 0;
  bool ok = _azm_layout_add(&offset, sizeof(_azm_client));
  out->inflight = offset;
  ok = ok && _azm_layout_add(&offset, out->inflight_count * sizeof(az_mqtt_inflight_entry));
  out->send = offset;
  ok = ok && _azm_layout_add(&offset, limits->send_buffer_size);
  out->receive = offset;
  ok = ok && _azm_layout_add(&offset, limits->receive_buffer_size);
  out->strings = offset;
  /* The strings of one received PUBLISH (topic, Content Type, Response Topic), one NUL each. */
  ok = ok && _azm_layout_add(&offset, limits->receive_buffer_size + 3u);
  out->transport = offset;
  ok = ok && _azm_layout_add(&offset, (size_t)transport_size);
  out->connect_strings = offset;
  ok = ok && _azm_layout_add(&offset, limits->connect_strings_size);
  out->store = offset;
  ok = ok && _azm_layout_add(&offset, limits->message_store_size);
  ok = ok && offset <= SIZE_MAX - 15u;
  out->total = offset + 15u; /* The workspace may start anywhere. */
  return ok;
}

size_t _AZM_GET_WORKSPACE_SIZE(az_iot_mqtt_limits const* limits)
{
  _azm_layout layout;
  return _azm_layout_of(limits, &layout) ? layout.total : 0;
}

az_iot_result _AZM_INIT(
    az_iot_mqtt_limits const* limits,
    uint8_t* workspace,
    size_t workspace_size,
    az_iot_mqtt_client** out_client)
{
  if (out_client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  *out_client = NULL;
  _azm_layout layout;
  if (workspace == NULL || !_azm_layout_of(limits, &layout))
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_AZ_MQTT, "init: no workspace or invalid limits");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (workspace_size < layout.total)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_AZ_MQTT,
        "init: the workspace has %lu bytes; these limits need %lu",
        (unsigned long)workspace_size,
        (unsigned long)layout.total);
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  uint8_t* const base = (uint8_t*)(((uintptr_t)workspace + 15u) & ~(uintptr_t)15u);
  memset(base, 0, layout.send); /* Client state and in-flight entries. */
  _azm_client* m = (_azm_client*)(void*)base;
  m->base.iface = &_azm_iface;
  m->workspace = workspace;
  m->workspace_size = workspace_size;
  m->inflight = (az_mqtt_inflight_entry*)(void*)(base + layout.inflight);
  m->inflight_count = layout.inflight_count;
  m->max_in_flight_messages = limits->max_in_flight_messages;
  m->send_buffer = base + layout.send;
  m->send_buffer_size = limits->send_buffer_size;
  m->receive_buffer = base + layout.receive;
  m->receive_buffer_size = limits->receive_buffer_size;
  m->strings = (char*)(base + layout.strings);
  m->transport = (az_mqtt_transport*)(void*)(base + layout.transport);
  m->connect_strings = (char*)(base + layout.connect_strings);
  m->connect_strings_size = limits->connect_strings_size;
  m->message_store_size = limits->message_store_size;
  m->message_storage = limits->message_store_size > 0 ? base + layout.store : NULL;
  *out_client = &m->base;
  return AZ_IOT_OK;
}

/** @brief A factory client: the workspace and, as needed, connect strings and store allocated. */
static az_iot_mqtt_client* _azm_create(void* factory_context)
{
  (void)factory_context;
  az_iot_mqtt_limits const limits = {
    .send_buffer_size = AZ_IOT_AZ_MQTT_SEND_BUFFER_SIZE,
    .receive_buffer_size = AZ_IOT_AZ_MQTT_RECEIVE_BUFFER_SIZE,
    .max_in_flight_messages = (uint16_t)AZ_IOT_AZ_MQTT_INFLIGHT_MAX,
    .message_store_size = 0, /* Allocated on the first connect that needs it. */
    .connect_strings_size = 0, /* Each copy allocated. */
  };
  size_t const size = _AZM_GET_WORKSPACE_SIZE(&limits);
  uint8_t* workspace = size > 0 ? (uint8_t*)malloc(size) : NULL;
  az_iot_mqtt_client* client = NULL;
  if (workspace == NULL || _AZM_INIT(&limits, workspace, size, &client) != AZ_IOT_OK)
  {
    free(workspace);
    return NULL;
  }
  _azm_client* m = _azm_self(client);
  m->heap_mode = true;
  m->message_store_size = AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE;
  return client;
}

az_iot_mqtt_factory* _AZM_FACTORY_CREATE(void)
{
  az_iot_mqtt_factory* factory = (az_iot_mqtt_factory*)calloc(1, sizeof(*factory));
  if (factory != NULL)
  {
    factory->version = _AZM_VERSION;
    factory->create = _azm_create;
    factory->factory_ctx = factory;
  }
  return factory;
}
