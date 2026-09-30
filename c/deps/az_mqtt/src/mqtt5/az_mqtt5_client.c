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

// ============================================================================
// Packet dispatch
// ============================================================================

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
    _CORE(client).state = AZ_MQTT_CLIENT_STATE_CONNECTED;
    _CORE(client).keep_alive_seconds = connack.server_keep_alive_present
        ? connack.server_keep_alive
        : client->_internal.connect_options.keep_alive_seconds;
  }

  if (client->_internal.on_connack != NULL)
  {
    client->_internal.on_connack(client, &connack);
  }

  return AZ_OK;
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
    az_span send_buf = _SEND_BUFFER(client);
    rc = az_mqtt5_codec_encode_pubrec(&send_buf, publish.packet_id, AZ_MQTT5_REASON_SUCCESS);
    if (az_result_failed(rc))
      return rc;
    rc = _az_mqtt_core_send(&client->_internal.core, send_buf);
    if (az_result_failed(rc))
      return rc;
  }

  if (client->_internal.on_publish != NULL)
  {
    client->_internal.on_publish(client, &publish);
  }

  return AZ_OK;
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

  az_mqtt5_on_puback_fn callback = client->_internal.on_pubcomp;
  if (type == AZ_MQTT_PACKET_TYPE_PUBACK)
  {
    callback = client->_internal.on_puback;
  }
  else if (type != AZ_MQTT_PACKET_TYPE_PUBCOMP)
  {
    // PUBREC: answer PUBREL. PUBREL: answer PUBCOMP, then report completion.
    az_span send_buf = _SEND_BUFFER(client);
    rc = type == AZ_MQTT_PACKET_TYPE_PUBREC
        ? az_mqtt5_codec_encode_pubrel(&send_buf, ack.packet_id, AZ_MQTT5_REASON_SUCCESS)
        : az_mqtt5_codec_encode_pubcomp(&send_buf, ack.packet_id, AZ_MQTT5_REASON_SUCCESS);
    if (az_result_succeeded(rc))
    {
      rc = _az_mqtt_core_send(&client->_internal.core, send_buf);
    }
    if (az_result_failed(rc) || type == AZ_MQTT_PACKET_TYPE_PUBREC)
    {
      return rc;
    }
  }

  if (callback != NULL)
  {
    callback(client, &ack);
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
  if (az_result_failed(rc))
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
  if (az_result_failed(rc))
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
      return AZ_OK; // Enhanced authentication is not implemented.
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

  memset(client, 0, sizeof(*client));
  _CORE(client).transport = options->transport;
  _CORE(client).send_buffer = options->send_buffer;
  _CORE(client).receive_buffer = options->receive_buffer;
  _CORE(client).hostname = options->hostname;
  _CORE(client).port = options->port;
  _CORE(client).tls_options = options->tls_options;
  _CORE(client).on_closed = _on_closed;
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
  client->_internal.user_context = options->user_context;
  return AZ_OK;
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

  uint16_t packet_id = 0;
  if (options->qos != AZ_MQTT_QOS_AT_MOST_ONCE)
  {
    packet_id = _az_mqtt_core_next_packet_id(&client->_internal.core);
  }

  az_span send_buf = _SEND_BUFFER(client);
  az_result rc = az_mqtt5_codec_encode_publish(&send_buf, options, packet_id);
  if (az_result_succeeded(rc))
  {
    rc = _az_mqtt_core_send_request(&client->_internal.core, send_buf);
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

  uint16_t packet_id = _az_mqtt_core_next_packet_id(&client->_internal.core);
  az_span send_buf = _SEND_BUFFER(client);
  az_result rc
      = az_mqtt5_codec_encode_subscribe(&send_buf, subscriptions, subscription_count, packet_id);
  if (az_result_succeeded(rc))
  {
    rc = _az_mqtt_core_send_request(&client->_internal.core, send_buf);
  }
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

  uint16_t packet_id = _az_mqtt_core_next_packet_id(&client->_internal.core);
  az_span send_buf = _SEND_BUFFER(client);
  az_result rc
      = az_mqtt5_codec_encode_unsubscribe(&send_buf, topic_filters, filter_count, packet_id);
  if (az_result_succeeded(rc))
  {
    rc = _az_mqtt_core_send_request(&client->_internal.core, send_buf);
  }
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
