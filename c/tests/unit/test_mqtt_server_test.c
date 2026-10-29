// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* Pins the wire format of the packets a broker sends.
 *
 * These builders replaced literal byte arrays that were written out in each
 * conformance case. Those cases assert on client *behaviour*, so they would
 * still pass if a reason code changed to a different refusal -- a refused
 * SUBACK is a refused SUBACK whichever code carries it. The exact bytes have to
 * be asserted somewhere, and this is that somewhere.
 *
 * The expected values below are the literals the conformance cases used before
 * the builders existed, which is what makes the change provably one of shape
 * and not of behaviour. */

#include "az_iot_test_mqtt_server.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

static void assert_packet(
    const az_iot_test_mqtt_packet* actual,
    const uint8_t* expected,
    size_t expected_len)
{
  assert_int_equal(actual->len, expected_len);
  assert_memory_equal(actual->bytes, expected, expected_len);
}

static void connack_v3_carries_the_v3_return_code(void** state)
{
  (void)state;
  /* ack flags 0, return code 0x05 Not authorized. */
  static const uint8_t expected[] = { 0x20, 0x02, 0x00, 0x05 };
  az_iot_test_mqtt_packet p
      = az_iot_test_mqtt_connack(AZ_IOT_TEST_MQTT_V3_1_1, AZ_IOT_TEST_MQTT_REASON_NOT_AUTHORIZED);
  assert_packet(&p, expected, sizeof(expected));
  /* A CONNACK answers nothing, so it carries no id and nothing to echo. */
  assert_int_equal(p.packet_id_offset, 0);
  assert_int_equal(p.echo_packet_id, 0);
}

static void connack_v5_carries_a_reason_code_and_property_length(void** state)
{
  (void)state;
  /* session-present 0, reason 0x87 Not authorized, property length 0. */
  static const uint8_t expected[] = { 0x20, 0x03, 0x00, 0x87, 0x00 };
  az_iot_test_mqtt_packet p
      = az_iot_test_mqtt_connack(AZ_IOT_TEST_MQTT_V5, AZ_IOT_TEST_MQTT_REASON_NOT_AUTHORIZED);
  assert_packet(&p, expected, sizeof(expected));
}

static void connack_success_differs_only_in_the_code(void** state)
{
  (void)state;
  static const uint8_t v3[] = { 0x20, 0x02, 0x00, 0x00 };
  static const uint8_t v5[] = { 0x20, 0x03, 0x00, 0x00, 0x00 };
  az_iot_test_mqtt_packet p3
      = az_iot_test_mqtt_connack(AZ_IOT_TEST_MQTT_V3_1_1, AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  az_iot_test_mqtt_packet p5
      = az_iot_test_mqtt_connack(AZ_IOT_TEST_MQTT_V5, AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  assert_packet(&p3, v3, sizeof(v3));
  assert_packet(&p5, v5, sizeof(v5));
}

static void connack_session_present_sets_the_ack_flag(void** state)
{
  (void)state;
  /* The only bit defined in the acknowledge flags byte. */
  az_iot_test_mqtt_packet p = az_iot_test_mqtt_connack_raw(AZ_IOT_TEST_MQTT_V5, 0x00, 1);
  assert_int_equal(p.bytes[2], 0x01);
  az_iot_test_mqtt_packet clear = az_iot_test_mqtt_connack_raw(AZ_IOT_TEST_MQTT_V5, 0x00, 0);
  assert_int_equal(clear.bytes[2], 0x00);
}

static void suback_v3_uses_the_single_failure_code(void** state)
{
  (void)state;
  /* packet id (patched later), return code 0x80 Failure. */
  static const uint8_t expected[] = { 0x90, 0x03, 0x00, 0x00, 0x80 };
  az_iot_test_mqtt_packet p
      = az_iot_test_mqtt_suback(AZ_IOT_TEST_MQTT_V3_1_1, AZ_IOT_TEST_MQTT_REASON_REFUSED);
  assert_packet(&p, expected, sizeof(expected));
}

static void suback_v5_carries_a_property_length_before_the_reason(void** state)
{
  (void)state;
  static const uint8_t expected[] = { 0x90, 0x04, 0x00, 0x00, 0x00, 0x87 };
  az_iot_test_mqtt_packet p
      = az_iot_test_mqtt_suback(AZ_IOT_TEST_MQTT_V5, AZ_IOT_TEST_MQTT_REASON_REFUSED);
  assert_packet(&p, expected, sizeof(expected));
}

/* An acknowledgement has to carry the id of the packet it answers, and the
 * proxy patches that in. The builder is what tells it where. */
static void an_acknowledgement_reports_where_its_packet_id_sits(void** state)
{
  (void)state;
  az_iot_test_mqtt_packet suback
      = az_iot_test_mqtt_suback(AZ_IOT_TEST_MQTT_V5, AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  az_iot_test_mqtt_packet puback
      = az_iot_test_mqtt_puback(AZ_IOT_TEST_MQTT_V5, AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  assert_int_equal(suback.echo_packet_id, 1);
  assert_int_equal(suback.packet_id_offset, 2);
  assert_int_equal(puback.echo_packet_id, 1);
  assert_int_equal(puback.packet_id_offset, 2);
}

static void puback_success_is_the_same_in_both_versions(void** state)
{
  (void)state;
  static const uint8_t expected[] = { 0x40, 0x02, 0x00, 0x00 };
  az_iot_test_mqtt_packet p3
      = az_iot_test_mqtt_puback(AZ_IOT_TEST_MQTT_V3_1_1, AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  az_iot_test_mqtt_packet p5
      = az_iot_test_mqtt_puback(AZ_IOT_TEST_MQTT_V5, AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  assert_packet(&p3, expected, sizeof(expected));
  assert_packet(&p5, expected, sizeof(expected));
}

static void puback_v5_omits_the_property_length_when_it_carries_a_reason(void** state)
{
  (void)state;
  /* MQTT 5.0 3.4.2.2.1: below a remaining length of 4 there is no property
   * length and 0 is assumed. */
  static const uint8_t expected[] = { 0x40, 0x03, 0x00, 0x00, 0x87 };
  az_iot_test_mqtt_packet p
      = az_iot_test_mqtt_puback(AZ_IOT_TEST_MQTT_V5, AZ_IOT_TEST_MQTT_REASON_NOT_AUTHORIZED);
  assert_packet(&p, expected, sizeof(expected));
}

/* An absent Reason String is no property; an empty one is property 0x1F with length 0. */
static void puback_v5_raw_tells_an_absent_reason_string_from_an_empty_one(void** state)
{
  (void)state;
  static const uint8_t absent[] = { 0x40, 0x04, 0x00, 0x00, 0x87, 0x00 };
  static const uint8_t empty[] = { 0x40, 0x07, 0x00, 0x00, 0x87, 0x03, 0x1F, 0x00, 0x00 };
  static const uint8_t text[] = { 0x40, 0x09, 0x00, 0x00, 0x97, 0x05, 0x1F, 0x00, 0x02, 'n', 'o' };
  az_iot_test_mqtt_packet p = az_iot_test_mqtt_puback_v5_raw(0x87, NULL);
  assert_packet(&p, absent, sizeof(absent));
  p = az_iot_test_mqtt_puback_v5_raw(0x87, "");
  assert_packet(&p, empty, sizeof(empty));
  p = az_iot_test_mqtt_puback_v5_raw(0x97, "no");
  assert_packet(&p, text, sizeof(text));
  assert_int_equal(p.packet_id_offset, 2);
  assert_int_equal(p.echo_packet_id, 1);
}

/* Single-byte lengths only: 120 bytes is the longest Reason String (Remaining Length 127). */
static void puback_v5_raw_refuses_a_reason_string_past_single_byte_lengths(void** state)
{
  (void)state;
  char reason[122];
  memset(reason, 'r', sizeof(reason));
  reason[120] = '\0';
  az_iot_test_mqtt_packet p = az_iot_test_mqtt_puback_v5_raw(0x87, reason);
  assert_int_equal(p.len, 129);
  assert_int_equal(p.bytes[1], 127);
  assert_int_equal(p.bytes[5], 123);
  assert_int_equal(p.bytes[8], 120);
  assert_memory_equal(&p.bytes[9], reason, 120);

  reason[120] = 'r';
  reason[121] = '\0';
  p = az_iot_test_mqtt_puback_v5_raw(0x87, reason);
  assert_int_equal(p.len, 0);
}

/* As for PUBACK: absent is no property, empty is property 0x1F with length 0. No packet id. */
static void connack_v5_raw_tells_an_absent_reason_string_from_an_empty_one(void** state)
{
  (void)state;
  static const uint8_t absent[] = { 0x20, 0x03, 0x00, 0x87, 0x00 };
  static const uint8_t empty[] = { 0x20, 0x06, 0x00, 0x87, 0x03, 0x1F, 0x00, 0x00 };
  static const uint8_t text[] = { 0x20, 0x08, 0x00, 0x86, 0x05, 0x1F, 0x00, 0x02, 'n', 'o' };
  az_iot_test_mqtt_packet p = az_iot_test_mqtt_connack_v5_raw(0x87, NULL);
  assert_packet(&p, absent, sizeof(absent));
  p = az_iot_test_mqtt_connack_v5_raw(0x87, "");
  assert_packet(&p, empty, sizeof(empty));
  p = az_iot_test_mqtt_connack_v5_raw(0x86, "no");
  assert_packet(&p, text, sizeof(text));
  assert_int_equal(p.packet_id_offset, 0);
  assert_int_equal(p.echo_packet_id, 0);
}

/* The reason code stays at bytes[2], where server_disconnect_is_reported() reads it. */
static void disconnect_v5_raw_tells_an_absent_reason_string_from_an_empty_one(void** state)
{
  (void)state;
  static const uint8_t absent[] = { 0xE0, 0x02, 0x8B, 0x00 };
  static const uint8_t empty[] = { 0xE0, 0x05, 0x8B, 0x03, 0x1F, 0x00, 0x00 };
  static const uint8_t text[] = { 0xE0, 0x07, 0x97, 0x05, 0x1F, 0x00, 0x02, 'n', 'o' };
  az_iot_test_mqtt_packet p = az_iot_test_mqtt_disconnect_v5_raw(0x8B, NULL);
  assert_packet(&p, absent, sizeof(absent));
  p = az_iot_test_mqtt_disconnect_v5_raw(0x8B, "");
  assert_packet(&p, empty, sizeof(empty));
  p = az_iot_test_mqtt_disconnect_v5_raw(0x97, "no");
  assert_packet(&p, text, sizeof(text));
  assert_int_equal(p.packet_id_offset, 0);
}

/* Single-byte lengths: 122 bytes is the longest DISCONNECT Reason String (Remaining Length 127). */
static void disconnect_v5_raw_refuses_a_reason_string_past_single_byte_lengths(void** state)
{
  (void)state;
  char reason[124];
  memset(reason, 'r', sizeof(reason));
  reason[122] = '\0';
  az_iot_test_mqtt_packet p = az_iot_test_mqtt_disconnect_v5_raw(0x8B, reason);
  assert_int_equal(p.len, 129);
  assert_int_equal(p.bytes[1], 127);
  assert_memory_equal(&p.bytes[7], reason, 122);

  reason[122] = 'r';
  reason[123] = '\0';
  p = az_iot_test_mqtt_disconnect_v5_raw(0x8B, reason);
  assert_int_equal(p.len, 0);
}

/* 3.1.1 has no field for it, so the builder reports "not expressible" rather
 * than inventing a packet. A zero-length injection is refused by the proxy, so
 * a test that asks for one fails where it is set up. */
static void a_refusal_a_version_cannot_express_yields_nothing(void** state)
{
  (void)state;
  az_iot_test_mqtt_packet puback
      = az_iot_test_mqtt_puback(AZ_IOT_TEST_MQTT_V3_1_1, AZ_IOT_TEST_MQTT_REASON_NOT_AUTHORIZED);
  az_iot_test_mqtt_packet bye
      = az_iot_test_mqtt_disconnect(AZ_IOT_TEST_MQTT_V3_1_1, AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  assert_int_equal(puback.len, 0);
  assert_int_equal(bye.len, 0);
}

static void server_disconnect_carries_the_v5_reason(void** state)
{
  (void)state;
  static const uint8_t expected[] = { 0xE0, 0x01, 0x8B }; /* server shutting down */
  az_iot_test_mqtt_packet p = az_iot_test_mqtt_disconnect(
      AZ_IOT_TEST_MQTT_V5, AZ_IOT_TEST_MQTT_REASON_SERVER_SHUTTING_DOWN);
  assert_packet(&p, expected, sizeof(expected));
}

static void pingresp_is_a_bare_fixed_header(void** state)
{
  (void)state;
  static const uint8_t expected[] = { 0xD0, 0x00 };
  az_iot_test_mqtt_packet p = az_iot_test_mqtt_pingresp();
  assert_packet(&p, expected, sizeof(expected));
}

/* The v5 PUBLISH builder, byte for byte. The property section is where an
 * inbound-property test gets its fixture, so its layout is the assertion:
 * identifier, then each string as a two-byte length followed by the bytes, with
 * no NUL anywhere -- which is exactly what a client must not mistake for a C
 * string. */
static void publish_v5_lays_out_topic_properties_and_payload(void** state)
{
  (void)state;
  static const uint8_t body[] = { 'h', 'i' };
  const az_iot_test_mqtt_user_property props[] = { { "k", "v" } };
  static const uint8_t expected[] = {
    0x30, 0x13, /* PUBLISH QoS 0, remaining length 19 */
    0x00, 0x02, 't',  'p', /* topic "tp" */
    0x0C, /* property length 12 */
    0x03, 0x00, 0x02, 'c', 't', /* content type "ct" */
    0x26, 0x00, 0x01, 'k', 0x00, 0x01, 'v', /* user property k=v */
    'h',  'i' /* payload */
  };
  az_iot_test_mqtt_packet p = az_iot_test_mqtt_publish_v5("tp", body, sizeof(body), props, 1, "ct");
  assert_packet(&p, expected, sizeof(expected));
  /* QoS 0 carries no packet id, so there is nothing for the proxy to echo. */
  assert_int_equal(p.packet_id_offset, 0);
  assert_int_equal(p.echo_packet_id, 0);
}

/* A zero-length key or value is legal MQTT and must be expressible, or the
 * empty-property case has no fixture to be driven with. NULL means the same
 * thing, so a caller does not have to spell "" to omit half a pair. */
static void publish_v5_emits_zero_length_keys_and_values(void** state)
{
  (void)state;
  const az_iot_test_mqtt_user_property props[] = { { "", NULL } };
  static const uint8_t expected[]
      = { 0x30, 0x0A, 0x00, 0x02, 't', 'p', 0x05, 0x26, 0x00, 0x00, 0x00, 0x00 };
  az_iot_test_mqtt_packet p = az_iot_test_mqtt_publish_v5("tp", NULL, 0, props, 1, NULL);
  assert_packet(&p, expected, sizeof(expected));
}

/* Past 127 bytes the Remaining Length and Property Length take two bytes, as a
 * PUBLISH carrying many User Properties needs. */
static void publish_v5_uses_multi_byte_lengths(void** state)
{
  (void)state;
  az_iot_test_mqtt_user_property props[16];
  for (size_t i = 0; i < 16; ++i)
  {
    props[i].key = "key";
    props[i].value = "value";
  }
  /* 16 x (1 + 2 + 3 + 2 + 5) = 208 bytes of properties. */
  az_iot_test_mqtt_packet p = az_iot_test_mqtt_publish_v5("tp", NULL, 0, props, 16, NULL);
  size_t const remaining = 2u + 2u + 2u + 208u;
  assert_int_equal(p.len, 1u + 2u + remaining);
  assert_int_equal(p.bytes[1], (uint8_t)(0x80u | (remaining % 128u)));
  assert_int_equal(p.bytes[2], (uint8_t)(remaining / 128u));
  assert_int_equal(p.bytes[7], (uint8_t)(0x80u | (208u % 128u)));
  assert_int_equal(p.bytes[8], (uint8_t)(208u / 128u));
  assert_int_equal(p.bytes[9], 0x26u);
}

/* Too big to express comes back empty rather than truncated: the proxy rejects
 * a zero-length injection, so the test fails where it was set up instead of
 * asserting against half a packet. */
static void publish_v5_refuses_a_packet_that_does_not_fit(void** state)
{
  (void)state;
#define K40 "0123456789012345678901234567890123456789"
  const az_iot_test_mqtt_user_property props[]
      = { { K40 K40, K40 K40 }, { K40 K40, K40 K40 }, { K40 K40, K40 K40 }, { K40 K40, K40 K40 } };
#undef K40
  az_iot_test_mqtt_packet p = az_iot_test_mqtt_publish_v5("tp", NULL, 0, props, 4, NULL);
  assert_int_equal(p.len, 0);

  /* A length whose sum with the header would wrap is refused, not copied. */
  static const uint8_t body[1] = { 0 };
  az_iot_test_mqtt_packet huge
      = az_iot_test_mqtt_publish_v5("tp", body, SIZE_MAX - 8u, NULL, 0, NULL);
  assert_int_equal(huge.len, 0);

  /* And a missing topic is not a packet at all. */
  az_iot_test_mqtt_packet none = az_iot_test_mqtt_publish_v5(NULL, NULL, 0, NULL, 0, NULL);
  assert_int_equal(none.len, 0);
}

/* Setting an id on purpose must also stop the proxy echoing the real one over
 * it, or "an ack for a packet nobody sent" quietly becomes a valid ack. */
static void pinning_a_packet_id_clears_the_echo(void** state)
{
  (void)state;
  az_iot_test_mqtt_packet p
      = az_iot_test_mqtt_puback(AZ_IOT_TEST_MQTT_V3_1_1, AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  az_iot_test_mqtt_set_packet_id(&p, 0xBEEF);
  static const uint8_t expected[] = { 0x40, 0x02, 0xBE, 0xEF };
  assert_packet(&p, expected, sizeof(expected));
  /* The id stays; only the echo is cleared, or the proxy would overwrite it. */
  assert_int_equal(p.packet_id_offset, 2);
  assert_int_equal(p.echo_packet_id, 0);
}

static void pinning_a_packet_id_on_a_packet_without_one_is_ignored(void** state)
{
  (void)state;
  az_iot_test_mqtt_packet p
      = az_iot_test_mqtt_connack(AZ_IOT_TEST_MQTT_V5, AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  static const uint8_t before[] = { 0x20, 0x03, 0x00, 0x00, 0x00 };
  az_iot_test_mqtt_set_packet_id(&p, 0xBEEF);
  assert_packet(&p, before, sizeof(before));
  az_iot_test_mqtt_set_packet_id(NULL, 0xBEEF); /* must not crash */
}

/* The struct is public, so a caller can hand over one it filled in itself. An
 * offset that does not fit has to be refused rather than written through: this
 * is test code, and a stray write here would corrupt whatever follows. */
static void a_packet_id_offset_that_does_not_fit_is_refused(void** state)
{
  (void)state;
  az_iot_test_mqtt_packet p
      = az_iot_test_mqtt_puback(AZ_IOT_TEST_MQTT_V3_1_1, AZ_IOT_TEST_MQTT_REASON_SUCCESS);
  static const uint8_t before[] = { 0x40, 0x02, 0x00, 0x00 };

  p.packet_id_offset = p.len - 1; /* two bytes would run one past the end */
  az_iot_test_mqtt_set_packet_id(&p, 0xBEEF);
  assert_packet(&p, before, sizeof(before));

  p.packet_id_offset = 2;
  p.len = AZ_IOT_TEST_MQTT_PACKET_MAX + 1; /* a length that cannot be real */
  az_iot_test_mqtt_set_packet_id(&p, 0xBEEF);
  assert_memory_equal(p.bytes, before, sizeof(before));
}

/* Every built packet has to fit the proxy's injection buffer, or it cannot be
 * used for the job it exists for. */
static void every_packet_fits_the_injection_bound(void** state)
{
  (void)state;
  az_iot_test_mqtt_packet packets[] = {
    az_iot_test_mqtt_connack(AZ_IOT_TEST_MQTT_V5, AZ_IOT_TEST_MQTT_REASON_NOT_AUTHORIZED),
    az_iot_test_mqtt_suback(AZ_IOT_TEST_MQTT_V5, AZ_IOT_TEST_MQTT_REASON_REFUSED),
    az_iot_test_mqtt_puback(AZ_IOT_TEST_MQTT_V5, AZ_IOT_TEST_MQTT_REASON_NOT_AUTHORIZED),
    az_iot_test_mqtt_disconnect(AZ_IOT_TEST_MQTT_V5, AZ_IOT_TEST_MQTT_REASON_SERVER_SHUTTING_DOWN),
    az_iot_test_mqtt_pingresp(),
  };
  for (size_t i = 0; i < sizeof(packets) / sizeof(packets[0]); ++i)
  {
    assert_true(packets[i].len > 0);
    assert_true(packets[i].len <= AZ_IOT_TEST_MQTT_PACKET_MAX);
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(connack_v3_carries_the_v3_return_code),
    cmocka_unit_test(connack_v5_carries_a_reason_code_and_property_length),
    cmocka_unit_test(connack_success_differs_only_in_the_code),
    cmocka_unit_test(connack_session_present_sets_the_ack_flag),
    cmocka_unit_test(suback_v3_uses_the_single_failure_code),
    cmocka_unit_test(suback_v5_carries_a_property_length_before_the_reason),
    cmocka_unit_test(an_acknowledgement_reports_where_its_packet_id_sits),
    cmocka_unit_test(puback_success_is_the_same_in_both_versions),
    cmocka_unit_test(puback_v5_omits_the_property_length_when_it_carries_a_reason),
    cmocka_unit_test(puback_v5_raw_tells_an_absent_reason_string_from_an_empty_one),
    cmocka_unit_test(puback_v5_raw_refuses_a_reason_string_past_single_byte_lengths),
    cmocka_unit_test(connack_v5_raw_tells_an_absent_reason_string_from_an_empty_one),
    cmocka_unit_test(disconnect_v5_raw_tells_an_absent_reason_string_from_an_empty_one),
    cmocka_unit_test(disconnect_v5_raw_refuses_a_reason_string_past_single_byte_lengths),
    cmocka_unit_test(a_refusal_a_version_cannot_express_yields_nothing),
    cmocka_unit_test(server_disconnect_carries_the_v5_reason),
    cmocka_unit_test(pingresp_is_a_bare_fixed_header),
    cmocka_unit_test(publish_v5_lays_out_topic_properties_and_payload),
    cmocka_unit_test(publish_v5_emits_zero_length_keys_and_values),
    cmocka_unit_test(publish_v5_uses_multi_byte_lengths),
    cmocka_unit_test(publish_v5_refuses_a_packet_that_does_not_fit),
    cmocka_unit_test(pinning_a_packet_id_clears_the_echo),
    cmocka_unit_test(pinning_a_packet_id_on_a_packet_without_one_is_ignored),
    cmocka_unit_test(a_packet_id_offset_that_does_not_fit_is_refused),
    cmocka_unit_test(every_packet_fits_the_injection_bound),
  };
  return cmocka_run_group_tests_name("test_mqtt_server_tests", tests, NULL, NULL);
}
