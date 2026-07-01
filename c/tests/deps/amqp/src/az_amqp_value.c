// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

#include <azure/amqp/az_amqp_value.h>

#include "_az_amqp_codec.h"
#include "_az_amqp_internal.h"

#include <azure/core/az_span.h>

#include <string.h>

#define _AZ_RET(expr)                       \
  do                                        \
  {                                         \
    az_result const _r = (expr);            \
    if (az_result_failed(_r))               \
    {                                       \
      return _r;                            \
    }                                       \
  } while (0)

// ============================ Encoder ============================

AZ_NODISCARD az_result az_amqp_encoder_init(az_amqp_encoder* encoder, az_span destination)
{
  if (encoder == NULL)
  {
    return AZ_ERROR_ARG;
  }
  encoder->destination = destination;
  encoder->length = 0;
  encoder->depth = 0;
  return AZ_OK;
}

AZ_NODISCARD az_span az_amqp_encoder_get_bytes(az_amqp_encoder const* encoder)
{
  return az_span_slice(encoder->destination, 0, encoder->length);
}

static az_result _put(az_amqp_encoder* e, uint8_t b)
{
  if (e->length + 1 > az_span_size(e->destination))
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  az_span_ptr(e->destination)[e->length++] = b;
  return AZ_OK;
}

static az_result _put_bytes(az_amqp_encoder* e, uint8_t const* p, int32_t n)
{
  if (e->length + n > az_span_size(e->destination))
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  if (n > 0)
  {
    memcpy(az_span_ptr(e->destination) + e->length, p, (size_t)n);
    e->length += n;
  }
  return AZ_OK;
}

static az_result _put_u32(az_amqp_encoder* e, uint32_t v)
{
  uint8_t b[4];
  _az_amqp_write_u32_be(b, v);
  return _put_bytes(e, b, 4);
}

static az_result _put_u64(az_amqp_encoder* e, uint64_t v)
{
  uint8_t b[8];
  _az_amqp_write_u64_be(b, v);
  return _put_bytes(e, b, 8);
}

// Increment the element count of the enclosing open compound (if any).
static void _completed(az_amqp_encoder* e)
{
  if (e->depth > 0)
  {
    e->patch_counts[e->depth - 1]++;
  }
}

// Maps an element kind to the single shared array-element constructor byte. Variable-width kinds use
// their 32-bit form so every element is encoded with an identical constructor, as arrays require.
static az_result _array_ctor_byte(az_amqp_value_kind kind, uint8_t* out_fc)
{
  switch (kind)
  {
    case AZ_AMQP_VALUE_KIND_NULL: *out_fc = _AZ_AMQP_FC_NULL; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_BOOL: *out_fc = _AZ_AMQP_FC_BOOL; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_UBYTE: *out_fc = _AZ_AMQP_FC_UBYTE; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_USHORT: *out_fc = _AZ_AMQP_FC_USHORT; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_UINT: *out_fc = _AZ_AMQP_FC_UINT; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_ULONG: *out_fc = _AZ_AMQP_FC_ULONG; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_BYTE: *out_fc = _AZ_AMQP_FC_BYTE; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_SHORT: *out_fc = _AZ_AMQP_FC_SHORT; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_INT: *out_fc = _AZ_AMQP_FC_INT; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_LONG: *out_fc = _AZ_AMQP_FC_LONG; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_FLOAT: *out_fc = _AZ_AMQP_FC_FLOAT; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_DOUBLE: *out_fc = _AZ_AMQP_FC_DOUBLE; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_CHAR: *out_fc = _AZ_AMQP_FC_CHAR; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_TIMESTAMP: *out_fc = _AZ_AMQP_FC_TIMESTAMP; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_UUID: *out_fc = _AZ_AMQP_FC_UUID; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_BINARY: *out_fc = _AZ_AMQP_FC_VBIN32; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_STRING: *out_fc = _AZ_AMQP_FC_STR32; return AZ_OK;
    case AZ_AMQP_VALUE_KIND_SYMBOL: *out_fc = _AZ_AMQP_FC_SYM32; return AZ_OK;
    default: return AZ_ERROR_NOT_IMPLEMENTED; // arrays of compound/described elements: unsupported
  }
}

// Called at the start of every scalar appender. If the current open compound is an array, it
// validates the element kind, emits the shared element constructor once (ahead of the first
// element), and reports that the caller must write VALUE-ONLY bytes (no per-element constructor).
static az_result _pre_element(az_amqp_encoder* e, az_amqp_value_kind kind, bool* out_value_only)
{
  *out_value_only = false;
  if (e->depth > 0 && e->patch_is_array[e->depth - 1])
  {
    int32_t const d = e->depth - 1;
    if (e->patch_array_kind[d] != kind)
    {
      return AZ_ERROR_AMQP_WRONG_STATE; // all array elements must share the declared kind
    }
    if (!e->patch_array_ctor[d])
    {
      uint8_t fc;
      _AZ_RET(_array_ctor_byte(kind, &fc));
      _AZ_RET(_put(e, fc));
      e->patch_array_ctor[d] = true;
    }
    *out_value_only = true;
  }
  return AZ_OK;
}

// Writes a variable-length value (binary/string/symbol) in the 32-bit form WITHOUT a constructor,
// for use as an array element after the shared 32-bit constructor has been emitted.
static az_result _put_var32(az_amqp_encoder* e, az_span value)
{
  int32_t const n = az_span_size(value);
  _AZ_RET(_put_u32(e, (uint32_t)n));
  return _put_bytes(e, az_span_ptr(value), n);
}

AZ_NODISCARD az_result az_amqp_encoder_append_null(az_amqp_encoder* e)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_NULL, &vo));
  if (!vo) // in an array, `null` elements are zero-width (only the 0x40 constructor is emitted)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_NULL));
  }
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_bool(az_amqp_encoder* e, bool value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_BOOL, &vo));
  if (vo)
  {
    _AZ_RET(_put(e, value ? 0x01 : 0x00)); // 1 byte per element under the 0x56 constructor
  }
  else
  {
    _AZ_RET(_put(e, value ? _AZ_AMQP_FC_TRUE : _AZ_AMQP_FC_FALSE));
  }
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_ubyte(az_amqp_encoder* e, uint8_t value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_UBYTE, &vo));
  if (!vo)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_UBYTE));
  }
  _AZ_RET(_put(e, value));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_ushort(az_amqp_encoder* e, uint16_t value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_USHORT, &vo));
  uint8_t b[2];
  _az_amqp_write_u16_be(b, value);
  if (!vo)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_USHORT));
  }
  _AZ_RET(_put_bytes(e, b, 2));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_uint(az_amqp_encoder* e, uint32_t value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_UINT, &vo));
  if (!vo)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_UINT));
  }
  _AZ_RET(_put_u32(e, value));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_ulong(az_amqp_encoder* e, uint64_t value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_ULONG, &vo));
  if (!vo)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_ULONG));
  }
  _AZ_RET(_put_u64(e, value));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_byte(az_amqp_encoder* e, int8_t value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_BYTE, &vo));
  if (!vo)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_BYTE));
  }
  _AZ_RET(_put(e, (uint8_t)value));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_short(az_amqp_encoder* e, int16_t value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_SHORT, &vo));
  uint8_t b[2];
  _az_amqp_write_u16_be(b, (uint16_t)value);
  if (!vo)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_SHORT));
  }
  _AZ_RET(_put_bytes(e, b, 2));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_int(az_amqp_encoder* e, int32_t value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_INT, &vo));
  if (!vo)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_INT));
  }
  _AZ_RET(_put_u32(e, (uint32_t)value));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_long(az_amqp_encoder* e, int64_t value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_LONG, &vo));
  if (!vo)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_LONG));
  }
  _AZ_RET(_put_u64(e, (uint64_t)value));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_float(az_amqp_encoder* e, float value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_FLOAT, &vo));
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  if (!vo)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_FLOAT));
  }
  _AZ_RET(_put_u32(e, bits));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_double(az_amqp_encoder* e, double value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_DOUBLE, &vo));
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  if (!vo)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_DOUBLE));
  }
  _AZ_RET(_put_u64(e, bits));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_char(az_amqp_encoder* e, uint32_t utf32)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_CHAR, &vo));
  if (!vo)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_CHAR));
  }
  _AZ_RET(_put_u32(e, utf32));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result
az_amqp_encoder_append_timestamp(az_amqp_encoder* e, int64_t milliseconds_since_epoch)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_TIMESTAMP, &vo));
  if (!vo)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_TIMESTAMP));
  }
  _AZ_RET(_put_u64(e, (uint64_t)milliseconds_since_epoch));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_uuid(az_amqp_encoder* e, uint8_t const uuid[16])
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_UUID, &vo));
  if (!vo)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_UUID));
  }
  _AZ_RET(_put_bytes(e, uuid, 16));
  _completed(e);
  return AZ_OK;
}

// Writes a variable-length value (binary/string/symbol) WITHOUT updating the element count.
static az_result _write_var(az_amqp_encoder* e, uint8_t fc8, uint8_t fc32, az_span value)
{
  int32_t const n = az_span_size(value);
  if (n <= 0xFF)
  {
    _AZ_RET(_put(e, fc8));
    _AZ_RET(_put(e, (uint8_t)n));
  }
  else
  {
    _AZ_RET(_put(e, fc32));
    _AZ_RET(_put_u32(e, (uint32_t)n));
  }
  return _put_bytes(e, az_span_ptr(value), n);
}

AZ_NODISCARD az_result az_amqp_encoder_append_binary(az_amqp_encoder* e, az_span value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_BINARY, &vo));
  _AZ_RET(vo ? _put_var32(e, value) : _write_var(e, _AZ_AMQP_FC_VBIN8, _AZ_AMQP_FC_VBIN32, value));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_string(az_amqp_encoder* e, az_span value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_STRING, &vo));
  _AZ_RET(vo ? _put_var32(e, value) : _write_var(e, _AZ_AMQP_FC_STR8, _AZ_AMQP_FC_STR32, value));
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_append_symbol(az_amqp_encoder* e, az_span value)
{
  bool vo;
  _AZ_RET(_pre_element(e, AZ_AMQP_VALUE_KIND_SYMBOL, &vo));
  _AZ_RET(vo ? _put_var32(e, value) : _write_var(e, _AZ_AMQP_FC_SYM8, _AZ_AMQP_FC_SYM32, value));
  _completed(e);
  return AZ_OK;
}

static az_result _begin_compound(az_amqp_encoder* e, uint8_t fc)
{
  if (e->depth >= AZ_AMQP_VALUE_MAX_NESTING)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  _AZ_RET(_put(e, fc));
  e->patch_offsets[e->depth] = e->length; // size field begins here
  _AZ_RET(_put_u32(e, 0)); // size placeholder
  _AZ_RET(_put_u32(e, 0)); // count placeholder
  e->patch_counts[e->depth] = 0;
  e->patch_is_array[e->depth] = false;
  e->depth++;
  return AZ_OK;
}

static az_result _end_compound(az_amqp_encoder* e)
{
  if (e->depth == 0)
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  e->depth--;
  int32_t const off = e->patch_offsets[e->depth];
  int32_t const count = e->patch_counts[e->depth];
  int32_t const element_bytes = e->length - (off + 8);
  uint8_t* p = az_span_ptr(e->destination);
  _az_amqp_write_u32_be(p + off, (uint32_t)(4 + element_bytes));
  _az_amqp_write_u32_be(p + off + 4, (uint32_t)count);
  _completed(e);
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_begin_list(az_amqp_encoder* e)
{
  return _begin_compound(e, _AZ_AMQP_FC_LIST32);
}

AZ_NODISCARD az_result az_amqp_encoder_end_list(az_amqp_encoder* e) { return _end_compound(e); }

AZ_NODISCARD az_result az_amqp_encoder_begin_map(az_amqp_encoder* e)
{
  return _begin_compound(e, _AZ_AMQP_FC_MAP32);
}

AZ_NODISCARD az_result az_amqp_encoder_end_map(az_amqp_encoder* e) { return _end_compound(e); }

AZ_NODISCARD az_result
az_amqp_encoder_begin_array(az_amqp_encoder* e, az_amqp_value_kind element_kind)
{
  uint8_t ctor;
  _AZ_RET(_array_ctor_byte(element_kind, &ctor)); // reject kinds we cannot array-encode up front
  if (e->depth >= AZ_AMQP_VALUE_MAX_NESTING)
  {
    return AZ_ERROR_NOT_ENOUGH_SPACE;
  }
  // array32: 0xF0 | size(4) | count(4) | shared-constructor | element-values...
  // The shared constructor is emitted lazily by _pre_element before the first element (or by
  // end_array for an empty array), so every element writes value-only bytes.
  _AZ_RET(_put(e, _AZ_AMQP_FC_ARRAY32));
  e->patch_offsets[e->depth] = e->length; // size field begins here
  _AZ_RET(_put_u32(e, 0)); // size placeholder
  _AZ_RET(_put_u32(e, 0)); // count placeholder
  e->patch_counts[e->depth] = 0;
  e->patch_is_array[e->depth] = true;
  e->patch_array_kind[e->depth] = element_kind;
  e->patch_array_ctor[e->depth] = false;
  e->depth++;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_encoder_end_array(az_amqp_encoder* e)
{
  if (e->depth == 0 || !e->patch_is_array[e->depth - 1])
  {
    return AZ_ERROR_AMQP_WRONG_STATE;
  }
  int32_t const d = e->depth - 1;
  if (!e->patch_array_ctor[d])
  {
    // An empty array still carries its shared element constructor.
    uint8_t ctor;
    _AZ_RET(_array_ctor_byte(e->patch_array_kind[d], &ctor));
    _AZ_RET(_put(e, ctor));
    e->patch_array_ctor[d] = true;
  }
  return _end_compound(e);
}

AZ_NODISCARD az_result
az_amqp_encoder_append_descriptor_ulong(az_amqp_encoder* e, uint64_t descriptor_code)
{
  _AZ_RET(_put(e, _AZ_AMQP_FC_DESCRIBED));
  if (descriptor_code <= 0xFF)
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_SMALLULONG));
    _AZ_RET(_put(e, (uint8_t)descriptor_code));
  }
  else
  {
    _AZ_RET(_put(e, _AZ_AMQP_FC_ULONG));
    _AZ_RET(_put_u64(e, descriptor_code));
  }
  // No _completed(): the following value completes the described element.
  return AZ_OK;
}

AZ_NODISCARD az_result
az_amqp_encoder_append_descriptor_symbol(az_amqp_encoder* e, az_span descriptor_symbol)
{
  _AZ_RET(_put(e, _AZ_AMQP_FC_DESCRIBED));
  return _write_var(e, _AZ_AMQP_FC_SYM8, _AZ_AMQP_FC_SYM32, descriptor_symbol);
}

AZ_NODISCARD az_result _az_amqp_encoder_append_raw(az_amqp_encoder* e, az_span raw)
{
  _AZ_RET(_put_bytes(e, az_span_ptr(raw), az_span_size(raw)));
  _completed(e);
  return AZ_OK;
}

// ============================ Decoder ============================

AZ_NODISCARD az_result az_amqp_decoder_init(az_amqp_decoder* decoder, az_span encoded)
{
  if (decoder == NULL)
  {
    return AZ_ERROR_ARG;
  }
  decoder->buffer = encoded;
  decoder->offset = 0;
  decoder->array_element_fc = -1;
  return AZ_OK;
}

AZ_NODISCARD bool az_amqp_decoder_has_next(az_amqp_decoder const* decoder)
{
  return decoder->offset < az_span_size(decoder->buffer);
}

// Maps a constructor format code to the value kind it produces (used to report an array's shared
// element kind, since array elements carry no per-element constructor).
static az_amqp_value_kind _fc_to_kind(uint8_t fc)
{
  switch (fc)
  {
    case _AZ_AMQP_FC_BOOL:
    case _AZ_AMQP_FC_TRUE:
    case _AZ_AMQP_FC_FALSE: return AZ_AMQP_VALUE_KIND_BOOL;
    case _AZ_AMQP_FC_UBYTE: return AZ_AMQP_VALUE_KIND_UBYTE;
    case _AZ_AMQP_FC_USHORT: return AZ_AMQP_VALUE_KIND_USHORT;
    case _AZ_AMQP_FC_UINT0:
    case _AZ_AMQP_FC_SMALLUINT:
    case _AZ_AMQP_FC_UINT: return AZ_AMQP_VALUE_KIND_UINT;
    case _AZ_AMQP_FC_ULONG0:
    case _AZ_AMQP_FC_SMALLULONG:
    case _AZ_AMQP_FC_ULONG: return AZ_AMQP_VALUE_KIND_ULONG;
    case _AZ_AMQP_FC_BYTE: return AZ_AMQP_VALUE_KIND_BYTE;
    case _AZ_AMQP_FC_SHORT: return AZ_AMQP_VALUE_KIND_SHORT;
    case _AZ_AMQP_FC_SMALLINT:
    case _AZ_AMQP_FC_INT: return AZ_AMQP_VALUE_KIND_INT;
    case _AZ_AMQP_FC_SMALLLONG:
    case _AZ_AMQP_FC_LONG: return AZ_AMQP_VALUE_KIND_LONG;
    case _AZ_AMQP_FC_FLOAT: return AZ_AMQP_VALUE_KIND_FLOAT;
    case _AZ_AMQP_FC_DOUBLE: return AZ_AMQP_VALUE_KIND_DOUBLE;
    case _AZ_AMQP_FC_CHAR: return AZ_AMQP_VALUE_KIND_CHAR;
    case _AZ_AMQP_FC_TIMESTAMP: return AZ_AMQP_VALUE_KIND_TIMESTAMP;
    case _AZ_AMQP_FC_UUID: return AZ_AMQP_VALUE_KIND_UUID;
    case _AZ_AMQP_FC_VBIN8:
    case _AZ_AMQP_FC_VBIN32: return AZ_AMQP_VALUE_KIND_BINARY;
    case _AZ_AMQP_FC_STR8:
    case _AZ_AMQP_FC_STR32: return AZ_AMQP_VALUE_KIND_STRING;
    case _AZ_AMQP_FC_SYM8:
    case _AZ_AMQP_FC_SYM32: return AZ_AMQP_VALUE_KIND_SYMBOL;
    case _AZ_AMQP_FC_LIST0:
    case _AZ_AMQP_FC_LIST8:
    case _AZ_AMQP_FC_LIST32: return AZ_AMQP_VALUE_KIND_LIST;
    case _AZ_AMQP_FC_MAP8:
    case _AZ_AMQP_FC_MAP32: return AZ_AMQP_VALUE_KIND_MAP;
    case _AZ_AMQP_FC_ARRAY8:
    case _AZ_AMQP_FC_ARRAY32: return AZ_AMQP_VALUE_KIND_ARRAY;
    default: return AZ_AMQP_VALUE_KIND_NULL;
  }
}

// Decodes a single VALUE-ONLY array element at `off` given the array's shared constructor `fc` (the
// element carries no constructor byte of its own). Reports the element's byte length in *consumed.
static az_result
_decode_array_element(az_span buf, int32_t off, uint8_t fc, az_amqp_value* v, int32_t* consumed)
{
  uint8_t const* p = az_span_ptr(buf);
  int32_t const cap = az_span_size(buf);
  memset(v, 0, sizeof(*v));
  int32_t len = 0;

#define _NEED(n)                      \
  do                                  \
  {                                   \
    if (off + (n) > cap)              \
    {                                 \
      return AZ_ERROR_UNEXPECTED_END; \
    }                                 \
  } while (0)

  v->kind = _fc_to_kind(fc);
  switch (fc)
  {
    case _AZ_AMQP_FC_NULL:
    case _AZ_AMQP_FC_UINT0:
    case _AZ_AMQP_FC_ULONG0:
      len = 0;
      break;
    case _AZ_AMQP_FC_TRUE:
      v->scalar.boolean = true;
      len = 0;
      break;
    case _AZ_AMQP_FC_FALSE:
      v->scalar.boolean = false;
      len = 0;
      break;
    case _AZ_AMQP_FC_BOOL:
      _NEED(1);
      v->scalar.boolean = (p[off] != 0);
      len = 1;
      break;
    case _AZ_AMQP_FC_UBYTE:
    case _AZ_AMQP_FC_SMALLUINT:
    case _AZ_AMQP_FC_SMALLULONG:
      _NEED(1);
      v->scalar.u64 = p[off];
      len = 1;
      break;
    case _AZ_AMQP_FC_BYTE:
    case _AZ_AMQP_FC_SMALLINT:
    case _AZ_AMQP_FC_SMALLLONG:
      _NEED(1);
      v->scalar.i64 = (int8_t)p[off];
      len = 1;
      break;
    case _AZ_AMQP_FC_USHORT:
      _NEED(2);
      v->scalar.u64 = _az_amqp_read_u16_be(p + off);
      len = 2;
      break;
    case _AZ_AMQP_FC_SHORT:
      _NEED(2);
      v->scalar.i64 = (int16_t)_az_amqp_read_u16_be(p + off);
      len = 2;
      break;
    case _AZ_AMQP_FC_UINT:
    case _AZ_AMQP_FC_CHAR:
      _NEED(4);
      v->scalar.u64 = _az_amqp_read_u32_be(p + off);
      len = 4;
      break;
    case _AZ_AMQP_FC_INT:
      _NEED(4);
      v->scalar.i64 = (int32_t)_az_amqp_read_u32_be(p + off);
      len = 4;
      break;
    case _AZ_AMQP_FC_FLOAT:
    {
      _NEED(4);
      uint32_t bits = _az_amqp_read_u32_be(p + off);
      float f;
      memcpy(&f, &bits, sizeof(f));
      v->scalar.f64 = (double)f;
      len = 4;
      break;
    }
    case _AZ_AMQP_FC_ULONG:
      _NEED(8);
      v->scalar.u64 = _az_amqp_read_u64_be(p + off);
      len = 8;
      break;
    case _AZ_AMQP_FC_LONG:
    case _AZ_AMQP_FC_TIMESTAMP:
      _NEED(8);
      v->scalar.i64 = (int64_t)_az_amqp_read_u64_be(p + off);
      len = 8;
      break;
    case _AZ_AMQP_FC_DOUBLE:
    {
      _NEED(8);
      uint64_t bits = _az_amqp_read_u64_be(p + off);
      double d;
      memcpy(&d, &bits, sizeof(d));
      v->scalar.f64 = d;
      len = 8;
      break;
    }
    case _AZ_AMQP_FC_UUID:
      _NEED(16);
      v->payload = az_span_slice(buf, off, off + 16);
      len = 16;
      break;
    case _AZ_AMQP_FC_VBIN8:
    case _AZ_AMQP_FC_STR8:
    case _AZ_AMQP_FC_SYM8:
    {
      _NEED(1);
      int32_t n = p[off];
      _NEED(1 + n);
      v->payload = az_span_slice(buf, off + 1, off + 1 + n);
      len = 1 + n;
      break;
    }
    case _AZ_AMQP_FC_VBIN32:
    case _AZ_AMQP_FC_STR32:
    case _AZ_AMQP_FC_SYM32:
    {
      _NEED(4);
      int32_t n = (int32_t)_az_amqp_read_u32_be(p + off);
      _NEED(4 + n);
      v->payload = az_span_slice(buf, off + 4, off + 4 + n);
      len = 4 + n;
      break;
    }
    default:
      return AZ_ERROR_NOT_IMPLEMENTED; // arrays of compound/described elements are not supported
  }

#undef _NEED

  v->encoded = az_span_slice(buf, off, off + len);
  *consumed = len;
  return AZ_OK;
}

// Decodes the value at `off` within `buf`, filling `v` and reporting `*consumed` bytes.
static az_result _decode_one(az_span buf, int32_t off, az_amqp_value* v, int32_t* consumed)
{
  uint8_t const* p = az_span_ptr(buf);
  int32_t const cap = az_span_size(buf);
  if (off >= cap)
  {
    return AZ_ERROR_UNEXPECTED_END;
  }

  memset(v, 0, sizeof(*v));
  uint8_t const fc = p[off];
  int32_t len = 0;

#define _NEED(n)            \
  do                        \
  {                         \
    if (off + (n) > cap)    \
    {                       \
      return AZ_ERROR_UNEXPECTED_END; \
    }                       \
  } while (0)

  switch (fc)
  {
    case _AZ_AMQP_FC_NULL:
      v->kind = AZ_AMQP_VALUE_KIND_NULL;
      len = 1;
      break;
    case _AZ_AMQP_FC_TRUE:
      v->kind = AZ_AMQP_VALUE_KIND_BOOL;
      v->scalar.boolean = true;
      len = 1;
      break;
    case _AZ_AMQP_FC_FALSE:
      v->kind = AZ_AMQP_VALUE_KIND_BOOL;
      v->scalar.boolean = false;
      len = 1;
      break;
    case _AZ_AMQP_FC_BOOL:
      _NEED(2);
      v->kind = AZ_AMQP_VALUE_KIND_BOOL;
      v->scalar.boolean = (p[off + 1] != 0);
      len = 2;
      break;
    case _AZ_AMQP_FC_UBYTE:
      _NEED(2);
      v->kind = AZ_AMQP_VALUE_KIND_UBYTE;
      v->scalar.u64 = p[off + 1];
      len = 2;
      break;
    case _AZ_AMQP_FC_USHORT:
      _NEED(3);
      v->kind = AZ_AMQP_VALUE_KIND_USHORT;
      v->scalar.u64 = _az_amqp_read_u16_be(p + off + 1);
      len = 3;
      break;
    case _AZ_AMQP_FC_UINT0:
      v->kind = AZ_AMQP_VALUE_KIND_UINT;
      v->scalar.u64 = 0;
      len = 1;
      break;
    case _AZ_AMQP_FC_SMALLUINT:
      _NEED(2);
      v->kind = AZ_AMQP_VALUE_KIND_UINT;
      v->scalar.u64 = p[off + 1];
      len = 2;
      break;
    case _AZ_AMQP_FC_UINT:
      _NEED(5);
      v->kind = AZ_AMQP_VALUE_KIND_UINT;
      v->scalar.u64 = _az_amqp_read_u32_be(p + off + 1);
      len = 5;
      break;
    case _AZ_AMQP_FC_ULONG0:
      v->kind = AZ_AMQP_VALUE_KIND_ULONG;
      v->scalar.u64 = 0;
      len = 1;
      break;
    case _AZ_AMQP_FC_SMALLULONG:
      _NEED(2);
      v->kind = AZ_AMQP_VALUE_KIND_ULONG;
      v->scalar.u64 = p[off + 1];
      len = 2;
      break;
    case _AZ_AMQP_FC_ULONG:
      _NEED(9);
      v->kind = AZ_AMQP_VALUE_KIND_ULONG;
      v->scalar.u64 = _az_amqp_read_u64_be(p + off + 1);
      len = 9;
      break;
    case _AZ_AMQP_FC_BYTE:
      _NEED(2);
      v->kind = AZ_AMQP_VALUE_KIND_BYTE;
      v->scalar.i64 = (int8_t)p[off + 1];
      len = 2;
      break;
    case _AZ_AMQP_FC_SHORT:
      _NEED(3);
      v->kind = AZ_AMQP_VALUE_KIND_SHORT;
      v->scalar.i64 = (int16_t)_az_amqp_read_u16_be(p + off + 1);
      len = 3;
      break;
    case _AZ_AMQP_FC_SMALLINT:
      _NEED(2);
      v->kind = AZ_AMQP_VALUE_KIND_INT;
      v->scalar.i64 = (int8_t)p[off + 1];
      len = 2;
      break;
    case _AZ_AMQP_FC_INT:
      _NEED(5);
      v->kind = AZ_AMQP_VALUE_KIND_INT;
      v->scalar.i64 = (int32_t)_az_amqp_read_u32_be(p + off + 1);
      len = 5;
      break;
    case _AZ_AMQP_FC_SMALLLONG:
      _NEED(2);
      v->kind = AZ_AMQP_VALUE_KIND_LONG;
      v->scalar.i64 = (int8_t)p[off + 1];
      len = 2;
      break;
    case _AZ_AMQP_FC_LONG:
      _NEED(9);
      v->kind = AZ_AMQP_VALUE_KIND_LONG;
      v->scalar.i64 = (int64_t)_az_amqp_read_u64_be(p + off + 1);
      len = 9;
      break;
    case _AZ_AMQP_FC_FLOAT:
    {
      _NEED(5);
      uint32_t bits = _az_amqp_read_u32_be(p + off + 1);
      float f;
      memcpy(&f, &bits, sizeof(f));
      v->kind = AZ_AMQP_VALUE_KIND_FLOAT;
      v->scalar.f64 = (double)f;
      len = 5;
      break;
    }
    case _AZ_AMQP_FC_DOUBLE:
    {
      _NEED(9);
      uint64_t bits = _az_amqp_read_u64_be(p + off + 1);
      double d;
      memcpy(&d, &bits, sizeof(d));
      v->kind = AZ_AMQP_VALUE_KIND_DOUBLE;
      v->scalar.f64 = d;
      len = 9;
      break;
    }
    case _AZ_AMQP_FC_CHAR:
      _NEED(5);
      v->kind = AZ_AMQP_VALUE_KIND_CHAR;
      v->scalar.u64 = _az_amqp_read_u32_be(p + off + 1);
      len = 5;
      break;
    case _AZ_AMQP_FC_TIMESTAMP:
      _NEED(9);
      v->kind = AZ_AMQP_VALUE_KIND_TIMESTAMP;
      v->scalar.i64 = (int64_t)_az_amqp_read_u64_be(p + off + 1);
      len = 9;
      break;
    case _AZ_AMQP_FC_UUID:
      _NEED(17);
      v->kind = AZ_AMQP_VALUE_KIND_UUID;
      v->payload = az_span_slice(buf, off + 1, off + 17);
      len = 17;
      break;
    case _AZ_AMQP_FC_VBIN8:
    case _AZ_AMQP_FC_STR8:
    case _AZ_AMQP_FC_SYM8:
    {
      _NEED(2);
      int32_t n = p[off + 1];
      _NEED(2 + n);
      v->kind = (fc == _AZ_AMQP_FC_VBIN8) ? AZ_AMQP_VALUE_KIND_BINARY
          : (fc == _AZ_AMQP_FC_STR8)     ? AZ_AMQP_VALUE_KIND_STRING
                                         : AZ_AMQP_VALUE_KIND_SYMBOL;
      v->payload = az_span_slice(buf, off + 2, off + 2 + n);
      len = 2 + n;
      break;
    }
    case _AZ_AMQP_FC_VBIN32:
    case _AZ_AMQP_FC_STR32:
    case _AZ_AMQP_FC_SYM32:
    {
      _NEED(5);
      int32_t n = (int32_t)_az_amqp_read_u32_be(p + off + 1);
      _NEED(5 + n);
      v->kind = (fc == _AZ_AMQP_FC_VBIN32) ? AZ_AMQP_VALUE_KIND_BINARY
          : (fc == _AZ_AMQP_FC_STR32)      ? AZ_AMQP_VALUE_KIND_STRING
                                           : AZ_AMQP_VALUE_KIND_SYMBOL;
      v->payload = az_span_slice(buf, off + 5, off + 5 + n);
      len = 5 + n;
      break;
    }
    case _AZ_AMQP_FC_LIST0:
      v->kind = AZ_AMQP_VALUE_KIND_LIST;
      v->count = 0;
      v->payload = az_span_slice(buf, off + 1, off + 1);
      len = 1;
      break;
    case _AZ_AMQP_FC_LIST8:
    case _AZ_AMQP_FC_MAP8:
    {
      _NEED(3);
      int32_t size = p[off + 1];
      _NEED(2 + size);
      v->kind = (fc == _AZ_AMQP_FC_LIST8) ? AZ_AMQP_VALUE_KIND_LIST : AZ_AMQP_VALUE_KIND_MAP;
      v->count = p[off + 2];
      v->payload = az_span_slice(buf, off + 3, off + 2 + size);
      len = 2 + size;
      break;
    }
    case _AZ_AMQP_FC_LIST32:
    case _AZ_AMQP_FC_MAP32:
    {
      _NEED(9);
      int32_t size = (int32_t)_az_amqp_read_u32_be(p + off + 1);
      _NEED(5 + size);
      v->kind = (fc == _AZ_AMQP_FC_LIST32) ? AZ_AMQP_VALUE_KIND_LIST : AZ_AMQP_VALUE_KIND_MAP;
      v->count = _az_amqp_read_u32_be(p + off + 5);
      v->payload = az_span_slice(buf, off + 9, off + 5 + size);
      len = 5 + size;
      break;
    }
    case _AZ_AMQP_FC_ARRAY8:
    {
      _NEED(3);
      int32_t size = p[off + 1];
      _NEED(2 + size);
      v->kind = AZ_AMQP_VALUE_KIND_ARRAY;
      v->count = p[off + 2];
      v->payload = az_span_slice(buf, off + 3, off + 2 + size);
      v->element_kind = (az_span_size(v->payload) > 0) ? _fc_to_kind(az_span_ptr(v->payload)[0])
                                                       : AZ_AMQP_VALUE_KIND_NULL;
      len = 2 + size;
      break;
    }
    case _AZ_AMQP_FC_ARRAY32:
    {
      _NEED(9);
      int32_t size = (int32_t)_az_amqp_read_u32_be(p + off + 1);
      _NEED(5 + size);
      v->kind = AZ_AMQP_VALUE_KIND_ARRAY;
      v->count = _az_amqp_read_u32_be(p + off + 5);
      v->payload = az_span_slice(buf, off + 9, off + 5 + size);
      v->element_kind = (az_span_size(v->payload) > 0) ? _fc_to_kind(az_span_ptr(v->payload)[0])
                                                       : AZ_AMQP_VALUE_KIND_NULL;
      len = 5 + size;
      break;
    }
    case _AZ_AMQP_FC_DESCRIBED:
    {
      // 0x00 + descriptor value + described value.
      az_amqp_value descriptor;
      int32_t desc_len = 0;
      _AZ_RET(_decode_one(buf, off + 1, &descriptor, &desc_len));
      az_amqp_value described;
      int32_t described_len = 0;
      _AZ_RET(_decode_one(buf, off + 1 + desc_len, &described, &described_len));
      v->kind = AZ_AMQP_VALUE_KIND_DESCRIBED;
      len = 1 + desc_len + described_len;
      v->payload = az_span_slice(buf, off + 1, off + len);
      break;
    }
    default:
      return AZ_ERROR_AMQP_DECODE;
  }

#undef _NEED

  v->encoded = az_span_slice(buf, off, off + len);
  *consumed = len;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_decoder_decode(az_amqp_decoder* decoder, az_amqp_value* out_value)
{
  int32_t consumed = 0;
  if (decoder->array_element_fc >= 0)
  {
    _AZ_RET(_decode_array_element(
        decoder->buffer,
        decoder->offset,
        (uint8_t)decoder->array_element_fc,
        out_value,
        &consumed));
  }
  else
  {
    _AZ_RET(_decode_one(decoder->buffer, decoder->offset, out_value, &consumed));
  }
  decoder->offset += consumed;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_value_get_bool(az_amqp_value const* value, bool* out)
{
  if (value->kind != AZ_AMQP_VALUE_KIND_BOOL)
  {
    return AZ_ERROR_AMQP_DECODE;
  }
  *out = value->scalar.boolean;
  return AZ_OK;
}

static az_result _get_unsigned(az_amqp_value const* value, az_amqp_value_kind kind, uint64_t* out)
{
  if (value->kind != kind)
  {
    return AZ_ERROR_AMQP_DECODE;
  }
  *out = value->scalar.u64;
  return AZ_OK;
}

static az_result _get_signed(az_amqp_value const* value, az_amqp_value_kind kind, int64_t* out)
{
  if (value->kind != kind)
  {
    return AZ_ERROR_AMQP_DECODE;
  }
  *out = value->scalar.i64;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_value_get_ubyte(az_amqp_value const* value, uint8_t* out)
{
  uint64_t u;
  _AZ_RET(_get_unsigned(value, AZ_AMQP_VALUE_KIND_UBYTE, &u));
  *out = (uint8_t)u;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_value_get_ushort(az_amqp_value const* value, uint16_t* out)
{
  uint64_t u;
  _AZ_RET(_get_unsigned(value, AZ_AMQP_VALUE_KIND_USHORT, &u));
  *out = (uint16_t)u;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_value_get_uint(az_amqp_value const* value, uint32_t* out)
{
  uint64_t u;
  _AZ_RET(_get_unsigned(value, AZ_AMQP_VALUE_KIND_UINT, &u));
  *out = (uint32_t)u;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_value_get_ulong(az_amqp_value const* value, uint64_t* out)
{
  return _get_unsigned(value, AZ_AMQP_VALUE_KIND_ULONG, out);
}

AZ_NODISCARD az_result az_amqp_value_get_byte(az_amqp_value const* value, int8_t* out)
{
  int64_t i;
  _AZ_RET(_get_signed(value, AZ_AMQP_VALUE_KIND_BYTE, &i));
  *out = (int8_t)i;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_value_get_short(az_amqp_value const* value, int16_t* out)
{
  int64_t i;
  _AZ_RET(_get_signed(value, AZ_AMQP_VALUE_KIND_SHORT, &i));
  *out = (int16_t)i;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_value_get_int(az_amqp_value const* value, int32_t* out)
{
  int64_t i;
  _AZ_RET(_get_signed(value, AZ_AMQP_VALUE_KIND_INT, &i));
  *out = (int32_t)i;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_value_get_long(az_amqp_value const* value, int64_t* out)
{
  return _get_signed(value, AZ_AMQP_VALUE_KIND_LONG, out);
}

AZ_NODISCARD az_result az_amqp_value_get_float(az_amqp_value const* value, float* out)
{
  if (value->kind != AZ_AMQP_VALUE_KIND_FLOAT)
  {
    return AZ_ERROR_AMQP_DECODE;
  }
  *out = (float)value->scalar.f64;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_value_get_double(az_amqp_value const* value, double* out)
{
  if (value->kind != AZ_AMQP_VALUE_KIND_DOUBLE)
  {
    return AZ_ERROR_AMQP_DECODE;
  }
  *out = value->scalar.f64;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_value_get_timestamp(az_amqp_value const* value, int64_t* out_ms)
{
  return _get_signed(value, AZ_AMQP_VALUE_KIND_TIMESTAMP, out_ms);
}

AZ_NODISCARD az_result az_amqp_value_get_uuid(az_amqp_value const* value, uint8_t out_uuid[16])
{
  if (value->kind != AZ_AMQP_VALUE_KIND_UUID || az_span_size(value->payload) != 16)
  {
    return AZ_ERROR_AMQP_DECODE;
  }
  memcpy(out_uuid, az_span_ptr(value->payload), 16);
  return AZ_OK;
}

static az_result _get_view(az_amqp_value const* value, az_amqp_value_kind kind, az_span* out)
{
  if (value->kind != kind)
  {
    return AZ_ERROR_AMQP_DECODE;
  }
  *out = value->payload;
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_value_get_binary(az_amqp_value const* value, az_span* out)
{
  return _get_view(value, AZ_AMQP_VALUE_KIND_BINARY, out);
}

AZ_NODISCARD az_result az_amqp_value_get_string(az_amqp_value const* value, az_span* out)
{
  return _get_view(value, AZ_AMQP_VALUE_KIND_STRING, out);
}

AZ_NODISCARD az_result az_amqp_value_get_symbol(az_amqp_value const* value, az_span* out)
{
  return _get_view(value, AZ_AMQP_VALUE_KIND_SYMBOL, out);
}

AZ_NODISCARD az_result az_amqp_value_get_list_decoder(
    az_amqp_value const* value,
    az_amqp_decoder* out_elements,
    uint32_t* out_count)
{
  if (value->kind != AZ_AMQP_VALUE_KIND_LIST)
  {
    return AZ_ERROR_AMQP_DECODE;
  }
  if (out_count != NULL)
  {
    *out_count = value->count;
  }
  return az_amqp_decoder_init(out_elements, value->payload);
}

AZ_NODISCARD az_result az_amqp_value_get_map_decoder(
    az_amqp_value const* value,
    az_amqp_decoder* out_pairs,
    uint32_t* out_pair_count)
{
  if (value->kind != AZ_AMQP_VALUE_KIND_MAP)
  {
    return AZ_ERROR_AMQP_DECODE;
  }
  if (out_pair_count != NULL)
  {
    *out_pair_count = value->count / 2;
  }
  return az_amqp_decoder_init(out_pairs, value->payload);
}

AZ_NODISCARD az_result az_amqp_value_get_array_decoder(
    az_amqp_value const* value,
    az_amqp_decoder* out_elements,
    az_amqp_value_kind* out_element_kind,
    uint32_t* out_count)
{
  if (value->kind != AZ_AMQP_VALUE_KIND_ARRAY)
  {
    return AZ_ERROR_AMQP_DECODE;
  }
  if (out_element_kind != NULL)
  {
    *out_element_kind = value->element_kind;
  }
  if (out_count != NULL)
  {
    *out_count = value->count;
  }
  // value->payload = [shared-constructor][value0][value1]... Iterate value-only elements by
  // skipping the constructor byte and decoding each element with it.
  out_elements->buffer = value->payload;
  if (az_span_size(value->payload) >= 1)
  {
    out_elements->offset = 1;
    out_elements->array_element_fc = az_span_ptr(value->payload)[0];
  }
  else
  {
    out_elements->offset = 0;
    out_elements->array_element_fc = -1;
  }
  return AZ_OK;
}

AZ_NODISCARD az_result az_amqp_value_get_described(
    az_amqp_value const* value,
    az_amqp_value* out_descriptor,
    az_amqp_decoder* out_body)
{
  if (value->kind != AZ_AMQP_VALUE_KIND_DESCRIBED)
  {
    return AZ_ERROR_AMQP_DECODE;
  }
  // value->payload covers descriptor + described value.
  int32_t desc_len = 0;
  _AZ_RET(_decode_one(value->payload, 0, out_descriptor, &desc_len));
  return az_amqp_decoder_init(
      out_body, az_span_slice_to_end(value->payload, desc_len));
}

AZ_NODISCARD az_result
_az_amqp_list_field(az_amqp_value const* list_value, uint32_t index, az_amqp_value* out)
{
  memset(out, 0, sizeof(*out));
  out->kind = AZ_AMQP_VALUE_KIND_NULL;
  if (list_value->kind != AZ_AMQP_VALUE_KIND_LIST)
  {
    return AZ_ERROR_AMQP_DECODE;
  }
  az_amqp_decoder d;
  uint32_t count = 0;
  _AZ_RET(az_amqp_value_get_list_decoder(list_value, &d, &count));
  if (index >= count)
  {
    return AZ_OK; // absent -> null
  }
  for (uint32_t i = 0; i <= index; i++)
  {
    _AZ_RET(az_amqp_decoder_decode(&d, out));
  }
  return AZ_OK;
}
