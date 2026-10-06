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
#include <azure/core/internal/az_result_internal.h>
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

static void _on_transport_error(az_mqtt_core* core, az_mqtt_native_error const* error)
{
  az_mqtt3_client* client = (az_mqtt3_client*)core;
  if (client->_internal.on_transport_error != NULL)
  {
    client->_internal.on_transport_error(client, error);
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

/** @brief _az_mqtt_core_publish_dropped_fn: on_puback / on_pubcomp with @p status. */
static void _publish_dropped(az_mqtt_core* core, uint16_t packet_id, bool qos1, az_result status)
{
  az_mqtt3_client* client = (az_mqtt3_client*)core;
  az_mqtt3_ack_data ack;
  memset(&ack, 0, sizeof(ack));
  ack.packet_id = packet_id;
  ack.status = status;
  az_mqtt3_on_puback_fn const callback
      = qos1 ? client->_internal.on_puback : client->_internal.on_pubcomp;
  if (callback != NULL)
  {
    callback(client, &ack);
  }
}

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
    uint32_t const generation = _CORE(client).session_generation;
    _az_RETURN_IF_FAILED(_az_mqtt_core_inflight_resume(
        &client->_internal.core,
        connack.session_present,
        az_mqtt3_codec_encode_pubrel,
        UINT16_MAX,
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
    bool is_duplicate;
    rc = _az_mqtt_core_inflight_track_inbound_qos2(
        &client->_internal.core, publish.packet_id, &is_duplicate);
    if (az_result_failed(rc))
      return rc; // Ends the session; the server resends it on a resumed one.
    rc = _send_ack(client, az_mqtt3_codec_encode_pubrec, publish.packet_id);
    if (is_duplicate)
      return rc; // Delivered already.
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

  az_mqtt_core* const core = &client->_internal.core;
  az_mqtt3_on_puback_fn callback = NULL;
  _az_mqtt_inflight_kind kind = _AZ_MQTT_INFLIGHT_UNSUBSCRIBE;
  switch (type)
  {
    case AZ_MQTT_PACKET_TYPE_PUBACK:
      callback = client->_internal.on_puback;
      kind = _AZ_MQTT_INFLIGHT_PUBLISH_QOS1;
      break;
    case AZ_MQTT_PACKET_TYPE_PUBREC:
    {
      az_mqtt_inflight_entry* entry
          = _az_mqtt_core_inflight_find_entry(core, _AZ_MQTT_INFLIGHT_PUBLISH_QOS2, ack.packet_id);
      if (entry != NULL)
      {
        _az_mqtt_core_inflight_to_pubrel(core, entry);
      }
      return _send_ack(client, az_mqtt3_codec_encode_pubrel, ack.packet_id);
    }
    case AZ_MQTT_PACKET_TYPE_PUBREL:
      // Always acknowledged: an untracked one may have been delivered without an entry.
      _az_RETURN_IF_FAILED(_send_ack(client, az_mqtt3_codec_encode_pubcomp, ack.packet_id));
      callback = client->_internal.on_pubcomp;
      kind = _AZ_MQTT_INFLIGHT_INBOUND_QOS2;
      ack.incoming = true;
      break;
    case AZ_MQTT_PACKET_TYPE_PUBCOMP:
      callback = client->_internal.on_pubcomp;
      kind = _AZ_MQTT_INFLIGHT_PUBREL;
      break;
    case AZ_MQTT_PACKET_TYPE_UNSUBACK:
      callback = client->_internal.on_unsuback;
      break;
    case AZ_MQTT_PACKET_TYPE_CONNECT: // Not acknowledgements: _dispatch_packet never passes these.
    case AZ_MQTT_PACKET_TYPE_CONNACK:
    case AZ_MQTT_PACKET_TYPE_PUBLISH:
    case AZ_MQTT_PACKET_TYPE_SUBSCRIBE:
    case AZ_MQTT_PACKET_TYPE_SUBACK:
    case AZ_MQTT_PACKET_TYPE_UNSUBSCRIBE:
    case AZ_MQTT_PACKET_TYPE_PINGREQ:
    case AZ_MQTT_PACKET_TYPE_PINGRESP:
    case AZ_MQTT_PACKET_TYPE_DISCONNECT:
    case AZ_MQTT_PACKET_TYPE_AUTH:
    default:
      return AZ_MQTT_ERROR_PROTOCOL;
  }
  if (!_az_mqtt_core_inflight_release_entry(core, kind, ack.packet_id))
  {
    return AZ_OK; // Not in flight.
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
      return az_span_size(body) == 0 ? AZ_OK : AZ_MQTT_ERROR_MALFORMED_PACKET;
    case AZ_MQTT_PACKET_TYPE_CONNECT: // Client to server only.
    case AZ_MQTT_PACKET_TYPE_SUBSCRIBE:
    case AZ_MQTT_PACKET_TYPE_UNSUBSCRIBE:
    case AZ_MQTT_PACKET_TYPE_PINGREQ:
    case AZ_MQTT_PACKET_TYPE_DISCONNECT: // Client to server only in 3.1.1.
    case AZ_MQTT_PACKET_TYPE_AUTH: // Reserved in 3.1.1.
    default:
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
  client->_internal.on_connack = options->on_connack;
  client->_internal.on_publish = options->on_publish;
  client->_internal.on_suback = options->on_suback;
  client->_internal.on_unsuback = options->on_unsuback;
  client->_internal.on_puback = options->on_puback;
  client->_internal.on_pubcomp = options->on_pubcomp;
  client->_internal.on_connection_closed = options->on_connection_closed;
  client->_internal.on_transport_error = options->on_transport_error;
  client->_internal.user_context = options->user_context;
  // A session outliving the connection resends QoS 1/2 PUBLISH: they must be stored.
  return _az_mqtt_core_inflight_init(
      &client->_internal.core,
      options->inflight_control_buffer,
      options->inflight_message_buffer,
      !options->connect_options.clean_session);
}

AZ_NODISCARD az_result az_mqtt3_client_connect_start(az_mqtt3_client* client, int32_t timeout_ms)
{
  _az_PRECONDITION_NOT_NULL(client);

  if (_CORE(client).state != AZ_MQTT_CLIENT_STATE_DISCONNECTED)
  {
    return AZ_MQTT_ERROR_INVALID_STATE;
  }

  // Encoded now; the core sends it once the transport is up.
  az_span send_buf = _SEND_BUFFER(client);
  az_result rc = az_mqtt3_codec_encode_connect(&send_buf, &client->_internal.connect_options);
  if (az_result_failed(rc))
  {
    return rc;
  }
  return _az_mqtt_core_connect_start(&client->_internal.core, timeout_ms);
}

AZ_NODISCARD az_result az_mqtt3_client_connect(az_mqtt3_client* client, int32_t timeout_ms)
{
  az_result rc = az_mqtt3_client_connect_start(client, timeout_ms);
  return az_result_succeeded(rc) ? _az_mqtt_core_connect_wait(&client->_internal.core, _dispatch_packet)
                                 : rc;
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

  az_mqtt_core* const core = &client->_internal.core;
  az_result rc;
  uint16_t packet_id = 0;
  if (options->qos == AZ_MQTT_QOS_AT_MOST_ONCE)
  {
    az_span send_buf = _SEND_BUFFER(client);
    rc = az_mqtt3_codec_encode_publish(&send_buf, options, 0);
    rc = _az_mqtt_core_send_tracked_request(core, NULL, rc, send_buf);
  }
  else
  {
    az_span buffer;
    _az_RETURN_IF_FAILED(_az_mqtt_core_publish_buffer(core, &buffer));
    az_mqtt_inflight_entry* entry = NULL;
    _az_RETURN_IF_FAILED(_az_mqtt_core_inflight_reserve_entry(
        core,
        options->qos == AZ_MQTT_QOS_AT_LEAST_ONCE ? _AZ_MQTT_INFLIGHT_PUBLISH_QOS1
                                                  : _AZ_MQTT_INFLIGHT_PUBLISH_QOS2,
        UINT16_MAX,
        &entry));
    packet_id = entry->_internal.packet_id;
    az_span remaining = buffer;
    rc = az_mqtt3_codec_encode_publish(&remaining, options, packet_id);
    int64_t const deadline = options->message_expiry_interval == 0
        ? -1
        : az_mqtt_transport_clock_ms() + (int64_t)options->message_expiry_interval * 1000;
    rc = _az_mqtt_core_send_publish(core, entry, rc, buffer, remaining, deadline, 0);
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

  az_mqtt_inflight_entry* entry = NULL;
  _az_RETURN_IF_FAILED(_az_mqtt_core_inflight_reserve_entry(
      &client->_internal.core, _AZ_MQTT_INFLIGHT_SUBSCRIBE, UINT16_MAX, &entry));
  uint16_t const packet_id = entry->_internal.packet_id;
  az_span send_buf = _SEND_BUFFER(client);
  az_result rc = az_mqtt3_codec_encode_subscribe(
      &send_buf, subscriptions, subscription_count, packet_id);
  rc = _az_mqtt_core_send_tracked_request(&client->_internal.core, entry, rc, send_buf);
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

  az_mqtt_inflight_entry* entry = NULL;
  _az_RETURN_IF_FAILED(_az_mqtt_core_inflight_reserve_entry(
      &client->_internal.core, _AZ_MQTT_INFLIGHT_UNSUBSCRIBE, UINT16_MAX, &entry));
  uint16_t const packet_id = entry->_internal.packet_id;
  az_span send_buf = _SEND_BUFFER(client);
  az_result rc
      = az_mqtt3_codec_encode_unsubscribe(&send_buf, topic_filters, filter_count, packet_id);
  rc = _az_mqtt_core_send_tracked_request(&client->_internal.core, entry, rc, send_buf);
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
