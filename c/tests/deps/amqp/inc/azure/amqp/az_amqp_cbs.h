// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief Claims-Based Security (CBS): authorizing access to AMQP nodes by putting a security token
 * (SAS or JWT) over the well-known `$cbs` management node.
 *
 * @details Azure brokers (Event Hubs, Service Bus, IoT Hub) authorize each entity with CBS rather
 * than (or in addition to) SASL: the client attaches a sender and a receiver link to `$cbs` and
 * issues a `put-token` request whose reply carries an HTTP-style status code. An #az_amqp_cbs object
 * owns those two internal links (two embedded #az_amqp_link instances) and the request/reply
 * correlation. It is bound to a session and driven by the owning connection's
 * #az_amqp_connection_process pump.
 *
 * @note Because CBS uses two links, the session it is created on must have room for two more links
 * in its link registry (see #az_amqp_session_storage).
 */

#ifndef _az_AMQP_CBS_H
#define _az_AMQP_CBS_H

#include <azure/amqp/az_amqp_common.h>
#include <azure/amqp/az_amqp_link.h>
#include <azure/amqp/az_amqp_session.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

/// The CBS token type for an Azure Shared Access Signature, usable with `AZ_SPAN_FROM_STR`.
#define AZ_AMQP_CBS_TOKEN_TYPE_SAS "servicebus.windows.net:sastoken"

/// The CBS token type for a JSON Web Token (OAuth 2.0 bearer), usable with `AZ_SPAN_FROM_STR`.
#define AZ_AMQP_CBS_TOKEN_TYPE_JWT "jwt"

/**
 * @brief The lifecycle states of a CBS handshake object.
 *
 * @remark The first enumerator is the error state, per the SDK design guidance.
 */
typedef enum
{
  AZ_AMQP_CBS_STATE_ERROR = 0, ///< A `$cbs` link failed.
  AZ_AMQP_CBS_STATE_CLOSED, ///< Initialized but the `$cbs` links are not attached.
  AZ_AMQP_CBS_STATE_OPENING, ///< The `$cbs` links are attaching.
  AZ_AMQP_CBS_STATE_OPEN, ///< Both `$cbs` links are attached; put-token requests may be issued.
  AZ_AMQP_CBS_STATE_CLOSING, ///< The `$cbs` links are detaching.
} az_amqp_cbs_state;

/**
 * @brief Invoked when a #az_amqp_cbs_put_token request completes.
 *
 * @param[in] cbs The CBS object.
 * @param[in] status_code The HTTP-style status code (200/202 indicate success).
 * @param[in] status_description __[nullable]__ A human-readable description; valid only during the
 * callback.
 * @param[in] user_data The pointer supplied to #az_amqp_cbs_put_token.
 */
typedef void (*az_amqp_cbs_put_token_complete_callback)(
    az_amqp_cbs* cbs,
    uint32_t status_code,
    az_span status_description,
    void* user_data);

/**
 * @brief Options for a CBS object.
 */
typedef struct
{
  /// Caller-owned buffer the internal `$cbs` receiver uses to assemble reply messages. A few
  /// hundred bytes is ample. Required.
  az_span reply_buffer;
} az_amqp_cbs_options;

/**
 * @brief A CBS handshake object. Caller-allocated. Owns two internal links registered on the
 * session. All fields are visible and library-managed; treat them as read-only after init.
 */
struct az_amqp_cbs
{
  az_amqp_link request_link; ///< The `$cbs` sender-role link. Library-managed.
  az_amqp_link reply_link; ///< The `$cbs` receiver-role link. Library-managed.
  az_amqp_link_unsettled request_unsettled[1]; ///< One in-flight put-token slot. Library-managed.
  az_amqp_session* session; ///< The owning session. Library-managed.
  az_amqp_cbs_options options; ///< The effective options.
  az_amqp_cbs_state state; ///< Current state. Library-managed.
  az_amqp_cbs_put_token_complete_callback put_token_complete; ///< Completion callback. Library-managed.
  void* put_token_user_data; ///< Completion callback context. Library-managed.
  az_amqp_error_detail last_error; ///< Most recent failure detail. Library-managed.
  uint64_t pending_correlation_id; ///< Correlation id of the in-flight request. Library-managed.
  bool put_token_in_flight; ///< Whether a put-token is awaiting its reply. Library-managed.
};

/**
 * @brief Gets the default CBS options (empty reply buffer — the caller MUST set one).
 */
AZ_NODISCARD az_amqp_cbs_options az_amqp_cbs_options_default(void);

/**
 * @brief Initializes a CBS object and registers its two internal `$cbs` links with the session.
 *
 * @param[out] cbs The CBS object to initialize. Must not be `NULL`.
 * @param[in] session The owning, initialized session. Must outlive @p cbs and have room for two
 * links in its registry.
 * @param[in] options CBS options; #az_amqp_cbs_options.reply_buffer is required.
 * @return #AZ_OK; #AZ_ERROR_ARG on a `NULL`/empty argument; #AZ_ERROR_AMQP_NO_STORAGE if the
 * session's link registry lacked two free slots.
 */
AZ_NODISCARD az_result az_amqp_cbs_init(
    az_amqp_cbs* cbs,
    az_amqp_session* session,
    az_amqp_cbs_options const* options);

/**
 * @brief Attaches the internal `$cbs` sender and receiver links.
 *
 * @return #AZ_OK if opening started; #AZ_ERROR_AMQP_WRONG_STATE if not closed.
 */
AZ_NODISCARD az_result az_amqp_cbs_open(az_amqp_cbs* cbs);

/**
 * @brief Detaches the internal `$cbs` links.
 *
 * @return #AZ_OK if closing started; #AZ_ERROR_AMQP_WRONG_STATE if already closed.
 */
AZ_NODISCARD az_result az_amqp_cbs_close(az_amqp_cbs* cbs);

/**
 * @brief Issues a `put-token` request authorizing access to an audience until the token expires.
 *
 * @details Only one put-token may be in flight at a time per CBS object.
 *
 * @param[in] cbs The CBS object. Must be #AZ_AMQP_CBS_STATE_OPEN.
 * @param[in] token_type The token type, e.g. #AZ_AMQP_CBS_TOKEN_TYPE_SAS or #AZ_AMQP_CBS_TOKEN_TYPE_JWT.
 * @param[in] audience The resource URI the token authorizes, e.g.
 * `amqps://myns.servicebus.windows.net/myqueue`.
 * @param[in] token The token text. Referenced, not copied; keep it valid until @p on_complete fires.
 * @param[in] expires_at_unix_ms The token's absolute expiry (ms since the Unix epoch), or `0`.
 * @param[in] on_complete __[nullable]__ Invoked with the reply status, or `NULL`.
 * @param[in] user_data __[nullable]__ Opaque pointer passed back to @p on_complete.
 * @return #AZ_OK; #AZ_ERROR_AMQP_WRONG_STATE (not open); #AZ_ERROR_AMQP_NO_CREDIT (one already in
 * flight); #AZ_ERROR_NOT_ENOUGH_SPACE.
 */
AZ_NODISCARD az_result az_amqp_cbs_put_token(
    az_amqp_cbs* cbs,
    az_span token_type,
    az_span audience,
    az_span token,
    int64_t expires_at_unix_ms,
    az_amqp_cbs_put_token_complete_callback on_complete,
    void* user_data);

/**
 * @brief Returns the current CBS state.
 */
AZ_NODISCARD az_amqp_cbs_state az_amqp_cbs_get_state(az_amqp_cbs const* cbs);

/**
 * @brief Returns the actionable detail of the most recent failure on this CBS object.
 */
AZ_NODISCARD az_amqp_error_detail az_amqp_cbs_get_last_error(az_amqp_cbs const* cbs);

#include <azure/core/_az_cfg_suffix.h>

#endif // _az_AMQP_CBS_H
