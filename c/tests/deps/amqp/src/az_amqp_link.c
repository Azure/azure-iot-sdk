// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include <azure/amqp/az_amqp_link.h>
#include <azure/amqp/az_amqp_message.h>
#include <azure/amqp/az_amqp_session.h>

#include "_az_amqp_codec.h"
#include "_az_amqp_internal.h"

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

// ============================ termini + options ============================

AZ_NODISCARD az_amqp_source az_amqp_source_default(void)
{
  az_amqp_source s = { 0 };
  s.durable = AZ_AMQP_TERMINUS_DURABILITY_NONE;
  s.expiry_policy = AZ_AMQP_TERMINUS_EXPIRY_POLICY_SESSION_END;
  s.default_outcome = AZ_AMQP_DELIVERY_OUTCOME_NONE;
  return s;
}

AZ_NODISCARD az_amqp_source az_amqp_source_from_address(az_span address)
{
  az_amqp_source s = az_amqp_source_default();
  s.address = address;
  return s;
}

AZ_NODISCARD az_amqp_target az_amqp_target_default(void)
{
  az_amqp_target t = { 0 };
  t.durable = AZ_AMQP_TERMINUS_DURABILITY_NONE;
  t.expiry_policy = AZ_AMQP_TERMINUS_EXPIRY_POLICY_SESSION_END;
  return t;
}

AZ_NODISCARD az_amqp_target az_amqp_target_from_address(az_span address)
{
  az_amqp_target t = az_amqp_target_default();
  t.address = address;
  return t;
}

AZ_NODISCARD az_amqp_link_options az_amqp_link_options_default(void)
{
  az_amqp_link_options o = { 0 };
  o.role = AZ_AMQP_ROLE_SENDER;
  o.source = az_amqp_source_default();
  o.target = az_amqp_target_default();
  o.sender_settle_mode = AZ_AMQP_SENDER_SETTLE_MODE_UNSETTLED;
  o.receiver_settle_mode = AZ_AMQP_RECEIVER_SETTLE_MODE_FIRST;
  return o;
}

AZ_NODISCARD az_amqp_link_options az_amqp_link_sender_options_default(
    az_span name,
    az_amqp_target target,
    az_amqp_sender_settle_mode sender_settle_mode,
    az_amqp_link_unsettled* unsettled_storage,
    int32_t unsettled_capacity)
{
  az_amqp_link_options o = az_amqp_link_options_default();
  o.role = AZ_AMQP_ROLE_SENDER;
  o.name = name;
  o.target = target;
  o.sender_settle_mode = sender_settle_mode;
  o.unsettled_storage = unsettled_storage;
  o.unsettled_capacity = unsettled_capacity;
  return o;
}

AZ_NODISCARD az_amqp_link_options az_amqp_link_receiver_options_default(
    az_span name,
    az_amqp_source source,
    az_amqp_receiver_settle_mode receiver_settle_mode,
    az_span message_buffer,
    uint32_t prefetch_credit)
{
  az_amqp_link_options o = az_amqp_link_options_default();
  o.role = AZ_AMQP_ROLE_RECEIVER;
  o.name = name;
  o.source = source;
  o.receiver_settle_mode = receiver_settle_mode;
  o.message_buffer = message_buffer;
  o.prefetch_credit = prefetch_credit;
  return o;
}

// ============================ state ============================

static void _set_state(az_amqp_link* l, az_amqp_link_state st, az_amqp_error_detail const* err)
{
  if (l->state == st)
  {
    return;
  }
  az_amqp_link_state const prev = l->state;
  l->state = st;
  if (l->state_changed != NULL)
  {
    l->state_changed(l, prev, st, err, l->state_changed_user_data);
  }
}

AZ_NODISCARD az_result az_amqp_link_init(
    az_amqp_link* link,
    az_amqp_session* session,
    az_amqp_link_options const* options)
{
  if (link == NULL || session == NULL || options == NULL)
  {
    return AZ_ERROR_ARG;
  }
  if (options->role == AZ_AMQP_ROLE_RECEIVER && az_span_ptr(options->message_buffer) == NULL)
  {
    return AZ_ERROR_ARG;
  }
  memset(link, 0, sizeof(*link));
  link->session = session;
  link->options = *options;
  link->state = AZ_AMQP_LINK_STATE_DETACHED;
  // The sender's unsettled slots are library-managed (their `in_use` flags in particular), so clear
  // the caller-provided array here rather than requiring the caller to zero it first.
  if (link->options.unsettled_storage != NULL && link->options.unsettled_capacity > 0)
  {
    memset(
        link->options.unsettled_storage,
        0,
        (size_t)link->options.unsettled_capacity * sizeof(*link->options.unsettled_storage));
  }
  uint32_t handle = 0;
  _AZ_RET(_az_amqp_session_add_link(session, link, &handle));
  link->handle = handle;
  return AZ_OK;
}

void az_amqp_link_set_state_callback(
    az_amqp_link* link,
    az_amqp_link_state_changed_callback state_changed,
    void* user_data)
{
  link->state_changed = state_changed;
  link->state_changed_user_data = user_data;
}

void az_amqp_link_set_credit_callback(
    az_amqp_link* link,
    az_amqp_link_credit_available_callback credit_available,
    void* user_data)
{
  link->credit_available = credit_available;
  link->credit_available_user_data = user_data;
}

void az_amqp_link_set_message_callback(
    az_amqp_link* link,
    az_amqp_link_message_received_callback message_received,
    void* user_data)
{
  link->message_received = message_received;
  link->message_received_user_data = user_data;
}

// ============================ encode helpers ============================

// Emits a pre-encoded value span as one list field, or null when the span is empty.
static az_result _emit_raw_or_null(az_amqp_encoder* e, az_span raw)
{
  if (az_span_size(raw) > 0)
  {
    return _az_amqp_encoder_append_raw(e, raw);
  }
  return az_amqp_encoder_append_null(e);
}

static az_result _encode_source(az_amqp_encoder* e, az_amqp_source const* s)
{
  // §3.5.3 positional fields: 0 address, 1 durable, 2 expiry-policy, 3 timeout, 4 dynamic,
  // 5 dynamic-node-properties, 6 distribution-mode, 7 filter, 8 default-outcome, 9 outcomes,
  // 10 capabilities. Trailing defaulted fields are omitted; intermediate gaps are emitted as null.
  int32_t max_idx = 0;
  if (s->durable != AZ_AMQP_TERMINUS_DURABILITY_NONE)
  {
    max_idx = 1;
  }
  if (s->timeout_seconds > 0 && max_idx < 3)
  {
    max_idx = 3;
  }
  if (s->dynamic && max_idx < 4)
  {
    max_idx = 4;
  }
  if (az_span_size(s->dynamic_node_properties) > 0 && max_idx < 5)
  {
    max_idx = 5;
  }
  if (az_span_size(s->distribution_mode) > 0 && max_idx < 6)
  {
    max_idx = 6;
  }
  if (az_span_size(s->filter) > 0 && max_idx < 7)
  {
    max_idx = 7;
  }
  if (az_span_size(s->capabilities) > 0)
  {
    max_idx = 10;
  }

  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(e, _AZ_AMQP_DESC_SOURCE));
  _AZ_RET(az_amqp_encoder_begin_list(e));
  if (az_span_size(s->address) > 0) // 0 address
  {
    _AZ_RET(az_amqp_encoder_append_string(e, s->address));
  }
  else
  {
    _AZ_RET(az_amqp_encoder_append_null(e));
  }
  if (max_idx >= 1) // 1 durable
  {
    _AZ_RET(az_amqp_encoder_append_uint(e, (uint32_t)s->durable));
  }
  if (max_idx >= 2) // 2 expiry-policy (leave to the peer's default)
  {
    _AZ_RET(az_amqp_encoder_append_null(e));
  }
  if (max_idx >= 3) // 3 timeout (seconds)
  {
    _AZ_RET(az_amqp_encoder_append_uint(e, s->timeout_seconds));
  }
  if (max_idx >= 4) // 4 dynamic
  {
    _AZ_RET(az_amqp_encoder_append_bool(e, s->dynamic));
  }
  if (max_idx >= 5) // 5 dynamic-node-properties (pre-encoded map)
  {
    _AZ_RET(_emit_raw_or_null(e, s->dynamic_node_properties));
  }
  if (max_idx >= 6) // 6 distribution-mode (symbol)
  {
    if (az_span_size(s->distribution_mode) > 0)
    {
      _AZ_RET(az_amqp_encoder_append_symbol(e, s->distribution_mode));
    }
    else
    {
      _AZ_RET(az_amqp_encoder_append_null(e));
    }
  }
  if (max_idx >= 7) // 7 filter (pre-encoded filter-set map, e.g. an Event Hubs offset selector)
  {
    _AZ_RET(_emit_raw_or_null(e, s->filter));
  }
  if (max_idx >= 8) // 8 default-outcome (leave to the peer's default)
  {
    _AZ_RET(az_amqp_encoder_append_null(e));
  }
  if (max_idx >= 9) // 9 outcomes
  {
    _AZ_RET(az_amqp_encoder_append_null(e));
  }
  if (max_idx >= 10) // 10 capabilities (pre-encoded symbol or array of symbol)
  {
    _AZ_RET(_emit_raw_or_null(e, s->capabilities));
  }
  return az_amqp_encoder_end_list(e);
}

static az_result _encode_target(az_amqp_encoder* e, az_amqp_target const* t)
{
  // §3.5.4 positional fields: 0 address, 1 durable, 2 expiry-policy, 3 timeout, 4 dynamic,
  // 5 dynamic-node-properties, 6 capabilities.
  int32_t max_idx = 0;
  if (t->durable != AZ_AMQP_TERMINUS_DURABILITY_NONE)
  {
    max_idx = 1;
  }
  if (t->timeout_seconds > 0 && max_idx < 3)
  {
    max_idx = 3;
  }
  if (t->dynamic && max_idx < 4)
  {
    max_idx = 4;
  }
  if (az_span_size(t->dynamic_node_properties) > 0 && max_idx < 5)
  {
    max_idx = 5;
  }
  if (az_span_size(t->capabilities) > 0)
  {
    max_idx = 6;
  }

  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(e, _AZ_AMQP_DESC_TARGET));
  _AZ_RET(az_amqp_encoder_begin_list(e));
  if (az_span_size(t->address) > 0) // 0 address
  {
    _AZ_RET(az_amqp_encoder_append_string(e, t->address));
  }
  else
  {
    _AZ_RET(az_amqp_encoder_append_null(e));
  }
  if (max_idx >= 1) // 1 durable
  {
    _AZ_RET(az_amqp_encoder_append_uint(e, (uint32_t)t->durable));
  }
  if (max_idx >= 2) // 2 expiry-policy (leave to the peer's default)
  {
    _AZ_RET(az_amqp_encoder_append_null(e));
  }
  if (max_idx >= 3) // 3 timeout (seconds)
  {
    _AZ_RET(az_amqp_encoder_append_uint(e, t->timeout_seconds));
  }
  if (max_idx >= 4) // 4 dynamic
  {
    _AZ_RET(az_amqp_encoder_append_bool(e, t->dynamic));
  }
  if (max_idx >= 5) // 5 dynamic-node-properties (pre-encoded map)
  {
    _AZ_RET(_emit_raw_or_null(e, t->dynamic_node_properties));
  }
  if (max_idx >= 6) // 6 capabilities (pre-encoded symbol or array of symbol)
  {
    _AZ_RET(_emit_raw_or_null(e, t->capabilities));
  }
  return az_amqp_encoder_end_list(e);
}

static az_result _send_flow(az_amqp_link* link, uint32_t credit)
{
  az_amqp_session* s = link->session;
  az_amqp_encoder e;
  _AZ_RET(_az_amqp_connection_frame_begin(s->connection, &e));
  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_FLOW));
  _AZ_RET(az_amqp_encoder_begin_list(&e));
  _AZ_RET(az_amqp_encoder_append_uint(&e, s->next_incoming_id)); // 0 next-incoming-id
  _AZ_RET(az_amqp_encoder_append_uint(&e, s->options.incoming_window)); // 1 incoming-window
  _AZ_RET(az_amqp_encoder_append_uint(&e, s->next_outgoing_id)); // 2 next-outgoing-id
  _AZ_RET(az_amqp_encoder_append_uint(&e, s->options.outgoing_window)); // 3 outgoing-window
  _AZ_RET(az_amqp_encoder_append_uint(&e, link->handle)); // 4 handle
  _AZ_RET(az_amqp_encoder_append_uint(&e, link->delivery_count)); // 5 delivery-count
  _AZ_RET(az_amqp_encoder_append_uint(&e, credit)); // 6 link-credit
  _AZ_RET(az_amqp_encoder_end_list(&e));
  return _az_amqp_connection_frame_end(
      s->connection, &e, _AZ_AMQP_FRAME_TYPE_AMQP, s->local_channel);
}

// ============================ public ops ============================

AZ_NODISCARD az_result az_amqp_link_attach(az_amqp_link* link)
{
  if (link->state != AZ_AMQP_LINK_STATE_DETACHED
      || az_amqp_session_get_state(link->session) != AZ_AMQP_SESSION_STATE_MAPPED)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  az_amqp_session* s = link->session;
  az_amqp_encoder e;
  _AZ_RET(_az_amqp_connection_frame_begin(s->connection, &e));
  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_ATTACH));
  _AZ_RET(az_amqp_encoder_begin_list(&e));
  _AZ_RET(az_amqp_encoder_append_string(&e, link->options.name)); // 0 name
  _AZ_RET(az_amqp_encoder_append_uint(&e, link->handle)); // 1 handle
  _AZ_RET(az_amqp_encoder_append_bool(&e, link->options.role == AZ_AMQP_ROLE_RECEIVER)); // 2 role
  _AZ_RET(az_amqp_encoder_append_ubyte(&e, (uint8_t)link->options.sender_settle_mode)); // 3 snd
  _AZ_RET(az_amqp_encoder_append_ubyte(&e, (uint8_t)link->options.receiver_settle_mode)); // 4 rcv
  _AZ_RET(_encode_source(&e, &link->options.source)); // 5 source
  _AZ_RET(_encode_target(&e, &link->options.target)); // 6 target
  _AZ_RET(az_amqp_encoder_append_null(&e)); // 7 unsettled
  _AZ_RET(az_amqp_encoder_append_null(&e)); // 8 incomplete-unsettled
  _AZ_RET(az_amqp_encoder_append_uint(&e, link->delivery_count)); // 9 initial-delivery-count
  if (link->options.max_message_size > 0) // 10 max-message-size
  {
    _AZ_RET(az_amqp_encoder_append_ulong(&e, link->options.max_message_size));
  }
  else
  {
    _AZ_RET(az_amqp_encoder_append_null(&e));
  }
  _AZ_RET(az_amqp_encoder_end_list(&e));
  _AZ_RET(_az_amqp_connection_frame_end(s->connection, &e, _AZ_AMQP_FRAME_TYPE_AMQP, s->local_channel));
  _set_state(link, AZ_AMQP_LINK_STATE_ATTACHING, NULL);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_link_detach(az_amqp_link* link, az_amqp_error const* error)
{
  if (link->state != AZ_AMQP_LINK_STATE_ATTACHED && link->state != AZ_AMQP_LINK_STATE_ATTACHING)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  az_amqp_session* s = link->session;
  az_amqp_encoder e;
  _AZ_RET(_az_amqp_connection_frame_begin(s->connection, &e));
  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_DETACH));
  _AZ_RET(az_amqp_encoder_begin_list(&e));
  _AZ_RET(az_amqp_encoder_append_uint(&e, link->handle)); // 0 handle
  _AZ_RET(az_amqp_encoder_append_bool(&e, true)); // 1 closed
  if (error != NULL && az_span_size(error->condition) > 0)
  {
    _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_ERROR));
    _AZ_RET(az_amqp_encoder_begin_list(&e));
    _AZ_RET(az_amqp_encoder_append_symbol(&e, error->condition));
    _AZ_RET(az_amqp_encoder_append_null(&e));
    _AZ_RET(az_amqp_encoder_end_list(&e));
  }
  _AZ_RET(az_amqp_encoder_end_list(&e));
  _AZ_RET(_az_amqp_connection_frame_end(s->connection, &e, _AZ_AMQP_FRAME_TYPE_AMQP, s->local_channel));
  _set_state(link, AZ_AMQP_LINK_STATE_DETACHING, NULL);
  return AZ_OK;
}

AZ_NODISCARD az_amqp_link_state az_amqp_link_get_state(az_amqp_link const* link)
{
  return link->state;
}

AZ_NODISCARD az_amqp_error_detail az_amqp_link_get_last_error(az_amqp_link const* link)
{
  return link->last_error;
}

AZ_NODISCARD uint32_t az_amqp_link_get_credit(az_amqp_link const* link) { return link->link_credit; }

AZ_NODISCARD az_result az_amqp_link_send(
    az_amqp_link* link,
    az_amqp_message const* message,
    az_span delivery_tag,
    az_amqp_link_send_complete_callback on_complete,
    void* user_data)
{
  if (link->options.role != AZ_AMQP_ROLE_SENDER || link->state != AZ_AMQP_LINK_STATE_ATTACHED)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  if (link->link_credit == 0)
  {
    return AZ_ERROR_AMQP_NO_CREDIT;
  }

  bool const settled = (link->options.sender_settle_mode == AZ_AMQP_SENDER_SETTLE_MODE_SETTLED);
  az_amqp_link_unsettled* slot = NULL;
  if (!settled)
  {
    for (int32_t i = 0; i < link->options.unsettled_capacity; i++)
    {
      if (!link->options.unsettled_storage[i].in_use)
      {
        slot = &link->options.unsettled_storage[i];
        break;
      }
    }
    if (slot == NULL)
    {
      return AZ_ERROR_AMQP_NO_STORAGE;
    }
  }

  az_amqp_session* s = link->session;
  uint32_t const delivery_id = s->next_outgoing_id;

  _AZ_RET(_az_amqp_connection_emit_transfer(
      s->connection, s->local_channel, link->handle, delivery_id, delivery_tag, settled, message));

  s->next_outgoing_id++;
  link->delivery_count++;
  if (link->link_credit > 0)
  {
    link->link_credit--;
  }

  if (settled)
  {
    if (on_complete != NULL)
    {
      az_amqp_delivery_state ds = { 0 };
      ds.outcome = AZ_AMQP_DELIVERY_OUTCOME_ACCEPTED;
      on_complete(link, delivery_tag, &ds, user_data);
    }
  }
  else
  {
    slot->in_use = true;
    slot->delivery_id = delivery_id;
    slot->delivery_tag = delivery_tag;
    slot->on_complete = (void*)on_complete;
    slot->user_data = user_data;
  }
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_link_add_credit(az_amqp_link* link, uint32_t credit)
{
  if (link->options.role != AZ_AMQP_ROLE_RECEIVER || link->state != AZ_AMQP_LINK_STATE_ATTACHED)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  link->link_credit += credit;
  return _send_flow(link, link->link_credit);
}

// ---- dispositions ----

static az_result _send_disposition(
    az_amqp_link* link,
    uint32_t delivery_number,
    az_amqp_delivery_outcome outcome,
    az_amqp_error const* error,
    bool delivery_failed,
    bool undeliverable_here,
    az_span message_annotations)
{
  az_amqp_session* s = link->session;
  az_amqp_encoder e;
  _AZ_RET(_az_amqp_connection_frame_begin(s->connection, &e));
  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_DISPOSITION));
  _AZ_RET(az_amqp_encoder_begin_list(&e));
  _AZ_RET(az_amqp_encoder_append_bool(&e, true)); // 0 role = receiver
  _AZ_RET(az_amqp_encoder_append_uint(&e, delivery_number)); // 1 first
  _AZ_RET(az_amqp_encoder_append_uint(&e, delivery_number)); // 2 last
  _AZ_RET(az_amqp_encoder_append_bool(&e, true)); // 3 settled
  // 4 state
  switch (outcome)
  {
    case AZ_AMQP_DELIVERY_OUTCOME_REJECTED:
      _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_REJECTED));
      _AZ_RET(az_amqp_encoder_begin_list(&e));
      if (error != NULL && az_span_size(error->condition) > 0)
      {
        _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_ERROR));
        _AZ_RET(az_amqp_encoder_begin_list(&e));
        _AZ_RET(az_amqp_encoder_append_symbol(&e, error->condition));
        _AZ_RET(az_amqp_encoder_append_null(&e));
        _AZ_RET(az_amqp_encoder_end_list(&e));
      }
      else
      {
        _AZ_RET(az_amqp_encoder_append_null(&e));
      }
      _AZ_RET(az_amqp_encoder_end_list(&e));
      break;
    case AZ_AMQP_DELIVERY_OUTCOME_RELEASED:
      _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_RELEASED));
      _AZ_RET(az_amqp_encoder_begin_list(&e));
      _AZ_RET(az_amqp_encoder_end_list(&e));
      break;
    case AZ_AMQP_DELIVERY_OUTCOME_MODIFIED:
      _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_MODIFIED));
      _AZ_RET(az_amqp_encoder_begin_list(&e));
      _AZ_RET(az_amqp_encoder_append_bool(&e, delivery_failed));
      _AZ_RET(az_amqp_encoder_append_bool(&e, undeliverable_here));
      if (az_span_size(message_annotations) > 0)
      {
        _AZ_RET(_az_amqp_encoder_append_raw(&e, message_annotations));
      }
      else
      {
        _AZ_RET(az_amqp_encoder_append_null(&e));
      }
      _AZ_RET(az_amqp_encoder_end_list(&e));
      break;
    case AZ_AMQP_DELIVERY_OUTCOME_ACCEPTED:
    default:
      _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_ACCEPTED));
      _AZ_RET(az_amqp_encoder_begin_list(&e));
      _AZ_RET(az_amqp_encoder_end_list(&e));
      break;
  }
  _AZ_RET(az_amqp_encoder_end_list(&e));
  _AZ_RET(_az_amqp_connection_frame_end(s->connection, &e, _AZ_AMQP_FRAME_TYPE_AMQP, s->local_channel));

  // Auto-replenish prefetch credit.
  if (link->options.prefetch_credit > 0)
  {
    if (link->link_credit > 0)
    {
      link->link_credit--;
    }
    if (link->link_credit * 2 <= link->options.prefetch_credit)
    {
      (void)_send_flow(link, link->options.prefetch_credit);
      link->link_credit = link->options.prefetch_credit;
    }
  }
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_link_accept(az_amqp_link* link, uint32_t delivery_number)
{
  return _send_disposition(
      link, delivery_number, AZ_AMQP_DELIVERY_OUTCOME_ACCEPTED, NULL, false, false, AZ_SPAN_EMPTY);
}

AZ_NODISCARD az_result
az_amqp_link_reject(az_amqp_link* link, uint32_t delivery_number, az_amqp_error const* error)
{
  return _send_disposition(
      link, delivery_number, AZ_AMQP_DELIVERY_OUTCOME_REJECTED, error, false, false, AZ_SPAN_EMPTY);
}

AZ_NODISCARD az_result az_amqp_link_release(az_amqp_link* link, uint32_t delivery_number)
{
  return _send_disposition(
      link, delivery_number, AZ_AMQP_DELIVERY_OUTCOME_RELEASED, NULL, false, false, AZ_SPAN_EMPTY);
}

AZ_NODISCARD az_result az_amqp_link_modify(
    az_amqp_link* link,
    uint32_t delivery_number,
    bool delivery_failed,
    bool undeliverable_here,
    az_span message_annotations)
{
  return _send_disposition(
      link,
      delivery_number,
      AZ_AMQP_DELIVERY_OUTCOME_MODIFIED,
      NULL,
      delivery_failed,
      undeliverable_here,
      message_annotations);
}

// ============================ inbound ============================

static void _parse_delivery_state(az_amqp_value const* state, az_amqp_delivery_state* out)
{
  memset(out, 0, sizeof(*out));
  out->outcome = AZ_AMQP_DELIVERY_OUTCOME_NONE;
  if (state->kind != AZ_AMQP_VALUE_KIND_DESCRIBED)
  {
    return;
  }
  az_amqp_value descriptor;
  az_amqp_decoder body;
  if (az_result_failed(az_amqp_value_get_described(state, &descriptor, &body))
      || descriptor.kind != AZ_AMQP_VALUE_KIND_ULONG)
  {
    return;
  }
  switch (descriptor.scalar.u64)
  {
    case _AZ_AMQP_DESC_ACCEPTED:
      out->outcome = AZ_AMQP_DELIVERY_OUTCOME_ACCEPTED;
      break;
    case _AZ_AMQP_DESC_RELEASED:
      out->outcome = AZ_AMQP_DELIVERY_OUTCOME_RELEASED;
      break;
    case _AZ_AMQP_DESC_REJECTED:
    {
      out->outcome = AZ_AMQP_DELIVERY_OUTCOME_REJECTED;
      az_amqp_value list;
      if (az_result_succeeded(az_amqp_decoder_decode(&body, &list)))
      {
        az_amqp_value err;
        if (az_result_succeeded(_az_amqp_list_field(&list, 0, &err))
            && err.kind == AZ_AMQP_VALUE_KIND_DESCRIBED)
        {
          az_amqp_value edesc;
          az_amqp_decoder ebody;
          if (az_result_succeeded(az_amqp_value_get_described(&err, &edesc, &ebody)))
          {
            az_amqp_value efields;
            if (az_result_succeeded(az_amqp_decoder_decode(&ebody, &efields)))
            {
              az_amqp_value cond;
              if (az_result_succeeded(_az_amqp_list_field(&efields, 0, &cond))
                  && cond.kind == AZ_AMQP_VALUE_KIND_SYMBOL)
              {
                out->error.condition = cond.payload;
              }
              az_amqp_value desc;
              if (az_result_succeeded(_az_amqp_list_field(&efields, 1, &desc))
                  && desc.kind == AZ_AMQP_VALUE_KIND_STRING)
              {
                out->error.description = desc.payload;
              }
            }
          }
        }
      }
      break;
    }
    case _AZ_AMQP_DESC_MODIFIED:
    {
      out->outcome = AZ_AMQP_DELIVERY_OUTCOME_MODIFIED;
      az_amqp_value list;
      if (az_result_succeeded(az_amqp_decoder_decode(&body, &list)))
      {
        az_amqp_value f;
        if (az_result_succeeded(_az_amqp_list_field(&list, 0, &f))
            && f.kind == AZ_AMQP_VALUE_KIND_BOOL)
        {
          out->delivery_failed = f.scalar.boolean;
        }
        if (az_result_succeeded(_az_amqp_list_field(&list, 1, &f))
            && f.kind == AZ_AMQP_VALUE_KIND_BOOL)
        {
          out->undeliverable_here = f.scalar.boolean;
        }
        if (az_result_succeeded(_az_amqp_list_field(&list, 2, &f))
            && f.kind == AZ_AMQP_VALUE_KIND_MAP)
        {
          out->message_annotations = f.encoded;
        }
      }
      break;
    }
    default:
      break;
  }
}

static void _on_attach(az_amqp_link* link, az_amqp_value const* fields)
{
  _set_state(link, AZ_AMQP_LINK_STATE_ATTACHED, NULL);
  if (link->options.role == AZ_AMQP_ROLE_RECEIVER)
  {
    // A receiver's delivery-count tracks the sender's, starting at the sender's
    // initial-delivery-count (AMQP 1.0 2.6.7).
    az_amqp_value v;
    if (az_result_succeeded(_az_amqp_list_field(fields, 9, &v))
        && v.kind == AZ_AMQP_VALUE_KIND_UINT)
    {
      link->delivery_count = (uint32_t)v.scalar.u64;
    }
  }
  if (link->options.role == AZ_AMQP_ROLE_RECEIVER && link->options.prefetch_credit > 0)
  {
    link->link_credit = link->options.prefetch_credit;
    (void)_send_flow(link, link->options.prefetch_credit);
  }
}

static void _on_flow(az_amqp_link* link, az_amqp_value const* fields)
{
  if (link->options.role != AZ_AMQP_ROLE_SENDER)
  {
    return;
  }
  az_amqp_value dc, lc;
  uint32_t link_credit = 0;
  bool have_lc = az_result_succeeded(_az_amqp_list_field(fields, 6, &lc))
      && lc.kind == AZ_AMQP_VALUE_KIND_UINT;
  bool have_dc = az_result_succeeded(_az_amqp_list_field(fields, 5, &dc))
      && dc.kind == AZ_AMQP_VALUE_KIND_UINT;
  if (have_lc && have_dc)
  {
    uint32_t const peer_limit = (uint32_t)dc.scalar.u64 + (uint32_t)lc.scalar.u64;
    link_credit = (peer_limit > link->delivery_count) ? peer_limit - link->delivery_count : 0;
  }
  else if (have_lc)
  {
    link_credit = (uint32_t)lc.scalar.u64;
  }
  link->link_credit = link_credit;
  if (link->credit_available != NULL && link_credit > 0)
  {
    link->credit_available(link, link_credit, link->credit_available_user_data);
  }
}

static void _on_transfer(az_amqp_link* link, az_amqp_value const* fields, az_span payload)
{
  if (link->options.role != AZ_AMQP_ROLE_RECEIVER)
  {
    return;
  }
  az_amqp_value v;
  if (az_result_succeeded(_az_amqp_list_field(fields, 1, &v)) && v.kind == AZ_AMQP_VALUE_KIND_UINT)
  {
    link->current_delivery_id = (uint32_t)v.scalar.u64;
  }
  az_span tag = AZ_SPAN_EMPTY;
  if (az_result_succeeded(_az_amqp_list_field(fields, 2, &v)) && v.kind == AZ_AMQP_VALUE_KIND_BINARY)
  {
    tag = v.payload;
  }
  bool settled = false;
  if (az_result_succeeded(_az_amqp_list_field(fields, 4, &v)) && v.kind == AZ_AMQP_VALUE_KIND_BOOL)
  {
    settled = v.scalar.boolean;
  }
  bool more = false;
  if (az_result_succeeded(_az_amqp_list_field(fields, 5, &v)) && v.kind == AZ_AMQP_VALUE_KIND_BOOL)
  {
    more = v.scalar.boolean;
  }

  // Accumulate the (possibly multi-frame) message payload into the caller buffer.
  int32_t const n = az_span_size(payload);
  if (link->partial_length + n <= az_span_size(link->options.message_buffer))
  {
    memcpy(
        az_span_ptr(link->options.message_buffer) + link->partial_length, az_span_ptr(payload),
        (size_t)n);
    link->partial_length += n;
  }

  link->session->next_incoming_id = link->current_delivery_id + 1;

  if (!more)
  {
    // Advance before the callback: a disposition sent from it may replenish
    // credit, and the sender grants credit relative to this count. Left at its
    // initial value, every replenishing flow grants nothing once the first
    // prefetch_credit deliveries have arrived.
    link->delivery_count++;
    az_span const full
        = az_span_slice(link->options.message_buffer, 0, link->partial_length);
    az_amqp_message message;
    if (az_result_succeeded(az_amqp_message_decode(full, &message))
        && link->message_received != NULL)
    {
      az_amqp_delivery delivery;
      delivery.number = link->current_delivery_id;
      delivery.tag = tag;
      delivery.settled = settled;
      link->message_received(link, &message, &delivery, link->message_received_user_data);
    }
    link->partial_length = 0;
  }
}

static void _on_disposition(az_amqp_link* link, az_amqp_value const* fields)
{
  if (link->options.role != AZ_AMQP_ROLE_SENDER || link->options.unsettled_storage == NULL)
  {
    return;
  }
  az_amqp_value v;
  uint32_t first = 0, last = 0;
  if (az_result_succeeded(_az_amqp_list_field(fields, 1, &v)) && v.kind == AZ_AMQP_VALUE_KIND_UINT)
  {
    first = (uint32_t)v.scalar.u64;
  }
  last = first;
  if (az_result_succeeded(_az_amqp_list_field(fields, 2, &v)) && v.kind == AZ_AMQP_VALUE_KIND_UINT)
  {
    last = (uint32_t)v.scalar.u64;
  }
  az_amqp_delivery_state ds;
  ds.outcome = AZ_AMQP_DELIVERY_OUTCOME_ACCEPTED;
  ds.error.condition = AZ_SPAN_EMPTY;
  ds.error.description = AZ_SPAN_EMPTY;
  ds.delivery_failed = false;
  ds.undeliverable_here = false;
  ds.message_annotations = AZ_SPAN_EMPTY;
  if (az_result_succeeded(_az_amqp_list_field(fields, 4, &v)))
  {
    _parse_delivery_state(&v, &ds);
  }

  for (int32_t i = 0; i < link->options.unsettled_capacity; i++)
  {
    az_amqp_link_unsettled* slot = &link->options.unsettled_storage[i];
    if (slot->in_use && slot->delivery_id >= first && slot->delivery_id <= last)
    {
      az_amqp_link_send_complete_callback cb = (az_amqp_link_send_complete_callback)slot->on_complete;
      az_span tag = slot->delivery_tag;
      void* ud = slot->user_data;
      slot->in_use = false;
      if (cb != NULL)
      {
        cb(link, tag, &ds, ud);
      }
    }
  }
}

void _az_amqp_link_handle_performative(
    az_amqp_link* link,
    uint64_t descriptor,
    az_amqp_value const* fields,
    az_span payload)
{
  switch (descriptor)
  {
    case _AZ_AMQP_DESC_ATTACH:
      _on_attach(link, fields);
      break;
    case _AZ_AMQP_DESC_FLOW:
      _on_flow(link, fields);
      break;
    case _AZ_AMQP_DESC_TRANSFER:
      _on_transfer(link, fields, payload);
      break;
    case _AZ_AMQP_DESC_DISPOSITION:
      _on_disposition(link, fields);
      break;
    case _AZ_AMQP_DESC_DETACH:
      _set_state(link, AZ_AMQP_LINK_STATE_DETACHED, NULL);
      break;
    default:
      break;
  }
}
