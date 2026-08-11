// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/**
 * @file az_iot_test_mqtt_server.h
 * @brief Builds the MQTT packets a *broker* sends.
 *
 * The test proxy deliberately traffics in opaque bytes: it schedules, drops and
 * shapes them without knowing what they mean. Something still has to produce
 * the bytes a synthetic broker answers with, and before this module that job
 * fell to the tests themselves, which hand-assembled fixed headers and reason
 * codes inline. Wire layout then lived in every case that needed a refusal, in
 * two versions, restated each time.
 *
 * This module owns that knowledge instead. A test says what it wants to happen
 * -- refuse the subscription -- and gets the bytes for the version under test.
 *
 * Scope: the packets a broker sends and a client must react to. It is not a
 * broker and not a general codec. In particular it does not parse anything;
 * asserting on what the *client* sent is a separate job that would need a
 * decoder, and is not built here.
 *
 * Standalone on purpose: no dependency on az_iot_core or on the proxy. What it
 * encodes is MQTT, not anything specific to this SDK, and the proxy must not
 * acquire a dependency on it -- that would put protocol knowledge back into the
 * layer this separation exists to keep clean.
 */

#ifndef AZ_IOT_TEST_MQTT_SERVER_H
#define AZ_IOT_TEST_MQTT_SERVER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

  /* Every packet built here is an acknowledgement or a control packet, so it is
   * small. The bound matches AZ_IOT_TEST_PROXY_RULE_BYTES_MAX, which is what an
   * injected packet has to fit into. */
#define AZ_IOT_TEST_MQTT_PACKET_MAX 64

  typedef enum az_iot_test_mqtt_version
  {
    AZ_IOT_TEST_MQTT_V3_1_1 = 0,
    AZ_IOT_TEST_MQTT_V5 = 1
  } az_iot_test_mqtt_version;

  /* What a test means, rather than the byte that expresses it. The two wire
   * versions disagree on the codes and MQTT 3.1.1 collapses several of these
   * into one value, so mapping them here keeps that spread out of the tests.
   *
   * When the specific byte *is* the assertion -- an unknown or reserved code,
   * say -- use the _raw builders instead. */
  typedef enum az_iot_test_mqtt_reason
  {
    AZ_IOT_TEST_MQTT_REASON_SUCCESS = 0,
    AZ_IOT_TEST_MQTT_REASON_NOT_AUTHORIZED,
    AZ_IOT_TEST_MQTT_REASON_REFUSED,
    AZ_IOT_TEST_MQTT_REASON_SERVER_SHUTTING_DOWN
  } az_iot_test_mqtt_reason;

  typedef struct az_iot_test_mqtt_packet
  {
    uint8_t bytes[AZ_IOT_TEST_MQTT_PACKET_MAX];
    /* 0 means the packet is not expressible in the requested version -- a
     * server DISCONNECT in 3.1.1, or a PUBACK carrying a failure reason, which
     * 3.1.1 has no field for. Callers do not need to check: a zero-length
     * injection is rejected by the proxy, so the test fails where it is set up
     * rather than silently asserting nothing. */
    size_t len;
    /* Where an acknowledgement carries the id of the packet it answers, or 0
     * when the packet carries no id at all -- offset 0 is the fixed header, so
     * it can never hold one. */
    size_t packet_id_offset;
    /* Whether the proxy should overwrite that id with the one from the packet
     * being answered. Distinct from "an id is present": pinning an id with
     * az_iot_test_mqtt_set_packet_id() leaves the id in place and clears this,
     * which is what makes an ack for a packet nobody sent stay wrong. Maps
     * straight onto az_iot_test_proxy_rule.echo_packet_id. */
    int echo_packet_id;
  } az_iot_test_mqtt_packet;

  /* CONNACK with session-present clear. */
  az_iot_test_mqtt_packet az_iot_test_mqtt_connack(
      az_iot_test_mqtt_version version,
      az_iot_test_mqtt_reason reason);

  /* SUBACK granting nothing, or refusing. Carries a packet id. */
  az_iot_test_mqtt_packet az_iot_test_mqtt_suback(
      az_iot_test_mqtt_version version,
      az_iot_test_mqtt_reason reason);

  /* PUBACK. Carries a packet id. A non-success reason is v5 only: MQTT 3.1.1
   * has no reason code in a PUBACK, so that combination yields len == 0. */
  az_iot_test_mqtt_packet az_iot_test_mqtt_puback(
      az_iot_test_mqtt_version version,
      az_iot_test_mqtt_reason reason);

  /* Server-sent DISCONNECT. v5 only; MQTT 3.1.1 has no such packet, so that
   * combination yields len == 0. */
  az_iot_test_mqtt_packet az_iot_test_mqtt_disconnect(
      az_iot_test_mqtt_version version,
      az_iot_test_mqtt_reason reason);

  /* Identical in both versions. */
  az_iot_test_mqtt_packet az_iot_test_mqtt_pingresp(void);

  /* Escape hatches for when the exact code is the subject of the test. */
  az_iot_test_mqtt_packet az_iot_test_mqtt_connack_raw(
      az_iot_test_mqtt_version version,
      uint8_t code,
      int session_present);
  az_iot_test_mqtt_packet az_iot_test_mqtt_suback_raw(
      az_iot_test_mqtt_version version,
      uint8_t code);

  /* Pin the packet id and clear the echo flag, for an acknowledgement that is
   * meant to answer something the client never sent. Does nothing to a packet
   * that carries no id, or if the recorded offset does not fit the packet. */
  void az_iot_test_mqtt_set_packet_id(az_iot_test_mqtt_packet* packet, uint16_t packet_id);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_TEST_MQTT_SERVER_H */
