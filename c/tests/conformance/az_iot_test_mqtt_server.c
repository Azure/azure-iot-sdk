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
#define MQTT_DISCONNECT 0xE0

static az_iot_test_mqtt_packet packet_empty(void)
{
  az_iot_test_mqtt_packet p;
  memset(&p, 0, sizeof(p));
  return p;
}

/* Reason-code mapping, kept in one place because the two versions disagree and
 * 3.1.1 has fewer codes than there are things to say. Written as if/else rather
 * than switch: the build enables -Wswitch-enum, and an exhaustive switch here
 * would have to restate every enumerator in every mapping. */

static uint8_t connack_code(az_iot_test_mqtt_version version, az_iot_test_mqtt_reason reason)
{
  if (reason == AZ_IOT_TEST_MQTT_REASON_SUCCESS)
  {
    return 0x00;
  }
  if (version == AZ_IOT_TEST_MQTT_V5)
  {
    /* 0x87 Not authorized; 0x80 Unspecified error for everything else. */
    return (reason == AZ_IOT_TEST_MQTT_REASON_NOT_AUTHORIZED) ? 0x87u : 0x80u;
  }
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
  return (version == AZ_IOT_TEST_MQTT_V5) ? 0x87u : 0x80u;
}

static uint8_t disconnect_code(az_iot_test_mqtt_reason reason)
{
  if (reason == AZ_IOT_TEST_MQTT_REASON_SERVER_SHUTTING_DOWN)
  {
    return 0x8Bu;
  }
  if (reason == AZ_IOT_TEST_MQTT_REASON_NOT_AUTHORIZED)
  {
    return 0x87u;
  }
  return (reason == AZ_IOT_TEST_MQTT_REASON_SUCCESS) ? 0x00u : 0x80u;
}

az_iot_test_mqtt_packet az_iot_test_mqtt_connack_raw(
    az_iot_test_mqtt_version version,
    uint8_t code,
    int session_present)
{
  az_iot_test_mqtt_packet p = packet_empty();
  uint8_t flags = session_present ? 0x01u : 0x00u;

  p.bytes[0] = MQTT_CONNACK;
  if (version == AZ_IOT_TEST_MQTT_V5)
  {
    /* ack flags, reason code, property length. */
    p.bytes[1] = 0x03;
    p.bytes[2] = flags;
    p.bytes[3] = code;
    p.bytes[4] = 0x00;
    p.len = 5;
  }
  else
  {
    /* ack flags, return code. */
    p.bytes[1] = 0x02;
    p.bytes[2] = flags;
    p.bytes[3] = code;
    p.len = 4;
  }
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
  if (version == AZ_IOT_TEST_MQTT_V5)
  {
    p.bytes[1] = 0x04;
    p.bytes[2] = 0x00;
    p.bytes[3] = 0x00;
    p.bytes[4] = 0x00; /* property length */
    p.bytes[5] = code;
    p.len = 6;
  }
  else
  {
    p.bytes[1] = 0x03;
    p.bytes[2] = 0x00;
    p.bytes[3] = 0x00;
    p.bytes[4] = code;
    p.len = 5;
  }
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

  if (reason != AZ_IOT_TEST_MQTT_REASON_SUCCESS && version != AZ_IOT_TEST_MQTT_V5)
  {
    return p; /* 3.1.1 has no reason code in a PUBACK */
  }

  p.bytes[0] = MQTT_PUBACK;
  p.bytes[2] = 0x00; /* packet id, patched by the proxy */
  p.bytes[3] = 0x00;
  if (reason == AZ_IOT_TEST_MQTT_REASON_SUCCESS)
  {
    /* Remaining length 2 means "success, no properties" in both versions. */
    p.bytes[1] = 0x02;
    p.len = 4;
  }
  else
  {
    /* v5: remaining length 3 carries a reason code and, per MQTT 5.0
     * 3.4.2.2.1, omits the property length entirely. */
    p.bytes[1] = 0x03;
    p.bytes[4] = (reason == AZ_IOT_TEST_MQTT_REASON_NOT_AUTHORIZED) ? 0x87u : 0x80u;
    p.len = 5;
  }
  p.packet_id_offset = 2;
  p.echo_packet_id = 1;
  return p;
}

az_iot_test_mqtt_packet az_iot_test_mqtt_disconnect(
    az_iot_test_mqtt_version version,
    az_iot_test_mqtt_reason reason)
{
  az_iot_test_mqtt_packet p = packet_empty();

  if (version != AZ_IOT_TEST_MQTT_V5)
  {
    return p; /* 3.1.1 has no server-sent DISCONNECT */
  }
  p.bytes[0] = MQTT_DISCONNECT;
  p.bytes[1] = 0x01;
  p.bytes[2] = disconnect_code(reason);
  p.len = 3;
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
