// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "azure/iot/az_iot_mqtt_iface.h"
#include "internal/mqtt_limits.h"

#include <stdint.h>

const char* az_iot_mqtt_version_to_string(az_iot_mqtt_version v)
{
  switch (v)
  {
    case AZ_IOT_MQTT_VERSION_3_1_1:
      return "MQTTv3.1.1";
    default:
      return "MQTT?";
  }
}

/* MQTT 3.1.1 CONNACK return codes (spec 3.2.2.3) that reject the identity. 1
 * (unacceptable protocol version) and 3 (server unavailable) are deliberately
 * absent: neither says anything about who the device claims to be, and 3 is the
 * canonical transient failure. */
#define CONNACK_V3_IDENTIFIER_REJECTED 2
#define CONNACK_V3_BAD_CREDENTIALS 4
#define CONNACK_V3_NOT_AUTHORIZED 5

AZ_NODISCARD az_iot_result az_iot_mqtt_connack_result(az_iot_mqtt_version version, int connack_code)
{
  if (connack_code == 0)
  {
    return AZ_IOT_OK;
  }

  /* Adapters signal their own failures (socket refused, TLS handshake, client
   * library error) with negative codes. Those never reached a broker, so they
   * carry no verdict about the identity. */
  if (connack_code < 0)
  {
    return AZ_IOT_ERR_MQTT;
  }

  if (version == AZ_IOT_MQTT_VERSION_3_1_1)
  {
    switch (connack_code)
    {
      case CONNACK_V3_IDENTIFIER_REJECTED:
      case CONNACK_V3_BAD_CREDENTIALS:
      case CONNACK_V3_NOT_AUTHORIZED:
        return AZ_IOT_ERR_IDENTITY_REJECTED;
      default:
        return AZ_IOT_ERR_MQTT;
    }
  }

  /* A version this function does not know. The code cannot be interpreted, so
   * guessing a scheme would be guessing whether the identity was refused.
   * Report a connection failure, which is the conservative half of the split: a
   * device retries instead of abandoning credentials that may be perfectly good.
   *
   * This is a public entry point that byo-MQTT adapters call with a version
   * they supply (see how_to_byo_mqtt_client.md), so the value is genuinely
   * untrusted here rather than an internal invariant. */
  return AZ_IOT_ERR_MQTT;
}

/* MQTT 3.1.1 SUBACK return codes (spec 3.9.3). 0x00..0x02 grant the filter at
 * that QoS; 0x80 is the entire failure vocabulary the version has, so a refusal
 * there carries no reason to read. */
#define SUBACK_V3_FAILURE 0x80

AZ_NODISCARD az_iot_result az_iot_mqtt_suback_result(az_iot_mqtt_version version, int suback_code)
{
  /* A granted QoS, including one below what was requested. That is still a
   * grant: the subscription exists, and delivery is min(publish QoS, granted
   * QoS). Reading it as a refusal would fail a session no broker objected to. */
  if (suback_code >= 0 && suback_code <= 2)
  {
    return AZ_IOT_OK;
  }

  /* Adapters report their own failures (socket, TLS, client library) with
   * negative codes. Those never reached a broker, so they carry no verdict
   * about the filter and must stay retryable. */
  if (suback_code < 0)
  {
    return AZ_IOT_ERR_MQTT;
  }

  if (version == AZ_IOT_MQTT_VERSION_3_1_1)
  {
    /* No reason code exists to consult, so the classification comes from what an
     * MQTTv3 device can subscribe to: a topic set fixed at compile time. That
     * makes a refusal a property of the filter rather than of the moment. */
    return (suback_code == SUBACK_V3_FAILURE) ? AZ_IOT_ERR_SUBSCRIPTION_REFUSED : AZ_IOT_ERR_MQTT;
  }

  /* Same reasoning as the CONNACK mapper: an unknown version cannot be
   * interpreted, and retrying is the conservative half of the split. */
  return AZ_IOT_ERR_MQTT;
}

AZ_NODISCARD az_iot_result az_iot_mqtt_puback_result(az_iot_mqtt_version version, int puback_code)
{
  if (puback_code == 0)
  {
    return AZ_IOT_OK;
  }

  /* Adapter-internal failures: no verdict from the broker. */
  if (puback_code < 0)
  {
    return AZ_IOT_ERR_MQTT;
  }

  /* MQTT 3.1.1 PUBACK carries no code: anything but 0 is the adapter's own
   * failure. An unknown version cannot be interpreted; retrying is the safe
   * answer. */
  (void)version;
  return AZ_IOT_ERR_MQTT;
}

bool az_iot_mqtt__limits_are_valid(const az_iot_mqtt_limits* limits)
{
  return limits != NULL && limits->send_buffer_size != 0 && limits->send_buffer_size <= INT32_MAX
      && limits->receive_buffer_size != 0 && limits->receive_buffer_size <= INT32_MAX - 3
      && limits->max_in_flight_messages != 0 && limits->max_in_flight_messages <= 32767
      && limits->message_store_size <= INT32_MAX && limits->connect_strings_size <= INT32_MAX
      && (limits->message_store_size == 0
          || limits->message_store_size
              >= limits->send_buffer_size + AZ_IOT_MQTT__MESSAGE_STORE_ENTRY_OVERHEAD);
}
