// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief The AMQP 1.0 type system: a non-allocating encoder that writes primitive, described, and
 * compound values into a caller-provided #az_span, and a cursor decoder that reads them back.
 *
 * @details AMQP defines its own self-describing binary type system (OASIS AMQP 1.0 §1.6). This
 * header exposes it directly so applications can build and parse arbitrary bodies, annotations, and
 * filters without the library allocating memory:
 *   - #az_amqp_encoder appends values into a destination #az_span, back-patching the size and count
 *     fields of compound types using a small fixed-depth stack.
 *   - #az_amqp_decoder walks an encoded #az_span one value at a time; compound values yield a child
 *     decoder so iteration is also allocation-free.
 *
 * @note All structures here are flat and visible; fields are library-managed and should not be
 * modified directly. There is no hidden `_internal` wrapper.
 */

#ifndef _az_AMQP_VALUE_H
#define _az_AMQP_VALUE_H

#include <azure/amqp/az_amqp_common.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

/**
 * @brief The maximum nesting depth of compound values an #az_amqp_encoder or #az_amqp_decoder can
 * traverse. Bounds the size of the encoder's visible back-patch arrays; it is a protocol depth
 * limit (AMQP nesting is shallow), not a fan-out capacity. Override with
 * `-DAZ_AMQP_VALUE_MAX_NESTING=...`.
 */
#ifndef AZ_AMQP_VALUE_MAX_NESTING
#define AZ_AMQP_VALUE_MAX_NESTING 16
#endif

/**
 * @brief Identifies the AMQP type of an #az_amqp_value (the "kind" of a decoded token).
 */
typedef enum
{
  AZ_AMQP_VALUE_KIND_NULL = 0, ///< The `null` value.
  AZ_AMQP_VALUE_KIND_BOOL, ///< `boolean`.
  AZ_AMQP_VALUE_KIND_UBYTE, ///< `ubyte` (8-bit unsigned).
  AZ_AMQP_VALUE_KIND_USHORT, ///< `ushort` (16-bit unsigned).
  AZ_AMQP_VALUE_KIND_UINT, ///< `uint` (32-bit unsigned).
  AZ_AMQP_VALUE_KIND_ULONG, ///< `ulong` (64-bit unsigned).
  AZ_AMQP_VALUE_KIND_BYTE, ///< `byte` (8-bit signed).
  AZ_AMQP_VALUE_KIND_SHORT, ///< `short` (16-bit signed).
  AZ_AMQP_VALUE_KIND_INT, ///< `int` (32-bit signed).
  AZ_AMQP_VALUE_KIND_LONG, ///< `long` (64-bit signed).
  AZ_AMQP_VALUE_KIND_FLOAT, ///< `float` (IEEE 754 binary32).
  AZ_AMQP_VALUE_KIND_DOUBLE, ///< `double` (IEEE 754 binary64).
  AZ_AMQP_VALUE_KIND_CHAR, ///< `char` (a single UTF-32 code point).
  AZ_AMQP_VALUE_KIND_TIMESTAMP, ///< `timestamp` (ms since the Unix epoch, signed 64-bit).
  AZ_AMQP_VALUE_KIND_UUID, ///< `uuid` (16 bytes).
  AZ_AMQP_VALUE_KIND_BINARY, ///< `binary` (opaque bytes).
  AZ_AMQP_VALUE_KIND_STRING, ///< `string` (UTF-8).
  AZ_AMQP_VALUE_KIND_SYMBOL, ///< `symbol` (7-bit ASCII).
  AZ_AMQP_VALUE_KIND_LIST, ///< `list` (an ordered sequence of polymorphic values).
  AZ_AMQP_VALUE_KIND_MAP, ///< `map` (an ordered sequence of distinct key/value pairs).
  AZ_AMQP_VALUE_KIND_ARRAY, ///< `array` (a sequence of values that share one type).
  AZ_AMQP_VALUE_KIND_DESCRIBED, ///< A described type (a descriptor value + a described value).
} az_amqp_value_kind;

/**
 * @brief A single decoded AMQP value.
 *
 * @details #kind is the discriminator. Scalar payloads and the byte ranges backing strings,
 * symbols, binaries, and compound bodies are read through the `az_amqp_value_get_*` accessors so
 * endianness and bounds are handled correctly. All #az_span views point into the buffer the owning
 * #az_amqp_decoder was initialized over and are valid only while that buffer remains valid. Fields
 * are library-managed.
 */
typedef struct
{
  az_amqp_value_kind kind; ///< The decoded type (the discriminator).
  az_span encoded; ///< The complete encoded bytes of this value (constructor through end).
  az_span payload; ///< For string/symbol/binary/compound: the bytes after the size field.
  union
  {
    bool boolean;
    uint64_t u64; ///< Holds ubyte/ushort/uint/ulong/char as appropriate.
    int64_t i64; ///< Holds byte/short/int/long/timestamp as appropriate.
    double f64; ///< Holds float (widened) and double.
  } scalar; ///< The scalar payload (selected by #kind).
  uint32_t count; ///< Element count (list/array) or item count (map keys+values).
  az_amqp_value_kind element_kind; ///< For arrays, the shared element kind.
} az_amqp_value;

// ================================ Encoder ================================

/**
 * @brief A non-allocating AMQP value encoder that appends into a caller-provided #az_span.
 *
 * @details Caller-allocated. Construct with #az_amqp_encoder_init, append values, then read the
 * encoded prefix with #az_amqp_encoder_get_bytes. Every `append`/`begin`/`end` returns
 * #AZ_ERROR_NOT_ENOUGH_SPACE if the destination would overflow. Fields are library-managed.
 */
typedef struct
{
  az_span destination; ///< The output buffer.
  int32_t length; ///< Bytes written so far.
  int32_t patch_offsets[AZ_AMQP_VALUE_MAX_NESTING]; ///< Back-patch stack for compound headers.
  int32_t patch_counts[AZ_AMQP_VALUE_MAX_NESTING]; ///< Element counts for the open compounds.
  bool patch_is_array[AZ_AMQP_VALUE_MAX_NESTING]; ///< Whether each open compound is an array.
  az_amqp_value_kind patch_array_kind[AZ_AMQP_VALUE_MAX_NESTING]; ///< An open array's element kind.
  bool patch_array_ctor[AZ_AMQP_VALUE_MAX_NESTING]; ///< Whether the array's shared element constructor was emitted.
  int32_t depth; ///< Current nesting depth.
} az_amqp_encoder;

/**
 * @brief Initializes an encoder to write into @p destination.
 *
 * @return #AZ_OK on success; #AZ_ERROR_ARG if @p encoder is `NULL`.
 */
AZ_NODISCARD az_result az_amqp_encoder_init(az_amqp_encoder* encoder, az_span destination);

/**
 * @brief Returns an #az_span over the bytes encoded so far (aliases the destination buffer).
 */
AZ_NODISCARD az_span az_amqp_encoder_get_bytes(az_amqp_encoder const* encoder);

/// @name Scalar appenders
/// Each appends one primitive value using AMQP's most compact applicable encoding.
/// @{
AZ_NODISCARD az_result az_amqp_encoder_append_null(az_amqp_encoder* encoder);
AZ_NODISCARD az_result az_amqp_encoder_append_bool(az_amqp_encoder* encoder, bool value);
AZ_NODISCARD az_result az_amqp_encoder_append_ubyte(az_amqp_encoder* encoder, uint8_t value);
AZ_NODISCARD az_result az_amqp_encoder_append_ushort(az_amqp_encoder* encoder, uint16_t value);
AZ_NODISCARD az_result az_amqp_encoder_append_uint(az_amqp_encoder* encoder, uint32_t value);
AZ_NODISCARD az_result az_amqp_encoder_append_ulong(az_amqp_encoder* encoder, uint64_t value);
AZ_NODISCARD az_result az_amqp_encoder_append_byte(az_amqp_encoder* encoder, int8_t value);
AZ_NODISCARD az_result az_amqp_encoder_append_short(az_amqp_encoder* encoder, int16_t value);
AZ_NODISCARD az_result az_amqp_encoder_append_int(az_amqp_encoder* encoder, int32_t value);
AZ_NODISCARD az_result az_amqp_encoder_append_long(az_amqp_encoder* encoder, int64_t value);
AZ_NODISCARD az_result az_amqp_encoder_append_float(az_amqp_encoder* encoder, float value);
AZ_NODISCARD az_result az_amqp_encoder_append_double(az_amqp_encoder* encoder, double value);
AZ_NODISCARD az_result az_amqp_encoder_append_char(az_amqp_encoder* encoder, uint32_t utf32);
/// @param[in] milliseconds_since_epoch Milliseconds since 1970-01-01T00:00:00Z (may be negative).
AZ_NODISCARD az_result
az_amqp_encoder_append_timestamp(az_amqp_encoder* encoder, int64_t milliseconds_since_epoch);
/// @param[in] uuid Pointer to exactly 16 bytes in network (big-endian) order.
AZ_NODISCARD az_result az_amqp_encoder_append_uuid(az_amqp_encoder* encoder, uint8_t const uuid[16]);
AZ_NODISCARD az_result az_amqp_encoder_append_binary(az_amqp_encoder* encoder, az_span value);
/// @param[in] value UTF-8 bytes (without a NUL terminator).
AZ_NODISCARD az_result az_amqp_encoder_append_string(az_amqp_encoder* encoder, az_span value);
/// @param[in] value 7-bit ASCII bytes (without a NUL terminator).
AZ_NODISCARD az_result az_amqp_encoder_append_symbol(az_amqp_encoder* encoder, az_span value);
/// @}

/// @name Compound builders
/// Begin/end pairs must be balanced. The encoder back-patches the size and count once `end` is
/// called, so the exact byte length need not be known in advance.
/// @{
AZ_NODISCARD az_result az_amqp_encoder_begin_list(az_amqp_encoder* encoder);
AZ_NODISCARD az_result az_amqp_encoder_end_list(az_amqp_encoder* encoder);
AZ_NODISCARD az_result az_amqp_encoder_begin_map(az_amqp_encoder* encoder);
AZ_NODISCARD az_result az_amqp_encoder_end_map(az_amqp_encoder* encoder);
/// @param[in] element_kind The single type every array element must use.
AZ_NODISCARD az_result
az_amqp_encoder_begin_array(az_amqp_encoder* encoder, az_amqp_value_kind element_kind);
AZ_NODISCARD az_result az_amqp_encoder_end_array(az_amqp_encoder* encoder);
/// @}

/// @name Described-type constructors
/// Appends a described-type constructor (`0x00` + descriptor) ahead of the value the next append
/// writes. Use `_ulong` for AMQP-domain numeric descriptors and `_symbol` for symbolic ones.
/// @{
AZ_NODISCARD az_result
az_amqp_encoder_append_descriptor_ulong(az_amqp_encoder* encoder, uint64_t descriptor_code);
AZ_NODISCARD az_result
az_amqp_encoder_append_descriptor_symbol(az_amqp_encoder* encoder, az_span descriptor_symbol);
/// @}

// ================================ Decoder ================================

/**
 * @brief A non-allocating cursor over a span of encoded AMQP bytes. Caller-allocated; fields are
 * library-managed.
 */
typedef struct
{
  az_span buffer; ///< The full encoded region.
  int32_t offset; ///< The cursor position within #buffer.
  int32_t array_element_fc; ///< Array-element mode: the shared constructor byte, or -1 for normal self-describing values. Library-managed.
} az_amqp_decoder;

/**
 * @brief Initializes a decoder over @p encoded.
 *
 * @return #AZ_OK on success; #AZ_ERROR_ARG if @p decoder is `NULL`.
 */
AZ_NODISCARD az_result az_amqp_decoder_init(az_amqp_decoder* decoder, az_span encoded);

/**
 * @brief Returns whether there are more bytes to decode at the cursor.
 */
AZ_NODISCARD bool az_amqp_decoder_has_next(az_amqp_decoder const* decoder);

/**
 * @brief Decodes the value at the cursor and advances past it.
 *
 * @return #AZ_OK; #AZ_ERROR_AMQP_DECODE on malformed input; #AZ_ERROR_UNEXPECTED_END if exhausted.
 */
AZ_NODISCARD az_result az_amqp_decoder_decode(az_amqp_decoder* decoder, az_amqp_value* out_value);

/// @name Scalar accessors
/// Return #AZ_ERROR_AMQP_DECODE (kind mismatch) if the value is not of the requested type.
/// @{
AZ_NODISCARD az_result az_amqp_value_get_bool(az_amqp_value const* value, bool* out);
AZ_NODISCARD az_result az_amqp_value_get_ubyte(az_amqp_value const* value, uint8_t* out);
AZ_NODISCARD az_result az_amqp_value_get_ushort(az_amqp_value const* value, uint16_t* out);
AZ_NODISCARD az_result az_amqp_value_get_uint(az_amqp_value const* value, uint32_t* out);
AZ_NODISCARD az_result az_amqp_value_get_ulong(az_amqp_value const* value, uint64_t* out);
AZ_NODISCARD az_result az_amqp_value_get_byte(az_amqp_value const* value, int8_t* out);
AZ_NODISCARD az_result az_amqp_value_get_short(az_amqp_value const* value, int16_t* out);
AZ_NODISCARD az_result az_amqp_value_get_int(az_amqp_value const* value, int32_t* out);
AZ_NODISCARD az_result az_amqp_value_get_long(az_amqp_value const* value, int64_t* out);
AZ_NODISCARD az_result az_amqp_value_get_float(az_amqp_value const* value, float* out);
AZ_NODISCARD az_result az_amqp_value_get_double(az_amqp_value const* value, double* out);
AZ_NODISCARD az_result az_amqp_value_get_timestamp(az_amqp_value const* value, int64_t* out_ms);
/// @param[out] out_uuid Receives exactly 16 bytes in big-endian order.
AZ_NODISCARD az_result az_amqp_value_get_uuid(az_amqp_value const* value, uint8_t out_uuid[16]);
/// The returned span aliases the decoder's buffer; it is not NUL-terminated.
AZ_NODISCARD az_result az_amqp_value_get_binary(az_amqp_value const* value, az_span* out);
AZ_NODISCARD az_result az_amqp_value_get_string(az_amqp_value const* value, az_span* out);
AZ_NODISCARD az_result az_amqp_value_get_symbol(az_amqp_value const* value, az_span* out);
/// @}

/**
 * @brief Obtains a child decoder over the elements of a #AZ_AMQP_VALUE_KIND_LIST value.
 *
 * @param[out] out_count __[nullable]__ Receives the number of elements.
 * @return #AZ_OK; #AZ_ERROR_AMQP_DECODE if @p value is not a list.
 */
AZ_NODISCARD az_result az_amqp_value_get_list_decoder(
    az_amqp_value const* value,
    az_amqp_decoder* out_elements,
    uint32_t* out_count);

/**
 * @brief Obtains a child decoder over the alternating keys and values of a
 * #AZ_AMQP_VALUE_KIND_MAP value.
 *
 * @param[out] out_pair_count __[nullable]__ Receives the number of key/value pairs.
 * @return #AZ_OK; #AZ_ERROR_AMQP_DECODE if @p value is not a map.
 */
AZ_NODISCARD az_result az_amqp_value_get_map_decoder(
    az_amqp_value const* value,
    az_amqp_decoder* out_pairs,
    uint32_t* out_pair_count);

/**
 * @brief Obtains a child decoder over the elements of a #AZ_AMQP_VALUE_KIND_ARRAY value.
 *
 * @param[out] out_element_kind __[nullable]__ Receives the shared element kind.
 * @param[out] out_count __[nullable]__ Receives the number of elements.
 * @return #AZ_OK; #AZ_ERROR_AMQP_DECODE if @p value is not an array.
 */
AZ_NODISCARD az_result az_amqp_value_get_array_decoder(
    az_amqp_value const* value,
    az_amqp_decoder* out_elements,
    az_amqp_value_kind* out_element_kind,
    uint32_t* out_count);

/**
 * @brief Splits a #AZ_AMQP_VALUE_KIND_DESCRIBED value into its descriptor and its described body.
 *
 * @return #AZ_OK; #AZ_ERROR_AMQP_DECODE if @p value is not a described type.
 */
AZ_NODISCARD az_result az_amqp_value_get_described(
    az_amqp_value const* value,
    az_amqp_value* out_descriptor,
    az_amqp_decoder* out_body);

#include <azure/core/_az_cfg_suffix.h>

#endif // _az_AMQP_VALUE_H
