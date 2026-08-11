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
    cmocka_unit_test(a_refusal_a_version_cannot_express_yields_nothing),
    cmocka_unit_test(server_disconnect_carries_the_v5_reason),
    cmocka_unit_test(pingresp_is_a_bare_fixed_header),
    cmocka_unit_test(pinning_a_packet_id_clears_the_echo),
    cmocka_unit_test(pinning_a_packet_id_on_a_packet_without_one_is_ignored),
    cmocka_unit_test(a_packet_id_offset_that_does_not_fit_is_refused),
    cmocka_unit_test(every_packet_fits_the_injection_bound),
  };
  return cmocka_run_group_tests_name("test_mqtt_server_tests", tests, NULL, NULL);
}
