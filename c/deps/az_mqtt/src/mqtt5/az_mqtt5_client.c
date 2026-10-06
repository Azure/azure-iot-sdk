// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt5_client.c
 * @brief MQTT 5.0 client: packet handling and callbacks on top of az_mqtt_core.
 */

#include <az_mqtt5/az_mqtt5_client.h>
#include <az_mqtt5/az_mqtt5_codec.h>

#include "az_mqtt_core_internal.h"

#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/internal/az_result_internal.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <string.h>

/** @brief The core's internal fields. */
#define _CORE(client) ((client)->_internal.core._internal)
/** @brief Whole send buffer. */
#define _SEND_BUFFER(client) (_CORE(client).send_buffer)

// ============================================================================
// Helpers
// ============================================================================

static int32_t _span_count(az_span span, int32_t element_size)
{
  return (element_size > 0) ? (az_span_size(span) / element_size) : 0;
}

static az_mqtt5_user_property* _span_user_properties(az_span span)
{
  return (az_mqtt5_user_property*)az_span_ptr(span);
}

static az_mqtt5_reason_code* _span_reason_codes(az_span span)
{
  return (az_mqtt5_reason_code*)az_span_ptr(span);
}

static int32_t* _span_i32(az_span span)
{
  return (int32_t*)az_span_ptr(span);
}

static void _on_closed(az_mqtt_core* core, az_result reason)
{
  az_mqtt5_client* client = (az_mqtt5_client*)core; // core is the first member.
  if (client->_internal.on_connection_closed != NULL)
  {
    client->_internal.on_connection_closed(client, reason);
  }
}

static void _on_transport_error(az_mqtt_core* core, az_mqtt_native_error const* error)
{
  az_mqtt5_client* client = (az_mqtt5_client*)core;
  if (client->_internal.on_transport_error != NULL)
  {
    client->_internal.on_transport_error(client, error);
  }
}

// ============================================================================
// Packet dispatch
// ============================================================================

static az_result _encode_pubrel(az_span* dest, uint16_t packet_id)
{
  return az_mqtt5_codec_encode_pubrel(dest, packet_id, AZ_MQTT5_REASON_SUCCESS);
}

/** @brief Skip a Variable Byte Integer at @p at; the offset after it. */
static uint32_t _skip_vbi(uint8_t const* packet, uint32_t at)
{
  while ((packet[at] & 0x80) != 0)
  {
    at++;
  }
  return at + 1;
}

/**
 * @brief Offset of the Message Expiry Interval value in a QoS 1/2 PUBLISH that has one: the
 * encoder writes it first, or right after the Payload Format Indicator.
 */
static uint32_t _expiry_offset(uint8_t const* packet)
{
  uint32_t at = _skip_vbi(packet, 1); // First byte, Remaining Length.
  at += 2 + (((uint32_t)packet[at] << 8) | packet[at + 1]) + 2; // Topic Name, Packet Identifier.
  at = _skip_vbi(packet, at); // Property Length.
  if (packet[at] == AZ_MQTT5_PROPERTY_PAYLOAD_FORMAT_INDICATOR)
  {
    at += 2;
  }
  return at + 1;
}

/** @brief _az_mqtt_core_publish_dropped_fn: on_puback / on_pubcomp with @p status. */
static void _publish_dropped(az_mqtt_core* core, uint16_t packet_id, bool qos1, az_result status)
{
  az_mqtt5_client* client = (az_mqtt5_client*)core;
  az_mqtt5_ack_data ack;
  memset(&ack, 0, sizeof(ack));
  ack.packet_id = packet_id;
  ack.reason_code = status == AZ_MQTT_ERROR_PACKET_TOO_LARGE ? AZ_MQTT5_REASON_PACKET_TOO_LARGE
                                                             : AZ_MQTT5_REASON_UNSPECIFIED_ERROR;
  ack.status = status;
  az_mqtt5_on_puback_fn const callback
      = qos1 ? client->_internal.on_puback : client->_internal.on_pubcomp;
  if (callback != NULL)
  {
    callback(client, &ack);
  }
}

static az_result _handle_connack(az_mqtt5_client* client, az_span body)
{
  az_mqtt5_connack_data connack;
  connack.user_properties = _span_user_properties(client->_internal.buffers.connack_user_properties);
  connack.user_property_count = 0;
  connack.user_property_capacity =
      _span_count(client->_internal.buffers.connack_user_properties, (int32_t)sizeof(az_mqtt5_user_property));

  az_result rc = az_mqtt5_codec_decode_connack(body, &connack);
  if (az_result_failed(rc))
    return rc;

  if (connack.reason_code == AZ_MQTT5_REASON_SUCCESS)
  {
    client->_internal.server_receive_maximum = connack.receive_maximum;
    client->_internal.server_maximum_qos = connack.maximum_qos;
    client->_internal.server_retain_available = connack.retain_available;
    _CORE(client).server_maximum_packet_size = connack.maximum_packet_size;
    client->_internal.server_topic_alias_maximum = connack.topic_alias_maximum;
    _CORE(client).state = AZ_MQTT_CLIENT_STATE_CONNECTED;
    _CORE(client).keep_alive_seconds = connack.server_keep_alive_present
        ? connack.server_keep_alive
        : client->_internal.connect_options.keep_alive_seconds;
    uint32_t const generation = _CORE(client).session_generation;
    _az_RETURN_IF_FAILED(_az_mqtt_core_inflight_resume(
        &client->_internal.core,
        connack.session_present,
        _encode_pubrel,
        client->_internal.server_receive_maximum,
        _publish_dropped));
    if (_CORE(client).session_generation != generation)
    {
      return AZ_OK; // Ended during the resends.
    }
  }

  if (client->_internal.on_connack != NULL)
  {
    client->_internal.on_connack(client, &connack);
  }

  return AZ_OK;
}

/** @brief Send DISCONNECT with @p reason (best effort); return @p result. */
static az_result
_disconnect_with(az_mqtt5_client* client, az_mqtt5_reason_code reason, az_result result)
{
  az_span disconnect = _SEND_BUFFER(client);
  if (az_result_succeeded(az_mqtt5_codec_encode_disconnect(&disconnect, reason, 0)))
  {
    az_result const send_rc = _az_mqtt_core_send(&client->_internal.core, disconnect);
    (void)send_rc;
  }
  return result;
}

static az_result _handle_publish(az_mqtt5_client* client, az_span body, uint8_t flags)
{
  az_mqtt5_publish_data publish;
  publish.user_properties = _span_user_properties(client->_internal.buffers.publish_user_properties);
  publish.user_property_count = 0;
  publish.subscription_identifiers = _span_i32(client->_internal.buffers.publish_subscription_identifiers);
  publish.subscription_identifier_count = 0;
  publish.user_property_capacity = _span_count(
      client->_internal.buffers.publish_user_properties, (int32_t)sizeof(az_mqtt5_user_property));
  publish.subscription_identifier_capacity = _span_count(
      client->_internal.buffers.publish_subscription_identifiers, (int32_t)sizeof(int32_t));

  az_result rc = az_mqtt5_codec_decode_publish(body, flags, &publish);
  if (az_result_failed(rc))
    return rc;
  // 3.3.2.3.4: above the Topic Alias Maximum this client sent.
  if (publish.topic_alias > client->_internal.connect_options.topic_alias_maximum)
  {
    return _disconnect_with(client, AZ_MQTT5_REASON_TOPIC_ALIAS_INVALID, AZ_MQTT_ERROR_PROTOCOL);
  }

  // Send acknowledgment for QoS > 0
  if (publish.qos == AZ_MQTT_QOS_AT_LEAST_ONCE)
  {
    az_span send_buf = _SEND_BUFFER(client);
    rc = az_mqtt5_codec_encode_puback(&send_buf, publish.packet_id, AZ_MQTT5_REASON_SUCCESS);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_core_send(&client->_internal.core, send_buf);
    if (az_result_failed(rc))
      return rc;
  }
  else if (publish.qos == AZ_MQTT_QOS_EXACTLY_ONCE)
  {
    bool is_duplicate;
    rc = _az_mqtt_core_inflight_track_inbound_qos2(
        &client->_internal.core, publish.packet_id, &is_duplicate);
    if (az_result_failed(rc))
    {
      // Ends the session; the server resends it on a resumed one.
      return _disconnect_with(client, AZ_MQTT5_REASON_QUOTA_EXCEEDED, rc);
    }
    az_span send_buf = _SEND_BUFFER(client);
    rc = az_mqtt5_codec_encode_pubrec(&send_buf, publish.packet_id, AZ_MQTT5_REASON_SUCCESS);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_core_send(&client->_internal.core, send_buf);
    if (az_result_failed(rc) || is_duplicate)
      return rc; // A duplicate was delivered already.
  }

  if (client->_internal.on_publish != NULL)
  {
    client->_internal.on_publish(client, &publish);
  }

  return AZ_OK;
}

/**
 * @brief AUTH: decoded, so a malformed one ends the session, then ignored (enhanced
 * authentication is not implemented).
 */
static az_result _handle_auth(az_span body)
{
  az_mqtt5_auth_data auth;
  memset(&auth, 0, sizeof(auth));
  return az_mqtt5_codec_decode_auth(body, &auth);
}

/** @brief PUBACK, PUBREC, PUBREL or PUBCOMP. */
static az_result _handle_ack(az_mqtt5_client* client, az_mqtt_packet_type type, az_span body)
{
  az_mqtt5_ack_data ack;
  ack.user_properties = _span_user_properties(client->_internal.buffers.ack_user_properties);
  ack.user_property_count = 0;
  ack.user_property_capacity = _span_count(
      client->_internal.buffers.ack_user_properties, (int32_t)sizeof(az_mqtt5_user_property));

  az_result rc = az_mqtt5_codec_decode_ack(body, &ack);
  if (az_result_failed(rc))
    return rc;

  az_mqtt_core* const core = &client->_internal.core;
  az_mqtt5_on_puback_fn callback = client->_internal.on_pubcomp;
  if (type == AZ_MQTT_PACKET_TYPE_PUBACK)
  {
    if (!_az_mqtt_core_inflight_release_entry(core, _AZ_MQTT_INFLIGHT_PUBLISH_QOS1, ack.packet_id))
    {
      return AZ_OK; // Not in flight.
    }
    callback = client->_internal.on_puback;
  }
  else if (type == AZ_MQTT_PACKET_TYPE_PUBCOMP)
  {
    if (!_az_mqtt_core_inflight_release_entry(core, _AZ_MQTT_INFLIGHT_PUBREL, ack.packet_id))
    {
      return AZ_OK;
    }
  }
  else if (type == AZ_MQTT_PACKET_TYPE_PUBREC)
  {
    az_mqtt_inflight_entry* entry
        = _az_mqtt_core_inflight_find_entry(core, _AZ_MQTT_INFLIGHT_PUBLISH_QOS2, ack.packet_id);
    if (entry != NULL && ack.reason_code >= 0x80)
    {
      _az_mqtt_core_inflight_free_entry(core, entry); // Failed: the exchange ends here.
    }
    else
    {
      if (entry != NULL)
      {
        _az_mqtt_core_inflight_to_pubrel(core, entry);
      }
      else
      {
        // A resent PUBREC.
        entry = _az_mqtt_core_inflight_find_entry(core, _AZ_MQTT_INFLIGHT_PUBREL, ack.packet_id);
      }
      az_span send_buf = _SEND_BUFFER(client);
      _az_RETURN_IF_FAILED(az_mqtt5_codec_encode_pubrel(
          &send_buf,
          ack.packet_id,
          entry != NULL ? AZ_MQTT5_REASON_SUCCESS : AZ_MQTT5_REASON_PACKET_IDENTIFIER_NOT_FOUND));
      return _az_mqtt_core_send(core, send_buf);
    }
  }
  else // PUBREL
  {
    bool const tracked
        = _az_mqtt_core_inflight_release_entry(core, _AZ_MQTT_INFLIGHT_INBOUND_QOS2, ack.packet_id);
    // Always acknowledged: an untracked one may have been delivered without an entry.
    az_span send_buf = _SEND_BUFFER(client);
    _az_RETURN_IF_FAILED(az_mqtt5_codec_encode_pubcomp(
        &send_buf,
        ack.packet_id,
        tracked ? AZ_MQTT5_REASON_SUCCESS : AZ_MQTT5_REASON_PACKET_IDENTIFIER_NOT_FOUND));
    _az_RETURN_IF_FAILED(_az_mqtt_core_send(core, send_buf));
    if (!tracked)
    {
      return AZ_OK;
    }
    ack.incoming = true;
  }
  uint32_t const generation = _CORE(client).session_generation;
  if (callback != NULL)
  {
    callback(client, &ack); // First: what follows may end the session.
  }
  if (type != AZ_MQTT_PACKET_TYPE_PUBREL && _CORE(client).session_generation == generation)
  {
    // A PUBLISH exchange ended: send what waits for resending within Receive Maximum.
    return _az_mqtt_core_inflight_resend_due(
        core, client->_internal.server_receive_maximum, _publish_dropped);
  }
  return AZ_OK;
}

static az_result _handle_suback(az_mqtt5_client* client, az_span body)
{
  az_mqtt5_suback_data suback;
  suback.reason_codes = _span_reason_codes(client->_internal.buffers.suback_reason_codes);
  suback.reason_code_count = 0;
  suback.reason_code_capacity =
      _span_count(client->_internal.buffers.suback_reason_codes, (int32_t)sizeof(az_mqtt5_reason_code));
  suback.user_properties = _span_user_properties(client->_internal.buffers.suback_user_properties);
  suback.user_property_count = 0;
  suback.user_property_capacity =
      _span_count(client->_internal.buffers.suback_user_properties, (int32_t)sizeof(az_mqtt5_user_property));

  az_result rc = az_mqtt5_codec_decode_suback(body, &suback);
  if (az_result_failed(rc)
      || !_az_mqtt_core_inflight_release_entry(
          &client->_internal.core, _AZ_MQTT_INFLIGHT_SUBSCRIBE, suback.packet_id))
    return rc;

  if (client->_internal.on_suback != NULL)
  {
    client->_internal.on_suback(client, &suback);
  }
  return AZ_OK;
}

static az_result _handle_unsuback(az_mqtt5_client* client, az_span body)
{
  az_mqtt5_suback_data unsuback;
  unsuback.reason_codes = _span_reason_codes(client->_internal.buffers.suback_reason_codes);
  unsuback.reason_code_count = 0;
  unsuback.reason_code_capacity =
      _span_count(client->_internal.buffers.suback_reason_codes, (int32_t)sizeof(az_mqtt5_reason_code));
  unsuback.user_properties = _span_user_properties(client->_internal.buffers.suback_user_properties);
  unsuback.user_property_count = 0;
  unsuback.user_property_capacity =
      _span_count(client->_internal.buffers.suback_user_properties, (int32_t)sizeof(az_mqtt5_user_property));

  az_result rc = az_mqtt5_codec_decode_unsuback(body, &unsuback);
  if (az_result_failed(rc)
      || !_az_mqtt_core_inflight_release_entry(
          &client->_internal.core, _AZ_MQTT_INFLIGHT_UNSUBSCRIBE, unsuback.packet_id))
    return rc;

  if (client->_internal.on_unsuback != NULL)
  {
    client->_internal.on_unsuback(client, &unsuback);
  }
  return AZ_OK;
}

static az_result _handle_disconnect(az_mqtt5_client* client, az_span body)
{
  az_mqtt5_disconnect_data disc;
  disc.user_properties = _span_user_properties(client->_internal.buffers.disconnect_user_properties);
  disc.user_property_count = 0;
  disc.user_property_capacity = _span_count(
      client->_internal.buffers.disconnect_user_properties, (int32_t)sizeof(az_mqtt5_user_property));

  az_result rc = az_mqtt5_codec_decode_disconnect(body, &disc);
  if (az_result_failed(rc))
    return rc;

  if (client->_internal.on_disconnect != NULL)
  {
    client->_internal.on_disconnect(client, &disc);
  }
  _az_mqtt_core_close(&client->_internal.core, AZ_MQTT_ERROR_SERVER_DISCONNECTED);
  return AZ_OK;
}

static az_result _dispatch_packet(
    az_mqtt_core* core,
    az_mqtt_packet_type type,
    uint8_t flags,
    az_span body)
{
  az_mqtt5_client* client = (az_mqtt5_client*)core; // core is the first member.
  switch (type)
  {
    case AZ_MQTT_PACKET_TYPE_CONNACK:
      return _handle_connack(client, body);
    case AZ_MQTT_PACKET_TYPE_PUBLISH:
      return _handle_publish(client, body, flags);
    case AZ_MQTT_PACKET_TYPE_PUBACK:
    case AZ_MQTT_PACKET_TYPE_PUBREC:
    case AZ_MQTT_PACKET_TYPE_PUBREL:
    case AZ_MQTT_PACKET_TYPE_PUBCOMP:
      return _handle_ack(client, type, body);
    case AZ_MQTT_PACKET_TYPE_SUBACK:
      return _handle_suback(client, body);
    case AZ_MQTT_PACKET_TYPE_UNSUBACK:
      return _handle_unsuback(client, body);
    case AZ_MQTT_PACKET_TYPE_PINGRESP:
      return AZ_OK; // Nothing to do
    case AZ_MQTT_PACKET_TYPE_DISCONNECT:
      return _handle_disconnect(client, body);
    case AZ_MQTT_PACKET_TYPE_AUTH:
      return _handle_auth(body);
    case AZ_MQTT_PACKET_TYPE_CONNECT: // Client to server only.
    case AZ_MQTT_PACKET_TYPE_SUBSCRIBE:
    case AZ_MQTT_PACKET_TYPE_UNSUBSCRIBE:
    case AZ_MQTT_PACKET_TYPE_PINGREQ:
    default:
      return AZ_MQTT_ERROR_PROTOCOL;
  }
}

// ============================================================================
// Public API
// ============================================================================

AZ_NODISCARD az_result
az_mqtt5_client_init(az_mqtt5_client* client, az_mqtt5_client_options const* options)
{
  _az_PRECONDITION_NOT_NULL(client);
  _az_PRECONDITION_NOT_NULL(options);
  _az_PRECONDITION_NOT_NULL(options->transport);

  _az_RETURN_IF_FAILED(az_mqtt_transport_set_proxy(options->transport, options->proxy_options));
  memset(client, 0, sizeof(*client));
  _CORE(client).transport = options->transport;
  _CORE(client).send_buffer = options->send_buffer;
  _CORE(client).receive_buffer = options->receive_buffer;
  _CORE(client).hostname = options->hostname;
  _CORE(client).port = options->port;
  _CORE(client).tls_options = options->tls_options;
  _CORE(client).proxy = options->proxy_options;
  _CORE(client).on_closed = _on_closed;
  _CORE(client).on_transport_error = _on_transport_error;
  _az_mqtt_core_register_transport_errors(&client->_internal.core);
  client->_internal.connect_options = options->connect_options;
  client->_internal.buffers = options->buffers;
  client->_internal.on_connack = options->on_connack;
  client->_internal.on_publish = options->on_publish;
  client->_internal.on_suback = options->on_suback;
  client->_internal.on_unsuback = options->on_unsuback;
  client->_internal.on_puback = options->on_puback;
  client->_internal.on_pubcomp = options->on_pubcomp;
  client->_internal.on_disconnect = options->on_disconnect;
  client->_internal.on_connection_closed = options->on_connection_closed;
  client->_internal.on_transport_error = options->on_transport_error;
  client->_internal.user_context = options->user_context;
  // A session outliving the connection resends QoS 1/2 PUBLISH: they must be stored.
  az_result const rc = _az_mqtt_core_inflight_init(
      &client->_internal.core,
      options->inflight_control_buffer,
      options->inflight_message_buffer,
      !options->connect_options.clean_start
          && options->connect_options.session_expiry_interval > 0);
  _az_mqtt_core_inflight_reserve_inbound(
      &client->_internal.core, options->connect_options.receive_maximum);
  return rc;
}

AZ_NODISCARD az_result az_mqtt5_client_connect_start(az_mqtt5_client* client, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(client);

  if (_CORE(client).state != AZ_MQTT_CLIENT_STATE_DISCONNECTED)
  {
    return AZ_MQTT_ERROR_INVALID_STATE;
  }

  // Encoded now; the core sends it once the transport is up.
  az_span send_buf = _SEND_BUFFER(client);
  az_result rc = az_mqtt5_codec_encode_connect(&send_buf, &client->_internal.connect_options);
  if (az_result_failed(rc))
  {
    return rc;
  }
  return _az_mqtt_core_connect_start(&client->_internal.core, timeout_ms);
}

AZ_NODISCARD az_result az_mqtt5_client_connect(az_mqtt5_client* client, int32_t timeout_ms)
{
  az_result rc = az_mqtt5_client_connect_start(client, timeout_ms);
  return az_result_succeeded(rc) ? _az_mqtt_core_connect_wait(&client->_internal.core, _dispatch_packet)
                                 : rc;
}

AZ_NODISCARD az_result az_mqtt5_client_process_loop(az_mqtt5_client* client, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(client);
  return _az_mqtt_core_process_loop(&client->_internal.core, timeout_ms, _dispatch_packet);
}

AZ_NODISCARD az_result az_mqtt5_client_publish(
    az_mqtt5_client* client,
    az_mqtt5_publish_options const* options,
    uint16_t* out_packet_id)
{
  _az_PRECONDITION_NOT_NULL(client);
  _az_PRECONDITION_NOT_NULL(options);

  if (_CORE(client).state != AZ_MQTT_CLIENT_STATE_CONNECTED)
  {
    return AZ_MQTT_ERROR_NOT_CONNECTED;
  }

  if ((uint8_t)options->qos > client->_internal.server_maximum_qos
      || (options->retain && !client->_internal.server_retain_available)
      || options->topic_alias > client->_internal.server_topic_alias_maximum)
  {
    return AZ_MQTT_ERROR_NOT_SUPPORTED;
  }

  az_mqtt_core* const core = &client->_internal.core;
  az_result rc;
  uint16_t packet_id = 0;
  if (options->qos == AZ_MQTT_QOS_AT_MOST_ONCE)
  {
    az_span send_buf = _SEND_BUFFER(client);
    rc = az_mqtt5_codec_encode_publish(&send_buf, options, 0);
    rc = _az_mqtt_core_send_tracked_request(core, NULL, rc, send_buf);
  }
  else
  {
    if (_CORE(client).keep_messages && options->topic_alias != 0)
    {
      return AZ_MQTT_ERROR_NOT_SUPPORTED; // A stored copy must not depend on this connection.
    }
    az_span buffer;
    _az_RETURN_IF_FAILED(_az_mqtt_core_publish_buffer(core, &buffer));
    az_mqtt_inflight_entry* entry = NULL;
    _az_RETURN_IF_FAILED(_az_mqtt_core_inflight_reserve_entry(
        core,
        options->qos == AZ_MQTT_QOS_AT_LEAST_ONCE ? _AZ_MQTT_INFLIGHT_PUBLISH_QOS1
                                                  : _AZ_MQTT_INFLIGHT_PUBLISH_QOS2,
        client->_internal.server_receive_maximum,
        &entry));
    packet_id = entry->_internal.packet_id;
    az_span remaining = buffer;
    rc = az_mqtt5_codec_encode_publish(&remaining, options, packet_id);
    int64_t deadline = -1;
    uint32_t expiry_offset = 0;
    if (az_result_succeeded(rc) && options->message_expiry_interval != 0)
    {
      deadline = az_mqtt_transport_clock_ms() + (int64_t)options->message_expiry_interval * 1000;
      expiry_offset = _expiry_offset(az_span_ptr(buffer));
    }
    rc = _az_mqtt_core_send_publish(core, entry, rc, buffer, remaining, deadline, expiry_offset);
  }
  if (az_result_succeeded(rc) && out_packet_id != NULL)
  {
    *out_packet_id = packet_id;
  }
  return rc;
}

AZ_NODISCARD az_result az_mqtt5_client_subscribe(
    az_mqtt5_client* client,
    az_mqtt5_subscription const* subscriptions,
    int32_t subscription_count,
    uint16_t* out_packet_id)
{
  _az_PRECONDITION_NOT_NULL(client);
  _az_PRECONDITION_NOT_NULL(subscriptions);

  if (_CORE(client).state != AZ_MQTT_CLIENT_STATE_CONNECTED)
  {
    return AZ_MQTT_ERROR_NOT_CONNECTED;
  }

  az_mqtt_inflight_entry* entry = NULL;
  _az_RETURN_IF_FAILED(_az_mqtt_core_inflight_reserve_entry(
      &client->_internal.core, _AZ_MQTT_INFLIGHT_SUBSCRIBE, UINT16_MAX, &entry));
  uint16_t const packet_id = entry->_internal.packet_id;
  az_span send_buf = _SEND_BUFFER(client);
  az_result rc = az_mqtt5_codec_encode_subscribe(
      &send_buf, subscriptions, subscription_count, packet_id);
  rc = _az_mqtt_core_send_tracked_request(&client->_internal.core, entry, rc, send_buf);
  if (az_result_succeeded(rc) && out_packet_id != NULL)
  {
    *out_packet_id = packet_id;
  }
  return rc;
}

AZ_NODISCARD az_result az_mqtt5_client_unsubscribe(
    az_mqtt5_client* client,
    az_span const* topic_filters,
    int32_t filter_count,
    uint16_t* out_packet_id)
{
  _az_PRECONDITION_NOT_NULL(client);
  _az_PRECONDITION_NOT_NULL(topic_filters);

  if (_CORE(client).state != AZ_MQTT_CLIENT_STATE_CONNECTED)
  {
    return AZ_MQTT_ERROR_NOT_CONNECTED;
  }

  az_mqtt_inflight_entry* entry = NULL;
  _az_RETURN_IF_FAILED(_az_mqtt_core_inflight_reserve_entry(
      &client->_internal.core, _AZ_MQTT_INFLIGHT_UNSUBSCRIBE, UINT16_MAX, &entry));
  uint16_t const packet_id = entry->_internal.packet_id;
  az_span send_buf = _SEND_BUFFER(client);
  az_result rc
      = az_mqtt5_codec_encode_unsubscribe(&send_buf, topic_filters, filter_count, packet_id);
  rc = _az_mqtt_core_send_tracked_request(&client->_internal.core, entry, rc, send_buf);
  if (az_result_succeeded(rc) && out_packet_id != NULL)
  {
    *out_packet_id = packet_id;
  }
  return rc;
}

AZ_NODISCARD az_result
az_mqtt5_client_disconnect(az_mqtt5_client* client, az_mqtt5_reason_code reason_code)
{
  _az_PRECONDITION_NOT_NULL(client);

  if (_CORE(client).state == AZ_MQTT_CLIENT_STATE_DISCONNECTED)
  {
    return AZ_OK;
  }
  if (_CORE(client).state == AZ_MQTT_CLIENT_STATE_CONNECTED)
  {
    az_span send_buf = _SEND_BUFFER(client);
    if (az_result_succeeded(az_mqtt5_codec_encode_disconnect(&send_buf, reason_code, 0)))
    {
      // Best effort: the session ends either way.
      az_result const rc = _az_mqtt_core_send(&client->_internal.core, send_buf);
      (void)rc;
    }
  }
  _az_mqtt_core_close(&client->_internal.core, AZ_OK);
  return AZ_OK;
}
