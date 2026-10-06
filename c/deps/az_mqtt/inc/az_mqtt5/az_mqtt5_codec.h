// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

/**
 * @file az_mqtt5_codec.h
 * @brief MQTT 5.0 codec (az_mqttv5 library).
 *
 * All functions operate on caller-provided az_span buffers with zero dynamic allocation.
 */

#ifndef AZ_MQTT5_CODEC_H
#define AZ_MQTT5_CODEC_H

#include <az_mqtt/az_mqtt_types.h>
#include <az_mqtt5/az_mqtt5_types.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <azure/core/_az_cfg_prefix.h>

/** @brief CONNECT Protocol Level this codec speaks (MQTT 5.0). */
#define AZ_MQTT5_PROTOCOL_VERSION 5


// ──────────────────────── Encoding ───────────────────────────

/**
 * @brief Encode a CONNECT packet into the destination buffer.
 * @param[in,out] dest  Buffer to write into. On success, advanced past the written bytes.
 * @param[in]     opts  CONNECT options.
 * @retval AZ_ERROR_NOT_ENOUGH_SPACE @p dest too small.
 * @retval AZ_ERROR_ARG A string not valid UTF-8 or over 65,535 bytes, a Will Topic empty or with
 * a wildcard, or a Will Response Topic with a wildcard.
 */
AZ_NODISCARD az_result az_mqtt5_codec_encode_connect(az_span* dest, az_mqtt5_connect_options const* opts);

/**
 * @brief Encode a PUBLISH packet.
 * @param[in,out] dest      Buffer; advanced on success.
 * @param[in]     opts      Publish options.
 * @param[in]     packet_id Packet identifier (must be non-zero for QoS>0).
 * @retval AZ_ERROR_ARG Topic with a wildcard or empty without a Topic Alias; Response Topic with
 * a wildcard; either not valid UTF-8.
 */
AZ_NODISCARD az_result az_mqtt5_codec_encode_publish(
    az_span* dest,
    az_mqtt5_publish_options const* opts,
    uint16_t packet_id);

/**
 * @brief Encode a PUBACK packet.
 */
AZ_NODISCARD az_result az_mqtt5_codec_encode_puback(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt5_reason_code reason_code);

/**
 * @brief Encode a PUBREC packet.
 */
AZ_NODISCARD az_result az_mqtt5_codec_encode_pubrec(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt5_reason_code reason_code);

/**
 * @brief Encode a PUBREL packet.
 */
AZ_NODISCARD az_result az_mqtt5_codec_encode_pubrel(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt5_reason_code reason_code);

/**
 * @brief Encode a PUBCOMP packet.
 */
AZ_NODISCARD az_result az_mqtt5_codec_encode_pubcomp(
    az_span* dest,
    uint16_t packet_id,
    az_mqtt5_reason_code reason_code);

/**
 * @brief Encode a SUBSCRIBE packet.
 * @param[in]     subs      Array of subscriptions.
 * @param[in]     sub_count Number of subscriptions.
 * @param[in]     packet_id Packet identifier.
 * @retval AZ_ERROR_ARG A Topic Filter empty, with a misplaced wildcard or not valid UTF-8.
 */
AZ_NODISCARD az_result az_mqtt5_codec_encode_subscribe(
    az_span* dest,
    az_mqtt5_subscription const* subs,
    int32_t sub_count,
    uint16_t packet_id);

/**
 * @brief Encode an UNSUBSCRIBE packet.
 * @retval AZ_ERROR_ARG A Topic Filter empty, with a misplaced wildcard or not valid UTF-8.
 */
AZ_NODISCARD az_result az_mqtt5_codec_encode_unsubscribe(
    az_span* dest,
    az_span const* topic_filters,
    int32_t filter_count,
    uint16_t packet_id);

/**
 * @brief Encode a DISCONNECT packet.
 */
AZ_NODISCARD az_result az_mqtt5_codec_encode_disconnect(
    az_span* dest,
    az_mqtt5_reason_code reason_code,
    uint32_t session_expiry_interval);

/**
 * @brief Encode an AUTH packet.
 */
AZ_NODISCARD az_result az_mqtt5_codec_encode_auth(
    az_span* dest,
    az_mqtt5_reason_code reason_code,
    az_span authentication_method,
    az_span authentication_data);

// ──────────────────────── Decoding ───────────────────────────

/**
 * @brief Decode a CONNACK packet body (after fixed header).
 */
AZ_NODISCARD az_result az_mqtt5_codec_decode_connack(az_span body, az_mqtt5_connack_data* out);

/**
 * @brief Decode a PUBLISH packet body (after fixed header).
 * @param[in] flags  Fixed header flags.
 * @param[in,out] out  In: user_properties / subscription_identifiers (may be NULL) and
 * their capacities. Out: every other field; entries beyond a capacity are dropped.
 * @retval AZ_MQTT_ERROR_MALFORMED_PACKET QoS 3 (reserved), among others.
 */
AZ_NODISCARD az_result
az_mqtt5_codec_decode_publish(az_span body, uint8_t flags, az_mqtt5_publish_data* out);

/**
 * @brief Decode PUBACK/PUBREC/PUBREL/PUBCOMP body.
 */
AZ_NODISCARD az_result az_mqtt5_codec_decode_ack(az_span body, az_mqtt5_ack_data* out);

/**
 * @brief Decode SUBACK body.
 * @retval AZ_MQTT_ERROR_MALFORMED_PACKET No reason code, among others.
 */
AZ_NODISCARD az_result az_mqtt5_codec_decode_suback(az_span body, az_mqtt5_suback_data* out);

/**
 * @brief Decode UNSUBACK body.
 * @retval AZ_MQTT_ERROR_MALFORMED_PACKET No reason code, among others.
 */
AZ_NODISCARD az_result az_mqtt5_codec_decode_unsuback(az_span body, az_mqtt5_suback_data* out);

/**
 * @brief Decode a DISCONNECT packet body.
 */
AZ_NODISCARD az_result az_mqtt5_codec_decode_disconnect(az_span body, az_mqtt5_disconnect_data* out);

/**
 * @brief Decode an AUTH packet body.
 */
AZ_NODISCARD az_result az_mqtt5_codec_decode_auth(az_span body, az_mqtt5_auth_data* out);

#include <azure/core/_az_cfg_suffix.h>

#endif // AZ_MQTT5_CODEC_H
