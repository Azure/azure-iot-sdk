// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

#include "az_iot_test_mqtt_server.h"

#include <string.h>

/* MQTT fixed-header first bytes for the packets a broker sends. */
#define MQTT_CONNACK 0x20
#define MQTT_PUBACK 0x40
#define MQTT_SUBACK 0x90
#define MQTT_PINGRESP 0xD0

static az_iot_test_mqtt_packet packet_empty(void)
{
  az_iot_test_mqtt_packet p;
  memset(&p, 0, sizeof(p));
  return p;
}

/* Reason-code mapping, kept in one place because
 * 3.1.1 has fewer codes than there are things to say. Written as if/else rather
 * than switch: the build enables -Wswitch-enum, and an exhaustive switch here
 * would have to restate every enumerator in every mapping. */

static uint8_t connack_code(az_iot_test_mqtt_version version, az_iot_test_mqtt_reason reason)
{
  if (reason == AZ_IOT_TEST_MQTT_REASON_SUCCESS)
  {
    return 0x00;
  }
  (void)version;
  /* 3.1.1 return codes: 0x05 is the only refusal that fits these reasons. */
  return 0x05u;
}

static uint8_t suback_code(az_iot_test_mqtt_version version, az_iot_test_mqtt_reason reason)
{
  if (reason == AZ_IOT_TEST_MQTT_REASON_SUCCESS)
  {
    return 0x00u; /* granted QoS 0 */
  }
  /* 3.1.1 has exactly one failure code, so both refusal reasons land on it. */
  (void)version;
  return 0x80u;
}

az_iot_test_mqtt_packet az_iot_test_mqtt_connack_raw(
    az_iot_test_mqtt_version version,
    uint8_t code,
    int session_present)
{
  az_iot_test_mqtt_packet p = packet_empty();
  uint8_t flags = session_present ? 0x01u : 0x00u;

  (void)version;
  p.bytes[0] = MQTT_CONNACK;
  /* ack flags, return code. */
  p.bytes[1] = 0x02;
  p.bytes[2] = flags;
  p.bytes[3] = code;
  p.len = 4;
  return p;
}

az_iot_test_mqtt_packet az_iot_test_mqtt_connack(
    az_iot_test_mqtt_version version,
    az_iot_test_mqtt_reason reason)
{
  return az_iot_test_mqtt_connack_raw(version, connack_code(version, reason), 0);
}

az_iot_test_mqtt_packet az_iot_test_mqtt_suback_raw(az_iot_test_mqtt_version version, uint8_t code)
{
  az_iot_test_mqtt_packet p = packet_empty();

  p.bytes[0] = MQTT_SUBACK;
  /* The packet id is left zero: the proxy patches in the id of the SUBSCRIBE
   * being answered, which is what makes the ack match rather than be discarded
   * as unsolicited. */
  (void)version;
  p.bytes[1] = 0x03;
  p.bytes[2] = 0x00;
  p.bytes[3] = 0x00;
  p.bytes[4] = code;
  p.len = 5;
  p.packet_id_offset = 2;
  p.echo_packet_id = 1;
  return p;
}

az_iot_test_mqtt_packet az_iot_test_mqtt_suback(
    az_iot_test_mqtt_version version,
    az_iot_test_mqtt_reason reason)
{
  return az_iot_test_mqtt_suback_raw(version, suback_code(version, reason));
}

az_iot_test_mqtt_packet az_iot_test_mqtt_puback(
    az_iot_test_mqtt_version version,
    az_iot_test_mqtt_reason reason)
{
  az_iot_test_mqtt_packet p = packet_empty();

  (void)version;
  if (reason != AZ_IOT_TEST_MQTT_REASON_SUCCESS)
  {
    return p; /* 3.1.1 has no reason code in a PUBACK */
  }

  p.bytes[0] = MQTT_PUBACK;
  p.bytes[2] = 0x00; /* packet id, patched by the proxy */
  p.bytes[3] = 0x00;
  p.bytes[1] = 0x02;
  p.len = 4;
  p.packet_id_offset = 2;
  p.echo_packet_id = 1;
  return p;
}

az_iot_test_mqtt_packet az_iot_test_mqtt_pingresp(void)
{
  az_iot_test_mqtt_packet p = packet_empty();
  p.bytes[0] = MQTT_PINGRESP;
  p.bytes[1] = 0x00;
  p.len = 2;
  return p;
}

void az_iot_test_mqtt_set_packet_id(az_iot_test_mqtt_packet* packet, uint16_t packet_id)
{
  /* Offset 0 means the packet carries no id. The bounds check matters because
   * the struct is public: a caller can hand over one it filled in itself, and a
   * test helper should refuse that rather than write past the buffer. */
  if (packet == NULL || packet->packet_id_offset == 0 || packet->len > sizeof(packet->bytes)
      || packet->packet_id_offset + 2 > packet->len)
  {
    return;
  }
  packet->bytes[packet->packet_id_offset] = (uint8_t)(packet_id >> 8);
  packet->bytes[packet->packet_id_offset + 1] = (uint8_t)(packet_id & 0xFFu);
  /* An id set on purpose must survive: echoing would overwrite it with the id
   * of whatever packet triggered the rule. */
  packet->echo_packet_id = 0;
}
