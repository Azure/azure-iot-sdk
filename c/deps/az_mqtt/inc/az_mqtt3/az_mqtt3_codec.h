// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt3_codec.h
 * @brief MQTT 3.1.1 packet encoders and decoders (az_mqttv3 library).
 *
 * Encoders write a whole packet into @p dest and advance it past what they
 * wrote; they fail with AZ_ERROR_NOT_ENOUGH_SPACE. Decoders take the packet
 * body (after the fixed header) and fail with AZ_ERROR_UNEXPECTED_END or
 * AZ_MQTT_ERROR_MALFORMED_PACKET. No dynamic allocation.
 */

#ifndef AZ_MQTT3_CODEC_H
#define AZ_MQTT3_CODEC_H

#include <az_mqtt/az_mqtt_types.h>
#include <az_mqtt3/az_mqtt3_types.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

/** @brief CONNECT Protocol Level of MQTT 3.1.1. */
#define AZ_MQTT3_PROTOCOL_VERSION 4

// ──────────────────────── Encoding ───────────────────────────

/** @brief Encode a CONNECT packet. */
AZ_NODISCARD az_result
az_mqtt3_codec_encode_connect(az_span* dest, az_mqtt3_connect_options const* opts);

/**
 * @brief Encode a PUBLISH packet.
 * @param[in] packet_id Non-zero for QoS > 0; ignored for QoS 0.
 * @retval AZ_ERROR_ARG Topic empty or with a wildcard.
 */
AZ_NODISCARD az_result az_mqtt3_codec_encode_publish(
    az_span* dest,
    az_mqtt3_publish_options const* opts,
    uint16_t packet_id);

/** @brief Encode a PUBACK packet. */
AZ_NODISCARD az_result az_mqtt3_codec_encode_puback(az_span* dest, uint16_t packet_id);

/** @brief Encode a PUBREC packet. */
AZ_NODISCARD az_result az_mqtt3_codec_encode_pubrec(az_span* dest, uint16_t packet_id);

/** @brief Encode a PUBREL packet. */
AZ_NODISCARD az_result az_mqtt3_codec_encode_pubrel(az_span* dest, uint16_t packet_id);

/** @brief Encode a PUBCOMP packet. */
AZ_NODISCARD az_result az_mqtt3_codec_encode_pubcomp(az_span* dest, uint16_t packet_id);

/**
 * @brief Encode a SUBSCRIBE packet. @pre @p sub_count > 0.
 * @retval AZ_ERROR_ARG A Topic Filter empty or with a misplaced wildcard.
 */
AZ_NODISCARD az_result az_mqtt3_codec_encode_subscribe(
    az_span* dest,
    az_mqtt3_subscription const* subs,
    int32_t sub_count,
    uint16_t packet_id);

/**
 * @brief Encode an UNSUBSCRIBE packet. @pre @p filter_count > 0.
 * @retval AZ_ERROR_ARG A Topic Filter empty or with a misplaced wildcard.
 */
AZ_NODISCARD az_result az_mqtt3_codec_encode_unsubscribe(
    az_span* dest,
    az_span const* topic_filters,
    int32_t filter_count,
    uint16_t packet_id);

/** @brief Encode a DISCONNECT packet (2 bytes). */
AZ_NODISCARD az_result az_mqtt3_codec_encode_disconnect(az_span* dest);

// ──────────────────────── Decoding ───────────────────────────

/** @brief Decode a CONNACK body. */
AZ_NODISCARD az_result az_mqtt3_codec_decode_connack(az_span body, az_mqtt3_connack_data* out);

/**
 * @brief Decode a PUBLISH body.
 * @param[in] flags Fixed header flags.
 * @retval AZ_MQTT_ERROR_MALFORMED_PACKET QoS 3 (reserved), among others.
 */
AZ_NODISCARD az_result
az_mqtt3_codec_decode_publish(az_span body, uint8_t flags, az_mqtt3_publish_data* out);

/**
 * @brief Decode a PUBACK, PUBREC, PUBREL, PUBCOMP or UNSUBACK body.
 * @retval AZ_MQTT_ERROR_MALFORMED_PACKET Bytes follow the packet identifier.
 */
AZ_NODISCARD az_result az_mqtt3_codec_decode_ack(az_span body, az_mqtt3_ack_data* out);

/**
 * @brief Decode a SUBACK body; out->return_codes points into @p body.
 * @retval AZ_MQTT_ERROR_MALFORMED_PACKET No return code, among others.
 */
AZ_NODISCARD az_result az_mqtt3_codec_decode_suback(az_span body, az_mqtt3_suback_data* out);

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT3_CODEC_H
