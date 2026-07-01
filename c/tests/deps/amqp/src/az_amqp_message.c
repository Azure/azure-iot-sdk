// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include <azure/amqp/az_amqp_message.h>

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

// ============================ message-id ============================

AZ_NODISCARD az_amqp_message_id az_amqp_message_id_from_ulong(uint64_t value)
{
  az_amqp_message_id id = { 0 };
  id.kind = AZ_AMQP_MESSAGE_ID_KIND_ULONG;
  id.u64 = value;
  return id;
}

AZ_NODISCARD az_amqp_message_id az_amqp_message_id_from_uuid(uint8_t const uuid[16])
{
  az_amqp_message_id id = { 0 };
  id.kind = AZ_AMQP_MESSAGE_ID_KIND_UUID;
  id.bytes = az_span_create((uint8_t*)(uintptr_t)uuid, 16);
  return id;
}

AZ_NODISCARD az_amqp_message_id az_amqp_message_id_from_binary(az_span value)
{
  az_amqp_message_id id = { 0 };
  id.kind = AZ_AMQP_MESSAGE_ID_KIND_BINARY;
  id.bytes = value;
  return id;
}

AZ_NODISCARD az_amqp_message_id az_amqp_message_id_from_string(az_span value)
{
  az_amqp_message_id id = { 0 };
  id.kind = AZ_AMQP_MESSAGE_ID_KIND_STRING;
  id.bytes = value;
  return id;
}

// ============================ message build ============================

AZ_NODISCARD az_result az_amqp_message_init(az_amqp_message* message)
{
  if (message == NULL)
  {
    return AZ_ERROR_ARG;
  }
  memset(message, 0, sizeof(*message));
  message->body_kind = AZ_AMQP_MESSAGE_BODY_KIND_NONE;
  return AZ_OK;
}

AZ_NODISCARD az_result
az_amqp_message_set_header(az_amqp_message* message, az_amqp_message_header const* header)
{
  if (message == NULL || header == NULL)
  {
    return AZ_ERROR_ARG;
  }
  message->header = *header;
  message->has_header = true;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_message_set_properties(
    az_amqp_message* message,
    az_amqp_message_properties const* properties)
{
  if (message == NULL || properties == NULL)
  {
    return AZ_ERROR_ARG;
  }
  message->properties = *properties;
  message->has_properties = true;
  return AZ_OK;
}

AZ_NODISCARD az_result
az_amqp_message_set_delivery_annotations(az_amqp_message* message, az_span encoded_map)
{
  if (message == NULL)
  {
    return AZ_ERROR_ARG;
  }
  message->delivery_annotations = encoded_map;
  return AZ_OK;
}

AZ_NODISCARD az_result
az_amqp_message_set_message_annotations(az_amqp_message* message, az_span encoded_map)
{
  if (message == NULL)
  {
    return AZ_ERROR_ARG;
  }
  message->message_annotations = encoded_map;
  return AZ_OK;
}

AZ_NODISCARD az_result
az_amqp_message_set_application_properties(az_amqp_message* message, az_span encoded_map)
{
  if (message == NULL)
  {
    return AZ_ERROR_ARG;
  }
  message->application_properties = encoded_map;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_message_set_footer(az_amqp_message* message, az_span encoded_map)
{
  if (message == NULL)
  {
    return AZ_ERROR_ARG;
  }
  message->footer = encoded_map;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_message_set_body_data(az_amqp_message* message, az_span data)
{
  if (message == NULL)
  {
    return AZ_ERROR_ARG;
  }
  message->body_kind = AZ_AMQP_MESSAGE_BODY_KIND_DATA;
  message->body = data;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_message_set_body_value(az_amqp_message* message, az_span encoded_value)
{
  if (message == NULL)
  {
    return AZ_ERROR_ARG;
  }
  message->body_kind = AZ_AMQP_MESSAGE_BODY_KIND_VALUE;
  message->body = encoded_value;
  return AZ_OK;
}

AZ_NODISCARD az_result
az_amqp_message_set_body_sequence(az_amqp_message* message, az_span encoded_list)
{
  if (message == NULL)
  {
    return AZ_ERROR_ARG;
  }
  message->body_kind = AZ_AMQP_MESSAGE_BODY_KIND_SEQUENCE;
  message->body = encoded_list;
  return AZ_OK;
}

// ============================ encode ============================

static az_result _encode_message_id(az_amqp_encoder* e, az_amqp_message_id const* id)
{
  switch (id->kind)
  {
    case AZ_AMQP_MESSAGE_ID_KIND_ULONG:
      return az_amqp_encoder_append_ulong(e, id->u64);
    case AZ_AMQP_MESSAGE_ID_KIND_UUID:
      return az_amqp_encoder_append_uuid(e, az_span_ptr(id->bytes));
    case AZ_AMQP_MESSAGE_ID_KIND_BINARY:
      return az_amqp_encoder_append_binary(e, id->bytes);
    case AZ_AMQP_MESSAGE_ID_KIND_STRING:
      return az_amqp_encoder_append_string(e, id->bytes);
    default:
      return az_amqp_encoder_append_null(e);
  }
}

static az_result _encode_str_or_null(az_amqp_encoder* e, az_span s)
{
  return az_span_size(s) > 0 ? az_amqp_encoder_append_string(e, s) : az_amqp_encoder_append_null(e);
}

static az_result _encode_sym_or_null(az_amqp_encoder* e, az_span s)
{
  return az_span_size(s) > 0 ? az_amqp_encoder_append_symbol(e, s) : az_amqp_encoder_append_null(e);
}

static az_result _encode_header(az_amqp_encoder* e, az_amqp_message_header const* h)
{
  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(e, _AZ_AMQP_DESC_HEADER));
  _AZ_RET(az_amqp_encoder_begin_list(e));
  _AZ_RET(az_amqp_encoder_append_bool(e, h->durable));
  if (h->has_priority)
  {
    _AZ_RET(az_amqp_encoder_append_ubyte(e, h->priority));
  }
  else
  {
    _AZ_RET(az_amqp_encoder_append_null(e));
  }
  if (h->has_time_to_live)
  {
    _AZ_RET(az_amqp_encoder_append_uint(e, h->time_to_live_milliseconds));
  }
  else
  {
    _AZ_RET(az_amqp_encoder_append_null(e));
  }
  _AZ_RET(az_amqp_encoder_append_bool(e, h->first_acquirer));
  _AZ_RET(az_amqp_encoder_append_uint(e, h->delivery_count));
  return az_amqp_encoder_end_list(e);
}

static az_result _encode_properties(az_amqp_encoder* e, az_amqp_message_properties const* p)
{
  _AZ_RET(az_amqp_encoder_append_descriptor_ulong(e, _AZ_AMQP_DESC_PROPERTIES));
  _AZ_RET(az_amqp_encoder_begin_list(e));
  _AZ_RET(_encode_message_id(e, &p->message_id)); // 0 message-id
  if (az_span_size(p->user_id) > 0) // 1 user-id (binary)
  {
    _AZ_RET(az_amqp_encoder_append_binary(e, p->user_id));
  }
  else
  {
    _AZ_RET(az_amqp_encoder_append_null(e));
  }
  _AZ_RET(_encode_str_or_null(e, p->to)); // 2 to
  _AZ_RET(_encode_str_or_null(e, p->subject)); // 3 subject
  _AZ_RET(_encode_str_or_null(e, p->reply_to)); // 4 reply-to
  _AZ_RET(_encode_message_id(e, &p->correlation_id)); // 5 correlation-id
  _AZ_RET(_encode_sym_or_null(e, p->content_type)); // 6 content-type
  _AZ_RET(_encode_sym_or_null(e, p->content_encoding)); // 7 content-encoding
  if (p->has_absolute_expiry_time) // 8 absolute-expiry-time
  {
    _AZ_RET(az_amqp_encoder_append_timestamp(e, p->absolute_expiry_time_ms));
  }
  else
  {
    _AZ_RET(az_amqp_encoder_append_null(e));
  }
  if (p->has_creation_time) // 9 creation-time
  {
    _AZ_RET(az_amqp_encoder_append_timestamp(e, p->creation_time_ms));
  }
  else
  {
    _AZ_RET(az_amqp_encoder_append_null(e));
  }
  _AZ_RET(_encode_str_or_null(e, p->group_id)); // 10 group-id
  if (p->has_group_sequence) // 11 group-sequence
  {
    _AZ_RET(az_amqp_encoder_append_uint(e, p->group_sequence));
  }
  else
  {
    _AZ_RET(az_amqp_encoder_append_null(e));
  }
  _AZ_RET(_encode_str_or_null(e, p->reply_to_group_id)); // 12 reply-to-group-id
  return az_amqp_encoder_end_list(e);
}

AZ_NODISCARD az_result az_amqp_message_encode(
    az_amqp_message const* message,
    az_span destination,
    az_span* out_encoded)
{
  if (message == NULL || out_encoded == NULL)
  {
    return AZ_ERROR_ARG;
  }

  az_amqp_encoder e;
  _AZ_RET(az_amqp_encoder_init(&e, destination));

  if (message->has_header)
  {
    _AZ_RET(_encode_header(&e, &message->header));
  }
  if (az_span_size(message->delivery_annotations) > 0)
  {
    _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_DELIVERY_ANNOTATIONS));
    _AZ_RET(_az_amqp_encoder_append_raw(&e, message->delivery_annotations));
  }
  if (az_span_size(message->message_annotations) > 0)
  {
    _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_MESSAGE_ANNOTATIONS));
    _AZ_RET(_az_amqp_encoder_append_raw(&e, message->message_annotations));
  }
  if (message->has_properties)
  {
    _AZ_RET(_encode_properties(&e, &message->properties));
  }
  if (az_span_size(message->application_properties) > 0)
  {
    _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_APPLICATION_PROPERTIES));
    _AZ_RET(_az_amqp_encoder_append_raw(&e, message->application_properties));
  }
  switch (message->body_kind)
  {
    case AZ_AMQP_MESSAGE_BODY_KIND_DATA:
      _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_DATA));
      _AZ_RET(az_amqp_encoder_append_binary(&e, message->body));
      break;
    case AZ_AMQP_MESSAGE_BODY_KIND_VALUE:
      _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_AMQP_VALUE));
      _AZ_RET(_az_amqp_encoder_append_raw(&e, message->body));
      break;
    case AZ_AMQP_MESSAGE_BODY_KIND_SEQUENCE:
      _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_AMQP_SEQUENCE));
      _AZ_RET(_az_amqp_encoder_append_raw(&e, message->body));
      break;
    default:
      break;
  }
  if (az_span_size(message->footer) > 0)
  {
    _AZ_RET(az_amqp_encoder_append_descriptor_ulong(&e, _AZ_AMQP_DESC_FOOTER));
    _AZ_RET(_az_amqp_encoder_append_raw(&e, message->footer));
  }

  *out_encoded = az_amqp_encoder_get_bytes(&e);
  return AZ_OK;
}

// ============================ decode ============================

// Reads the next list element as an optional value. Returns true if present (non-null).
static bool _next_field(az_amqp_decoder* list, uint32_t* remaining, az_amqp_value* out)
{
  if (*remaining == 0 || !az_amqp_decoder_has_next(list))
  {
    return false;
  }
  (*remaining)--;
  if (az_result_failed(az_amqp_decoder_decode(list, out)))
  {
    return false;
  }
  return out->kind != AZ_AMQP_VALUE_KIND_NULL;
}

static void _decode_message_id(az_amqp_value const* v, az_amqp_message_id* out)
{
  switch (v->kind)
  {
    case AZ_AMQP_VALUE_KIND_ULONG:
      out->kind = AZ_AMQP_MESSAGE_ID_KIND_ULONG;
      out->u64 = v->scalar.u64;
      break;
    case AZ_AMQP_VALUE_KIND_UUID:
      out->kind = AZ_AMQP_MESSAGE_ID_KIND_UUID;
      out->bytes = v->payload;
      break;
    case AZ_AMQP_VALUE_KIND_BINARY:
      out->kind = AZ_AMQP_MESSAGE_ID_KIND_BINARY;
      out->bytes = v->payload;
      break;
    case AZ_AMQP_VALUE_KIND_STRING:
      out->kind = AZ_AMQP_MESSAGE_ID_KIND_STRING;
      out->bytes = v->payload;
      break;
    default:
      out->kind = AZ_AMQP_MESSAGE_ID_KIND_NULL;
      break;
  }
}

static void _decode_header(az_amqp_value const* list, az_amqp_message_header* h)
{
  memset(h, 0, sizeof(*h));
  az_amqp_decoder d;
  uint32_t n = 0;
  if (az_result_failed(az_amqp_value_get_list_decoder(list, &d, &n)))
  {
    return;
  }
  az_amqp_value v;
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_BOOL)
  {
    h->durable = v.scalar.boolean;
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_UBYTE)
  {
    h->priority = (uint8_t)v.scalar.u64;
    h->has_priority = true;
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_UINT)
  {
    h->time_to_live_milliseconds = (uint32_t)v.scalar.u64;
    h->has_time_to_live = true;
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_BOOL)
  {
    h->first_acquirer = v.scalar.boolean;
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_UINT)
  {
    h->delivery_count = (uint32_t)v.scalar.u64;
  }
}

static void _decode_properties(az_amqp_value const* list, az_amqp_message_properties* p)
{
  memset(p, 0, sizeof(*p));
  az_amqp_decoder d;
  uint32_t n = 0;
  if (az_result_failed(az_amqp_value_get_list_decoder(list, &d, &n)))
  {
    return;
  }
  az_amqp_value v;
  if (_next_field(&d, &n, &v))
  {
    _decode_message_id(&v, &p->message_id);
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_BINARY)
  {
    p->user_id = v.payload;
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_STRING)
  {
    p->to = v.payload;
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_STRING)
  {
    p->subject = v.payload;
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_STRING)
  {
    p->reply_to = v.payload;
  }
  if (_next_field(&d, &n, &v))
  {
    _decode_message_id(&v, &p->correlation_id);
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_SYMBOL)
  {
    p->content_type = v.payload;
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_SYMBOL)
  {
    p->content_encoding = v.payload;
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_TIMESTAMP)
  {
    p->absolute_expiry_time_ms = v.scalar.i64;
    p->has_absolute_expiry_time = true;
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_TIMESTAMP)
  {
    p->creation_time_ms = v.scalar.i64;
    p->has_creation_time = true;
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_STRING)
  {
    p->group_id = v.payload;
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_UINT)
  {
    p->group_sequence = (uint32_t)v.scalar.u64;
    p->has_group_sequence = true;
  }
  if (_next_field(&d, &n, &v) && v.kind == AZ_AMQP_VALUE_KIND_STRING)
  {
    p->reply_to_group_id = v.payload;
  }
}

AZ_NODISCARD az_result az_amqp_message_decode(az_span encoded, az_amqp_message* out_message)
{
  if (out_message == NULL)
  {
    return AZ_ERROR_ARG;
  }
  _AZ_RET(az_amqp_message_init(out_message));

  az_amqp_decoder dec;
  _AZ_RET(az_amqp_decoder_init(&dec, encoded));

  while (az_amqp_decoder_has_next(&dec))
  {
    az_amqp_value section;
    _AZ_RET(az_amqp_decoder_decode(&dec, &section));
    if (section.kind != AZ_AMQP_VALUE_KIND_DESCRIBED)
    {
      return AZ_ERROR_AMQP_DECODE;
    }

    az_amqp_value descriptor;
    az_amqp_decoder body;
    _AZ_RET(az_amqp_value_get_described(&section, &descriptor, &body));
    if (descriptor.kind != AZ_AMQP_VALUE_KIND_ULONG)
    {
      continue; // unknown descriptor form; skip section
    }

    az_amqp_value value;
    _AZ_RET(az_amqp_decoder_decode(&body, &value));

    switch (descriptor.scalar.u64)
    {
      case _AZ_AMQP_DESC_HEADER:
        _decode_header(&value, &out_message->header);
        out_message->has_header = true;
        break;
      case _AZ_AMQP_DESC_DELIVERY_ANNOTATIONS:
        out_message->delivery_annotations = value.encoded;
        break;
      case _AZ_AMQP_DESC_MESSAGE_ANNOTATIONS:
        out_message->message_annotations = value.encoded;
        break;
      case _AZ_AMQP_DESC_PROPERTIES:
        _decode_properties(&value, &out_message->properties);
        out_message->has_properties = true;
        break;
      case _AZ_AMQP_DESC_APPLICATION_PROPERTIES:
        out_message->application_properties = value.encoded;
        break;
      case _AZ_AMQP_DESC_DATA:
        out_message->body_kind = AZ_AMQP_MESSAGE_BODY_KIND_DATA;
        out_message->body = value.payload;
        break;
      case _AZ_AMQP_DESC_AMQP_VALUE:
        out_message->body_kind = AZ_AMQP_MESSAGE_BODY_KIND_VALUE;
        out_message->body = value.encoded;
        break;
      case _AZ_AMQP_DESC_AMQP_SEQUENCE:
        out_message->body_kind = AZ_AMQP_MESSAGE_BODY_KIND_SEQUENCE;
        out_message->body = value.encoded;
        break;
      case _AZ_AMQP_DESC_FOOTER:
        out_message->footer = value.encoded;
        break;
      default:
        break;
    }
  }
  return AZ_OK;
}

// ============================ getters ============================

AZ_NODISCARD az_result
az_amqp_message_get_header(az_amqp_message const* message, az_amqp_message_header* out_header)
{
  if (!message->has_header)
  {
    return AZ_ERROR_ITEM_NOT_FOUND;
  }
  *out_header = message->header;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_message_get_properties(
    az_amqp_message const* message,
    az_amqp_message_properties* out_properties)
{
  if (!message->has_properties)
  {
    return AZ_ERROR_ITEM_NOT_FOUND;
  }
  *out_properties = message->properties;
  return AZ_OK;
}

static az_result _get_span_or_not_found(az_span value, az_span* out)
{
  if (az_span_size(value) == 0)
  {
    return AZ_ERROR_ITEM_NOT_FOUND;
  }
  *out = value;
  return AZ_OK;
}

AZ_NODISCARD az_result
az_amqp_message_get_message_annotations(az_amqp_message const* message, az_span* out_encoded_map)
{
  return _get_span_or_not_found(message->message_annotations, out_encoded_map);
}

AZ_NODISCARD az_result
az_amqp_message_get_delivery_annotations(az_amqp_message const* message, az_span* out_encoded_map)
{
  return _get_span_or_not_found(message->delivery_annotations, out_encoded_map);
}

AZ_NODISCARD az_result
az_amqp_message_get_application_properties(az_amqp_message const* message, az_span* out_encoded_map)
{
  return _get_span_or_not_found(message->application_properties, out_encoded_map);
}

AZ_NODISCARD az_result
az_amqp_message_get_footer(az_amqp_message const* message, az_span* out_encoded_map)
{
  return _get_span_or_not_found(message->footer, out_encoded_map);
}

AZ_NODISCARD az_result az_amqp_message_get_body(
    az_amqp_message const* message,
    az_amqp_message_body_kind* out_kind,
    az_span* out_body)
{
  if (message->body_kind == AZ_AMQP_MESSAGE_BODY_KIND_NONE)
  {
    return AZ_ERROR_ITEM_NOT_FOUND;
  }
  *out_kind = message->body_kind;
  *out_body = message->body;
  return AZ_OK;
}

// ============================ property map builder ============================

AZ_NODISCARD az_result az_amqp_property_map_init(az_amqp_property_map* map, az_span buffer)
{
  if (map == NULL || az_span_size(buffer) == 0)
  {
    return AZ_ERROR_ARG;
  }
  map->sealed = false;
  _AZ_RET(az_amqp_encoder_init(&map->encoder, buffer));
  return az_amqp_encoder_begin_map(&map->encoder);
}

AZ_NODISCARD az_result
az_amqp_property_map_add_string(az_amqp_property_map* map, az_span key, az_span value)
{
  if (map->sealed)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  _AZ_RET(az_amqp_encoder_append_string(&map->encoder, key));
  return az_amqp_encoder_append_string(&map->encoder, value);
}

AZ_NODISCARD az_result
az_amqp_property_map_add_long(az_amqp_property_map* map, az_span key, int64_t value)
{
  if (map->sealed)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  _AZ_RET(az_amqp_encoder_append_string(&map->encoder, key));
  return az_amqp_encoder_append_long(&map->encoder, value);
}

AZ_NODISCARD az_result
az_amqp_property_map_add_bool(az_amqp_property_map* map, az_span key, bool value)
{
  if (map->sealed)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  _AZ_RET(az_amqp_encoder_append_string(&map->encoder, key));
  return az_amqp_encoder_append_bool(&map->encoder, value);
}

AZ_NODISCARD az_result
az_amqp_property_map_add_binary(az_amqp_property_map* map, az_span key, az_span value)
{
  if (map->sealed)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  _AZ_RET(az_amqp_encoder_append_string(&map->encoder, key));
  return az_amqp_encoder_append_binary(&map->encoder, value);
}

AZ_NODISCARD az_span az_amqp_property_map_get_bytes(az_amqp_property_map* map)
{
  if (!map->sealed)
  {
    (void)az_amqp_encoder_end_map(&map->encoder);
    map->sealed = true;
  }
  return az_amqp_encoder_get_bytes(&map->encoder);
}
