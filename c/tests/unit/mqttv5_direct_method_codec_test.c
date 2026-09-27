// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Wire-format tests for the hand-rolled proto3 codec behind the AEG direct
 * method phases (common/Protos/directmethods.proto).
 *
 * The expected bytes below are written out from the .proto by hand -- field
 * number, wire type, then the value -- rather than captured from this codec's
 * own output, so a change in what it emits shows up as a failure instead of
 * being ratified. A generated codec would not need this; a hand-rolled one is
 * only as trustworthy as the frames it is pinned against.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_result.h"

#include "internal/direct_method_codec.h"

/* ------------------------------------------------------------------------- */
/* Probe (service -> device)                                                 */
/* ------------------------------------------------------------------------- */

static void probe_decodes_both_fields(void** state)
{
  (void)state;

  /* Probe { method_name = "reboot", response_timeout_seconds = 300 }
   *   0x0A len=6 "reboot"      f1, wire type 2
   *   0x10 0xAC 0x02           f2, wire type 0, varint 300 */
  static const uint8_t wire[] = { 0x0A, 0x06, 'r', 'e', 'b', 'o', 'o', 't', 0x10, 0xAC, 0x02 };

  az_iot_dm_proto_probe probe;
  assert_int_equal(az_iot_dm_proto_decode_probe(wire, sizeof(wire), &probe), AZ_IOT_OK);
  assert_int_equal(probe.method_name_len, 6);
  assert_memory_equal(probe.method_name, "reboot", 6);
  assert_int_equal(probe.response_timeout_seconds, 300);
}

static void probe_with_omitted_fields_decodes_to_defaults(void** state)
{
  (void)state;

  /* proto3 drops default-valued fields, so a probe for a nameless method with
   * no timeout is a zero-length message rather than a malformed one. */
  az_iot_dm_proto_probe probe;
  assert_int_equal(az_iot_dm_proto_decode_probe(NULL, 0, &probe), AZ_IOT_OK);
  assert_int_equal(probe.method_name_len, 0);
  assert_int_equal(probe.response_timeout_seconds, 0);
}

static void probe_skips_a_field_it_does_not_know(void** state)
{
  (void)state;

  /* A later revision adding a field must not stop this client from reading the
   * ones it does understand -- otherwise the protocol could never be extended
   * without breaking every deployed device. One of each wire type. */
  static const uint8_t wire[] = {
    0x0A, 0x04, 'p',  'i',  'n',  'g', /* f1  method_name */
    0x28, 0x7F, /* f5  varint */
    0x31, 1,    2,    3,    4,    5,   6, 7, 8, /* f6  64-bit */
    0x3A, 0x03, 0xAA, 0xBB, 0xCC, /* f7  length-delimited */
    0x45, 9,    8,    7,    6, /* f8  32-bit */
    0x10, 0x1E /* f2  response_timeout_seconds = 30 */
  };

  az_iot_dm_proto_probe probe;
  assert_int_equal(az_iot_dm_proto_decode_probe(wire, sizeof(wire), &probe), AZ_IOT_OK);
  assert_int_equal(probe.method_name_len, 4);
  assert_memory_equal(probe.method_name, "ping", 4);
  assert_int_equal(probe.response_timeout_seconds, 30);
}

static void probe_with_a_length_past_the_buffer_is_rejected(void** state)
{
  (void)state;

  /* Claims a 32-byte name inside an 8-byte message. Trusting the length would
   * read past the MQTT payload. */
  static const uint8_t wire[] = { 0x0A, 0x20, 'r', 'e', 'b', 'o', 'o', 't' };

  az_iot_dm_proto_probe probe;
  assert_int_equal(az_iot_dm_proto_decode_probe(wire, sizeof(wire), &probe), AZ_IOT_ERR_PROTOCOL);
}

static void probe_with_a_truncated_varint_is_rejected(void** state)
{
  (void)state;

  /* Every byte has the continuation bit set and the message ends. */
  static const uint8_t wire[] = { 0x10, 0xAC };

  az_iot_dm_proto_probe probe;
  assert_int_equal(az_iot_dm_proto_decode_probe(wire, sizeof(wire), &probe), AZ_IOT_ERR_PROTOCOL);
}

static void probe_with_an_unterminated_varint_run_is_rejected(void** state)
{
  (void)state;

  /* Eleven continuation bytes: past the ten a 64-bit varint can occupy, so the
   * decoder must stop rather than keep shifting. */
  static const uint8_t wire[]
      = { 0x10, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01 };

  az_iot_dm_proto_probe probe;
  assert_int_equal(az_iot_dm_proto_decode_probe(wire, sizeof(wire), &probe), AZ_IOT_ERR_PROTOCOL);
}

static void probe_with_a_group_wire_type_is_rejected(void** state)
{
  (void)state;

  /* Wire types 3 and 4 are the deprecated group encoding, which proto3 cannot
   * produce; skipping them is impossible without knowing the group's shape. */
  static const uint8_t wire[] = { 0x2B, 0x00 };

  az_iot_dm_proto_probe probe;
  assert_int_equal(az_iot_dm_proto_decode_probe(wire, sizeof(wire), &probe), AZ_IOT_ERR_PROTOCOL);
}

static void probe_with_a_zero_field_number_is_rejected(void** state)
{
  (void)state;

  /* Field number 0 is not assignable, so this is corruption rather than an
   * unknown field to step over. */
  static const uint8_t wire[] = { 0x00, 0x01 };

  az_iot_dm_proto_probe probe;
  assert_int_equal(az_iot_dm_proto_decode_probe(wire, sizeof(wire), &probe), AZ_IOT_ERR_PROTOCOL);
}

static void probe_with_an_oversized_timeout_is_rejected(void** state)
{
  (void)state;

  /* response_timeout_seconds is uint32; a value that does not fit is a
   * misencoding, not a very long timeout. */
  static const uint8_t wire[] = { 0x10, 0x80, 0x80, 0x80, 0x80, 0x20 };

  az_iot_dm_proto_probe probe;
  assert_int_equal(az_iot_dm_proto_decode_probe(wire, sizeof(wire), &probe), AZ_IOT_ERR_PROTOCOL);
}

/* ------------------------------------------------------------------------- */
/* Exec (service -> device)                                                  */
/* ------------------------------------------------------------------------- */

static void exec_decodes_ready_id_and_params(void** state)
{
  (void)state;

  static const uint8_t wire[] = {
    0x0A, 0x10, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, /* f1 ready_id */
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x12, 0x07,
    '{',  '"',  'x',  '"',  ':',  '1',  '}' /* f2 params */
  };

  az_iot_dm_proto_exec exec;
  assert_int_equal(az_iot_dm_proto_decode_exec(wire, sizeof(wire), &exec), AZ_IOT_OK);
  assert_int_equal(exec.ready_id_len, 16);
  assert_int_equal(exec.ready_id[15], 0x0F);
  assert_int_equal(exec.params_len, 7);
  assert_memory_equal(exec.params, "{\"x\":1}", 7);
}

static void exec_skips_the_exec_start_timestamp(void** state)
{
  (void)state;

  /* exec_start is observability-only and the protocol forbids deriving a
   * deadline from it, so it is stepped over like any other unread field --
   * its presence must not disturb the fields that are read. */
  static const uint8_t wire[] = {
    0x0A, 0x02, 0xAA, 0xBB, /* f1 ready_id */
    0x1A, 0x06, 0x08, 0xC0, 0x84, 0x3D, 0x10, 0x80, /* f3 Timestamp submessage */
    0x12, 0x02, 'h',  'i' /* f2 params */
  };

  az_iot_dm_proto_exec exec;
  assert_int_equal(az_iot_dm_proto_decode_exec(wire, sizeof(wire), &exec), AZ_IOT_OK);
  assert_int_equal(exec.ready_id_len, 2);
  assert_int_equal(exec.params_len, 2);
  assert_memory_equal(exec.params, "hi", 2);
}

static void exec_with_an_empty_params_field_decodes(void** state)
{
  (void)state;

  /* A method that takes no arguments still gets an exec; params is simply
   * absent, and that must not read as a decode failure. */
  static const uint8_t wire[] = { 0x0A, 0x02, 0xAA, 0xBB };

  az_iot_dm_proto_exec exec;
  assert_int_equal(az_iot_dm_proto_decode_exec(wire, sizeof(wire), &exec), AZ_IOT_OK);
  assert_int_equal(exec.ready_id_len, 2);
  assert_int_equal(exec.params_len, 0);
  assert_null(exec.params);
}

/* ------------------------------------------------------------------------- */
/* ProbeAck / Abandon / Result (device -> service)                           */
/* ------------------------------------------------------------------------- */

static void probe_ack_ready_encodes_the_nested_ready_message(void** state)
{
  (void)state;

  static const uint8_t ready_id[16] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F };
  /* ProbeAck { ready: Ready { ready_id } }
   *   0x0A len=18   f1 of ProbeAck, the Ready submessage
   *     0x0A len=16 f1 of Ready, the id */
  static const uint8_t expected[] = { 0x0A, 0x12, 0x0A, 0x10, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
                                      0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F };

  uint8_t frame[AZ_IOT_DM_PROTO_PROBE_ACK_MAX];
  size_t len = 0;
  assert_int_equal(
      az_iot_dm_proto_encode_probe_ack_ready(frame, sizeof(frame), ready_id, 16, &len), AZ_IOT_OK);
  assert_int_equal(len, sizeof(expected));
  assert_memory_equal(frame, expected, sizeof(expected));
}

static void probe_ack_rejected_encodes_the_reason(void** state)
{
  (void)state;

  /* ProbeAck { rejected: Rejected { reason: DEVICE_BUSY } }
   *   0x12 len=2    f2 of ProbeAck, the Rejected submessage
   *     0x08 0x03   f1 of Rejected, varint 3 */
  static const uint8_t expected[] = { 0x12, 0x02, 0x08, 0x03 };

  uint8_t frame[AZ_IOT_DM_PROTO_PROBE_ACK_MAX];
  size_t len = 0;
  assert_int_equal(
      az_iot_dm_proto_encode_probe_ack_rejected(
          frame, sizeof(frame), AZ_IOT_DM_PROTO_REJECTED_DEVICE_BUSY, &len),
      AZ_IOT_OK);
  assert_int_equal(len, sizeof(expected));
  assert_memory_equal(frame, expected, sizeof(expected));
}

static void probe_ack_rejected_keeps_the_oneof_case_for_the_default_reason(void** state)
{
  (void)state;

  /* proto3 omits a default-valued scalar, so Rejected{UNSPECIFIED} is empty --
   * but the oneof arm still has to be on the wire, or the service reads an
   * unset ProbeAck instead of a rejection. */
  static const uint8_t expected[] = { 0x12, 0x00 };

  uint8_t frame[AZ_IOT_DM_PROTO_PROBE_ACK_MAX];
  size_t len = 0;
  assert_int_equal(
      az_iot_dm_proto_encode_probe_ack_rejected(
          frame, sizeof(frame), AZ_IOT_DM_PROTO_REJECTED_UNSPECIFIED, &len),
      AZ_IOT_OK);
  assert_int_equal(len, sizeof(expected));
  assert_memory_equal(frame, expected, sizeof(expected));
}

static void abandon_encodes_the_ready_id_and_reason(void** state)
{
  (void)state;

  static const uint8_t ready_id[16] = { 0xF0 };
  uint8_t expected[20] = { 0x0A, 0x10 };
  memcpy(expected + 2, ready_id, 16);
  expected[18] = 0x10; /* f2, varint */
  expected[19] = 0x01; /* READY_WAIT_TIMEOUT */

  uint8_t frame[AZ_IOT_DM_PROTO_ABANDON_MAX];
  size_t len = 0;
  assert_int_equal(
      az_iot_dm_proto_encode_abandon(
          frame, sizeof(frame), ready_id, 16, AZ_IOT_DM_PROTO_ABANDON_READY_WAIT_TIMEOUT, &len),
      AZ_IOT_OK);
  assert_int_equal(len, sizeof(expected));
  assert_memory_equal(frame, expected, sizeof(expected));
}

static void abandon_omits_a_default_reason(void** state)
{
  (void)state;

  static const uint8_t ready_id[16] = { 0xF0 };
  uint8_t frame[AZ_IOT_DM_PROTO_ABANDON_MAX];
  size_t len = 0;
  assert_int_equal(
      az_iot_dm_proto_encode_abandon(
          frame, sizeof(frame), ready_id, 16, AZ_IOT_DM_PROTO_ABANDON_UNSPECIFIED, &len),
      AZ_IOT_OK);
  /* Only the ready_id field: tag, length, sixteen bytes. */
  assert_int_equal(len, 18);
  assert_int_equal(frame[0], 0x0A);
  assert_int_equal(frame[1], 0x10);
}

static void result_encodes_status_and_body(void** state)
{
  (void)state;

  /* Result { status = 200, body = "ok" }
   *   0x08 0xC8 0x01   f1, varint 200
   *   0x12 0x02 "ok"   f2, length-delimited */
  static const uint8_t expected[] = { 0x08, 0xC8, 0x01, 0x12, 0x02, 'o', 'k' };

  uint8_t frame[64];
  size_t len = 0;
  assert_int_equal(
      az_iot_dm_proto_encode_result(frame, sizeof(frame), 200, (const uint8_t*)"ok", 2, &len),
      AZ_IOT_OK);
  assert_int_equal(len, sizeof(expected));
  assert_memory_equal(frame, expected, sizeof(expected));
}

static void result_sign_extends_a_negative_status(void** state)
{
  (void)state;

  /* proto3 encodes a negative int32 as a ten-byte two's-complement varint of
   * the widened value. Truncating it to five bytes would decode as a large
   * positive number on the service. */
  static const uint8_t expected[]
      = { 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01 };

  uint8_t frame[64];
  size_t len = 0;
  assert_int_equal(
      az_iot_dm_proto_encode_result(frame, sizeof(frame), -1, NULL, 0, &len), AZ_IOT_OK);
  assert_int_equal(len, sizeof(expected));
  assert_memory_equal(frame, expected, sizeof(expected));
}

static void result_omits_a_zero_status_and_an_empty_body(void** state)
{
  (void)state;

  uint8_t frame[64];
  size_t len = 0;
  assert_int_equal(
      az_iot_dm_proto_encode_result(frame, sizeof(frame), 0, NULL, 0, &len), AZ_IOT_OK);
  assert_int_equal(len, 0);
}

static void result_reports_a_body_that_does_not_fit(void** state)
{
  (void)state;

  /* The frame is built into a fixed buffer, so an oversized body has to be
   * refused rather than silently truncated into a message the service would
   * decode as a shorter one. */
  static const uint8_t body[32] = { 0 };
  uint8_t frame[8];
  size_t len = 0;
  assert_int_equal(
      az_iot_dm_proto_encode_result(frame, sizeof(frame), 200, body, sizeof(body), &len),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

static void encoders_reject_a_missing_destination(void** state)
{
  (void)state;

  static const uint8_t ready_id[16] = { 0 };
  size_t len = 0;
  uint8_t frame[32] = { 0 };
  assert_int_equal(
      az_iot_dm_proto_encode_probe_ack_ready(NULL, 32, ready_id, 16, &len), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_dm_proto_encode_abandon(
          frame, sizeof(frame), NULL, 16, AZ_IOT_DM_PROTO_ABANDON_UNSPECIFIED, &len),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_dm_proto_encode_result(frame, sizeof(frame), 200, NULL, 4, &len),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_dm_proto_decode_probe(frame, sizeof(frame), NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_dm_proto_decode_exec(frame, sizeof(frame), NULL), AZ_IOT_ERR_INVALID_ARG);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(probe_decodes_both_fields),
    cmocka_unit_test(probe_with_omitted_fields_decodes_to_defaults),
    cmocka_unit_test(probe_skips_a_field_it_does_not_know),
    cmocka_unit_test(probe_with_a_length_past_the_buffer_is_rejected),
    cmocka_unit_test(probe_with_a_truncated_varint_is_rejected),
    cmocka_unit_test(probe_with_an_unterminated_varint_run_is_rejected),
    cmocka_unit_test(probe_with_a_group_wire_type_is_rejected),
    cmocka_unit_test(probe_with_a_zero_field_number_is_rejected),
    cmocka_unit_test(probe_with_an_oversized_timeout_is_rejected),
    cmocka_unit_test(exec_decodes_ready_id_and_params),
    cmocka_unit_test(exec_skips_the_exec_start_timestamp),
    cmocka_unit_test(exec_with_an_empty_params_field_decodes),
    cmocka_unit_test(probe_ack_ready_encodes_the_nested_ready_message),
    cmocka_unit_test(probe_ack_rejected_encodes_the_reason),
    cmocka_unit_test(probe_ack_rejected_keeps_the_oneof_case_for_the_default_reason),
    cmocka_unit_test(abandon_encodes_the_ready_id_and_reason),
    cmocka_unit_test(abandon_omits_a_default_reason),
    cmocka_unit_test(result_encodes_status_and_body),
    cmocka_unit_test(result_sign_extends_a_negative_status),
    cmocka_unit_test(result_omits_a_zero_status_and_an_empty_body),
    cmocka_unit_test(result_reports_a_body_that_does_not_fit),
    cmocka_unit_test(encoders_reject_a_missing_destination),
  };
  return cmocka_run_group_tests_name("mqttv5_direct_method_codec", tests, NULL, NULL);
}
