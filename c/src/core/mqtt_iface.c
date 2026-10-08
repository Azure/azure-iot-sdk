// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "azure/iot/az_iot_mqtt_iface.h"

const char* az_iot_mqtt_version_to_string(az_iot_mqtt_version v)
{
  switch (v)
  {
    case AZ_IOT_MQTT_VERSION_3_1_1:
      return "MQTTv3.1.1";
    case AZ_IOT_MQTT_VERSION_5:
      return "MQTTv5";
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

/* MQTT 5 CONNACK reason codes (spec 3.2.2.2) that reject the identity. 0x86 is
 * included alongside the three the core needs because it is the v5 spelling of
 * v3.1.1's code 4, and treating the same refusal differently per protocol
 * version would make identity recovery depend on the hub flavor. */
#define CONNACK_V5_CLIENT_ID_NOT_VALID 0x85
#define CONNACK_V5_BAD_CREDENTIALS 0x86
#define CONNACK_V5_NOT_AUTHORIZED 0x87
#define CONNACK_V5_BAD_AUTH_METHOD 0x8C

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

  if (version == AZ_IOT_MQTT_VERSION_5)
  {
    switch (connack_code)
    {
      case CONNACK_V5_CLIENT_ID_NOT_VALID:
      case CONNACK_V5_BAD_CREDENTIALS:
      case CONNACK_V5_NOT_AUTHORIZED:
      case CONNACK_V5_BAD_AUTH_METHOD:
        return AZ_IOT_ERR_IDENTITY_REJECTED;
      default:
        return AZ_IOT_ERR_MQTT;
    }
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

  /* A version this function does not know. The code cannot be interpreted --
   * the two schemes overlap numerically (2, 4 and 5 mean identity refusals in
   * v3.1.1 and something else entirely in v5) -- so guessing a scheme would be
   * guessing whether the identity was refused. Report a connection failure, which is the
   * conservative half of the split: a device retries instead of abandoning
   * credentials that may be perfectly good.
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

/* MQTT 5 SUBACK reason codes (spec 3.9.3) that a retry cannot change: the
 * filter is not permitted, or is not one this broker will ever accept. Quota
 * exceeded (0x97), unspecified (0x80) and implementation specific (0x83) are
 * deliberately absent -- those are how a transient service-side fault presents,
 * and re-subscribing is the right response to them. */
#define SUBACK_V5_NOT_AUTHORIZED 0x87
#define SUBACK_V5_TOPIC_FILTER_INVALID 0x8F
#define SUBACK_V5_SHARED_SUBS_NOT_SUPPORTED 0x9E
#define SUBACK_V5_SUB_IDS_NOT_SUPPORTED 0xA1
#define SUBACK_V5_WILDCARD_SUBS_NOT_SUPPORTED 0xA2

/* Server-sent DISCONNECT reason codes worth naming. The rest are transient
 * service-side conditions -- 0x89 server busy, 0x8B shutting down, 0x8D
 * keep-alive timeout, 0x97 quota exceeded, 0x9C/0x9D use-another-server -- and
 * reconnecting is the right response to them. */
#define DISCONNECT_V5_NORMAL 0x00
#define DISCONNECT_V5_NOT_AUTHORIZED 0x87

AZ_NODISCARD az_iot_result
az_iot_mqtt_disconnect_result(az_iot_mqtt_version version, int disconnect_code)
{
  /* No code: an ordinary end of session, and what v3.1.1 always reports. */
  if (disconnect_code == DISCONNECT_V5_NORMAL || version != AZ_IOT_MQTT_VERSION_5)
  {
    return AZ_IOT_OK;
  }

  /* Adapters report their own failures with negative codes; those never came
   * off the wire, so they carry no verdict from the server. */
  if (disconnect_code < 0)
  {
    return AZ_IOT_ERR_MQTT;
  }

  /* The credential was refused. Reconnecting with it returns the same answer,
   * which is what AZ_IOT_ERR_AUTH tells the application through is_retriable. */
  if (disconnect_code == DISCONNECT_V5_NOT_AUTHORIZED)
  {
    return AZ_IOT_ERR_AUTH;
  }

  return AZ_IOT_ERR_MQTT;
}

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

  if (version == AZ_IOT_MQTT_VERSION_5)
  {
    switch (suback_code)
    {
      case SUBACK_V5_NOT_AUTHORIZED:
      case SUBACK_V5_TOPIC_FILTER_INVALID:
      case SUBACK_V5_SHARED_SUBS_NOT_SUPPORTED:
      case SUBACK_V5_SUB_IDS_NOT_SUPPORTED:
      case SUBACK_V5_WILDCARD_SUBS_NOT_SUPPORTED:
        return AZ_IOT_ERR_SUBSCRIPTION_REFUSED;
      default:
        return AZ_IOT_ERR_MQTT;
    }
  }

  if (version == AZ_IOT_MQTT_VERSION_3_1_1)
  {
    /* No reason code exists to consult, so the classification comes from what an
     * MQTTv3 device can subscribe to: a topic set fixed at compile time. That
     * makes a refusal a property of the filter rather than of the moment. */
    return (suback_code == SUBACK_V3_FAILURE) ? AZ_IOT_ERR_SUBSCRIPTION_REFUSED : AZ_IOT_ERR_MQTT;
  }

  /* Same reasoning as the CONNACK mapper: the schemes overlap numerically, so
   * an unknown version cannot be interpreted, and retrying is the conservative
   * half of the split. */
  return AZ_IOT_ERR_MQTT;
}
