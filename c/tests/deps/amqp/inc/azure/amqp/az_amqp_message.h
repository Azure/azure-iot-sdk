// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief An allocation-free representation of an AMQP 1.0 message: the standard sections (header,
 * annotations, properties, application-properties, body, footer) as caller-owned views, plus a
 * small #az_amqp_property_map builder for the map-shaped sections.
 *
 * @details An AMQP message (OASIS AMQP 1.0 §3.2) is a sequence of optional, described sections. To
 * avoid allocation, an #az_amqp_message does not own a parse tree: the structured `header` and
 * `properties` sections are plain C structs, while the map-shaped sections (annotations,
 * application-properties, footer) and the body are #az_span views over bytes the application
 * encoded (when sending) or that the library decoded in place (when receiving). The
 * #az_amqp_property_map helper builds those map sections key-by-key into a caller buffer, so common
 * cases need not touch the raw #az_amqp_encoder.
 *
 * @note All structures here are flat and visible; fields are library-managed and should not be
 * modified directly except through the provided setters. There is no hidden `_internal` wrapper.
 */

#ifndef _az_AMQP_MESSAGE_H
#define _az_AMQP_MESSAGE_H

#include <azure/amqp/az_amqp_common.h>
#include <azure/amqp/az_amqp_value.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

/**
 * @brief The AMQP type used to carry a `message-id` or `correlation-id` (§3.2.4).
 */
typedef enum
{
  AZ_AMQP_MESSAGE_ID_KIND_NULL = 0, ///< Absent / not set.
  AZ_AMQP_MESSAGE_ID_KIND_ULONG, ///< A 64-bit unsigned integer id.
  AZ_AMQP_MESSAGE_ID_KIND_UUID, ///< A 16-byte UUID id.
  AZ_AMQP_MESSAGE_ID_KIND_BINARY, ///< An opaque binary id.
  AZ_AMQP_MESSAGE_ID_KIND_STRING, ///< A UTF-8 string id.
} az_amqp_message_id_kind;

/**
 * @brief A polymorphic `message-id`/`correlation-id` value. Build with one of the
 * `az_amqp_message_id_from_*` helpers; read #kind then call the matching accessor. Flat and visible.
 */
typedef struct
{
  az_amqp_message_id_kind kind; ///< The active alternative (the discriminator).
  uint64_t u64; ///< Valid when #kind is #AZ_AMQP_MESSAGE_ID_KIND_ULONG.
  az_span bytes; ///< UUID (16 bytes), binary, or string view for the other kinds.
} az_amqp_message_id;

/// @name message-id / correlation-id constructors
/// @{
AZ_NODISCARD az_amqp_message_id az_amqp_message_id_from_ulong(uint64_t value);
AZ_NODISCARD az_amqp_message_id az_amqp_message_id_from_uuid(uint8_t const uuid[16]);
AZ_NODISCARD az_amqp_message_id az_amqp_message_id_from_binary(az_span value);
AZ_NODISCARD az_amqp_message_id az_amqp_message_id_from_string(az_span value);
/// @}

/**
 * @brief The `header` section of a message (§3.2.1). All fields are optional in AMQP; the `has_*`
 * flags distinguish "explicitly set" from "use the protocol default".
 */
typedef struct
{
  bool durable; ///< If `true`, the message must not be lost due to a failure.
  uint8_t priority; ///< Relative priority (default 4). Honored only when #has_priority is `true`.
  bool has_priority; ///< Whether #priority was set.
  uint32_t time_to_live_milliseconds; ///< TTL. Honored only when #has_time_to_live is `true`.
  bool has_time_to_live; ///< Whether #time_to_live_milliseconds was set.
  bool first_acquirer; ///< `true` if no other link has acquired this message before.
  uint32_t delivery_count; ///< The number of prior unsuccessful delivery attempts.
} az_amqp_message_header;

/**
 * @brief The `properties` section of a message (§3.2.4). Unset #az_span fields are empty; unset id
 * fields have kind #AZ_AMQP_MESSAGE_ID_KIND_NULL.
 */
typedef struct
{
  az_amqp_message_id message_id; ///< Application-defined unique id for the message.
  az_span user_id; ///< Binary identity of the user responsible for producing the message.
  az_span to; ///< The address of the node the message is destined for (string).
  az_span subject; ///< A common subject/summary for the message (string).
  az_span reply_to; ///< The node to send replies to (string).
  az_amqp_message_id correlation_id; ///< The id this message correlates to (e.g. a request id).
  az_span content_type; ///< MIME type of the body (symbol), e.g. `application/json`.
  az_span content_encoding; ///< Content encoding of the body (symbol), e.g. `gzip`.
  int64_t absolute_expiry_time_ms; ///< Absolute expiry (ms since epoch). See #has_absolute_expiry_time.
  bool has_absolute_expiry_time; ///< Whether #absolute_expiry_time_ms was set.
  int64_t creation_time_ms; ///< Creation time (ms since epoch). See #has_creation_time.
  bool has_creation_time; ///< Whether #creation_time_ms was set.
  az_span group_id; ///< The group the message belongs to (string).
  uint32_t group_sequence; ///< Position within the group. See #has_group_sequence.
  bool has_group_sequence; ///< Whether #group_sequence was set.
  az_span reply_to_group_id; ///< The group replies should belong to (string).
} az_amqp_message_properties;

/**
 * @brief Which of the three mutually exclusive body forms a message carries (§3.2.6).
 */
typedef enum
{
  AZ_AMQP_MESSAGE_BODY_KIND_NONE = 0, ///< No body section.
  AZ_AMQP_MESSAGE_BODY_KIND_DATA, ///< One `data` section of opaque bytes.
  AZ_AMQP_MESSAGE_BODY_KIND_VALUE, ///< One `amqp-value` section holding a single value.
  AZ_AMQP_MESSAGE_BODY_KIND_SEQUENCE, ///< One `amqp-sequence` section holding a list.
} az_amqp_message_body_kind;

/**
 * @brief An assembled AMQP message. Caller-allocated; populated field by field for sending, or by
 * #az_amqp_message_decode for receiving. Flat and visible; use the setters to mutate.
 */
typedef struct
{
  az_amqp_message_header header; ///< The header section (present when #has_header).
  bool has_header; ///< Whether a header section is present.
  az_span delivery_annotations; ///< Encoded map bytes, or empty.
  az_span message_annotations; ///< Encoded map bytes, or empty.
  az_amqp_message_properties properties; ///< The properties section (present when #has_properties).
  bool has_properties; ///< Whether a properties section is present.
  az_span application_properties; ///< Encoded map bytes, or empty.
  az_amqp_message_body_kind body_kind; ///< The body form.
  az_span body; ///< DATA: raw bytes; VALUE: one encoded value; SEQUENCE: an encoded list body.
  az_span footer; ///< Encoded map bytes, or empty.
} az_amqp_message;

/**
 * @brief Initializes an empty message (no sections present).
 *
 * @return #AZ_OK; #AZ_ERROR_ARG if @p message is `NULL`.
 */
AZ_NODISCARD az_result az_amqp_message_init(az_amqp_message* message);

/// @name Section setters (for sending)
/// The map-shaped setters take bytes produced by #az_amqp_encoder or #az_amqp_property_map. The
/// message stores the spans by reference; they must outlive the send.
/// @{
AZ_NODISCARD az_result
az_amqp_message_set_header(az_amqp_message* message, az_amqp_message_header const* header);
AZ_NODISCARD az_result az_amqp_message_set_properties(
    az_amqp_message* message,
    az_amqp_message_properties const* properties);
AZ_NODISCARD az_result
az_amqp_message_set_delivery_annotations(az_amqp_message* message, az_span encoded_map);
AZ_NODISCARD az_result
az_amqp_message_set_message_annotations(az_amqp_message* message, az_span encoded_map);
AZ_NODISCARD az_result
az_amqp_message_set_application_properties(az_amqp_message* message, az_span encoded_map);
AZ_NODISCARD az_result az_amqp_message_set_footer(az_amqp_message* message, az_span encoded_map);
/// Sets the body to a single `data` section carrying @p data verbatim.
AZ_NODISCARD az_result az_amqp_message_set_body_data(az_amqp_message* message, az_span data);
/// Sets the body to an `amqp-value` section carrying one value encoded with #az_amqp_encoder.
AZ_NODISCARD az_result az_amqp_message_set_body_value(az_amqp_message* message, az_span encoded_value);
/// Sets the body to an `amqp-sequence` section carrying an encoded `list`.
AZ_NODISCARD az_result
az_amqp_message_set_body_sequence(az_amqp_message* message, az_span encoded_list);
/// @}

/// @name Section getters (for receiving)
/// Each returns #AZ_ERROR_ITEM_NOT_FOUND if the section is absent. Returned spans/structs view the
/// decode buffer and are valid only while it is.
/// @{
AZ_NODISCARD az_result
az_amqp_message_get_header(az_amqp_message const* message, az_amqp_message_header* out_header);
AZ_NODISCARD az_result az_amqp_message_get_properties(
    az_amqp_message const* message,
    az_amqp_message_properties* out_properties);
AZ_NODISCARD az_result
az_amqp_message_get_message_annotations(az_amqp_message const* message, az_span* out_encoded_map);
AZ_NODISCARD az_result
az_amqp_message_get_delivery_annotations(az_amqp_message const* message, az_span* out_encoded_map);
AZ_NODISCARD az_result
az_amqp_message_get_application_properties(az_amqp_message const* message, az_span* out_encoded_map);
AZ_NODISCARD az_result
az_amqp_message_get_footer(az_amqp_message const* message, az_span* out_encoded_map);
/**
 * @brief Returns the body form and its bytes.
 *
 * @return #AZ_OK; #AZ_ERROR_ITEM_NOT_FOUND if the message has no body.
 */
AZ_NODISCARD az_result az_amqp_message_get_body(
    az_amqp_message const* message,
    az_amqp_message_body_kind* out_kind,
    az_span* out_body);
/// @}

/**
 * @brief Encodes the full annotated message (all present sections, in canonical order) into
 * @p destination. No allocation occurs.
 *
 * @param[out] out_encoded An #az_span over the bytes written (aliases @p destination).
 * @return #AZ_OK; #AZ_ERROR_NOT_ENOUGH_SPACE if @p destination is too small.
 */
AZ_NODISCARD az_result az_amqp_message_encode(
    az_amqp_message const* message,
    az_span destination,
    az_span* out_encoded);

/**
 * @brief Decodes a serialized annotated message into section views over @p encoded.
 *
 * @return #AZ_OK; #AZ_ERROR_AMQP_DECODE if the bytes are malformed.
 */
AZ_NODISCARD az_result az_amqp_message_decode(az_span encoded, az_amqp_message* out_message);

// ===================== Application-property / annotation builder =====================

/**
 * @brief A convenience builder that assembles a map section (application-properties, message-
 * annotations, etc.) key-by-key into a caller buffer, without the application driving an
 * #az_amqp_encoder's begin/end calls directly.
 *
 * @details Initialize with #az_amqp_property_map_init, add entries with the `add_*` helpers, then
 * pass #az_amqp_property_map_get_bytes to the relevant message setter. Keys are AMQP `string`s
 * (application-properties) or `symbol`s depending on the section; these helpers emit `string` keys,
 * which is what application-properties require. Flat and visible; fields are library-managed.
 */
typedef struct
{
  az_amqp_encoder encoder; ///< The underlying encoder over the caller buffer. Library-managed.
  bool sealed; ///< Set once #az_amqp_property_map_get_bytes finalizes the map. Library-managed.
} az_amqp_property_map;

/**
 * @brief Initializes a property-map builder over @p buffer and opens the map.
 *
 * @return #AZ_OK; #AZ_ERROR_ARG on a `NULL`/empty argument; #AZ_ERROR_NOT_ENOUGH_SPACE if too small.
 */
AZ_NODISCARD az_result az_amqp_property_map_init(az_amqp_property_map* map, az_span buffer);

/// @name Entry appenders
/// Each appends a string key and a value of the given type. Return #AZ_ERROR_NOT_ENOUGH_SPACE on
/// overflow, or #AZ_ERROR_AMQP_WRONG_STATE if the map was already sealed.
/// @{
AZ_NODISCARD az_result
az_amqp_property_map_add_string(az_amqp_property_map* map, az_span key, az_span value);
AZ_NODISCARD az_result
az_amqp_property_map_add_long(az_amqp_property_map* map, az_span key, int64_t value);
AZ_NODISCARD az_result
az_amqp_property_map_add_bool(az_amqp_property_map* map, az_span key, bool value);
AZ_NODISCARD az_result
az_amqp_property_map_add_binary(az_amqp_property_map* map, az_span key, az_span value);
/// @}

/**
 * @brief Finalizes the map (back-patching its size/count) and returns the encoded bytes, suitable
 * for #az_amqp_message_set_application_properties or the annotation setters. Idempotent after the
 * first call.
 *
 * @return An #az_span over the encoded map (aliases the builder's buffer).
 */
AZ_NODISCARD az_span az_amqp_property_map_get_bytes(az_amqp_property_map* map);

#include <azure/core/_az_cfg_suffix.h>

#endif // _az_AMQP_MESSAGE_H
