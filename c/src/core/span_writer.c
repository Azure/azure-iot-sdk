// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <string.h>

#include "internal/span_writer.h"

/* Longest uint32_t in decimal is 4294967295: ten digits. */
#define AZ_IOT_SPAN_WRITER_DECIMAL_DIGITS_MAX 10

/* Longest uint32_t in hexadecimal is eight digits. */
#define AZ_IOT_SPAN_WRITER_HEX_DIGITS_MAX 8

static void writer_fail(az_iot_span_writer* writer, az_iot_result failure)
{
    if (writer != NULL && writer->_internal.failure == AZ_IOT_OK)
    {
        writer->_internal.failure = failure;
    }
}

/* Claims @p count bytes and returns where to write them, or NULL when the
 * writer has already failed or the bytes do not fit. Both operands of the
 * subtraction are non-negative and length never exceeds capacity, so the
 * remaining-space computation cannot overflow. */
static uint8_t* writer_reserve(az_iot_span_writer* writer, int32_t count)
{
    if (writer == NULL || writer->_internal.failure != AZ_IOT_OK)
    {
        return NULL;
    }
    if (count > writer->_internal.capacity - writer->_internal.length)
    {
        writer_fail(writer, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
        return NULL;
    }

    uint8_t* cursor = writer->_internal.buffer + writer->_internal.length;
    writer->_internal.length += count;
    return cursor;
}

void az_iot_span_writer_init(az_iot_span_writer* writer, az_span destination)
{
    if (writer == NULL) return;

    memset(writer, 0, sizeof(*writer));
    writer->_internal.buffer = az_span_ptr(destination);
    writer->_internal.capacity = az_span_size(destination);

    if (writer->_internal.buffer == NULL || writer->_internal.capacity <= 0)
    {
        writer->_internal.buffer = NULL;
        writer->_internal.capacity = 0;
        writer->_internal.failure = AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
}

void az_iot_span_writer_append_span(az_iot_span_writer* writer, az_span value)
{
    int32_t size = az_span_size(value);
    uint8_t* source = az_span_ptr(value);

    if (size < 0 || (size > 0 && source == NULL))
    {
        writer_fail(writer, AZ_IOT_ERR_INVALID_ARG);
        return;
    }
    if (size == 0) return; /* Appending nothing always succeeds. */

    uint8_t* cursor = writer_reserve(writer, size);
    if (cursor != NULL)
    {
        memcpy(cursor, source, (size_t)size);
    }
}

void az_iot_span_writer_append_str(az_iot_span_writer* writer, const char* value)
{
    if (value == NULL)
    {
        writer_fail(writer, AZ_IOT_ERR_INVALID_ARG);
        return;
    }

    /* strlen is the crossing point between the const char* MQTT interface and
     * the span world; it disappears from a call site only once the value
     * itself arrives as a span. */
    size_t length = strlen(value);
    if (length > (size_t)INT32_MAX)
    {
        writer_fail(writer, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
        return;
    }
    if (length == 0) return;

    uint8_t* cursor = writer_reserve(writer, (int32_t)length);
    if (cursor != NULL)
    {
        memcpy(cursor, value, length);
    }
}

/* RFC 3986 unreserved set. Kept as an explicit predicate rather than a lookup
 * table so the rule is readable at the point it is applied. */
static bool url_should_encode(uint8_t c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
    {
        return false;
    }
    return !(c == '-' || c == '_' || c == '.' || c == '~');
}

void az_iot_span_writer_append_url_encoded(az_iot_span_writer* writer, const char* value)
{
    static const char k_upper_hex_digits[] = "0123456789ABCDEF";

    if (value == NULL)
    {
        writer_fail(writer, AZ_IOT_ERR_INVALID_ARG);
        return;
    }

    size_t length = strlen(value);
    /* Each byte can grow to three, so refuse an input whose encoded length
     * could not be counted in the int32_t the writer measures in. */
    if (length > (size_t)(INT32_MAX / 3))
    {
        writer_fail(writer, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
        return;
    }

    const uint8_t* source = (const uint8_t*)value;
    int32_t encoded_length = 0;
    for (size_t i = 0; i < length; ++i)
    {
        encoded_length += url_should_encode(source[i]) ? 3 : 1;
    }
    if (encoded_length == 0) return;

    /* Reserving the encoded length up front keeps this all-or-nothing, unlike
     * an encoder that discovers the overflow partway through the output. */
    uint8_t* cursor = writer_reserve(writer, encoded_length);
    if (cursor == NULL) return;

    for (size_t i = 0; i < length; ++i)
    {
        uint8_t c = source[i];
        if (url_should_encode(c))
        {
            *cursor++ = (uint8_t)'%';
            *cursor++ = (uint8_t)k_upper_hex_digits[c >> 4];
            *cursor++ = (uint8_t)k_upper_hex_digits[c & 0x0Fu];
        }
        else
        {
            *cursor++ = c;
        }
    }
}

void az_iot_span_writer_append_u8(az_iot_span_writer* writer, uint8_t value)
{
    uint8_t* cursor = writer_reserve(writer, 1);
    if (cursor != NULL)
    {
        *cursor = value;
    }
}

/* Emits @p magnitude in decimal, preceded by '-' when @p negative. Digits fall
 * out least-significant first, so they are staged and then reversed into the
 * destination. Nothing is written unless the whole number fits. */
static void writer_append_decimal(
    az_iot_span_writer* writer, uint32_t magnitude, bool negative)
{
    uint8_t digits[AZ_IOT_SPAN_WRITER_DECIMAL_DIGITS_MAX];
    int32_t count = 0;

    do
    {
        digits[count++] = (uint8_t)('0' + (magnitude % 10u));
        magnitude /= 10u;
    } while (magnitude != 0u);

    uint8_t* cursor = writer_reserve(writer, count + (negative ? 1 : 0));
    if (cursor == NULL) return;

    if (negative)
    {
        *cursor++ = (uint8_t)'-';
    }
    while (count > 0)
    {
        *cursor++ = digits[--count];
    }
}

void az_iot_span_writer_append_u32(az_iot_span_writer* writer, uint32_t value)
{
    writer_append_decimal(writer, value, false);
}

void az_iot_span_writer_append_i32(az_iot_span_writer* writer, int32_t value)
{
    if (value < 0)
    {
        /* Negating INT32_MIN overflows, so build the magnitude in two steps
         * that each stay in range. */
        writer_append_decimal(writer, (uint32_t)(-(value + 1)) + 1u, true);
    }
    else
    {
        writer_append_decimal(writer, (uint32_t)value, false);
    }
}

void az_iot_span_writer_append_hex32(
    az_iot_span_writer* writer, uint32_t value, uint8_t min_digits)
{
    static const char k_hex_digits[] = "0123456789abcdef";

    int32_t width = min_digits < 1 ? 1 : min_digits;
    if (width > AZ_IOT_SPAN_WRITER_HEX_DIGITS_MAX)
    {
        width = AZ_IOT_SPAN_WRITER_HEX_DIGITS_MAX;
    }

    int32_t significant = 1;
    for (uint32_t rest = value >> 4; rest != 0u; rest >>= 4)
    {
        significant++;
    }
    if (significant > width)
    {
        width = significant;
    }

    uint8_t* cursor = writer_reserve(writer, width);
    if (cursor == NULL) return;

    for (int32_t i = width - 1; i >= 0; --i)
    {
        cursor[i] = (uint8_t)k_hex_digits[value & 0xFu];
        value >>= 4;
    }
}

az_iot_result az_iot_span_writer_end(az_iot_span_writer* writer, az_span* out_written)
{
    if (writer == NULL) return AZ_IOT_ERR_INVALID_ARG;
    if (writer->_internal.failure != AZ_IOT_OK) return writer->_internal.failure;

    if (out_written != NULL)
    {
        /* buffer is non-NULL and length is within [0, capacity] on every
         * healthy path, so az_span_create's preconditions provably hold. */
        *out_written = az_span_create(writer->_internal.buffer, writer->_internal.length);
    }
    return AZ_IOT_OK;
}

az_iot_result az_iot_span_writer_end_str(az_iot_span_writer* writer, size_t* out_length)
{
    if (writer == NULL) return AZ_IOT_ERR_INVALID_ARG;

    az_iot_result result = writer->_internal.failure;
    if (result == AZ_IOT_OK && writer->_internal.length >= writer->_internal.capacity)
    {
        result = AZ_IOT_ERR_NOT_ENOUGH_SPACE; /* Content fits, terminator does not. */
    }

    if (result != AZ_IOT_OK)
    {
        if (writer->_internal.capacity > 0)
        {
            writer->_internal.buffer[0] = (uint8_t)'\0';
        }
        writer->_internal.length = 0;
        writer->_internal.failure = result;
        return result;
    }

    writer->_internal.buffer[writer->_internal.length] = (uint8_t)'\0';
    if (out_length != NULL)
    {
        *out_length = (size_t)writer->_internal.length;
    }
    return AZ_IOT_OK;
}

az_iot_result az_iot_span_writer_build_str(
    az_span destination, size_t* out_length, const char* const* parts, size_t count)
{
    if (parts == NULL && count > 0) return AZ_IOT_ERR_INVALID_ARG;

    az_iot_span_writer writer;
    az_iot_span_writer_init(&writer, destination);
    for (size_t i = 0; i < count; ++i)
    {
        az_iot_span_writer_append_str(&writer, parts[i]);
    }
    return az_iot_span_writer_end_str(&writer, out_length);
}
