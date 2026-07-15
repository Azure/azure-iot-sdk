// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

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

AZ_NODISCARD az_amqp_session_options az_amqp_session_options_default(void)
{
  az_amqp_session_options o = { 0 };
  o.incoming_window = AZ_AMQP_DEFAULT_INCOMING_WINDOW;
  o.outgoing_window = AZ_AMQP_DEFAULT_OUTGOING_WINDOW;
  o.handle_max = AZ_AMQP_DEFAULT_HANDLE_MAX;
  return o;
}

AZ_NODISCARD az_amqp_session_options az_amqp_session_options_for_constrained_device(void)
{
  az_amqp_session_options o = az_amqp_session_options_default();
  o.incoming_window = 64;
  o.outgoing_window = 64;
  return o;
}

AZ_NODISCARD az_amqp_session_storage az_amqp_session_storage_for_constrained_device(void)
{
  static az_amqp_link* s_links[3]; // room for the CBS pair + one data link
  az_amqp_session_storage s;
  s.links = s_links;
  s.links_capacity = (int32_t)(sizeof(s_links) / sizeof(s_links[0]));
  return s;
}

AZ_NODISCARD az_amqp_session_storage az_amqp_session_storage_for_host(void)
{
  static az_amqp_link* s_links[8];
  az_amqp_session_storage s;
  s.links = s_links;
  s.links_capacity = (int32_t)(sizeof(s_links) / sizeof(s_links[0]));
  return s;
}

static void _set_state(az_amqp_session* s, az_amqp_session_state st, az_amqp_error_detail const* err)
{
  if (s->state == st)
  {
    return;
  }
  az_amqp_session_state const prev = s->state;
  s->state = st;
  if (s->state_changed != NULL)
  {
    s->state_changed(s, prev, st, err, s->state_changed_user_data);
  }
}

AZ_NODISCARD az_result az_amqp_session_init(
    az_amqp_session* session,
    az_amqp_connection* connection,
    az_amqp_session_storage const* storage,
    az_amqp_session_options const* options)
{
  if (session == NULL || connection == NULL || storage == NULL || storage->links == NULL
      || storage->links_capacity <= 0)
  {
    return AZ_ERROR_ARG;
  }
  memset(session, 0, sizeof(*session));
  session->connection = connection;
  session->storage = *storage;
  session->options = (options != NULL) ? *options : az_amqp_session_options_default();
  session->state = AZ_AMQP_SESSION_STATE_UNMAPPED;
  uint16_t channel = 0;
  _AZ_RET(_az_amqp_connection_add_session(connection, session, &channel));
  session->local_channel = channel;
  return AZ_OK;
}

void az_amqp_session_set_state_callback(
    az_amqp_session* session,
    az_amqp_session_state_changed_callback state_changed,
    void* user_data)
{
  session->state_changed = state_changed;
  session->state_changed_user_data = user_data;
}

AZ_NODISCARD az_result az_amqp_session_begin(az_amqp_session* session)
{
  if (session->state != AZ_AMQP_SESSION_STATE_UNMAPPED)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  if (az_amqp_connection_get_state(session->connection) != AZ_AMQP_CONNECTION_STATE_OPENED)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  az_amqp_encoder e;
  _AZ_RET(_az_amqp_connection_frame_begin(session->connection, &e));
  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_BEGIN));
  _AZ_RET(az_amqp_encoder_begin_list(&e));
  _AZ_RET(az_amqp_encoder_append_null(&e)); // 0 remote-channel (we initiate)
  _AZ_RET(az_amqp_encoder_append_uint(&e, session->next_outgoing_id)); // 1 next-outgoing-id
  _AZ_RET(az_amqp_encoder_append_uint(&e, session->options.incoming_window)); // 2 incoming-window
  _AZ_RET(az_amqp_encoder_append_uint(&e, session->options.outgoing_window)); // 3 outgoing-window
  _AZ_RET(az_amqp_encoder_append_uint(&e, session->options.handle_max)); // 4 handle-max
  _AZ_RET(az_amqp_encoder_end_list(&e));
  _AZ_RET(_az_amqp_connection_frame_end(
      session->connection, &e, _AZ_AMQP_FRAME_TYPE_AMQP, session->local_channel));
  _set_state(session, AZ_AMQP_SESSION_STATE_BEGINNING, NULL);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_session_end(az_amqp_session* session, az_amqp_error const* error)
{
  if (session->state == AZ_AMQP_SESSION_STATE_UNMAPPED_ENDED
      || session->state == AZ_AMQP_SESSION_STATE_UNMAPPED)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  az_amqp_encoder e;
  _AZ_RET(_az_amqp_connection_frame_begin(session->connection, &e));
  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_END));
  _AZ_RET(az_amqp_encoder_begin_list(&e));
  if (error != NULL && az_span_size(error->condition) > 0)
  {
    _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_ERROR));
    _AZ_RET(az_amqp_encoder_begin_list(&e));
    _AZ_RET(az_amqp_encoder_append_symbol(&e, error->condition));
    _AZ_RET(az_amqp_encoder_append_null(&e));
    _AZ_RET(az_amqp_encoder_end_list(&e));
  }
  _AZ_RET(az_amqp_encoder_end_list(&e));
  _AZ_RET(_az_amqp_connection_frame_end(
      session->connection, &e, _AZ_AMQP_FRAME_TYPE_AMQP, session->local_channel));
  _set_state(session, AZ_AMQP_SESSION_STATE_ENDING, NULL);
  return AZ_OK;
}

AZ_NODISCARD az_amqp_session_state az_amqp_session_get_state(az_amqp_session const* session)
{
  return session->state;
}

AZ_NODISCARD az_amqp_error_detail az_amqp_session_get_last_error(az_amqp_session const* session)
{
  return session->last_error;
}

AZ_NODISCARD az_result
_az_amqp_session_add_link(az_amqp_session* session, az_amqp_link* link, uint32_t* out_handle)
{
  if (session->link_count >= session->storage.links_capacity)
  {
    return AZ_ERROR_AMQP_NO_STORAGE;
  }
  session->storage.links[session->link_count] = link;
  *out_handle = (uint32_t)session->link_count;
  session->link_count++;
  return AZ_OK;
}

// ---- dispatch ----

static az_amqp_link* _find_link_by_name(az_amqp_session* s, az_span name)
{
  for (int32_t i = 0; i < s->link_count; i++)
  {
    az_amqp_link* l = s->storage.links[i];
    if (l != NULL && az_span_size(l->options.name) == az_span_size(name)
        && (az_span_size(name) == 0
            || memcmp(az_span_ptr(l->options.name), az_span_ptr(name), (size_t)az_span_size(name))
                == 0))
    {
      return l;
    }
  }
  return NULL;
}

static az_amqp_link* _find_link_by_remote_handle(az_amqp_session* s, uint32_t handle)
{
  for (int32_t i = 0; i < s->link_count; i++)
  {
    az_amqp_link* l = s->storage.links[i];
    if (l != NULL && l->state != AZ_AMQP_LINK_STATE_DETACHED && l->remote_handle == handle)
    {
      return l;
    }
  }
  return NULL;
}

void _az_amqp_session_handle_performative(
    az_amqp_session* session,
    uint16_t channel,
    uint64_t descriptor,
    az_amqp_value const* fields,
    az_span payload)
{
  (void)channel;
  az_amqp_value v;

  switch (descriptor)
  {
    case _AZ_AMQP_DESC_BEGIN:
      if (az_result_succeeded(_az_amqp_list_field(fields, 1, &v))
          && v.kind == AZ_AMQP_VALUE_KIND_UINT)
      {
        session->next_incoming_id = (uint32_t)v.scalar.u64;
      }
      if (az_result_succeeded(_az_amqp_list_field(fields, 2, &v))
          && v.kind == AZ_AMQP_VALUE_KIND_UINT)
      {
        session->remote_incoming_window = (uint32_t)v.scalar.u64;
      }
      if (az_result_succeeded(_az_amqp_list_field(fields, 3, &v))
          && v.kind == AZ_AMQP_VALUE_KIND_UINT)
      {
        session->remote_outgoing_window = (uint32_t)v.scalar.u64;
      }
      _set_state(session, AZ_AMQP_SESSION_STATE_MAPPED, NULL);
      break;

    case _AZ_AMQP_DESC_END:
      _set_state(session, AZ_AMQP_SESSION_STATE_UNMAPPED_ENDED, NULL);
      break;

    case _AZ_AMQP_DESC_ATTACH:
    {
      az_span name = AZ_SPAN_EMPTY;
      if (az_result_succeeded(_az_amqp_list_field(fields, 0, &v))
          && v.kind == AZ_AMQP_VALUE_KIND_STRING)
      {
        name = v.payload;
      }
      az_amqp_link* link = _find_link_by_name(session, name);
      if (link != NULL)
      {
        if (az_result_succeeded(_az_amqp_list_field(fields, 1, &v))
            && v.kind == AZ_AMQP_VALUE_KIND_UINT)
        {
          link->remote_handle = (uint32_t)v.scalar.u64;
        }
        _az_amqp_link_handle_performative(link, descriptor, fields, payload);
      }
      break;
    }

    case _AZ_AMQP_DESC_DISPOSITION:
      // No handle; offer to every link so the relevant sender(s) can settle their deliveries.
      for (int32_t i = 0; i < session->link_count; i++)
      {
        az_amqp_link* l = session->storage.links[i];
        if (l != NULL && l->options.role == AZ_AMQP_ROLE_SENDER)
        {
          _az_amqp_link_handle_performative(l, descriptor, fields, payload);
        }
      }
      break;

    case _AZ_AMQP_DESC_TRANSFER:
    case _AZ_AMQP_DESC_DETACH:
    {
      uint32_t handle = 0;
      if (az_result_succeeded(_az_amqp_list_field(fields, 0, &v))
          && v.kind == AZ_AMQP_VALUE_KIND_UINT)
      {
        handle = (uint32_t)v.scalar.u64;
      }
      az_amqp_link* link = _find_link_by_remote_handle(session, handle);
      if (link != NULL)
      {
        _az_amqp_link_handle_performative(link, descriptor, fields, payload);
      }
      break;
    }

    case _AZ_AMQP_DESC_FLOW:
    {
      // Update session window, then route to a link if a handle is present (field 4).
      if (az_result_succeeded(_az_amqp_list_field(fields, 0, &v))
          && v.kind == AZ_AMQP_VALUE_KIND_UINT)
      {
        session->next_incoming_id = (uint32_t)v.scalar.u64;
      }
      az_amqp_value handle_field;
      if (az_result_succeeded(_az_amqp_list_field(fields, 4, &handle_field))
          && handle_field.kind == AZ_AMQP_VALUE_KIND_UINT)
      {
        az_amqp_link* link
            = _find_link_by_remote_handle(session, (uint32_t)handle_field.scalar.u64);
        if (link != NULL)
        {
          _az_amqp_link_handle_performative(link, descriptor, fields, payload);
        }
      }
      break;
    }

    default:
      break;
  }
}
