// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt_core.c
 * @brief Session engine shared by the MQTT 3.1.1 and 5.0 clients: connection,
 * packet framing, keep-alive and session state. Zero dynamic allocation.
 */

#include "az_mqtt_codec_internal.h"
#include "az_mqtt_core_internal.h"

#include <azure/core/internal/az_log_internal.h>
#include <azure/core/internal/az_precondition_internal.h>
#include <azure/core/internal/az_result_internal.h>
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

/** @brief The in-flight entries; @p out_count of them. */
static az_mqtt_inflight_entry* _get_inflight_entries(az_mqtt_core* core, int32_t* out_count)
{
  *out_count
      = az_span_size(_S(core).inflight_control_buffer) / (int32_t)sizeof(az_mqtt_inflight_entry);
  return (az_mqtt_inflight_entry*)az_span_ptr(_S(core).inflight_control_buffer);
}

// ──────────────────────── Logging ────────────────────────────
// Only host, port, packet types and lengths, results and native error codes:
// never credentials, keys, certificates, topics or payloads.

#ifndef AZ_NO_LOGGING
/** @brief Append @p text to @p *out, truncated to what fits. */
static void _log_append(az_span* out, az_span text)
{
  int32_t const room = az_span_size(*out);
  int32_t const size = az_span_size(text) < room ? az_span_size(text) : room;
  *out = az_span_copy(*out, az_span_slice(text, 0, size));
}

/** @brief Append @p value in decimal to @p *out. */
static void _log_append_i32(az_span* out, int32_t value)
{
  az_span rest;
  if (az_result_succeeded(az_span_i32toa(*out, value, &rest)))
  {
    *out = rest;
  }
}

/** @brief Append @p value as 0x and 8 hex digits to @p *out. */
static void _log_append_hex(az_span* out, uint32_t value)
{
  uint8_t digits[10] = { '0', 'x' };
  for (int i = 9; i >= 2; i--, value >>= 4)
  {
    digits[i] = (uint8_t)"0123456789abcdef"[value & 0xFU];
  }
  _log_append(out, AZ_SPAN_FROM_BUFFER(digits));
}

/** @brief Write what @p buffer holds up to @p rest. */
static void _log_write(az_log_classification classification, az_span buffer, az_span rest)
{
  _az_LOG_WRITE(classification, az_span_slice(buffer, 0, az_span_size(buffer) - az_span_size(rest)));
}

/** @brief "connect <host>:<port>[ tls]". */
static void _log_connect(az_mqtt_core* core)
{
  if (!_az_LOG_SHOULD_WRITE(AZ_LOG_MQTT_CONNECTION))
  {
    return;
  }
  uint8_t buffer[240];
  az_span out = AZ_SPAN_FROM_BUFFER(buffer);
  _log_append(&out, AZ_SPAN_FROM_STR("connect "));
  int32_t const host_size = az_span_size(_S(core).hostname);
  _log_append(&out, az_span_slice(_S(core).hostname, 0, host_size > 96 ? 96 : host_size));
  _log_append(&out, AZ_SPAN_FROM_STR(":"));
  _log_append_i32(&out, _S(core).port);
  if (_S(core).tls_options != NULL)
  {
    _log_append(&out, AZ_SPAN_FROM_STR(" tls"));
  }
  az_mqtt_proxy_options const* const proxy = _S(core).proxy;
  if (proxy != NULL && az_span_size(proxy->host) > 0) // Never its credentials.
  {
    int32_t const proxy_size = az_span_size(proxy->host);
    _log_append(&out, AZ_SPAN_FROM_STR(" via "));
    _log_append(&out, az_span_slice(proxy->host, 0, proxy_size > 96 ? 96 : proxy_size));
    _log_append(&out, AZ_SPAN_FROM_STR(":"));
    _log_append_i32(&out, proxy->port);
  }
  _log_write(AZ_LOG_MQTT_CONNECTION, AZ_SPAN_FROM_BUFFER(buffer), out);
}

/** @brief "closed <result> native <source>:<code>". */
static void _log_close(az_mqtt_core* core, az_result reason)
{
  if (!_az_LOG_SHOULD_WRITE(AZ_LOG_MQTT_CONNECTION))
  {
    return;
  }
  (void)core;
  uint8_t buffer[24];
  az_span out = AZ_SPAN_FROM_BUFFER(buffer);
  _log_append(&out, AZ_SPAN_FROM_STR("closed "));
  _log_append_hex(&out, (uint32_t)reason);
  _log_write(AZ_LOG_MQTT_CONNECTION, AZ_SPAN_FROM_BUFFER(buffer), out);
}

/** @brief "native <source>:<code> <result> attempt <n>". */
static void _log_native_error(az_mqtt_native_error const* error)
{
  if (!_az_LOG_SHOULD_WRITE(AZ_LOG_MQTT_CONNECTION))
  {
    return;
  }
  uint8_t buffer[64];
  az_span out = AZ_SPAN_FROM_BUFFER(buffer);
  _log_append(&out, AZ_SPAN_FROM_STR("native "));
  _log_append_i32(&out, (int32_t)error->source);
  _log_append(&out, AZ_SPAN_FROM_STR(":"));
  _log_append_i32(&out, error->code);
  _log_append(&out, AZ_SPAN_FROM_STR(" "));
  _log_append_hex(&out, (uint32_t)error->result);
  _log_append(&out, AZ_SPAN_FROM_STR(" attempt "));
  _log_append_i32(&out, (int32_t)error->connect_attempt);
  _log_write(AZ_LOG_MQTT_CONNECTION, AZ_SPAN_FROM_BUFFER(buffer), out);
}

/** @brief "<sent|received> <TYPE> <length>"; @p first_byte is the fixed header's. */
static void _log_packet(az_span direction, uint8_t first_byte, int32_t length)
{
  if (!_az_LOG_SHOULD_WRITE(AZ_LOG_MQTT_PACKET))
  {
    return;
  }
  static char const names[16][12]
      = { "RESERVED", "CONNECT", "CONNACK",     "PUBLISH",  "PUBACK",  "PUBREC",
          "PUBREL",   "PUBCOMP", "SUBSCRIBE",   "SUBACK",   "UNSUBSCRIBE", "UNSUBACK",
          "PINGREQ",  "PINGRESP", "DISCONNECT", "AUTH" };
  uint8_t buffer[40];
  az_span out = AZ_SPAN_FROM_BUFFER(buffer);
  _log_append(&out, direction);
  _log_append(&out, az_span_create_from_str((char*)(uintptr_t)names[first_byte >> 4]));
  _log_append(&out, AZ_SPAN_FROM_STR(" "));
  _log_append_i32(&out, length);
  _log_write(AZ_LOG_MQTT_PACKET, AZ_SPAN_FROM_BUFFER(buffer), out);
}
#else
#define _log_connect(core)
#define _log_close(core, reason)
#define _log_native_error(error)
#define _log_packet(direction, first_byte, length)
#endif // AZ_NO_LOGGING

static void _on_native_error(az_mqtt_native_error const* error, void* context)
{
  az_mqtt_core* const core = (az_mqtt_core*)context;
  _log_native_error(error);
  if (_S(core).on_transport_error != NULL)
  {
    _S(core).on_transport_error(core, error);
  }
}

void _az_mqtt_core_register_transport_errors(az_mqtt_core* core)
{
  az_mqtt_transport_set_error_callback(_S(core).transport, _on_native_error, core);
}

// ──────────────────────── Stored PUBLISH packets ─────────────
// inflight_message_buffer holds records oldest first, each a header then the packet:
// packet id (2 bytes), packet length (4), deadline (8; -1: none), expiry offset (4).

/** @brief Header of a stored PUBLISH. */
typedef struct
{
  uint16_t packet_id;
  int32_t length;
  int64_t deadline_ms;
  uint32_t expiry_offset;
} _message_header;

static void _write_header(uint8_t* at, _message_header const* header)
{
  memcpy(at, &header->packet_id, 2);
  memcpy(at + 2, &header->length, 4);
  memcpy(at + 6, &header->deadline_ms, 8);
  memcpy(at + 14, &header->expiry_offset, 4);
}

static void _read_header(uint8_t const* at, _message_header* header)
{
  memcpy(&header->packet_id, at, 2);
  memcpy(&header->length, at + 2, 4);
  memcpy(&header->deadline_ms, at + 6, 8);
  memcpy(&header->expiry_offset, at + 14, 4);
}

/** @brief Offset of the stored PUBLISH @p packet_id, or -1. */
static int32_t _find_message(az_mqtt_core* core, uint16_t packet_id, _message_header* out_header)
{
  uint8_t const* const base = az_span_ptr(_S(core).inflight_message_buffer);
  for (int32_t at = 0; at < _S(core).inflight_message_used;
       at += AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD + out_header->length)
  {
    _read_header(base + at, out_header);
    if (out_header->packet_id == packet_id)
    {
      return at;
    }
  }
  return -1;
}

/** @brief Free the stored PUBLISH @p packet_id, if any, keeping the others in order. */
static void _free_message(az_mqtt_core* core, uint16_t packet_id)
{
  _message_header header;
  int32_t const at = _find_message(core, packet_id, &header);
  if (at >= 0)
  {
    uint8_t* const base = az_span_ptr(_S(core).inflight_message_buffer);
    int32_t const size = AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD + header.length;
    memmove(base + at, base + at + size, (size_t)(_S(core).inflight_message_used - at - size));
    _S(core).inflight_message_used -= size;
  }
}

/** @brief Free every in-flight entry and stored PUBLISH. */
static void _clear_inflight_entries(az_mqtt_core* core)
{
  if (az_span_size(_S(core).inflight_control_buffer) > 0) // az_span_fill: no NULL pointer.
  {
    az_span_fill(_S(core).inflight_control_buffer, 0);
  }
  _S(core).inflight_message_used = 0;
}

/** @brief Drop free entries from among those in use, keeping their order. */
static void _compact_inflight_entries(az_mqtt_core* core)
{
  int32_t count;
  az_mqtt_inflight_entry* entries = _get_inflight_entries(core, &count);
  int32_t used = 0;
  for (int32_t i = 0; i < count; i++)
  {
    if (entries[i]._internal.kind != _AZ_MQTT_INFLIGHT_FREE)
    {
      entries[used++] = entries[i];
    }
  }
  for (int32_t i = used; i < count; i++)
  {
    memset(&entries[i], 0, sizeof(entries[i]));
  }
}

/**
 * @brief At a session end: keep what a resumed session continues (PUBLISH, PUBREL, inbound QoS 2;
 * see _az_mqtt_core_inflight_resume()); free SUBSCRIBE and UNSUBSCRIBE, which are never resent.
 */
static void _keep_resumable_inflight_entries(az_mqtt_core* core)
{
  int32_t count;
  az_mqtt_inflight_entry* entries = _get_inflight_entries(core, &count);
  for (int32_t i = 0; i < count; i++)
  {
    uint8_t const kind = entries[i]._internal.kind;
    if (kind == _AZ_MQTT_INFLIGHT_SUBSCRIBE || kind == _AZ_MQTT_INFLIGHT_UNSUBSCRIBE)
    {
      entries[i]._internal.kind = _AZ_MQTT_INFLIGHT_FREE;
    }
    entries[i]._internal.mark = _AZ_MQTT_INFLIGHT_MARK_NONE; // Set again by the next CONNACK.
  }
  _compact_inflight_entries(core);
}

void _az_mqtt_core_close(
    az_mqtt_core* core,
    az_result reason)
{
  bool const was_open = _S(core).state != AZ_MQTT_CLIENT_STATE_DISCONNECTED;
  if (reason == AZ_OK && _S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTED)
  {
    az_mqtt_transport_shutdown(_S(core).transport); // Orderly: after DISCONNECT.
  }
  az_mqtt_transport_close(_S(core).transport);
  if (was_open)
  {
    _log_close(core, reason);
  }
  _S(core).state = AZ_MQTT_CLIENT_STATE_DISCONNECTED;
  _S(core).recv_buf_pos = 0;
  _S(core).ping_outstanding = false;
  _keep_resumable_inflight_entries(core);
  _S(core).server_maximum_packet_size = 0;
  // on_closed may reconnect: callers compare generations before touching
  // anything that belonged to the old session.
  _S(core).session_generation++;
  if (was_open && _S(core).on_closed != NULL)
  {
    _S(core).on_closed(core, reason);
  }
}

/** @brief Send @p packet, unless over the server's Maximum Packet Size. Does not close. */
static az_result _send_packet(az_mqtt_core* core, az_span packet)
{
  int32_t const size = az_span_size(packet);
  if (_S(core).server_maximum_packet_size > 0
      && (uint32_t)size > _S(core).server_maximum_packet_size)
  {
    return AZ_MQTT_ERROR_PACKET_TOO_LARGE;
  }
  az_result rc = az_mqtt_transport_send(_S(core).transport, packet);
  if (az_result_succeeded(rc))
  {
    _S(core).last_send_time_ms = _get_clock_ms();
    _log_packet(AZ_SPAN_FROM_STR("sent "), az_span_ptr(packet)[0], size);
  }
  return rc;
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
  return _send_packet(core, az_span_slice(_S(core).send_buffer, 0, written));
}

// ──────────────────────── In-flight table ────────────────────

az_result _az_mqtt_core_inflight_init(
    az_mqtt_core* core,
    az_span entries,
    az_span messages,
    bool keep_messages)
{
  int32_t const messages_size = az_span_size(messages);
  if (messages_size > 0
      && messages_size < az_span_size(_S(core).send_buffer) + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD)
  {
    return AZ_MQTT_ERROR_INVALID_CONFIG; // Must hold any PUBLISH the send buffer can.
  }
  int32_t count = az_span_size(entries) / (int32_t)sizeof(az_mqtt_inflight_entry);
  count = count > UINT16_MAX ? UINT16_MAX : count;
  _S(core).inflight_control_buffer
      = az_span_slice(entries, 0, count * (int32_t)sizeof(az_mqtt_inflight_entry));
  _S(core).inflight_message_buffer = messages;
  _S(core).keep_messages = keep_messages;
  _clear_inflight_entries(core);
  return AZ_OK;
}

/** @brief Whether an outgoing request holds @p packet_id (inbound QoS 2 uses server identifiers). */
static bool _is_outgoing_packet_id_in_use(az_mqtt_core* core, uint16_t packet_id)
{
  int32_t count;
  az_mqtt_inflight_entry const* entries = _get_inflight_entries(core, &count);
  for (int32_t i = 0; i < count; i++)
  {
    uint8_t const kind = entries[i]._internal.kind;
    if (entries[i]._internal.packet_id == packet_id && kind != _AZ_MQTT_INFLIGHT_FREE
        && kind != _AZ_MQTT_INFLIGHT_INBOUND_QOS2)
    {
      return true;
    }
  }
  return false;
}

/** @brief Whether @p entry is an outgoing QoS 1/2 PUBLISH exchange (PUBLISH or PUBREL stage). */
static bool _is_publish_exchange(az_mqtt_inflight_entry const* entry)
{
  uint8_t const k = entry->_internal.kind;
  return k != _AZ_MQTT_INFLIGHT_FREE && k <= _AZ_MQTT_INFLIGHT_PUBREL;
}

/** @brief Outgoing QoS 1/2 PUBLISH exchanges incomplete on this connection (none marked). */
static uint16_t _publishes_in_flight(az_mqtt_core* core)
{
  int32_t count;
  az_mqtt_inflight_entry const* entries = _get_inflight_entries(core, &count);
  uint16_t publishes = 0;
  for (int32_t i = 0; i < count; i++)
  {
    if (_is_publish_exchange(&entries[i])
        && entries[i]._internal.mark == _AZ_MQTT_INFLIGHT_MARK_NONE)
    {
      publishes++;
    }
  }
  return publishes;
}

az_result _az_mqtt_core_inflight_reserve_entry(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t publish_limit,
    az_mqtt_inflight_entry** out_entry)
{
  int32_t count;
  az_mqtt_inflight_entry* entries = _get_inflight_entries(core, &count);
  az_mqtt_inflight_entry* entry = NULL;
  for (int32_t i = 0; i < count && entry == NULL; i++)
  {
    if (entries[i]._internal.kind == _AZ_MQTT_INFLIGHT_FREE)
    {
      entry = &entries[i]; // After every entry in use: the newest.
    }
  }
  bool resend_pending = false;
  for (int32_t i = 0; i < count && kind <= _AZ_MQTT_INFLIGHT_PUBLISH_QOS2; i++)
  {
    resend_pending = resend_pending || entries[i]._internal.mark == _AZ_MQTT_INFLIGHT_MARK_RESEND;
  }
  if (entry == NULL || resend_pending || _publishes_in_flight(core) >= publish_limit)
  {
    return AZ_MQTT_ERROR_FLOW_CONTROL; // A new PUBLISH never overtakes a resend.
  }
  // A free entry leaves at most 65534 identifiers in use, so this ends.
  do
  {
    if (++_S(core).next_packet_id == 0)
    {
      _S(core).next_packet_id = 1;
    }
  } while (_is_outgoing_packet_id_in_use(core, _S(core).next_packet_id));
  entry->_internal.packet_id = _S(core).next_packet_id;
  entry->_internal.kind = (uint8_t)kind;
  entry->_internal.mark = _AZ_MQTT_INFLIGHT_MARK_NONE;
  *out_entry = entry;
  return AZ_OK;
}

void _az_mqtt_core_inflight_free_entry(az_mqtt_core* core, az_mqtt_inflight_entry* entry)
{
  uint8_t const kind = entry->_internal.kind;
  if (kind == _AZ_MQTT_INFLIGHT_PUBLISH_QOS1 || kind == _AZ_MQTT_INFLIGHT_PUBLISH_QOS2)
  {
    _free_message(core, entry->_internal.packet_id);
  }
  entry->_internal.kind = _AZ_MQTT_INFLIGHT_FREE;
  _compact_inflight_entries(core);
}

void _az_mqtt_core_inflight_to_pubrel(az_mqtt_core* core, az_mqtt_inflight_entry* entry)
{
  _free_message(core, entry->_internal.packet_id); // Only the PUBREL is resent from now on.
  entry->_internal.kind = _AZ_MQTT_INFLIGHT_PUBREL;
}

az_result _az_mqtt_core_publish_buffer(az_mqtt_core* core, az_span* out_buffer)
{
  if (!_S(core).keep_messages)
  {
    *out_buffer = _S(core).send_buffer;
    return AZ_OK;
  }
  int32_t const size = az_span_size(_S(core).inflight_message_buffer);
  if (size == 0)
  {
    return AZ_MQTT_ERROR_INVALID_CONFIG;
  }
  int32_t const start = _S(core).inflight_message_used + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD;
  if (start >= size)
  {
    return AZ_MQTT_ERROR_OUT_OF_STORAGE;
  }
  int32_t const send_size = az_span_size(_S(core).send_buffer);
  *out_buffer = az_span_slice(
      _S(core).inflight_message_buffer, start, size - start < send_size ? size : start + send_size);
  return AZ_OK;
}

az_result _az_mqtt_core_send_publish(
    az_mqtt_core* core,
    az_mqtt_inflight_entry* entry,
    az_result encode_result,
    az_span buffer,
    az_span remaining,
    int64_t deadline_ms,
    uint32_t expiry_offset)
{
  bool const stored = az_span_ptr(buffer) != az_span_ptr(_S(core).send_buffer);
  az_result rc = encode_result;
  int32_t const size = az_span_size(buffer) - az_span_size(remaining);
  if (rc == AZ_ERROR_NOT_ENOUGH_SPACE && stored
      && az_span_size(buffer) < az_span_size(_S(core).send_buffer))
  {
    rc = AZ_MQTT_ERROR_OUT_OF_STORAGE; // It would fit once acknowledgements free room.
  }
  if (az_result_succeeded(rc) && _S(core).server_maximum_packet_size > 0
      && (uint32_t)size > _S(core).server_maximum_packet_size)
  {
    rc = AZ_MQTT_ERROR_PACKET_TOO_LARGE; // Refused before sending: the session stays up.
  }
  if (az_result_failed(rc))
  {
    _az_mqtt_core_inflight_free_entry(core, entry);
    return rc;
  }
  if (stored)
  {
    _message_header const header = { entry->_internal.packet_id, size, deadline_ms, expiry_offset };
    _write_header(az_span_ptr(buffer) - AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD, &header);
    _S(core).inflight_message_used += AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD + size;
  }
  rc = _send_packet(core, az_span_slice(buffer, 0, size));
  if (az_result_failed(rc))
  {
    _az_mqtt_core_close(core, rc); // Kept: resent if the session resumes.
  }
  return rc;
}

#ifndef AZ_NO_LOGGING
/** @brief "dropped <packet id> <result>". */
static void _log_dropped(uint16_t packet_id, az_result status)
{
  if (!_az_LOG_SHOULD_WRITE(AZ_LOG_MQTT_CONNECTION))
  {
    return;
  }
  uint8_t buffer[40];
  az_span out = AZ_SPAN_FROM_BUFFER(buffer);
  _log_append(&out, AZ_SPAN_FROM_STR("dropped publish "));
  _log_append_i32(&out, packet_id);
  _log_append(&out, AZ_SPAN_FROM_STR(" "));
  _log_append_hex(&out, (uint32_t)status);
  _log_write(AZ_LOG_MQTT_CONNECTION, AZ_SPAN_FROM_BUFFER(buffer), out);
}
#else
#define _log_dropped(packet_id, status)
#endif // AZ_NO_LOGGING

/**
 * @brief Free @p entry and report it to @p dropped with @p status.
 * @return Whether the session is still @p generation (a callback may end it).
 */
static bool _drop(
    az_mqtt_core* core,
    az_mqtt_inflight_entry* entry,
    az_result status,
    _az_mqtt_core_publish_dropped_fn dropped,
    uint32_t generation)
{
  uint16_t const packet_id = entry->_internal.packet_id;
  bool const qos1 = entry->_internal.kind == _AZ_MQTT_INFLIGHT_PUBLISH_QOS1;
  _az_mqtt_core_inflight_free_entry(core, entry);
  _log_dropped(packet_id, status);
  dropped(core, packet_id, qos1, status);
  return _S(core).session_generation == generation;
}

az_result _az_mqtt_core_inflight_resume(
    az_mqtt_core* core,
    bool session_present,
    _az_mqtt_core_encode_pubrel_fn encode_pubrel,
    uint16_t publish_limit,
    _az_mqtt_core_publish_dropped_fn dropped)
{
  uint32_t const generation = _S(core).session_generation;
  int32_t count;
  az_mqtt_inflight_entry* entries = _get_inflight_entries(core, &count);
  if (!session_present)
  {
    // The server kept none of it. Mark first: callbacks may publish anew into the same table.
    for (int32_t i = 0; i < count; i++)
    {
      if (entries[i]._internal.kind == _AZ_MQTT_INFLIGHT_INBOUND_QOS2)
      {
        entries[i]._internal.kind = _AZ_MQTT_INFLIGHT_FREE;
      }
      else if (_is_publish_exchange(&entries[i]))
      {
        entries[i]._internal.mark = _AZ_MQTT_INFLIGHT_MARK_STALE;
      }
    }
    _compact_inflight_entries(core);
    for (int32_t i = 0; i < count; i++)
    {
      if (entries[i]._internal.mark == _AZ_MQTT_INFLIGHT_MARK_STALE)
      {
        if (!_drop(core, &entries[i], AZ_MQTT_ERROR_SESSION_NOT_RESUMED, dropped, generation))
        {
          return AZ_OK;
        }
        i = -1; // Entries moved, and new ones may follow: from the start.
      }
    }
    return AZ_OK;
  }
  for (int32_t i = 0; i < count; i++)
  {
    uint8_t const k = entries[i]._internal.kind;
    if (k == _AZ_MQTT_INFLIGHT_PUBLISH_QOS1 || k == _AZ_MQTT_INFLIGHT_PUBLISH_QOS2)
    {
      entries[i]._internal.mark = _AZ_MQTT_INFLIGHT_MARK_RESEND; // Before any callback.
    }
  }
  // Callbacks only add PUBLISH entries: the PUBRELs are those of the earlier connection.
  for (int32_t i = 0; i < count; i++)
  {
    if (entries[i]._internal.kind != _AZ_MQTT_INFLIGHT_PUBREL)
    {
      continue;
    }
    az_span send_buf = _S(core).send_buffer;
    _az_RETURN_IF_FAILED(encode_pubrel(&send_buf, entries[i]._internal.packet_id));
    int32_t const size = az_span_size(_S(core).send_buffer) - az_span_size(send_buf);
    if (_S(core).server_maximum_packet_size > 0
        && (uint32_t)size > _S(core).server_maximum_packet_size)
    {
      // [MQTT-3.1.2-25]: not sent. The exchange cannot complete: dropped, the connection kept.
      if (!_drop(core, &entries[i], AZ_MQTT_ERROR_PACKET_TOO_LARGE, dropped, generation))
      {
        return AZ_OK;
      }
      i--; // The next one moved into entries[i].
      continue;
    }
    _az_RETURN_IF_FAILED(_az_mqtt_core_send_request(core, send_buf));
  }
  return _az_mqtt_core_inflight_resend_due(core, publish_limit, dropped);
}

az_result _az_mqtt_core_inflight_resend_due(
    az_mqtt_core* core,
    uint16_t publish_limit,
    _az_mqtt_core_publish_dropped_fn dropped)
{
  uint32_t const generation = _S(core).session_generation;
  for (;;)
  {
    int32_t count;
    az_mqtt_inflight_entry* entries = _get_inflight_entries(core, &count);
    az_mqtt_inflight_entry* entry = NULL;
    for (int32_t i = 0; i < count && entry == NULL; i++)
    {
      // The oldest: entries are in reservation order.
      entry = entries[i]._internal.mark == _AZ_MQTT_INFLIGHT_MARK_RESEND ? &entries[i] : NULL;
    }
    if (entry == NULL)
    {
      return AZ_OK;
    }
    _message_header header;
    int32_t const at = _find_message(core, entry->_internal.packet_id, &header);
    az_result drop = AZ_OK;
    if (at < 0)
    {
      drop = AZ_MQTT_ERROR_SESSION_NOT_RESUMED; // No copy was kept.
    }
    else if (header.deadline_ms >= 0 && _get_clock_ms() >= header.deadline_ms)
    {
      drop = AZ_MQTT_ERROR_MESSAGE_EXPIRED;
    }
    else if (
        _S(core).server_maximum_packet_size > 0
        && (uint32_t)header.length > _S(core).server_maximum_packet_size)
    {
      drop = AZ_MQTT_ERROR_PACKET_TOO_LARGE; // [MQTT-3.1.2-25]: discarded as if sent.
    }
    if (az_result_failed(drop))
    {
      if (!_drop(core, entry, drop, dropped, generation))
      {
        return AZ_OK;
      }
      continue;
    }
    if (_publishes_in_flight(core) >= publish_limit)
    {
      return AZ_OK; // Resent once an acknowledgement makes room.
    }
    entry->_internal.mark = _AZ_MQTT_INFLIGHT_MARK_NONE;
    uint8_t* const packet
        = az_span_ptr(_S(core).inflight_message_buffer) + at + AZ_MQTT_INFLIGHT_MESSAGE_OVERHEAD;
    packet[0] |= 0x08; // DUP
    if (header.expiry_offset != 0 && header.deadline_ms >= 0)
    {
      // The time left, as a server forwarding it would [MQTT-3.3.2-6]: whole seconds, rounded up.
      uint32_t const left = (uint32_t)((header.deadline_ms - _get_clock_ms() + 999) / 1000);
      for (int b = 0; b < 4; b++)
      {
        packet[header.expiry_offset + (uint32_t)b] = (uint8_t)(left >> (24 - 8 * b));
      }
    }
    az_result const rc = _send_packet(core, az_span_create(packet, header.length));
    if (az_result_failed(rc))
    {
      _az_mqtt_core_close(core, rc);
      return rc;
    }
  }
}

az_mqtt_inflight_entry* _az_mqtt_core_inflight_find_entry(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t packet_id)
{
  int32_t count;
  az_mqtt_inflight_entry* entries = _get_inflight_entries(core, &count);
  for (int32_t i = 0; i < count; i++)
  {
    if (entries[i]._internal.kind == (uint8_t)kind && entries[i]._internal.packet_id == packet_id)
    {
      return &entries[i];
    }
  }
  return NULL;
}

bool _az_mqtt_core_inflight_release_entry(
    az_mqtt_core* core,
    _az_mqtt_inflight_kind kind,
    uint16_t packet_id)
{
  az_mqtt_inflight_entry* entry = _az_mqtt_core_inflight_find_entry(core, kind, packet_id);
  if (entry != NULL)
  {
    _az_mqtt_core_inflight_free_entry(core, entry);
  }
  return entry != NULL;
}

az_result _az_mqtt_core_inflight_track_inbound_qos2(
    az_mqtt_core* core,
    uint16_t packet_id,
    bool* out_is_duplicate)
{
  *out_is_duplicate = false;
  int32_t count;
  az_mqtt_inflight_entry* entries = _get_inflight_entries(core, &count);
  az_mqtt_inflight_entry* free_entry = NULL;
  for (int32_t i = 0; i < count; i++)
  {
    if (entries[i]._internal.kind == _AZ_MQTT_INFLIGHT_INBOUND_QOS2
        && entries[i]._internal.packet_id == packet_id)
    {
      *out_is_duplicate = true;
      return AZ_OK;
    }
    if (free_entry == NULL && entries[i]._internal.kind == _AZ_MQTT_INFLIGHT_FREE)
    {
      free_entry = &entries[i];
    }
  }
  if (free_entry == NULL)
  {
    return AZ_MQTT_ERROR_FLOW_CONTROL;
  }
  free_entry->_internal.packet_id = packet_id;
  free_entry->_internal.kind = _AZ_MQTT_INFLIGHT_INBOUND_QOS2;
  free_entry->_internal.mark = _AZ_MQTT_INFLIGHT_MARK_NONE;
  return AZ_OK;
}

az_result _az_mqtt_core_send_tracked_request(
    az_mqtt_core* core,
    az_mqtt_inflight_entry* entry,
    az_result encode_result,
    az_span remaining)
{
  az_result rc = encode_result;
  int32_t const size = az_span_size(_S(core).send_buffer) - az_span_size(remaining);
  if (az_result_succeeded(rc) && _S(core).server_maximum_packet_size > 0
      && (uint32_t)size > _S(core).server_maximum_packet_size)
  {
    rc = AZ_MQTT_ERROR_PACKET_TOO_LARGE; // Refused before sending: the session stays up.
  }
  if (az_result_failed(rc))
  {
    if (entry != NULL)
    {
      _az_mqtt_core_inflight_free_entry(core, entry);
    }
    return rc;
  }
  return _az_mqtt_core_send_request(core, remaining);
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
  // Fixed header: type byte + 1-4 byte Remaining Length. The type byte is checked first, so a
  // malformed one fails without waiting for more.
  az_result rc = _ensure_received(core, 1, deadline_ms);
  if (az_result_failed(rc))
    return rc;
  if (!_az_mqtt_fixed_header_flags_valid(az_span_ptr(_S(core).receive_buffer)[0]))
  {
    return AZ_MQTT_ERROR_MALFORMED_PACKET;
  }

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

az_result _az_mqtt_core_connect_start(az_mqtt_core* core, int32_t timeout_ms)
{
  _S(core).recv_buf_pos = 0;
  _S(core).ping_outstanding = false;
  _S(core).connect_sent = false;
  _S(core).timer_ms = _deadline(timeout_ms);
  _S(core).state = AZ_MQTT_CLIENT_STATE_CONNECTING;
  _log_connect(core);

  az_result rc = az_mqtt_transport_connect_start(
      _S(core).transport, _S(core).hostname, _S(core).port, _S(core).tls_options);
  if (az_result_failed(rc))
  {
    _az_mqtt_core_close(core, rc);
  }
  return rc;
}

az_result _az_mqtt_core_connect_wait(az_mqtt_core* core, _az_mqtt_core_dispatch_fn dispatch)
{
  uint32_t const generation = _S(core).session_generation;
  az_result rc = AZ_OK;
  while (az_result_succeeded(rc) && _S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTING
         && _S(core).session_generation == generation)
  {
    rc = _az_mqtt_core_process_loop(core, -1, dispatch);
  }
  if (az_result_failed(rc))
  {
    return rc;
  }
  return _S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTED ? AZ_OK : AZ_MQTT_ERROR_NOT_CONNECTED;
}

/** @brief Whether the connect deadline has passed. */
static bool _connect_expired(az_mqtt_core const* core) { return _remaining(_S(core).timer_ms) == 0; }

/**
 * @brief CONNECTING: progress the transport connect and send CONNECT once it is up.
 *
 * @param[in,out] deadline_ms Wait deadline; lowered to the connect deadline.
 */
static az_result _service_connect(az_mqtt_core* core, int64_t* deadline_ms)
{
  int64_t const connect_deadline = _S(core).timer_ms;
  if (connect_deadline >= 0 && (*deadline_ms < 0 || connect_deadline < *deadline_ms))
  {
    *deadline_ms = connect_deadline;
  }
  if (_S(core).connect_sent)
  {
    return AZ_OK;
  }

  az_result rc = az_mqtt_transport_connect_poll(_S(core).transport, _remaining(*deadline_ms));
  if (rc == AZ_MQTT_ERROR_TIMEOUT)
  {
    return _connect_expired(core) ? rc : AZ_OK; // Not up yet.
  }
  if (az_result_failed(rc))
  {
    return rc;
  }

  // Send the CONNECT encoded at the start of the send buffer.
  az_span packet = _S(core).send_buffer;
  az_mqtt_packet_type type;
  uint8_t flags;
  int32_t remaining_length;
  rc = _az_mqtt_decode_fixed_header(&packet, &type, &flags, &remaining_length);
  if (az_result_succeeded(rc))
  {
    rc = _az_mqtt_core_send(core, az_span_slice_to_end(packet, remaining_length));
  }
  _S(core).connect_sent = az_result_succeeded(rc);
  return rc;
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
    int64_t const waited = now - _S(core).timer_ms;
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
    _S(core).timer_ms = _get_clock_ms();
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

  if (_S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTING)
  {
    rc = _service_connect(core, &deadline);
    if (az_result_failed(rc))
    {
      _az_mqtt_core_close(core, rc);
      return rc;
    }
  }

  // Handle every complete packet already available, up to a bound.
  for (int i = 0; i < _AZ_MQTT_MAX_PACKETS_PER_LOOP
       && (_S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTED
           || (_S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTING && _S(core).connect_sent));
       i++)
  {
    bool const connecting = _S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTING;
    az_mqtt_packet_type type;
    uint8_t flags;
    az_span body;
    int32_t packet_size;

    uint32_t const generation = _S(core).session_generation;
    rc = _read_packet(core, deadline, &type, &flags, &body, &packet_size);
    if (rc == AZ_MQTT_ERROR_TIMEOUT && !(connecting && _connect_expired(core)))
    {
      rc = AZ_OK;
      break;
    }
    if (az_result_succeeded(rc))
    {
      _log_packet(AZ_SPAN_FROM_STR("received "), (uint8_t)(type << 4), packet_size);
      // While connecting, only a CONNACK is valid.
      rc = connecting && type != AZ_MQTT_PACKET_TYPE_CONNACK ? AZ_MQTT_ERROR_PROTOCOL
                                                             : dispatch(core, type, flags, body);
      if (_S(core).session_generation != generation)
      {
        // A callback ended this session, and may have connected a new one whose
        // receive buffer must not be touched: stop here.
        return az_result_failed(rc) ? rc : AZ_OK;
      }
      _consume_recv(core, packet_size);
      if (az_result_succeeded(rc) && _S(core).state == AZ_MQTT_CLIENT_STATE_CONNECTING)
      {
        rc = AZ_MQTT_ERROR_NOT_CONNECTED; // CONNACK refused; the callback got the reason.
      }
    }
    if (az_result_failed(rc))
    {
      _az_mqtt_core_close(core, rc);
      return rc;
    }
    if (connecting)
    {
      break; // Connected: later packets belong to the next call.
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
