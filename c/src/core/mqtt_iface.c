// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/* SPDX-License-Identifier: MIT */
#include "azure/iot/az_iot_mqtt_iface.h"

const char* az_iot_mqtt_version_to_string(az_iot_mqtt_version v)
{
    switch (v)
    {
        case AZ_IOT_MQTT_VERSION_3_1_1: return "MQTTv3.1.1";
        case AZ_IOT_MQTT_VERSION_5:     return "MQTTv5";
        default:                     return "MQTT?";
    }
}

/* MQTT 3.1.1 CONNACK return codes (spec 3.2.2.3) that reject the identity. 1
 * (unacceptable protocol version) and 3 (server unavailable) are deliberately
 * absent: neither says anything about who the device claims to be, and 3 is the
 * canonical transient failure. */
#define CONNACK_V3_IDENTIFIER_REJECTED   2
#define CONNACK_V3_BAD_CREDENTIALS       4
#define CONNACK_V3_NOT_AUTHORIZED        5

/* MQTT 5 CONNACK reason codes (spec 3.2.2.2) that reject the identity. 0x86 is
 * included alongside the three the core needs because it is the v5 spelling of
 * v3.1.1's code 4, and treating the same refusal differently per protocol
 * version would make the re-provisioning trigger depend on the hub flavor. */
#define CONNACK_V5_CLIENT_ID_NOT_VALID   0x85
#define CONNACK_V5_BAD_CREDENTIALS       0x86
#define CONNACK_V5_NOT_AUTHORIZED        0x87
#define CONNACK_V5_BAD_AUTH_METHOD       0x8C

az_iot_result az_iot_mqtt_connack_result(az_iot_mqtt_version version, int connack_code)
{
    if (connack_code == 0) return AZ_IOT_OK;

    /* Adapters signal their own failures (socket refused, TLS handshake, client
     * library error) with negative codes. Those never reached a broker, so they
     * carry no verdict about the identity. */
    if (connack_code < 0) return AZ_IOT_ERR_MQTT;

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
     * guessing whether to re-provision. Report a connection failure, which is the
     * conservative half of the split: a device retries instead of abandoning
     * credentials that may be perfectly good.
     *
     * This is a public entry point that byo-MQTT adapters call with a version
     * they supply (see how_to_byo_mqtt_client.md), so the value is genuinely
     * untrusted here rather than an internal invariant. */
    return AZ_IOT_ERR_MQTT;
}
