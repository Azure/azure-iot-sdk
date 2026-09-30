// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_core.c
 * @brief Session engine shared by the MQTT 3.1.1 and 5.0 clients: connection,
 * packet framing, keep-alive and session state. Zero dynamic allocation.
 */

#include "az_mqtt_codec_internal.h"
#include "az_mqtt_core_internal.h"

#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <string.h>

/** @brief Most packets handled by one process-loop call. */
#define _AZ_MQTT_MAX_PACKETS_PER_LOOP 32

/** @brief Shorthand for the core's internal fields. */
#define _S(core) ((core)->_internal)

static int64_t _get_clock_ms(void) { return az_mqtt_transport_clock_ms(); }

/** @brief Absolute deadline @p timeout_ms from now; -1 (no limit) for a negative timeout. */
static int64_t _deadline(int32_t timeout_ms)
{
  return timeout_ms < 0 ? -1 : _get_clock_ms() + timeout_ms;
}

/** @brief Milliseconds left until @p deadline_ms, 0 if passed, -1 if unlimited. */
static int32_t _remaining(int64_t deadline_ms)
{
  if (deadline_ms < 0)
  {
    return -1;
  }
  int64_t left = deadline_ms - _get_clock_ms();
  return left <= 0 ? 0 : (left > INT32_MAX ? INT32_MAX : (int32_t)left);
}

void _az_mqtt_core_close(
    az_mqtt_core* core,
    az_result reason)
{
  bool const was_open = _S(core).state != AZ_MQTT_CLIENT_STATE_DISCONNECTED;
  az_mqtt_transport_close(_S(core).transport);
  _S(core).state = AZ_MQTT_CLIENT_STATE_DISCONNECTED;
  _S(core).recv_buf_pos = 0;
  _S(core).ping_outstanding = false;
  // on_closed may reconnect: callers compare generations before touching
  // anything that belonged to the old session.
  _S(core).session_generation++;
  if (was_open && _S(core).on_closed != NULL)
  {
    _S(core).on_closed(core, reason);
  }
}

az_result _az_mqtt_core_send(
    az_mqtt_core* core,
    az_span remaining)
{
  int32_t written = az_span_size(_S(core).send_buffer) - az_span_size(remaining);
  if (written <= 0)
  {
    return AZ_OK;
  }
  az_result rc = az_mqtt_transport_send(
      _S(core).transport, az_span_slice(_S(core).send_buffer, 0, written));
  if (az_result_succeeded(rc))
  {
    _S(core).last_send_time_ms = _get_clock_ms();
  }
  return rc;
}

/** @brief Buffer at least @p needed bytes, waiting no later than @p deadline_ms. */
static az_result _ensure_received(
    az_mqtt_core* core,
    int32_t needed,
    int64_t deadline_ms)
{
  while (_S(core).recv_buf_pos < needed)
  {
    az_span free_space = az_span_slice_to_end(_S(core).receive_buffer, _S(core).recv_buf_pos);
    if (az_span_size(free_space) == 0)
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }

    int32_t const wait_ms = _remaining(deadline_ms);
    az_span received;
    az_result rc = az_mqtt_transport_receive(_S(core).transport, free_space, wait_ms, &received);
    if (az_result_failed(rc))
    {
      return rc;
    }
    if (az_span_size(received) == 0)
    {
      if (wait_ms == 0)
      {
        return AZ_MQTT_ERROR_TIMEOUT;
      }
      continue; // Woke early; wait out the rest of the deadline.
    }
    _S(core).recv_buf_pos += az_span_size(received);
  }
  return AZ_OK;
}

/** @brief Drop @p count bytes from the front of the receive buffer. */
static void
_consume_recv(az_mqtt_core* core, int32_t count)
{
  if (count <= 0)
  {
    return;
  }
  int32_t remaining = _S(core).recv_buf_pos - count;
  if (remaining > 0)
  {
    memmove(
        az_span_ptr(_S(core).receive_buffer),
        az_span_ptr(_S(core).receive_buffer) + count,
        (size_t)remaining);
  }
  _S(core).recv_buf_pos = remaining;
}

/**
 * @brief Read one whole packet into the receive buffer.
 *
 * The caller consumes @p out_packet_size bytes (_consume_recv()) after handling the body.
 */
static az_result _read_packet(
    az_mqtt_core* core,
    int64_t deadline_ms,
    az_mqtt_packet_type* out_type,
    uint8_t* out_flags,
    az_span* out_body,
    int32_t* out_packet_size)
{
  // Fixed header: type byte + 1-4 byte Remaining Length.
  az_result rc = _ensure_received(core, 2, deadline_ms);
  if (az_result_failed(rc))
    return rc;

  int32_t header_size = 1;
  int32_t remaining_length = 0;
  int shift = 0;
  bool vbi_complete = false;

  for (int i = 0; i < 4; i++)
  {
    rc = _ensure_received(core, header_size + 1 + i, deadline_ms);
    if (az_result_failed(rc))
      return rc;

    uint8_t b = az_span_ptr(_S(core).receive_buffer)[1 + i];
    remaining_length |= (int32_t)(b & 0x7F) << shift;
    shift += 7;
    if ((b & 0x80) == 0)
    {
      header_size = 2 + i;
      vbi_complete = true;
      break;
    }
  }

  if (!vbi_complete)
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }

  int32_t total_packet_size = header_size + remaining_length;
  rc = _ensure_received(core, total_packet_size, deadline_ms);
  if (az_result_failed(rc))
    return rc;

  az_span header_span = az_span_slice(_S(core).receive_buffer, 0, total_packet_size);
  int32_t decoded_remaining;
  rc = _az_mqtt_decode_fixed_header(&header_span, out_type, out_flags, &decoded_remaining);
  if (az_result_failed(rc))
    return rc;

  *out_body = az_span_slice(header_span, 0, decoded_remaining);
  *out_packet_size = total_packet_size;
  _S(core).last_receive_time_ms = _get_clock_ms();
  // Any packet proves the link is alive, not only a PINGRESP.
  _S(core).ping_outstanding = false;

  return AZ_OK;
}

az_result _az_mqtt_core_connect(
    az_mqtt_core* core,
    int32_t timeout_ms,
    int32_t connect_size,
    _az_mqtt_core_dispatch_fn dispatch)
{
  int64_t const deadline = _deadline(timeout_ms);
  _S(core).recv_buf_pos = 0;
  _S(core).ping_outstanding = false;
  _S(core).state = AZ_MQTT_CLIENT_STATE_CONNECTING;

  // TCP/TLS connect, bounded by the same deadline as the CONNACK.
  az_result rc = az_mqtt_transport_connect_start(
      _S(core).transport, _S(core).hostname, _S(core).port, _S(core).tls_options);
  while (rc == AZ_OK)
  {
    rc = az_mqtt_transport_connect_poll(_S(core).transport, _remaining(deadline));
    if (rc == AZ_OK)
    {
      break;
    }
    if (rc == AZ_MQTT_ERROR_TIMEOUT && _remaining(deadline) != 0)
    {
      rc = AZ_OK; // Woke early; keep polling until the deadline.
    }
  }

  if (az_result_succeeded(rc))
  {
    rc = _az_mqtt_core_send(core, az_span_slice_to_end(_S(core).send_buffer, connect_size));
  }

  if (az_result_succeeded(rc))
  {
    az_mqtt_packet_type type;
    uint8_t flags;
    az_span body;
    int32_t packet_size;

    uint32_t const generation = _S(core).session_generation;
    rc = _read_packet(core, deadline, &type, &flags, &body, &packet_size);
    if (az_result_succeeded(rc))
    {
      rc = type == AZ_MQTT_PACKET_TYPE_CONNACK ? dispatch(core, type, flags, body)
                                               : AZ_MQTT_ERROR_PROTOCOL;
      if (_S(core).session_generation != generation)
      {
        // The CONNACK callback ended the session (and may have started another): leave it be.
        return _S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTED ? AZ_OK
                                                                 : AZ_MQTT_ERROR_NOT_CONNECTED;
      }
      _consume_recv(core, packet_size);
    }
    if (az_result_succeeded(rc) && _S(core).state != AZ_MQTT_CLIENT_STATE_CONNECTED)
    {
      rc = AZ_MQTT_ERROR_NOT_CONNECTED; // CONNACK refused; the callback got the reason.
    }
  }

  if (az_result_failed(rc))
  {
    _az_mqtt_core_close(core, rc);
    return rc;
  }

  _S(core).last_receive_time_ms = _get_clock_ms();
  return AZ_OK;
}

/**
 * @brief Send PINGREQ when due and detect a missing response.
 *
 * @param[out] out_next_ms Milliseconds until keep-alive next needs attention; -1 if never.
 */
static az_result _service_keep_alive(
    az_mqtt_core* core,
    int32_t* out_next_ms)
{
  *out_next_ms = -1;
  if (_S(core).state != AZ_MQTT_CLIENT_STATE_CONNECTED || _S(core).keep_alive_seconds == 0)
  {
    return AZ_OK;
  }

  int64_t const now = _get_clock_ms();
  int64_t const keep_alive_ms = (int64_t)_S(core).keep_alive_seconds * 1000;

  if (_S(core).ping_outstanding)
  {
    int64_t const waited = now - _S(core).ping_sent_time_ms;
    if (waited >= keep_alive_ms)
    {
      return AZ_MQTT_ERROR_KEEP_ALIVE_TIMEOUT;
    }
    *out_next_ms = (int32_t)(keep_alive_ms - waited);
    return AZ_OK;
  }

  int64_t const idle = now - _S(core).last_send_time_ms;
  if (idle < keep_alive_ms)
  {
    *out_next_ms = (int32_t)(keep_alive_ms - idle);
    return AZ_OK;
  }

  az_span send_buf = _S(core).send_buffer;
  az_result rc = _az_mqtt_encode_pingreq(&send_buf);
  if (az_result_succeeded(rc))
  {
    rc = _az_mqtt_core_send(core, send_buf);
  }
  if (az_result_succeeded(rc))
  {
    // The response window starts once the PINGREQ is out, not before a slow send.
    _S(core).ping_outstanding = true;
    _S(core).ping_sent_time_ms = _get_clock_ms();
    *out_next_ms = (int32_t)keep_alive_ms;
  }
  return rc;
}

az_result _az_mqtt_core_process_loop(
    az_mqtt_core* core,
    int32_t timeout_ms,
    _az_mqtt_core_dispatch_fn dispatch)
{
  if (_S(core).state == AZ_MQTT_CLIENT_STATE_DISCONNECTED)
  {
    return AZ_MQTT_ERROR_NOT_CONNECTED;
  }

  int32_t next_keep_alive_ms;
  az_result rc = _service_keep_alive(core, &next_keep_alive_ms);
  if (az_result_failed(rc))
  {
    _az_mqtt_core_close(core, rc);
    return rc;
  }

  // Never sleep past the next keep-alive action.
  int32_t wait_ms = timeout_ms;
  if (next_keep_alive_ms >= 0 && (wait_ms < 0 || next_keep_alive_ms < wait_ms))
  {
    wait_ms = next_keep_alive_ms;
  }
  int64_t deadline = _deadline(wait_ms);

  // Handle every complete packet already available, up to a bound.
  for (int i = 0;
       i < _AZ_MQTT_MAX_PACKETS_PER_LOOP && _S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTED;
       i++)
  {
    az_mqtt_packet_type type;
    uint8_t flags;
    az_span body;
    int32_t packet_size;

    uint32_t const generation = _S(core).session_generation;
    rc = _read_packet(core, deadline, &type, &flags, &body, &packet_size);
    if (rc == AZ_MQTT_ERROR_TIMEOUT)
    {
      rc = AZ_OK;
      break;
    }
    if (az_result_succeeded(rc))
    {
      rc = dispatch(core, type, flags, body);
      if (_S(core).session_generation != generation)
      {
        // A callback ended this session, and may have connected a new one whose
        // receive buffer must not be touched: stop here.
        return az_result_failed(rc) ? rc : AZ_OK;
      }
      _consume_recv(core, packet_size);
    }
    if (az_result_failed(rc))
    {
      _az_mqtt_core_close(core, rc);
      return rc;
    }
    deadline = _get_clock_ms(); // Only what is already there from now on.
  }

  // The wait may have been cut to the keep-alive time: act on it now.
  rc = _service_keep_alive(core, &next_keep_alive_ms);
  if (az_result_failed(rc))
  {
    _az_mqtt_core_close(core, rc);
    return rc;
  }
  return AZ_OK;
}
