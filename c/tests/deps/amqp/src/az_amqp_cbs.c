// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include <azure/amqp/az_amqp_cbs.h>
#include <azure/amqp/az_amqp_message.h>

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

AZ_NODISCARD az_amqp_cbs_options az_amqp_cbs_options_default(void)
{
  az_amqp_cbs_options o = { 0 };
  o.reply_buffer = AZ_SPAN_EMPTY;
  return o;
}

static void _set_state(az_amqp_cbs* cbs, az_amqp_cbs_state st)
{
  cbs->state = st;
}

// Both $cbs links share this state callback; CBS becomes OPEN once both are attached.
static void _on_link_state(
    az_amqp_link* link,
    az_amqp_link_state previous_state,
    az_amqp_link_state current_state,
    az_amqp_error_detail const* error,
    void* user_data)
{
  (void)link;
  (void)previous_state;
  az_amqp_cbs* cbs = (az_amqp_cbs*)user_data;
  if (current_state == AZ_AMQP_LINK_STATE_ERROR)
  {
    if (error != NULL)
    {
      cbs->last_error = *error;
    }
    _set_state(cbs, AZ_AMQP_CBS_STATE_ERROR);
    return;
  }
  if (current_state == AZ_AMQP_LINK_STATE_ATTACHED && cbs->state == AZ_AMQP_CBS_STATE_OPENING)
  {
    if (az_amqp_link_get_state(&cbs->request_link) == AZ_AMQP_LINK_STATE_ATTACHED
        && az_amqp_link_get_state(&cbs->reply_link) == AZ_AMQP_LINK_STATE_ATTACHED)
    {
      _set_state(cbs, AZ_AMQP_CBS_STATE_OPEN);
    }
  }
}

// Look up a value by string key in an encoded application-properties map.
static bool _map_find(az_span encoded_map, az_span key, az_amqp_value* out_value)
{
  az_amqp_decoder dec;
  if (az_result_failed(az_amqp_decoder_init(&dec, encoded_map)))
  {
    return false;
  }
  az_amqp_value map;
  if (az_result_failed(az_amqp_decoder_decode(&dec, &map))
      || map.kind != AZ_AMQP_VALUE_KIND_MAP)
  {
    return false;
  }
  az_amqp_decoder pairs;
  uint32_t pc = 0;
  if (az_result_failed(az_amqp_value_get_map_decoder(&map, &pairs, &pc)))
  {
    return false;
  }
  for (uint32_t i = 0; i < pc; i++)
  {
    az_amqp_value k, v;
    if (az_result_failed(az_amqp_decoder_decode(&pairs, &k))
        || az_result_failed(az_amqp_decoder_decode(&pairs, &v)))
    {
      return false;
    }
    if (k.kind == AZ_AMQP_VALUE_KIND_STRING && az_span_size(k.payload) == az_span_size(key)
        && memcmp(az_span_ptr(k.payload), az_span_ptr(key), (size_t)az_span_size(key)) == 0)
    {
      *out_value = v;
      return true;
    }
  }
  return false;
}

static void _on_reply(
    az_amqp_link* link,
    az_amqp_message const* message,
    az_amqp_delivery const* delivery,
    void* user_data)
{
  az_amqp_cbs* cbs = (az_amqp_cbs*)user_data;
  (void)az_amqp_link_accept(link, delivery->number);

  uint32_t status_code = 0;
  az_span status_description = AZ_SPAN_EMPTY;
  az_span app_props;
  if (az_result_succeeded(az_amqp_message_get_application_properties(message, &app_props)))
  {
    az_amqp_value v;
    if (_map_find(app_props, AZ_SPAN_FROM_STR("status-code"), &v))
    {
      if (v.kind == AZ_AMQP_VALUE_KIND_INT || v.kind == AZ_AMQP_VALUE_KIND_UINT)
      {
        status_code = (uint32_t)((v.kind == AZ_AMQP_VALUE_KIND_INT) ? v.scalar.i64 : v.scalar.u64);
      }
    }
    if (_map_find(app_props, AZ_SPAN_FROM_STR("status-description"), &v)
        && v.kind == AZ_AMQP_VALUE_KIND_STRING)
    {
      status_description = v.payload;
    }
  }

  cbs->put_token_in_flight = false;
  if (cbs->put_token_complete != NULL)
  {
    cbs->put_token_complete(cbs, status_code, status_description, cbs->put_token_user_data);
  }
}

AZ_NODISCARD az_result az_amqp_cbs_init(
    az_amqp_cbs* cbs,
    az_amqp_session* session,
    az_amqp_cbs_options const* options)
{
  if (cbs == NULL || session == NULL || options == NULL
      || az_span_ptr(options->reply_buffer) == NULL)
  {
    return AZ_ERROR_ARG;
  }
  memset(cbs, 0, sizeof(*cbs));
  cbs->session = session;
  cbs->options = *options;

  az_amqp_link_options req = az_amqp_link_sender_options_default(
      AZ_SPAN_FROM_STR("cbs-sender"),
      az_amqp_target_from_address(AZ_SPAN_FROM_STR("$cbs")),
      AZ_AMQP_SENDER_SETTLE_MODE_UNSETTLED,
      cbs->request_unsettled,
      1);
  _AZ_RET(az_amqp_link_init(&cbs->request_link, session, &req));
  az_amqp_link_set_state_callback(&cbs->request_link, _on_link_state, cbs);

  az_amqp_link_options rep = az_amqp_link_receiver_options_default(
      AZ_SPAN_FROM_STR("cbs-receiver"),
      az_amqp_source_from_address(AZ_SPAN_FROM_STR("$cbs")),
      AZ_AMQP_RECEIVER_SETTLE_MODE_FIRST,
      cbs->options.reply_buffer,
      1);
  _AZ_RET(az_amqp_link_init(&cbs->reply_link, session, &rep));
  az_amqp_link_set_state_callback(&cbs->reply_link, _on_link_state, cbs);
  az_amqp_link_set_message_callback(&cbs->reply_link, _on_reply, cbs);

  cbs->state = AZ_AMQP_CBS_STATE_CLOSED;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_cbs_open(az_amqp_cbs* cbs)
{
  if (cbs->state != AZ_AMQP_CBS_STATE_CLOSED)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  _AZ_RET(az_amqp_link_attach(&cbs->reply_link));
  _AZ_RET(az_amqp_link_attach(&cbs->request_link));
  _set_state(cbs, AZ_AMQP_CBS_STATE_OPENING);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_cbs_close(az_amqp_cbs* cbs)
{
  if (cbs->state == AZ_AMQP_CBS_STATE_CLOSED)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  (void)az_amqp_link_detach(&cbs->request_link, NULL);
  (void)az_amqp_link_detach(&cbs->reply_link, NULL);
  _set_state(cbs, AZ_AMQP_CBS_STATE_CLOSING);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_cbs_put_token(
    az_amqp_cbs* cbs,
    az_span token_type,
    az_span audience,
    az_span token,
    int64_t expires_at_unix_ms,
    az_amqp_cbs_put_token_complete_callback on_complete,
    void* user_data)
{
  (void)expires_at_unix_ms;
  if (cbs->state != AZ_AMQP_CBS_STATE_OPEN)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  if (cbs->put_token_in_flight)
  {
    return AZ_ERROR_AMQP_NO_CREDIT;
  }

  // Application-properties: operation=put-token, type=<type>, name=<audience>.
  uint8_t app_props_buffer[512];
  az_amqp_property_map ap;
  _AZ_RET(az_amqp_property_map_init(&ap, AZ_SPAN_FROM_BUFFER(app_props_buffer)));
  _AZ_RET(az_amqp_property_map_add_string(
      &ap, AZ_SPAN_FROM_STR("operation"), AZ_SPAN_FROM_STR("put-token")));
  _AZ_RET(az_amqp_property_map_add_string(&ap, AZ_SPAN_FROM_STR("type"), token_type));
  _AZ_RET(az_amqp_property_map_add_string(&ap, AZ_SPAN_FROM_STR("name"), audience));
  az_span const app_props = az_amqp_property_map_get_bytes(&ap);

  // Body: amqp-value string = the token.
  uint8_t body_buffer[2048];
  if (az_span_size(token) + 8 > (int32_t)sizeof(body_buffer))
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  az_amqp_encoder be;
  _AZ_RET(az_amqp_encoder_init(&be, AZ_SPAN_FROM_BUFFER(body_buffer)));
  _AZ_RET(az_amqp_encoder_append_string(&be, token));
  az_span const body = az_amqp_encoder_get_bytes(&be);

  cbs->pending_correlation_id++;
  az_amqp_message message;
  _AZ_RET(az_amqp_message_init(&message));
  az_amqp_message_properties props = { 0 };
  props.message_id = az_amqp_message_id_from_ulong(cbs->pending_correlation_id);
  props.reply_to = AZ_SPAN_FROM_STR("$cbs");
  _AZ_RET(az_amqp_message_set_properties(&message, &props));
  _AZ_RET(az_amqp_message_set_application_properties(&message, app_props));
  _AZ_RET(az_amqp_message_set_body_value(&message, body));

  cbs->put_token_complete = on_complete;
  cbs->put_token_user_data = user_data;
  cbs->put_token_in_flight = true;

  uint8_t tag_bytes[4];
  tag_bytes[0] = (uint8_t)(cbs->pending_correlation_id >> 24);
  tag_bytes[1] = (uint8_t)(cbs->pending_correlation_id >> 16);
  tag_bytes[2] = (uint8_t)(cbs->pending_correlation_id >> 8);
  tag_bytes[3] = (uint8_t)(cbs->pending_correlation_id);
  az_result const r
      = az_amqp_link_send(&cbs->request_link, &message, AZ_SPAN_FROM_BUFFER(tag_bytes), NULL, NULL);
  if (az_result_failed(r))
  {
    cbs->put_token_in_flight = false;
    return r;
  }
  return AZ_OK;
}

AZ_NODISCARD az_amqp_cbs_state az_amqp_cbs_get_state(az_amqp_cbs const* cbs) { return cbs->state; }

AZ_NODISCARD az_amqp_error_detail az_amqp_cbs_get_last_error(az_amqp_cbs const* cbs)
{
  return cbs->last_error;
}
