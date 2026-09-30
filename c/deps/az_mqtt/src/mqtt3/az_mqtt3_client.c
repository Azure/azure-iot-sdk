// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt3_client.c
 * @brief MQTT 3.1.1 client: packet handling and callbacks on top of az_mqtt_core.
 */

#include <az_mqtt3/az_mqtt3_client.h>
#include <az_mqtt3/az_mqtt3_codec.h>

#include "az_mqtt_core_internal.h"

#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <string.h>

/** @brief The core's internal fields. */
#define _CORE(client) ((client)->_internal.core._internal)
/** @brief Whole send buffer. */
#define _SEND_BUFFER(client) (_CORE(client).send_buffer)

static void _on_closed(az_mqtt_core* core, az_result reason)
{
  az_mqtt3_client* client = (az_mqtt3_client*)core; // core is the first member.
  if (client->_internal.on_connection_closed != NULL)
  {
    client->_internal.on_connection_closed(client, reason);
  }
}

/** @brief Encode an acknowledgement with @p encode and send it. */
static az_result _send_ack(
    az_mqtt3_client* client,
    az_result (*encode)(az_span* dest, uint16_t packet_id),
    uint16_t packet_id)
{
  az_span send_buf = _SEND_BUFFER(client);
  az_result rc = encode(&send_buf, packet_id);
  return az_result_succeeded(rc) ? _az_mqtt_core_send(&client->_internal.core, send_buf) : rc;
}

// ============================================================================
// Packet dispatch
// ============================================================================

static az_result _handle_connack(az_mqtt3_client* client, az_span body)
{
  az_mqtt3_connack_data connack;
  az_result rc = az_mqtt3_codec_decode_connack(body, &connack);
  if (az_result_failed(rc))
    return rc;

  if (connack.return_code == AZ_MQTT3_CONNACK_ACCEPTED)
  {
    _CORE(client).state = AZ_MQTT_CLIENT_STATE_CONNECTED;
    _CORE(client).keep_alive_seconds = client->_internal.connect_options.keep_alive_seconds;
  }
  if (client->_internal.on_connack != NULL)
  {
    client->_internal.on_connack(client, &connack);
  }
  return AZ_OK;
}

static az_result _handle_publish(az_mqtt3_client* client, az_span body, uint8_t flags)
{
  az_mqtt3_publish_data publish;
  az_result rc = az_mqtt3_codec_decode_publish(body, flags, &publish);
  if (az_result_failed(rc))
    return rc;

  if (publish.qos == AZ_MQTT_QOS_AT_LEAST_ONCE)
  {
    rc = _send_ack(client, az_mqtt3_codec_encode_puback, publish.packet_id);
  }
  else if (publish.qos == AZ_MQTT_QOS_EXACTLY_ONCE)
  {
    rc = _send_ack(client, az_mqtt3_codec_encode_pubrec, publish.packet_id);
  }
  if (az_result_failed(rc))
    return rc;

  if (client->_internal.on_publish != NULL)
  {
    client->_internal.on_publish(client, &publish);
  }
  return AZ_OK;
}

static az_result _handle_ack(az_mqtt3_client* client, az_mqtt_packet_type type, az_span body)
{
  az_mqtt3_ack_data ack;
  az_result rc = az_mqtt3_codec_decode_ack(body, &ack);
  if (az_result_failed(rc))
    return rc;

  az_mqtt3_on_puback_fn callback = NULL;
  switch (type)
  {
    case AZ_MQTT_PACKET_TYPE_PUBACK:
      callback = client->_internal.on_puback;
      break;
    case AZ_MQTT_PACKET_TYPE_PUBREC:
      return _send_ack(client, az_mqtt3_codec_encode_pubrel, ack.packet_id);
    case AZ_MQTT_PACKET_TYPE_PUBREL:
      rc = _send_ack(client, az_mqtt3_codec_encode_pubcomp, ack.packet_id);
      if (az_result_failed(rc))
        return rc;
      callback = client->_internal.on_pubcomp;
      break;
    case AZ_MQTT_PACKET_TYPE_PUBCOMP:
      callback = client->_internal.on_pubcomp;
      break;
    default: // UNSUBACK
      callback = client->_internal.on_unsuback;
      break;
  }
  if (callback != NULL)
  {
    callback(client, &ack);
  }
  return AZ_OK;
}

static az_result _handle_suback(az_mqtt3_client* client, az_span body)
{
  az_mqtt3_suback_data suback;
  az_result rc = az_mqtt3_codec_decode_suback(body, &suback);
  if (az_result_failed(rc))
    return rc;

  if (client->_internal.on_suback != NULL)
  {
    client->_internal.on_suback(client, &suback);
  }
  return AZ_OK;
}

static az_result _dispatch_packet(
    az_mqtt_core* core,
    az_mqtt_packet_type type,
    uint8_t flags,
    az_span body)
{
  az_mqtt3_client* client = (az_mqtt3_client*)core; // core is the first member.
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
    case AZ_MQTT_PACKET_TYPE_UNSUBACK:
      return _handle_ack(client, type, body);
    case AZ_MQTT_PACKET_TYPE_SUBACK:
      return _handle_suback(client, body);
    case AZ_MQTT_PACKET_TYPE_PINGRESP:
      return AZ_OK;
    default: // Including DISCONNECT (client-to-server only) and AUTH (reserved) in 3.1.1.
      return AZ_MQTT_ERROR_PROTOCOL;
  }
}

// ============================================================================
// Public API
// ============================================================================

AZ_NODISCARD az_result
az_mqtt3_client_init(az_mqtt3_client* client, az_mqtt3_client_options const* options)
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
  client->_internal.on_connack = options->on_connack;
  client->_internal.on_publish = options->on_publish;
  client->_internal.on_suback = options->on_suback;
  client->_internal.on_unsuback = options->on_unsuback;
  client->_internal.on_puback = options->on_puback;
  client->_internal.on_pubcomp = options->on_pubcomp;
  client->_internal.on_connection_closed = options->on_connection_closed;
  client->_internal.user_context = options->user_context;
  return AZ_OK;
}

AZ_NODISCARD az_result az_mqtt3_client_connect(az_mqtt3_client* client, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(client);

  if (_CORE(client).state != AZ_MQTT_CLIENT_STATE_DISCONNECTED)
  {
    return AZ_MQTT_ERROR_INVALID_STATE;
  }

  az_span send_buf = _SEND_BUFFER(client);
  az_result rc = az_mqtt3_codec_encode_connect(&send_buf, &client->_internal.connect_options);
  if (az_result_failed(rc))
  {
    return rc;
  }
  return _az_mqtt_core_connect(
      &client->_internal.core,
      timeout_ms,
      az_span_size(_SEND_BUFFER(client)) - az_span_size(send_buf),
      _dispatch_packet);
}

AZ_NODISCARD az_result az_mqtt3_client_process_loop(az_mqtt3_client* client, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(client);
  return _az_mqtt_core_process_loop(&client->_internal.core, timeout_ms, _dispatch_packet);
}

AZ_NODISCARD az_result az_mqtt3_client_publish(
    az_mqtt3_client* client,
    az_mqtt3_publish_options const* options,
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
  az_result rc = az_mqtt3_codec_encode_publish(&send_buf, options, packet_id);
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

AZ_NODISCARD az_result az_mqtt3_client_subscribe(
    az_mqtt3_client* client,
    az_mqtt3_subscription const* subscriptions,
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
      = az_mqtt3_codec_encode_subscribe(&send_buf, subscriptions, subscription_count, packet_id);
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

AZ_NODISCARD az_result az_mqtt3_client_unsubscribe(
    az_mqtt3_client* client,
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
      = az_mqtt3_codec_encode_unsubscribe(&send_buf, topic_filters, filter_count, packet_id);
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

AZ_NODISCARD az_result az_mqtt3_client_disconnect(az_mqtt3_client* client)
{
  _az_PRECONDITION_NOT_NULL(client);

  if (_CORE(client).state == AZ_MQTT_CLIENT_STATE_DISCONNECTED)
  {
    return AZ_OK;
  }
  if (_CORE(client).state == AZ_MQTT_CLIENT_STATE_CONNECTED)
  {
    az_span send_buf = _SEND_BUFFER(client);
    if (az_result_succeeded(az_mqtt3_codec_encode_disconnect(&send_buf)))
    {
      // Best effort: the session ends either way.
      az_result const rc = _az_mqtt_core_send(&client->_internal.core, send_buf);
      (void)rc;
    }
  }
  _az_mqtt_core_close(&client->_internal.core, AZ_OK);
  return AZ_OK;
}
