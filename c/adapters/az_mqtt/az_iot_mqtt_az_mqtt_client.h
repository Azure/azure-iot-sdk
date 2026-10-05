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

#include "azure/iot/adapters/az_iot_adapter_az_mqtt.h"
#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_log_components.h"

#include <az_mqtt/az_mqtt_transport.h>
#include <az_mqtt/az_mqtt_websocket.h>

#include <azure/core/az_platform.h>

#if AZ_IOT_AZ_MQTT_V == 5
#include <az_mqtt5/az_mqtt5_client.h>
#define _AZM(name) az_mqtt5_##name
#define _AZM_VERSION AZ_IOT_MQTT_VERSION_5
#define _AZM_FACTORY_CREATE az_iot_az_mqtt_factory_create_v5
#elif AZ_IOT_AZ_MQTT_V == 3
#include <az_mqtt3/az_mqtt3_client.h>
#define _AZM(name) az_mqtt3_##name
#define _AZM_VERSION AZ_IOT_MQTT_VERSION_3_1_1
#define _AZM_FACTORY_CREATE az_iot_az_mqtt_factory_create_v3_1_1
#else
#error "Define AZ_IOT_AZ_MQTT_V as 3 or 5"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/** @brief Copies of unacknowledged QoS 1/2 PUBLISH; at least one of the largest. */
#ifndef AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE
#define AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE \
  (AZ_IOT_AZ_MQTT_BUFFER_SIZE + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD)
#endif

#if AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE \
    < AZ_IOT_AZ_MQTT_BUFFER_SIZE + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD
#error "AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE must hold one packet of AZ_IOT_AZ_MQTT_BUFFER_SIZE"
#endif

/** @brief Room for the strings of one received PUBLISH: its bytes, plus one NUL per string. */
#define _AZM_STRINGS_SIZE (AZ_IOT_AZ_MQTT_BUFFER_SIZE + 3 + 2 * AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX)

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
  int owned_count;
  uint8_t* send_buffer;
  uint8_t* receive_buffer;
  char* strings;
  uint8_t* message_storage; /* Allocated on the first connect whose session outlives it. */
  az_mqtt_inflight_entry inflight[AZ_IOT_AZ_MQTT_INFLIGHT_MAX];
#if AZ_IOT_AZ_MQTT_V == 5
  az_mqtt5_user_property connect_properties[AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX];
  az_mqtt5_user_property publish_properties[AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX];
  az_mqtt5_user_property received_properties[AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX];
  az_mqtt5_user_property ack_properties[AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX];
  az_mqtt5_reason_code suback_codes[4];
  int32_t subscription_ids[4];
  int32_t server_disconnect_code; /* -1: none. */
  uint8_t disconnect_reason_code;
#endif
  az_iot_mqtt_user_property message_properties[AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX];
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
  az_iot_mqtt_event_callback callback;
  void* callback_context;
} _azm_client;

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
 * those held on entry: what the callbacks raise is held for the next call.
 */
static bool _azm_deliver_pending(_azm_client* m)
{
  bool delivered = false;
  int events = m->pending_count;
  uint32_t qos0_acks = m->pending_qos0_acks;
  while (events > 0 || qos0_acks > 0)
  {
    az_iot_mqtt_event event;
    if (events > 0)
    {
      event = m->pending[0];
      m->pending_count--;
      events--;
      memmove(&m->pending[0], &m->pending[1], (size_t)m->pending_count * sizeof(m->pending[0]));
    }
    else
    {
      memset(&event, 0, sizeof(event));
      event.kind = AZ_IOT_MQTT_EVT_PUBLISH_ACK;
      event.status = AZ_IOT_OK;
      m->pending_qos0_acks--;
      qos0_acks--;
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
    m->pending[m->pending_count++] = *event;
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

static void _azm_on_publish(_AZM(client) * client, _AZM(publish_data) const* publish)
{
  _azm_client* m = _azm_of(client);
  az_iot_az_mqtt_string_writer strings = { m->strings, m->strings + _AZM_STRINGS_SIZE };
  az_iot_mqtt_message message;
  memset(&message, 0, sizeof(message));
  message.topic = az_iot_az_mqtt_string_writer_add(&strings, publish->topic);
  message.payload = az_span_ptr(publish->payload);
  message.payload_len = (size_t)az_span_size(publish->payload);
  message.qos = (az_iot_mqtt_qos)publish->qos;
  message.retain = publish->retain;
#if AZ_IOT_AZ_MQTT_V == 5
  int32_t const count = publish->user_property_count;
  for (int32_t i = 0; i < count; i++)
  {
    m->message_properties[i].key
        = az_iot_az_mqtt_string_writer_add(&strings, publish->user_properties[i].key);
    m->message_properties[i].value
        = az_iot_az_mqtt_string_writer_add(&strings, publish->user_properties[i].value);
  }
  message.user_properties = count > 0 ? m->message_properties : NULL;
  message.user_properties_count = (size_t)count;
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
  event.status = (az_result_failed(ack->status) || code >= 0x80) ? AZ_IOT_ERR_MQTT : AZ_IOT_OK;
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

static void _azm_release_owned(_azm_client* m)
{
  for (int i = 0; i < m->owned_count; i++)
  {
    free(m->owned[i]);
  }
  m->owned_count = 0;
}

/** @brief A copy of @p s owned until the next connect, as a span; empty for NULL or "". */
static bool _azm_own(_azm_client* m, const char* s, az_span* out)
{
  *out = AZ_SPAN_EMPTY;
  if (!az_iot_az_mqtt_has_text(s))
  {
    return true;
  }
  if (m->owned_count >= _AZM_OWNED_MAX)
  {
    return false;
  }
  char* copy = az_iot_az_mqtt_strdup(s);
  if (copy == NULL)
  {
    return false;
  }
  m->owned[m->owned_count++] = copy;
  *out = az_span_create_from_str(copy);
  return true;
}

static bool _azm_own_bytes(_azm_client* m, uint8_t const* data, size_t size, az_span* out)
{
  *out = AZ_SPAN_EMPTY;
  if (data == NULL || size == 0)
  {
    return true;
  }
  if (m->owned_count >= _AZM_OWNED_MAX || size > (size_t)INT32_MAX)
  {
    return false;
  }
  char* copy = (char*)malloc(size);
  if (copy == NULL)
  {
    return false;
  }
  memcpy(copy, data, size);
  m->owned[m->owned_count++] = copy;
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
#if defined(_WIN32)
  if (o->tls.client_cert_path != NULL || o->tls.client_cert_pem != NULL)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_AZ_MQTT, "client certificates are not supported on Windows");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  // Schannel takes a CA file only (trusted_ca_path wins when both are set).
  if (o->tls.trusted_ca_pem != NULL && o->tls.trusted_ca_path == NULL)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_AZ_MQTT, "in-memory CA PEM is not supported on Windows");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
#endif
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
  if (az_iot_az_mqtt_has_text(o->lwt.topic) && !_azm_qos_valid(o->lwt.qos))
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
  bool const tls = t->use_tls || t->trusted_ca_path || t->trusted_ca_pem || t->client_cert_path
      || t->client_cert_pem || t->client_key_path || t->client_key_pem || t->client_key_uri;
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
  if (self == NULL || o == NULL || o->host == NULL || o->client_id == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  _azm_client* m = _azm_self(self);
  if (m->connecting || m->connected)
  {
    return AZ_IOT_ERR_BUSY;
  }
  az_iot_result rc = _azm_check_options(o);
  if (rc != AZ_IOT_OK)
  {
    return rc;
  }

  bool const persists = _azm_session_persists(o);
  if (persists && m->message_storage == NULL)
  {
    m->message_storage = (uint8_t*)malloc(AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE);
    if (m->message_storage == NULL)
    {
      return AZ_IOT_ERR_OUT_OF_MEMORY;
    }
  }

  _azm_release_owned(m);
  _AZM(client_options) options;
  memset(&options, 0, sizeof(options));
  if (!_azm_copy_options(m, o, &options))
  {
    _azm_release_owned(m);
    return AZ_IOT_ERR_OUT_OF_MEMORY;
  }
  options.send_buffer = az_span_create(m->send_buffer, AZ_IOT_AZ_MQTT_BUFFER_SIZE);
  options.receive_buffer = az_span_create(m->receive_buffer, AZ_IOT_AZ_MQTT_BUFFER_SIZE);
  options.inflight_control_buffer = _AZM_SPAN_OF(m->inflight);
  options.inflight_message_buffer = persists
      ? az_span_create(m->message_storage, AZ_IOT_AZ_MQTT_MESSAGE_STORAGE_SIZE)
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
  options.buffers.connack_user_properties = _AZM_SPAN_OF(m->ack_properties);
  options.buffers.publish_user_properties = _AZM_SPAN_OF(m->received_properties);
  options.buffers.publish_subscription_identifiers = _AZM_SPAN_OF(m->subscription_ids);
  options.buffers.suback_reason_codes = _AZM_SPAN_OF(m->suback_codes);
  options.buffers.suback_user_properties = _AZM_SPAN_OF(m->ack_properties);
  options.buffers.ack_user_properties = _AZM_SPAN_OF(m->ack_properties);
  options.buffers.disconnect_user_properties = _AZM_SPAN_OF(m->ack_properties);
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
      return AZ_IOT_ERR_OUT_OF_MEMORY;
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
  if (self == NULL || topic_filter == NULL || !_azm_qos_valid(qos))
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
  if (self == NULL || topic_filter == NULL)
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
  publish.topic = az_span_create_from_str((char*)(uintptr_t)msg->topic);
  publish.payload = az_span_create((uint8_t*)(uintptr_t)msg->payload, (int32_t)msg->payload_len);
  publish.qos = (az_mqtt_qos)msg->qos;
  publish.retain = msg->retain;
#if AZ_IOT_AZ_MQTT_V == 5
  if ((msg->user_properties == NULL && msg->user_properties_count > 0)
      || (msg->correlation_data == NULL && msg->correlation_data_len > 0))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (msg->user_properties_count > AZ_IOT_AZ_MQTT_USER_PROPERTIES_MAX
      || msg->correlation_data_len > (size_t)INT32_MAX)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  int32_t count = 0;
  for (size_t i = 0; i < msg->user_properties_count; i++)
  {
    az_iot_mqtt_user_property const* p = &msg->user_properties[i];
    if (p->key == NULL)
    {
      continue;
    }
    m->publish_properties[count].key = az_span_create_from_str((char*)(uintptr_t)p->key);
    m->publish_properties[count].value
        = az_span_create_from_str((char*)(uintptr_t)(p->value != NULL ? p->value : ""));
    count++;
  }
  publish.user_properties = count > 0 ? m->publish_properties : NULL;
  publish.user_property_count = count;
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
  free(m->message_storage);
  free(m->send_buffer); // The block of _azm_create().
  free(m);
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

static az_iot_mqtt_client* _azm_create(void* factory_context)
{
  (void)factory_context;
  _azm_client* m = (_azm_client*)calloc(1, sizeof(*m));
  if (m == NULL)
  {
    return NULL;
  }
  // One block: send buffer, receive buffer, transport (16-byte aligned), received strings.
  size_t const transport_offset = (2u * AZ_IOT_AZ_MQTT_BUFFER_SIZE + 15u) & ~(size_t)15u;
  size_t const strings_offset
      = transport_offset + (((size_t)az_mqtt_transport_sizeof() + 15u) & ~(size_t)15u);
  uint8_t* block = (uint8_t*)malloc(strings_offset + (size_t)_AZM_STRINGS_SIZE);
  if (block == NULL)
  {
    free(m);
    return NULL;
  }
  m->send_buffer = block;
  m->receive_buffer = block + AZ_IOT_AZ_MQTT_BUFFER_SIZE;
  m->transport = (az_mqtt_transport*)(void*)(block + transport_offset);
  m->strings = (char*)(block + strings_offset);
  m->base.iface = &_azm_iface;
  return &m->base;
}

az_iot_mqtt_factory* _AZM_FACTORY_CREATE(void)
{
  az_iot_mqtt_factory* factory = (az_iot_mqtt_factory*)calloc(1, sizeof(*factory));
  if (factory != NULL)
  {
    factory->version = _AZM_VERSION;
    factory->create = _azm_create;
    // The connection client frees a registered factory through destroy(factory_ctx).
    factory->factory_ctx = factory;
    factory->destroy = az_iot_az_mqtt_factory_free;
  }
  return factory;
}
