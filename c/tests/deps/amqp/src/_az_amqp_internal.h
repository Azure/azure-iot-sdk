// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief Internal cross-file helpers for the AMQP implementation. Not part of the public API.
 */

#ifndef _az_AMQP_INTERNAL_H
#define _az_AMQP_INTERNAL_H

#include <azure/amqp/az_amqp_connection.h>
#include <azure/amqp/az_amqp_link.h>
#include <azure/amqp/az_amqp_session.h>
#include <azure/amqp/az_amqp_value.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdint.h>

// ----- encoder splice + list helper (value.c) -----

/**
 * @brief Appends pre-encoded AMQP bytes into the encoder as a single value (updates the enclosing
 * compound's element count). Used to splice already-encoded sections/payloads.
 */
AZ_NODISCARD az_result _az_amqp_encoder_append_raw(az_amqp_encoder* encoder, az_span raw);

/**
 * @brief Decodes a list value and returns its element at @p index. If the list has fewer elements,
 * @p out is set to an #AZ_AMQP_VALUE_KIND_NULL value. @p list_value must be a list.
 */
AZ_NODISCARD az_result
_az_amqp_list_field(az_amqp_value const* list_value, uint32_t index, az_amqp_value* out);

// ----- frame engine (connection.c) -----

/**
 * @brief Begins a frame in the connection's outgoing buffer: returns an encoder positioned just
 * after the reserved 8-byte frame header, writing directly into the outgoing buffer.
 */
AZ_NODISCARD az_result
_az_amqp_connection_frame_begin(az_amqp_connection* connection, az_amqp_encoder* out_encoder);

/**
 * @brief Completes the frame begun by #_az_amqp_connection_frame_begin: writes the 8-byte header
 * (size, doff=2, type, channel) and advances the outgoing buffer.
 */
AZ_NODISCARD az_result _az_amqp_connection_frame_end(
    az_amqp_connection* connection,
    az_amqp_encoder* encoder,
    uint8_t frame_type,
    uint16_t channel);

/**
 * @brief Emits a message as one or more `transfer` frames on @p channel. When the encoded message
 * does not fit in a single frame it is fragmented into continuation frames no larger than the
 * negotiated max-frame-size (`more = true` on all but the last). The full encoded message must fit
 * in the free portion of the connection's outgoing buffer.
 */
AZ_NODISCARD az_result _az_amqp_connection_emit_transfer(
    az_amqp_connection* connection,
    uint16_t channel,
    uint32_t handle,
    uint32_t delivery_id,
    az_span delivery_tag,
    bool settled,
    az_amqp_message const* message);

/**
 * @brief Registers a session with the connection and assigns its local channel.
 */
AZ_NODISCARD az_result _az_amqp_connection_add_session(
    az_amqp_connection* connection,
    az_amqp_session* session,
    uint16_t* out_channel);

// ----- session engine (session.c) -----

/**
 * @brief Registers a link with the session and assigns its local handle.
 */
AZ_NODISCARD az_result
_az_amqp_session_add_link(az_amqp_session* session, az_amqp_link* link, uint32_t* out_handle);

/**
 * @brief Dispatches a session/link performative (begin/end/flow/attach/transfer/disposition/detach)
 * decoded by the connection to this session. @p payload is the trailing bytes after the
 * performative (the message, for `transfer`); empty otherwise. @p channel is the incoming channel.
 */
void _az_amqp_session_handle_performative(
    az_amqp_session* session,
    uint16_t channel,
    uint64_t descriptor,
    az_amqp_value const* fields,
    az_span payload);

// ----- link engine (link.c) -----

/**
 * @brief Dispatches a link performative (attach/flow/transfer/disposition/detach) to this link.
 */
void _az_amqp_link_handle_performative(
    az_amqp_link* link,
    uint64_t descriptor,
    az_amqp_value const* fields,
    az_span payload);

#endif // _az_AMQP_INTERNAL_H
