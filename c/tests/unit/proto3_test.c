// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Unit tests for the shared proto3 reader/writer.
 *
 * The twin and presence clients exercise it only along the paths their own
 * protocols take, so the boundaries live here: the widest legal varint and the
 * ones just past it, writer rollback when a field does not fit, the fixed-width
 * skips, and the malformed inputs a broker could deliver.
 *
 * Expected encodings are written as literal bytes taken from the protobuf spec
 * rather than produced by the writer, so the reader and writer cannot agree
 * with each other on something wrong. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "internal/proto3.h"

/* ------------------------------------------------------------------------- */
/* reader: varints                                                           */
/* ------------------------------------------------------------------------- */

static void a_single_byte_varint_reads(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0x7F };
  size_t pos = 0;
  uint64_t v = 0;
  assert_true(az_iot_proto3_read_varint(buf, sizeof(buf), &pos, &v));
  assert_int_equal(v, 127);
  assert_int_equal(pos, 1);
}

static void a_multi_byte_varint_reads(void** state)
{
  (void)state;
  /* 300 = 0xAC 0x02 */
  const uint8_t buf[] = { 0xAC, 0x02 };
  size_t pos = 0;
  uint64_t v = 0;
  assert_true(az_iot_proto3_read_varint(buf, sizeof(buf), &pos, &v));
  assert_int_equal(v, 300);
  assert_int_equal(pos, 2);
}

/* Nine groups cover bits 0..62, so 2^63-1 is the widest value that fits in
 * nine bytes. */
static void a_nine_byte_varint_reads(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F };
  size_t pos = 0;
  uint64_t v = 0;
  assert_true(az_iot_proto3_read_varint(buf, sizeof(buf), &pos, &v));
  assert_true(v == 0x7FFFFFFFFFFFFFFFull);
}

/* Ten bytes is the widest legal uint64: the tenth contributes bit 63 alone. */
static void a_ten_byte_varint_reads_uint64_max(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
  size_t pos = 0;
  uint64_t v = 0;
  assert_true(az_iot_proto3_read_varint(buf, sizeof(buf), &pos, &v));
  assert_true(v == UINT64_MAX);
  assert_int_equal(pos, 10);
}

static void a_ten_byte_varint_reads_bit_63(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 };
  size_t pos = 0;
  uint64_t v = 0;
  assert_true(az_iot_proto3_read_varint(buf, sizeof(buf), &pos, &v));
  assert_true(v == (uint64_t)1 << 63);
}

/* The tenth byte can carry only bit 63, so a payload above 1 sets bits the
 * value cannot hold. Accepting it would silently wrap and pass a corrupt
 * version off as a valid one. */
static void a_tenth_byte_that_overflows_is_rejected(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x03 };
  size_t pos = 0;
  uint64_t v = 0;
  assert_false(az_iot_proto3_read_varint(buf, sizeof(buf), &pos, &v));
}

static void an_eleven_byte_varint_is_rejected(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };
  size_t pos = 0;
  uint64_t v = 0;
  assert_false(az_iot_proto3_read_varint(buf, sizeof(buf), &pos, &v));
}

/* A continuation bit at the end of the buffer has no byte to continue into. */
static void a_truncated_varint_is_rejected(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0xAC };
  size_t pos = 0;
  uint64_t v = 0;
  assert_false(az_iot_proto3_read_varint(buf, sizeof(buf), &pos, &v));
}

static void reading_a_varint_from_an_empty_buffer_is_rejected(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0x00 };
  size_t pos = 0;
  uint64_t v = 0;
  assert_false(az_iot_proto3_read_varint(buf, 0, &pos, &v));
}

static void read_varint_rejects_null_arguments(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0x01 };
  size_t pos = 0;
  uint64_t v = 0;
  assert_false(az_iot_proto3_read_varint(NULL, sizeof(buf), &pos, &v));
  assert_false(az_iot_proto3_read_varint(buf, sizeof(buf), NULL, &v));
  assert_false(az_iot_proto3_read_varint(buf, sizeof(buf), &pos, NULL));
}

/* ------------------------------------------------------------------------- */
/* reader: tags                                                              */
/* ------------------------------------------------------------------------- */

static void a_tag_splits_into_field_and_wire_type(void** state)
{
  (void)state;
  /* field 10, wire 2 -> (10 << 3) | 2 = 0x52 */
  const uint8_t buf[] = { 0x52 };
  size_t pos = 0;
  uint32_t field = 0;
  uint8_t wire = 0;
  assert_true(az_iot_proto3_read_tag(buf, sizeof(buf), &pos, &field, &wire));
  assert_int_equal(field, 10);
  assert_int_equal(wire, AZ_IOT_PROTO3_WIRE_LEN);
}

/* A field number above 15 needs a two-byte key, which must still split. */
static void a_two_byte_tag_splits(void** state)
{
  (void)state;
  /* field 300, wire 0 -> 300 << 3 = 2400 = 0xE0 0x12 */
  const uint8_t buf[] = { 0xE0, 0x12 };
  size_t pos = 0;
  uint32_t field = 0;
  uint8_t wire = 0;
  assert_true(az_iot_proto3_read_tag(buf, sizeof(buf), &pos, &field, &wire));
  assert_int_equal(field, 300);
  assert_int_equal(wire, AZ_IOT_PROTO3_WIRE_VARINT);
}

/* Field number 0 does not exist in protobuf, so a key carrying it means the
 * frame is malformed -- not that the field is one this SDK does not know.
 * Skipping it as unknown would let a corrupt message parse as a valid one. */
static void field_number_zero_is_rejected(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0x00 };
  size_t pos = 0;
  uint32_t field = 0;
  uint8_t wire = 0;
  assert_false(az_iot_proto3_read_tag(buf, sizeof(buf), &pos, &field, &wire));
}

static void read_tag_rejects_null_outputs(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0x52 };
  size_t pos = 0;
  uint32_t field = 0;
  uint8_t wire = 0;
  assert_false(az_iot_proto3_read_tag(buf, sizeof(buf), &pos, NULL, &wire));
  assert_false(az_iot_proto3_read_tag(buf, sizeof(buf), &pos, &field, NULL));
}

/* ------------------------------------------------------------------------- */
/* reader: length-delimited                                                  */
/* ------------------------------------------------------------------------- */

static void a_length_delimited_field_points_into_the_buffer(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0x03, 'a', 'b', 'c' };
  size_t pos = 0;
  const uint8_t* bytes = NULL;
  size_t len = 0;
  assert_true(az_iot_proto3_read_bytes(buf, sizeof(buf), &pos, &bytes, &len));
  assert_int_equal(len, 3);
  /* No copy: it points at the source bytes. */
  assert_ptr_equal(bytes, buf + 1);
  assert_int_equal(pos, 4);
}

/* An explicitly-present but empty field is legal, and is reported as a NULL
 * pointer with zero length rather than as a failure. */
static void a_zero_length_field_reads(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0x00 };
  size_t pos = 0;
  const uint8_t* bytes = (const uint8_t*)1;
  size_t len = 99;
  assert_true(az_iot_proto3_read_bytes(buf, sizeof(buf), &pos, &bytes, &len));
  assert_null(bytes);
  assert_int_equal(len, 0);
}

/* A length past the end of the buffer must be refused, not clamped: clamping
 * would hand the caller a payload the sender never sent. */
static void a_length_past_the_end_is_rejected(void** state)
{
  (void)state;
  const uint8_t buf[] = { 0x08, 'a', 'b' };
  size_t pos = 0;
  const uint8_t* bytes = NULL;
  size_t len = 0;
  assert_false(az_iot_proto3_read_bytes(buf, sizeof(buf), &pos, &bytes, &len));
}

/* ------------------------------------------------------------------------- */
/* reader: skipping                                                          */
/* ------------------------------------------------------------------------- */

static void skipping_each_wire_type_advances_correctly(void** state)
{
  (void)state;
  const uint8_t varint[] = { 0xAC, 0x02 };
  const uint8_t len_delim[] = { 0x02, 'x', 'y' };
  const uint8_t fixed32[] = { 1, 2, 3, 4 };
  const uint8_t fixed64[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  size_t pos;

  pos = 0;
  assert_true(
      az_iot_proto3_skip_field(varint, sizeof(varint), &pos, AZ_IOT_PROTO3_WIRE_VARINT));
  assert_int_equal(pos, 2);

  pos = 0;
  assert_true(
      az_iot_proto3_skip_field(len_delim, sizeof(len_delim), &pos, AZ_IOT_PROTO3_WIRE_LEN));
  assert_int_equal(pos, 3);

  pos = 0;
  assert_true(
      az_iot_proto3_skip_field(fixed32, sizeof(fixed32), &pos, AZ_IOT_PROTO3_WIRE_32BIT));
  assert_int_equal(pos, 4);

  pos = 0;
  assert_true(
      az_iot_proto3_skip_field(fixed64, sizeof(fixed64), &pos, AZ_IOT_PROTO3_WIRE_64BIT));
  assert_int_equal(pos, 8);
}

static void skipping_a_truncated_fixed_width_field_is_rejected(void** state)
{
  (void)state;
  const uint8_t three[] = { 1, 2, 3 };
  size_t pos = 0;
  assert_false(az_iot_proto3_skip_field(three, sizeof(three), &pos, AZ_IOT_PROTO3_WIRE_32BIT));
  pos = 0;
  assert_false(az_iot_proto3_skip_field(three, sizeof(three), &pos, AZ_IOT_PROTO3_WIRE_64BIT));
}

/* Groups (wire types 3 and 4) were removed in proto3, and anything above 5 is
 * not a wire type at all. */
static void skipping_a_group_or_unknown_wire_type_is_rejected(void** state)
{
  (void)state;
  const uint8_t buf[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  size_t pos = 0;
  assert_false(az_iot_proto3_skip_field(buf, sizeof(buf), &pos, 3));
  assert_false(az_iot_proto3_skip_field(buf, sizeof(buf), &pos, 4));
  assert_false(az_iot_proto3_skip_field(buf, sizeof(buf), &pos, 6));
  assert_false(az_iot_proto3_skip_field(buf, sizeof(buf), &pos, 7));
}

/* ------------------------------------------------------------------------- */
/* writer                                                                    */
/* ------------------------------------------------------------------------- */

static void a_varint_field_encodes_to_the_spec_bytes(void** state)
{
  (void)state;
  uint8_t buf[16];
  size_t pos = 0;
  assert_true(az_iot_proto3_write_varint_field(buf, sizeof(buf), &pos, 1, 300));
  /* key = (1 << 3) | 0 = 0x08, then 300 = 0xAC 0x02 */
  const uint8_t expect[] = { 0x08, 0xAC, 0x02 };
  assert_int_equal(pos, sizeof(expect));
  assert_memory_equal(buf, expect, sizeof(expect));
}

static void a_bytes_field_encodes_to_the_spec_bytes(void** state)
{
  (void)state;
  uint8_t buf[16];
  size_t pos = 0;
  assert_true(az_iot_proto3_write_bytes_field(buf, sizeof(buf), &pos, 2, (const uint8_t*)"hi", 2));
  /* key = (2 << 3) | 2 = 0x12, length 2, then the bytes */
  const uint8_t expect[] = { 0x12, 0x02, 'h', 'i' };
  assert_int_equal(pos, sizeof(expect));
  assert_memory_equal(buf, expect, sizeof(expect));
}

/* A zero-length bytes field is still emitted: that is how an explicitly present
 * but empty `optional bytes` is encoded, and the service distinguishes it from
 * an absent one. */
static void a_zero_length_bytes_field_is_still_emitted(void** state)
{
  (void)state;
  uint8_t buf[16];
  size_t pos = 0;
  assert_true(az_iot_proto3_write_bytes_field(buf, sizeof(buf), &pos, 2, NULL, 0));
  const uint8_t expect[] = { 0x12, 0x00 };
  assert_int_equal(pos, sizeof(expect));
  assert_memory_equal(buf, expect, sizeof(expect));
}

/* On overflow the cursor must not move: a caller that reports "too small" would
 * otherwise have already emitted a key with no value after it, and the partial
 * field would be sent as if it were whole. */
static void a_varint_field_that_does_not_fit_leaves_the_cursor_alone(void** state)
{
  (void)state;
  uint8_t buf[2];
  memset(buf, 0xEE, sizeof(buf));
  size_t pos = 0;
  /* The key fits, the value does not. */
  assert_false(az_iot_proto3_write_varint_field(buf, sizeof(buf), &pos, 1, 300));
  assert_int_equal(pos, 0);
}

static void a_bytes_field_that_does_not_fit_leaves_the_cursor_alone(void** state)
{
  (void)state;
  uint8_t buf[4];
  size_t pos = 0;
  /* Key and length fit; the payload does not. */
  assert_false(
      az_iot_proto3_write_bytes_field(buf, sizeof(buf), &pos, 2, (const uint8_t*)"abcd", 4));
  assert_int_equal(pos, 0);
}

/* A field written after an earlier one starts where that one ended, and a
 * failure part-way through still leaves everything written before it intact. */
static void a_failed_write_preserves_the_fields_before_it(void** state)
{
  (void)state;
  uint8_t buf[5];
  size_t pos = 0;
  assert_true(az_iot_proto3_write_varint_field(buf, sizeof(buf), &pos, 1, 1));
  assert_int_equal(pos, 2);

  assert_false(
      az_iot_proto3_write_bytes_field(buf, sizeof(buf), &pos, 2, (const uint8_t*)"abcd", 4));
  assert_int_equal(pos, 2);

  const uint8_t expect[] = { 0x08, 0x01 };
  assert_memory_equal(buf, expect, sizeof(expect));
}

static void writing_uint64_max_takes_ten_bytes(void** state)
{
  (void)state;
  uint8_t buf[16];
  size_t pos = 0;
  assert_true(az_iot_proto3_write_varint_field(buf, sizeof(buf), &pos, 1, UINT64_MAX));
  assert_int_equal(pos, 11); /* one key byte + ten value bytes */

  /* And it round-trips through the reader. */
  size_t rpos = 1;
  uint64_t v = 0;
  assert_true(az_iot_proto3_read_varint(buf, pos, &rpos, &v));
  assert_true(v == UINT64_MAX);
}

static void write_rejects_null_arguments(void** state)
{
  (void)state;
  uint8_t buf[8];
  size_t pos = 0;
  assert_false(az_iot_proto3_write_varint_field(NULL, sizeof(buf), &pos, 1, 1));
  assert_false(az_iot_proto3_write_varint_field(buf, sizeof(buf), NULL, 1, 1));
  assert_false(az_iot_proto3_write_bytes_field(buf, sizeof(buf), &pos, 2, NULL, 4));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(a_single_byte_varint_reads),
    cmocka_unit_test(a_multi_byte_varint_reads),
    cmocka_unit_test(a_nine_byte_varint_reads),
    cmocka_unit_test(a_ten_byte_varint_reads_uint64_max),
    cmocka_unit_test(a_ten_byte_varint_reads_bit_63),
    cmocka_unit_test(a_tenth_byte_that_overflows_is_rejected),
    cmocka_unit_test(an_eleven_byte_varint_is_rejected),
    cmocka_unit_test(a_truncated_varint_is_rejected),
    cmocka_unit_test(reading_a_varint_from_an_empty_buffer_is_rejected),
    cmocka_unit_test(read_varint_rejects_null_arguments),
    cmocka_unit_test(a_tag_splits_into_field_and_wire_type),
    cmocka_unit_test(a_two_byte_tag_splits),
    cmocka_unit_test(field_number_zero_is_rejected),
    cmocka_unit_test(read_tag_rejects_null_outputs),
    cmocka_unit_test(a_length_delimited_field_points_into_the_buffer),
    cmocka_unit_test(a_zero_length_field_reads),
    cmocka_unit_test(a_length_past_the_end_is_rejected),
    cmocka_unit_test(skipping_each_wire_type_advances_correctly),
    cmocka_unit_test(skipping_a_truncated_fixed_width_field_is_rejected),
    cmocka_unit_test(skipping_a_group_or_unknown_wire_type_is_rejected),
    cmocka_unit_test(a_varint_field_encodes_to_the_spec_bytes),
    cmocka_unit_test(a_bytes_field_encodes_to_the_spec_bytes),
    cmocka_unit_test(a_zero_length_bytes_field_is_still_emitted),
    cmocka_unit_test(a_varint_field_that_does_not_fit_leaves_the_cursor_alone),
    cmocka_unit_test(a_bytes_field_that_does_not_fit_leaves_the_cursor_alone),
    cmocka_unit_test(a_failed_write_preserves_the_fields_before_it),
    cmocka_unit_test(writing_uint64_max_takes_ten_bytes),
    cmocka_unit_test(write_rejects_null_arguments),
  };
  return cmocka_run_group_tests_name("proto3", tests, NULL, NULL);
}
