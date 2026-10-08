// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Bounded span writer: the az_span-native stand-in for snprintf.
 *
 * Why not az_span_copy()?
 *   az_span_copy(), az_span_copy_u8() and az_span_slice() validate their
 *   arguments through az_core preconditions. This project builds with
 *   AZ_NO_PRECONDITION_CHECKING=OFF and installs no handler, and az_core's
 *   default handler spins forever. A copy that does not fit therefore hangs
 *   the calling thread instead of reporting an error. Everything below
 *   bounds-checks first and reaches az_span only through az_span_ptr() and
 *   az_span_size(), which have no preconditions, so no input can hang.
 *
 * Why not snprintf()?
 *   snprintf() re-parses a format string on every call, pulls the C library's
 *   general formatting machinery (locale handling, and on many libc builds
 *   floating point) into the image, and is undefined when an argument for a
 *   string conversion is NULL. Conversions here are resolved when the code is
 *   compiled, integers are formatted directly, and a NULL string is reported
 *   as AZ_IOT_ERR_INVALID_ARG.
 *
 * The append functions do not report failures individually. The first failure
 * is latched and every later append becomes a no-op, so a multi-part build
 * needs one check at the end rather than one per part:
 *
 *     char topic[AZ_IOT_TELEMETRY_TOPIC_MAX];
 *     az_iot_span_writer writer;
 *     az_iot_span_writer_init(&writer, AZ_SPAN_FROM_BUFFER(topic));
 *     az_iot_span_writer_append_str(&writer, "ih/");
 *     az_iot_span_writer_append_str(&writer, device_id);
 *     az_iot_span_writer_append_str(&writer, "/srv/telemetry");
 *     az_iot_result result = az_iot_span_writer_end_str(&writer, NULL);
 *     if (result != AZ_IOT_OK) return result;
 */
#ifndef AZ_IOT_SPAN_WRITER_H
#define AZ_IOT_SPAN_WRITER_H

#include <stddef.h>
#include <stdint.h>

#include <azure/az_core.h>

#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct az_iot_span_writer
  {
    struct
    {
      uint8_t* buffer;
      int32_t capacity;
      int32_t length;
      /* First failure seen, latched. AZ_IOT_OK while the build is healthy. */
      az_iot_result failure;
    } _internal;
  } az_iot_span_writer;

  /* Binds the writer to @p destination and resets it. A NULL or empty
   * destination is accepted and latches AZ_IOT_ERR_NOT_ENOUGH_SPACE, so callers
   * can init and append unconditionally and check once at the end. */
  void az_iot_span_writer_init(az_iot_span_writer* writer, az_span destination);

  void az_iot_span_writer_append_span(az_iot_span_writer* writer, az_span value);

  /* Appends a NUL-terminated string, excluding its terminator. A NULL @p value
   * latches AZ_IOT_ERR_INVALID_ARG instead of invoking the undefined behaviour a
   * string conversion with a NULL argument has in printf. */
  void az_iot_span_writer_append_str(az_iot_span_writer* writer, const char* value);

  /* Appends a NUL-terminated string percent-encoded per RFC 3986: unreserved
   * bytes (A-Z, a-z, 0-9, '-', '_', '.', '~') pass through and everything else
   * becomes a percent sign followed by two uppercase hexadecimal digits. This is
   * the encoding the IoT Hub topic property bag requires, and it matches what
   * az_core applies to HTTP query parameters. */
  void az_iot_span_writer_append_url_encoded(az_iot_span_writer* writer, const char* value);

  /* Appends @p length bytes of @p value with percent-escapes decoded -- the
   * inverse of az_iot_span_writer_append_url_encoded(), used to turn an inbound
   * topic property bag back into the plain text the caller passed on the way
   * out. Takes an explicit length because the source is a slice of a topic
   * rather than a NUL-terminated string.
   *
   * A malformed escape (truncated, or not two hexadecimal digits) latches
   * AZ_IOT_ERR_PROTOCOL. Decoding never grows the input, so this cannot overflow
   * a destination that already holds the encoded form. */
  void az_iot_span_writer_append_url_decoded(
      az_iot_span_writer* writer,
      const char* value,
      size_t length);

  void az_iot_span_writer_append_u8(az_iot_span_writer* writer, uint8_t value);

  /* Bytes written so far. Useful when several NUL-terminated strings are built
   * back to back into one buffer and the caller needs to remember where each
   * one started. Returns the length even after a failure has latched, so an
   * offset recorded before an append stays meaningful. */
  size_t az_iot_span_writer_length(const az_iot_span_writer* writer);

  /* Decimal, unpadded, no locale involvement. */
  void az_iot_span_writer_append_u32(az_iot_span_writer* writer, uint32_t value);
  void az_iot_span_writer_append_i32(az_iot_span_writer* writer, int32_t value);

  /** @brief Decimal, zero-padded to @p min_digits (clamped to 1..10) and widened
   * past it when the value needs more digits. */
  void az_iot_span_writer_append_u32_padded(
      az_iot_span_writer* writer,
      uint32_t value,
      uint8_t min_digits);

  /* Lowercase hexadecimal, zero-padded to @p min_digits (clamped to 1..8) and
   * widened past it when the value needs more digits. */
  void az_iot_span_writer_append_hex32(
      az_iot_span_writer* writer,
      uint32_t value,
      uint8_t min_digits);

  /* Completes a build that needs no terminator, such as a message body.
   * @p out_written may be NULL; otherwise it receives a span over the bytes
   * produced. Returns the latched failure when there is one. */
  AZ_NODISCARD az_iot_result
  az_iot_span_writer_end(az_iot_span_writer* writer, az_span* out_written);

  /* Completes a build destined for the const char* MQTT interface: writes the
   * NUL terminator, which the bound capacity must have room for beyond the
   * content, and reports the length excluding it through @p out_length (which
   * may be NULL). On failure the destination is left holding an empty string
   * whenever it has room for one, so a caller that ignores the result cannot go
   * on to publish a half-built topic. */
  AZ_NODISCARD az_iot_result
  az_iot_span_writer_end_str(az_iot_span_writer* writer, size_t* out_length);

  /* Concatenates @p count NUL-terminated @p parts into @p destination and
   * terminates the result, reporting the length excluding the terminator through
   * @p out_length (which may be NULL). This is the whole writer sequence for the
   * shape that dominates this library, a fixed prefix around a device id, so
   * that those call sites stay one statement without giving up bounds checking:
   *
   *     const char* parts[] = { "ih/", device_id, "/dev/methods" };
   *     az_iot_result result = az_iot_span_writer_build_str(
   *         AZ_SPAN_FROM_BUFFER(topic), NULL, parts, 3);
   */
  AZ_NODISCARD az_iot_result az_iot_span_writer_build_str(
      az_span destination,
      size_t* out_length,
      const char* const* parts,
      size_t count);

#ifdef __cplusplus
}
#endif

/* An optional C string a caller may leave unset either way: NULL and "" both
 * mean "not supplied". Stated positively so call sites read as "this was
 * supplied" rather than a negated absence; the parameter is parenthesised so an
 * expression argument cannot misparse.
 *
 * The Paho adapter carries its own copy: adapters may only include public
 * azure/iot headers, so they cannot reach this one. */
#define is_nonempty_cstr(s) ((s) != NULL && (s)[0] != '\0')

#endif /* AZ_IOT_SPAN_WRITER_H */
