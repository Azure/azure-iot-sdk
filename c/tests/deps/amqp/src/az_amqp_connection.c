// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include <azure/amqp/az_amqp_connection.h>
#include <azure/amqp/az_amqp_message.h>
#include <azure/amqp/az_amqp_session.h>

#include "_az_amqp_codec.h"
#include "_az_amqp_internal.h"

#include <azure/core/az_platform.h>
#include <azure/core/az_span.h>

#include <string.h>

#define _AZ_RET(expr)            \
  do                             \
  {                              \
    az_result const _r = (expr); \
    if (az_result_failed(_r))    \
    {                            \
      return _r;                 \
    }                            \
  } while (0)

// Internal open/close sub-state-machine phases (stored in connection->phase).
enum
{
  _PHASE_CONNECTING = 0, // transport TCP/TLS connect in progress
  _PHASE_SEND_HEADER, // transport ready; send the first protocol header
  _PHASE_SASL_WAIT_HEADER, // sent SASL header; await peer's SASL header
  _PHASE_SASL_WAIT_MECH, // await sasl-mechanisms
  _PHASE_SASL_WAIT_OUTCOME, // sent sasl-init; await sasl-outcome
  _PHASE_AMQP_WAIT_HEADER, // sent AMQP header; await peer's AMQP header
  _PHASE_AMQP_WAIT_OPEN, // sent open; await peer's open
  _PHASE_OPENED,
  _PHASE_CLOSE_SENT, // sent close; await peer's close
  _PHASE_DONE,
};

// ============================ options + storage ============================

AZ_NODISCARD az_amqp_connection_options az_amqp_connection_options_default(void)
{
  az_amqp_connection_options o = { 0 };
  o.container_id = AZ_SPAN_EMPTY;
  o.hostname = AZ_SPAN_EMPTY;
  o.max_frame_size = AZ_AMQP_DEFAULT_MAX_FRAME_SIZE;
  o.channel_max = (uint16_t)AZ_AMQP_DEFAULT_CHANNEL_MAX;
  o.idle_timeout_milliseconds = 0;
  o.sasl = az_amqp_sasl_options_default();
  return o;
}

AZ_NODISCARD az_amqp_connection_options az_amqp_connection_options_for_constrained_device(void)
{
  az_amqp_connection_options o = az_amqp_connection_options_default();
  o.max_frame_size = AZ_AMQP_CONSTRAINED_MAX_FRAME_SIZE;
  o.idle_timeout_milliseconds = 240000;
  return o;
}

AZ_NODISCARD az_amqp_connection_storage az_amqp_connection_storage_for_constrained_device(void)
{
  static uint8_t s_incoming[AZ_AMQP_CONSTRAINED_MAX_FRAME_SIZE];
  static uint8_t s_outgoing[AZ_AMQP_CONSTRAINED_MAX_FRAME_SIZE];
  static az_amqp_session* s_sessions[1];
  az_amqp_connection_storage s;
  s.incoming_buffer = AZ_SPAN_FROM_BUFFER(s_incoming);
  s.outgoing_buffer = AZ_SPAN_FROM_BUFFER(s_outgoing);
  s.sessions = s_sessions;
  s.sessions_capacity = (int32_t)(sizeof(s_sessions) / sizeof(s_sessions[0]));
  return s;
}

AZ_NODISCARD az_amqp_connection_storage az_amqp_connection_storage_for_host(void)
{
  static uint8_t s_incoming[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
  static uint8_t s_outgoing[AZ_AMQP_DEFAULT_MAX_FRAME_SIZE];
  static az_amqp_session* s_sessions[4];
  az_amqp_connection_storage s;
  s.incoming_buffer = AZ_SPAN_FROM_BUFFER(s_incoming);
  s.outgoing_buffer = AZ_SPAN_FROM_BUFFER(s_outgoing);
  s.sessions = s_sessions;
  s.sessions_capacity = (int32_t)(sizeof(s_sessions) / sizeof(s_sessions[0]));
  return s;
}

// ============================ helpers ============================

static void _set_state(az_amqp_connection* c, az_amqp_connection_state s, az_amqp_error_detail const* err)
{
  if (c->state == s)
  {
    return;
  }
  az_amqp_connection_state const prev = c->state;
  c->state = s;
  if (c->state_changed != NULL)
  {
    c->state_changed(c, prev, s, err, c->state_changed_user_data);
  }
}

static az_result _fail(az_amqp_connection* c, az_result code, az_span message)
{
  c->last_error.code = code;
  c->last_error.message = message;
  // Pull transport backend detail if the adapter exposes it.
  if (c->transport != NULL && c->transport->vtable != NULL
      && c->transport->vtable->get_last_error != NULL)
  {
    int32_t status = 0;
    az_span msg = AZ_SPAN_EMPTY;
    c->transport->vtable->get_last_error(c->transport, &status, &msg);
    c->last_error.transport_status = status;
    if (az_span_size(message) == 0)
    {
      c->last_error.message = msg;
    }
  }
  c->phase = _PHASE_DONE;
  _set_state(c, AZ_AMQP_CONNECTION_STATE_ERROR, &c->last_error);
  return code;
}

static int64_t _now_msec(void)
{
  int64_t ms = 0;
  (void)az_platform_clock_msec(&ms);
  return ms;
}

AZ_NODISCARD az_result
_az_amqp_connection_frame_begin(az_amqp_connection* c, az_amqp_encoder* out_encoder)
{
  int32_t const start = c->outgoing_length + _AZ_AMQP_FRAME_HEADER_SIZE;
  if (start > az_span_size(c->storage.outgoing_buffer))
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  return az_amqp_encoder_init(
      out_encoder, az_span_slice_to_end(c->storage.outgoing_buffer, start));
}

AZ_NODISCARD az_result _az_amqp_connection_frame_end(
    az_amqp_connection* c,
    az_amqp_encoder* encoder,
    uint8_t frame_type,
    uint16_t channel)
{
  int32_t const body_len = az_span_size(az_amqp_encoder_get_bytes(encoder));
  int32_t const total = _AZ_AMQP_FRAME_HEADER_SIZE + body_len;
  uint8_t* p = az_span_ptr(c->storage.outgoing_buffer) + c->outgoing_length;
  _az_amqp_write_u32_be(p, (uint32_t)total);
  p[4] = _AZ_AMQP_FRAME_DOFF_MIN;
  p[5] = frame_type;
  _az_amqp_write_u16_be(p + 6, channel);
  c->outgoing_length += total;
  c->last_outgoing_frame_msec = _now_msec();
  return AZ_OK;
}

// Encodes a `transfer` performative into `e`, leaving the list closed. On the first frame of a
// delivery all fields are set; continuation frames carry only the handle (delivery-id/tag/format/
// settled default). `*out_more_off` receives the offset of the `more` byte so it can be patched
// once the payload split for the frame is known.
static az_result _encode_transfer_perf(
    az_amqp_encoder* e,
    uint32_t handle,
    bool first,
    uint32_t delivery_id,
    az_span delivery_tag,
    bool settled,
    int32_t* out_more_off)
{
  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(e, _AZ_AMQP_DESC_TRANSFER));
  _AZ_RET(az_amqp_encoder_begin_list(e));
  _AZ_RET(az_amqp_encoder_append_uint(e, handle)); // 0 handle
  if (first)
  {
    _AZ_RET(az_amqp_encoder_append_uint(e, delivery_id)); // 1 delivery-id
    _AZ_RET(az_amqp_encoder_append_binary(e, delivery_tag)); // 2 delivery-tag
    _AZ_RET(az_amqp_encoder_append_uint(e, 0)); // 3 message-format
    _AZ_RET(az_amqp_encoder_append_bool(e, settled)); // 4 settled
  }
  else
  {
    _AZ_RET(az_amqp_encoder_append_null(e)); // 1 delivery-id (same delivery)
    _AZ_RET(az_amqp_encoder_append_null(e)); // 2 delivery-tag
    _AZ_RET(az_amqp_encoder_append_null(e)); // 3 message-format
    _AZ_RET(az_amqp_encoder_append_null(e)); // 4 settled
  }
  _AZ_RET(az_amqp_encoder_append_bool(e, false)); // 5 more (placeholder; patched by the caller)
  *out_more_off = e->length - 1;
  return az_amqp_encoder_end_list(e);
}

AZ_NODISCARD az_result _az_amqp_connection_emit_transfer(
    az_amqp_connection* c,
    uint16_t channel,
    uint32_t handle,
    uint32_t delivery_id,
    az_span delivery_tag,
    bool settled,
    az_amqp_message const* message)
{
  int32_t const cap = az_span_size(c->storage.outgoing_buffer);
  int32_t const base = c->outgoing_length;

  // Per-frame body budget from the negotiated max-frame-size (fall back to ours if the peer's is
  // unset or larger than what our buffer can hold).
  uint32_t max_frame = c->options.max_frame_size;
  if (c->remote_max_frame_size >= 512 && c->remote_max_frame_size < max_frame)
  {
    max_frame = c->remote_max_frame_size;
  }
  int32_t const max_body = (int32_t)max_frame - _AZ_AMQP_FRAME_HEADER_SIZE;

  // Fast path: try to fit the performative + whole message in one frame.
  az_amqp_encoder e;
  _AZ_RET(_az_amqp_connection_frame_begin(c, &e));
  int32_t more_off;
  _AZ_RET(_encode_transfer_perf(&e, handle, true, delivery_id, delivery_tag, settled, &more_off));
  int32_t const perf1_len = e.length;
  az_span msg_encoded;
  _AZ_RET(az_amqp_message_encode(message, az_span_slice_to_end(e.destination, e.length), &msg_encoded));
  int32_t const msg_len = az_span_size(msg_encoded);
  e.length += msg_len;

  if (e.length <= max_body)
  {
    // `more` is already false; single frame.
    return _az_amqp_connection_frame_end(c, &e, _AZ_AMQP_FRAME_TYPE_AMQP, channel);
  }

  // Multi-frame: relocate the encoded message to the tail half of the buffer, then rebuild frames
  // from `base`, streaming the message across them.
  int32_t const scratch_off = base + (cap - base) / 2;
  if (scratch_off + msg_len > cap)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE; // message too large for this buffer to fragment
  }
  uint8_t* const buf = az_span_ptr(c->storage.outgoing_buffer);
  memmove(buf + scratch_off, buf + base + _AZ_AMQP_FRAME_HEADER_SIZE + perf1_len, (size_t)msg_len);

  // c->outgoing_length is still `base` (the fast-path frame was never finalized).
  int32_t sent = 0;
  bool first = true;
  while (sent < msg_len)
  {
    int32_t const frame_start = c->outgoing_length;
    az_amqp_encoder fe;
    _AZ_RET(_az_amqp_connection_frame_begin(c, &fe));
    int32_t mo;
    _AZ_RET(_encode_transfer_perf(&fe, handle, first, delivery_id, delivery_tag, settled, &mo));

    int32_t const by_frame = max_body - fe.length; // limit: negotiated max-frame-size
    int32_t const by_region // limit: stay clear of the message scratch at scratch_off
        = scratch_off - (frame_start + _AZ_AMQP_FRAME_HEADER_SIZE + fe.length);
    int32_t budget = (by_frame < by_region) ? by_frame : by_region;
    if (budget <= 0)
    {
      return AZ_ERROR_NOT_ENOUGH_SPACE;
    }
    int32_t chunk = msg_len - sent;
    if (chunk > budget)
    {
      chunk = budget;
    }
    memcpy(az_span_ptr(fe.destination) + fe.length, buf + scratch_off + sent, (size_t)chunk);
    fe.length += chunk;
    sent += chunk;

    bool const more = (sent < msg_len);
    az_span_ptr(fe.destination)[mo] = more ? _AZ_AMQP_FC_TRUE : _AZ_AMQP_FC_FALSE;
    _AZ_RET(_az_amqp_connection_frame_end(c, &fe, _AZ_AMQP_FRAME_TYPE_AMQP, channel));
    first = false;
  }
  return AZ_OK;
}

static az_result _send_protocol_header(az_amqp_connection* c, uint8_t protocol_id)
{
  if (c->outgoing_length + 8 > az_span_size(c->storage.outgoing_buffer))
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  uint8_t* p = az_span_ptr(c->storage.outgoing_buffer) + c->outgoing_length;
  p[0] = 'A';
  p[1] = 'M';
  p[2] = 'Q';
  p[3] = 'P';
  p[4] = protocol_id;
  p[5] = 1;
  p[6] = 0;
  p[7] = 0;
  c->outgoing_length += 8;
  return AZ_OK;
}

static az_result _send_open(az_amqp_connection* c)
{
  az_amqp_encoder e;
  _AZ_RET(_az_amqp_connection_frame_begin(c, &e));
  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_OPEN));
  _AZ_RET(az_amqp_encoder_begin_list(&e));
  _AZ_RET(az_amqp_encoder_append_string(&e, c->options.container_id)); // 0 container-id
  if (az_span_size(c->options.hostname) > 0) // 1 hostname
  {
    _AZ_RET(az_amqp_encoder_append_string(&e, c->options.hostname));
  }
  else
  {
    _AZ_RET(az_amqp_encoder_append_null(&e));
  }
  _AZ_RET(az_amqp_encoder_append_uint(&e, c->options.max_frame_size)); // 2 max-frame-size
  _AZ_RET(az_amqp_encoder_append_ushort(&e, c->options.channel_max)); // 3 channel-max
  if (c->options.idle_timeout_milliseconds > 0) // 4 idle-time-out
  {
    _AZ_RET(az_amqp_encoder_append_uint(&e, c->options.idle_timeout_milliseconds));
  }
  else
  {
    _AZ_RET(az_amqp_encoder_append_null(&e));
  }
  _AZ_RET(az_amqp_encoder_end_list(&e));
  return _az_amqp_connection_frame_end(c, &e, _AZ_AMQP_FRAME_TYPE_AMQP, 0);
}

static az_result _send_sasl_init(az_amqp_connection* c)
{
  az_amqp_sasl_options const* sasl = &c->options.sasl;
  az_span mechanism;
  uint8_t response[512];
  az_span initial_response = AZ_SPAN_EMPTY;

  switch (sasl->mechanism)
  {
    case AZ_AMQP_SASL_MECHANISM_PLAIN:
    {
      mechanism = AZ_SPAN_FROM_STR("PLAIN");
      // authzid \0 authcid \0 passwd
      az_span rest = AZ_SPAN_FROM_BUFFER(response);
      int32_t off = 0;
      az_span authz = sasl->authorization_identity;
      if (az_span_size(authz) > 0)
      {
        memcpy(az_span_ptr(rest) + off, az_span_ptr(authz), (size_t)az_span_size(authz));
        off += az_span_size(authz);
      }
      az_span_ptr(rest)[off++] = 0;
      memcpy(az_span_ptr(rest) + off, az_span_ptr(sasl->username), (size_t)az_span_size(sasl->username));
      off += az_span_size(sasl->username);
      az_span_ptr(rest)[off++] = 0;
      memcpy(az_span_ptr(rest) + off, az_span_ptr(sasl->password), (size_t)az_span_size(sasl->password));
      off += az_span_size(sasl->password);
      initial_response = az_span_slice(rest, 0, off);
      break;
    }
    case AZ_AMQP_SASL_MECHANISM_EXTERNAL:
      mechanism = AZ_SPAN_FROM_STR("EXTERNAL");
      initial_response = sasl->authorization_identity;
      break;
    case AZ_AMQP_SASL_MECHANISM_ANONYMOUS:
    default:
      mechanism = AZ_SPAN_FROM_STR("ANONYMOUS");
      initial_response = AZ_SPAN_FROM_STR("anonymous");
      break;
  }

  az_amqp_encoder e;
  _AZ_RET(_az_amqp_connection_frame_begin(c, &e));
  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_SASL_INIT));
  _AZ_RET(az_amqp_encoder_begin_list(&e));
  _AZ_RET(az_amqp_encoder_append_symbol(&e, mechanism)); // 0 mechanism
  _AZ_RET(az_amqp_encoder_append_binary(&e, initial_response)); // 1 initial-response
  _AZ_RET(az_amqp_encoder_append_null(&e)); // 2 hostname
  _AZ_RET(az_amqp_encoder_end_list(&e));
  return _az_amqp_connection_frame_end(c, &e, _AZ_AMQP_FRAME_TYPE_SASL, 0);
}

static az_result _send_close(az_amqp_connection* c, az_amqp_error const* error)
{
  az_amqp_encoder e;
  _AZ_RET(_az_amqp_connection_frame_begin(c, &e));
  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_CLOSE));
  _AZ_RET(az_amqp_encoder_begin_list(&e));
  if (error != NULL && az_span_size(error->condition) > 0)
  {
    _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_ERROR));
    _AZ_RET(az_amqp_encoder_begin_list(&e));
    _AZ_RET(az_amqp_encoder_append_symbol(&e, error->condition));
    if (az_span_size(error->description) > 0)
    {
      _AZ_RET(az_amqp_encoder_append_string(&e, error->description));
    }
    else
    {
      _AZ_RET(az_amqp_encoder_append_null(&e));
    }
    _AZ_RET(az_amqp_encoder_end_list(&e));
  }
  _AZ_RET(az_amqp_encoder_end_list(&e));
  return _az_amqp_connection_frame_end(c, &e, _AZ_AMQP_FRAME_TYPE_AMQP, 0);
}

// ============================ public ============================

AZ_NODISCARD az_result az_amqp_connection_init(
    az_amqp_connection* connection,
    az_amqp_transport* transport,
    az_amqp_connection_storage const* storage,
    az_amqp_connection_options const* options)
{
  if (connection == NULL || transport == NULL || storage == NULL
      || az_span_ptr(storage->incoming_buffer) == NULL
      || az_span_ptr(storage->outgoing_buffer) == NULL || storage->sessions == NULL
      || storage->sessions_capacity <= 0)
  {
    return AZ_ERROR_ARG;
  }
  memset(connection, 0, sizeof(*connection));
  connection->transport = transport;
  connection->storage = *storage;
  connection->options = (options != NULL) ? *options : az_amqp_connection_options_default();
  if ((int32_t)connection->options.max_frame_size > az_span_size(storage->incoming_buffer)
      || (int32_t)connection->options.max_frame_size > az_span_size(storage->outgoing_buffer))
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  connection->state = AZ_AMQP_CONNECTION_STATE_IDLE;
  connection->phase = _PHASE_CONNECTING;
  return AZ_OK;
}

void az_amqp_connection_set_state_callback(
    az_amqp_connection* connection,
    az_amqp_connection_state_changed_callback state_changed,
    void* user_data)
{
  connection->state_changed = state_changed;
  connection->state_changed_user_data = user_data;
}

AZ_NODISCARD az_result az_amqp_connection_open(az_amqp_connection* connection)
{
  if (connection->state != AZ_AMQP_CONNECTION_STATE_IDLE)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  az_amqp_transport_status const ts = connection->transport->vtable->open(connection->transport);
  if (ts == AZ_AMQP_TRANSPORT_STATUS_ERROR)
  {
    return _fail(connection, AZ_ERROR_AMQP_TRANSPORT, AZ_SPAN_FROM_STR("transport open failed"));
  }
  connection->phase = _PHASE_CONNECTING;
  _set_state(connection, AZ_AMQP_CONNECTION_STATE_OPENING, NULL);
  return AZ_OK;
}

AZ_NODISCARD az_result
az_amqp_connection_close(az_amqp_connection* connection, az_amqp_error const* error)
{
  if (connection->state == AZ_AMQP_CONNECTION_STATE_CLOSED
      || connection->state == AZ_AMQP_CONNECTION_STATE_ERROR)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  az_result const r = _send_close(connection, error);
  if (az_result_failed(r))
  {
    return r;
  }
  connection->phase = _PHASE_CLOSE_SENT;
  _set_state(connection, AZ_AMQP_CONNECTION_STATE_CLOSING, NULL);
  return AZ_OK;
}

AZ_NODISCARD az_amqp_connection_state az_amqp_connection_get_state(az_amqp_connection const* connection)
{
  return connection->state;
}

AZ_NODISCARD az_amqp_error_detail az_amqp_connection_get_last_error(az_amqp_connection const* connection)
{
  return connection->last_error;
}

AZ_NODISCARD az_result _az_amqp_connection_add_session(
    az_amqp_connection* connection,
    az_amqp_session* session,
    uint16_t* out_channel)
{
  if (connection->session_count >= connection->storage.sessions_capacity)
  {
    return AZ_ERROR_AMQP_NO_STORAGE;
  }
  connection->storage.sessions[connection->session_count] = session;
  *out_channel = (uint16_t)connection->session_count;
  connection->session_count++;
  return AZ_OK;
}

// ---- inbound dispatch ----

static az_result
_decode_performative(az_span body, uint64_t* descriptor, az_amqp_value* fields, az_span* payload)
{
  az_amqp_decoder d;
  _AZ_RET(az_amqp_decoder_init(&d, body));
  az_amqp_value perf;
  _AZ_RET(az_amqp_decoder_decode(&d, &perf));
  if (perf.kind != AZ_AMQP_VALUE_KIND_DESCRIBED)
  {
    return AZ_ERROR_AMQP_PROTOCOL;
  }
  az_amqp_value desc;
  az_amqp_decoder body_dec;
  _AZ_RET(az_amqp_value_get_described(&perf, &desc, &body_dec));
  if (desc.kind != AZ_AMQP_VALUE_KIND_ULONG)
  {
    return AZ_ERROR_AMQP_PROTOCOL;
  }
  *descriptor = desc.scalar.u64;
  _AZ_RET(az_amqp_decoder_decode(&body_dec, fields));
  *payload = az_span_slice_to_end(body, d.offset);
  return AZ_OK;
}

static az_amqp_session* _find_session_by_remote_channel(az_amqp_connection* c, uint16_t channel)
{
  for (int32_t i = 0; i < c->session_count; i++)
  {
    az_amqp_session* s = c->storage.sessions[i];
    if (s != NULL && s->remote_channel == channel
        && s->state != AZ_AMQP_SESSION_STATE_UNMAPPED)
    {
      return s;
    }
  }
  return NULL;
}

static az_amqp_session* _find_session_by_local_channel(az_amqp_connection* c, uint16_t channel)
{
  for (int32_t i = 0; i < c->session_count; i++)
  {
    az_amqp_session* s = c->storage.sessions[i];
    if (s != NULL && s->local_channel == channel)
    {
      return s;
    }
  }
  return NULL;
}

static az_result _handle_open(az_amqp_connection* c, az_amqp_value const* fields)
{
  az_amqp_value v;
  if (az_result_succeeded(_az_amqp_list_field(fields, 2, &v)) && v.kind == AZ_AMQP_VALUE_KIND_UINT)
  {
    c->remote_max_frame_size = (uint32_t)v.scalar.u64;
  }
  if (az_result_succeeded(_az_amqp_list_field(fields, 3, &v)) && v.kind == AZ_AMQP_VALUE_KIND_USHORT)
  {
    c->remote_channel_max = (uint16_t)v.scalar.u64;
  }
  if (az_result_succeeded(_az_amqp_list_field(fields, 4, &v)) && v.kind == AZ_AMQP_VALUE_KIND_UINT)
  {
    c->remote_idle_timeout_milliseconds = (uint32_t)v.scalar.u64;
  }
  c->phase = _PHASE_OPENED;
  _set_state(c, AZ_AMQP_CONNECTION_STATE_OPENED, NULL);
  return AZ_OK;
}

static az_result _handle_amqp_frame(az_amqp_connection* c, uint16_t channel, az_span body)
{
  if (az_span_size(body) == 0)
  {
    return AZ_OK; // heartbeat
  }
  uint64_t descriptor = 0;
  az_amqp_value fields;
  az_span payload;
  _AZ_RET(_decode_performative(body, &descriptor, &fields, &payload));

  if (descriptor == _AZ_AMQP_DESC_OPEN)
  {
    return _handle_open(c, &fields);
  }
  if (descriptor == _AZ_AMQP_DESC_CLOSE)
  {
    c->phase = _PHASE_DONE;
    _set_state(c, AZ_AMQP_CONNECTION_STATE_CLOSED, NULL);
    return AZ_OK;
  }

  az_amqp_session* session = NULL;
  if (descriptor == _AZ_AMQP_DESC_BEGIN)
  {
    // begin carries remote-channel (field 0) referencing our local channel when the peer answers.
    az_amqp_value rc;
    if (az_result_succeeded(_az_amqp_list_field(&fields, 0, &rc))
        && rc.kind == AZ_AMQP_VALUE_KIND_USHORT)
    {
      session = _find_session_by_local_channel(c, (uint16_t)rc.scalar.u64);
      if (session != NULL)
      {
        session->remote_channel = channel;
      }
    }
  }
  else
  {
    session = _find_session_by_remote_channel(c, channel);
  }

  if (session != NULL)
  {
    _az_amqp_session_handle_performative(session, channel, descriptor, &fields, payload);
  }
  return AZ_OK;
}

static az_result _handle_sasl_frame(az_amqp_connection* c, az_span body)
{
  if (az_span_size(body) == 0)
  {
    return AZ_OK;
  }
  uint64_t descriptor = 0;
  az_amqp_value fields;
  az_span payload;
  _AZ_RET(_decode_performative(body, &descriptor, &fields, &payload));

  if (descriptor == _AZ_AMQP_DESC_SASL_MECHANISMS && c->phase == _PHASE_SASL_WAIT_MECH)
  {
    _AZ_RET(_send_sasl_init(c));
    c->phase = _PHASE_SASL_WAIT_OUTCOME;
  }
  else if (descriptor == _AZ_AMQP_DESC_SASL_OUTCOME && c->phase == _PHASE_SASL_WAIT_OUTCOME)
  {
    az_amqp_value code;
    uint8_t outcome = 0;
    if (az_result_succeeded(_az_amqp_list_field(&fields, 0, &code))
        && code.kind == AZ_AMQP_VALUE_KIND_UBYTE)
    {
      outcome = (uint8_t)code.scalar.u64;
    }
    if (outcome != 0)
    {
      return _fail(c, AZ_ERROR_AMQP_SASL, AZ_SPAN_FROM_STR("SASL authentication failed"));
    }
    _AZ_RET(_send_protocol_header(c, _AZ_AMQP_PROTOCOL_ID_AMQP));
    c->phase = _PHASE_AMQP_WAIT_HEADER;
  }
  return AZ_OK;
}

// Consume a protocol header (8 bytes) from the front of the incoming buffer.
static bool _consume_protocol_header(az_amqp_connection* c)
{
  if (c->incoming_length < 8)
  {
    return false;
  }
  // (header content is not validated beyond length; the peer echoes our header)
  memmove(
      az_span_ptr(c->storage.incoming_buffer),
      az_span_ptr(c->storage.incoming_buffer) + 8,
      (size_t)(c->incoming_length - 8));
  c->incoming_length -= 8;
  return true;
}

static az_result _parse_frames(az_amqp_connection* c)
{
  uint8_t* buf = az_span_ptr(c->storage.incoming_buffer);
  while (c->incoming_length >= 4)
  {
    uint32_t const size = _az_amqp_read_u32_be(buf);
    if (size < _AZ_AMQP_FRAME_HEADER_SIZE)
    {
      return _fail(c, AZ_ERROR_AMQP_PROTOCOL, AZ_SPAN_FROM_STR("bad frame size"));
    }
    if ((int32_t)size > c->incoming_length)
    {
      break; // need more bytes
    }
    uint8_t const doff = buf[4];
    uint8_t const type = buf[5];
    uint16_t const channel = _az_amqp_read_u16_be(buf + 6);
    int32_t const body_offset = (int32_t)doff * 4;
    if (body_offset < _AZ_AMQP_FRAME_HEADER_SIZE || body_offset > (int32_t)size)
    {
      return _fail(c, AZ_ERROR_AMQP_PROTOCOL, AZ_SPAN_FROM_STR("bad data offset"));
    }
    az_span const body
        = az_span_create(buf + body_offset, (int32_t)size - body_offset);

    az_result r = AZ_OK;
    if (type == _AZ_AMQP_FRAME_TYPE_SASL)
    {
      r = _handle_sasl_frame(c, body);
    }
    else
    {
      r = _handle_amqp_frame(c, channel, body);
    }
    c->last_incoming_frame_msec = _now_msec();

    memmove(buf, buf + size, (size_t)(c->incoming_length - (int32_t)size));
    c->incoming_length -= (int32_t)size;

    if (az_result_failed(r))
    {
      return r;
    }
    if (c->phase == _PHASE_DONE)
    {
      break;
    }
  }
  return AZ_OK;
}

static az_result _flush_outgoing(az_amqp_connection* c)
{
  while (c->outgoing_length > 0)
  {
    size_t written = 0;
    az_span const pending = az_span_slice(c->storage.outgoing_buffer, 0, c->outgoing_length);
    az_amqp_transport_status const ts
        = c->transport->vtable->write(c->transport, pending, &written);
    if (ts == AZ_AMQP_TRANSPORT_STATUS_ERROR || ts == AZ_AMQP_TRANSPORT_STATUS_CLOSED)
    {
      if (c->state == AZ_AMQP_CONNECTION_STATE_CLOSING || c->phase >= _PHASE_CLOSE_SENT)
      {
        c->phase = _PHASE_DONE;
        _set_state(c, AZ_AMQP_CONNECTION_STATE_CLOSED, NULL);
        return AZ_OK; // benign: peer dropped the link during close
      }
      return _fail(c, AZ_ERROR_AMQP_TRANSPORT, AZ_SPAN_FROM_STR("transport write failed"));
    }
    if (written == 0)
    {
      break; // WANT_WRITE/WANT_READ
    }
    memmove(
        az_span_ptr(c->storage.outgoing_buffer),
        az_span_ptr(c->storage.outgoing_buffer) + written,
        (size_t)(c->outgoing_length - (int32_t)written));
    c->outgoing_length -= (int32_t)written;
  }
  return AZ_OK;
}

static az_result _read_incoming(az_amqp_connection* c)
{
  for (;;)
  {
    int32_t const space = az_span_size(c->storage.incoming_buffer) - c->incoming_length;
    if (space <= 0)
    {
      break;
    }
    size_t got = 0;
    az_span const dst
        = az_span_slice(c->storage.incoming_buffer, c->incoming_length, c->incoming_length + space);
    az_amqp_transport_status const ts = c->transport->vtable->read(c->transport, dst, &got);
    if (ts == AZ_AMQP_TRANSPORT_STATUS_ERROR || ts == AZ_AMQP_TRANSPORT_STATUS_CLOSED)
    {
      if (c->state == AZ_AMQP_CONNECTION_STATE_CLOSING || c->phase >= _PHASE_CLOSE_SENT)
      {
        c->phase = _PHASE_DONE;
        _set_state(c, AZ_AMQP_CONNECTION_STATE_CLOSED, NULL);
        return AZ_OK; // benign: peer dropped the link during close
      }
      return _fail(
          c,
          (ts == AZ_AMQP_TRANSPORT_STATUS_CLOSED) ? AZ_ERROR_AMQP_DISCONNECTED
                                                  : AZ_ERROR_AMQP_TRANSPORT,
          (ts == AZ_AMQP_TRANSPORT_STATUS_CLOSED) ? AZ_SPAN_FROM_STR("peer closed the connection")
                                                  : AZ_SPAN_FROM_STR("transport read failed"));
    }
    if (got == 0)
    {
      break; // WANT_READ/WANT_WRITE
    }
    c->incoming_length += (int32_t)got;
  }
  return AZ_OK;
}

// Advance the open handshake based on the current phase.
static az_result _advance_handshake(az_amqp_connection* c)
{
  switch (c->phase)
  {
    case _PHASE_SEND_HEADER:
      if (c->options.sasl.mechanism != AZ_AMQP_SASL_MECHANISM_NONE)
      {
        _AZ_RET(_send_protocol_header(c, _AZ_AMQP_PROTOCOL_ID_SASL));
        c->phase = _PHASE_SASL_WAIT_HEADER;
      }
      else
      {
        _AZ_RET(_send_protocol_header(c, _AZ_AMQP_PROTOCOL_ID_AMQP));
        c->phase = _PHASE_AMQP_WAIT_HEADER;
      }
      break;
    case _PHASE_SASL_WAIT_HEADER:
      if (_consume_protocol_header(c))
      {
        c->phase = _PHASE_SASL_WAIT_MECH;
      }
      break;
    case _PHASE_AMQP_WAIT_HEADER:
      if (_consume_protocol_header(c))
      {
        _AZ_RET(_send_open(c));
        c->phase = _PHASE_AMQP_WAIT_OPEN;
      }
      break;
    default:
      break;
  }
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_connection_process(
    az_amqp_connection* connection,
    az_amqp_connection_process_result* out_result)
{
  if (connection == NULL || out_result == NULL)
  {
    return AZ_ERROR_ARG;
  }
  out_result->io_interest = AZ_AMQP_IO_INTEREST_READ;
  out_result->next_activity_milliseconds = -1;

  if (connection->state == AZ_AMQP_CONNECTION_STATE_ERROR)
  {
    return connection->last_error.code;
  }

  // Drive the transport TCP/TLS connect.
  if (connection->phase == _PHASE_CONNECTING)
  {
    az_amqp_transport_status const ts
        = connection->transport->vtable->process(connection->transport);
    if (ts == AZ_AMQP_TRANSPORT_STATUS_ERROR)
    {
      return _fail(connection, AZ_ERROR_AMQP_TRANSPORT, AZ_SPAN_FROM_STR("transport connect failed"));
    }
    if (ts == AZ_AMQP_TRANSPORT_STATUS_WANT_WRITE)
    {
      out_result->io_interest = AZ_AMQP_IO_INTEREST_WRITE;
      return AZ_OK;
    }
    if (ts != AZ_AMQP_TRANSPORT_STATUS_OK)
    {
      return AZ_OK; // WANT_READ
    }
    connection->phase = _PHASE_SEND_HEADER;
  }

  // Pump a few rounds so multi-step transitions complete promptly.
  for (int round = 0; round < 8; round++)
  {
    _AZ_RET(_advance_handshake(connection));
    _AZ_RET(_flush_outgoing(connection));
    _AZ_RET(_read_incoming(connection));
    if (connection->phase >= _PHASE_SASL_WAIT_MECH || connection->incoming_length >= 8)
    {
      _AZ_RET(_parse_frames(connection));
    }
    if (connection->phase == _PHASE_DONE && connection->state == AZ_AMQP_CONNECTION_STATE_CLOSING)
    {
      _set_state(connection, AZ_AMQP_CONNECTION_STATE_CLOSED, NULL);
    }
  }
  _AZ_RET(_flush_outgoing(connection));

  // Idle timeout handling once opened.
  if (connection->phase == _PHASE_OPENED)
  {
    int64_t const now = _now_msec();

    // Outbound: emit an empty heartbeat frame at ~half the peer's advertised idle timeout so the
    // peer never considers us idle.
    if (connection->remote_idle_timeout_milliseconds > 0)
    {
      int64_t const half = (int64_t)connection->remote_idle_timeout_milliseconds / 2;
      if (now - connection->last_outgoing_frame_msec >= half)
      {
        // Emit an empty heartbeat frame.
        az_amqp_encoder e;
        if (az_result_succeeded(_az_amqp_connection_frame_begin(connection, &e)))
        {
          (void)_az_amqp_connection_frame_end(connection, &e, _AZ_AMQP_FRAME_TYPE_AMQP, 0);
          (void)_flush_outgoing(connection);
        }
      }
      out_result->next_activity_milliseconds = (int32_t)half;
    }

    // Inbound: if we advertised an idle timeout and the peer has sent nothing (not even an empty
    // frame) for more than twice that window, treat the connection as dead. The x2 grace absorbs
    // scheduling jitter and the peer's own half-interval heartbeat cadence.
    if (connection->options.idle_timeout_milliseconds > 0)
    {
      int64_t const deadline = (int64_t)connection->options.idle_timeout_milliseconds * 2;
      if (now - connection->last_incoming_frame_msec > deadline)
      {
        return _fail(
            connection,
            AZ_ERROR_AMQP_TIMEOUT,
            AZ_SPAN_FROM_STR("idle timeout: no frames received from peer"));
      }
      int32_t const until = (int32_t)connection->options.idle_timeout_milliseconds;
      if (out_result->next_activity_milliseconds < 0
          || until < out_result->next_activity_milliseconds)
      {
        out_result->next_activity_milliseconds = until;
      }
    }
  }

  out_result->io_interest
      = (connection->outgoing_length > 0) ? AZ_AMQP_IO_INTEREST_WRITE : AZ_AMQP_IO_INTEREST_READ;
  return AZ_OK;
}
