// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief The AMQP 1.0 session: a bidirectional, flow-controlled channel multiplexed over a
 * connection (OASIS AMQP 1.0 §2.5), onto which senders and receivers are attached.
 *
 * @details A session is begun on an opened connection. It is not pumped directly; the owning
 * #az_amqp_connection drives it from #az_amqp_connection_process. Its link registry is caller-
 * provided storage (#az_amqp_session_storage) rather than a hidden fixed array — use the
 * #az_amqp_session_storage_for_constrained_device / #az_amqp_session_storage_for_host helpers, or
 * supply your own. No dynamic memory is used.
 */

#ifndef _az_AMQP_SESSION_H
#define _az_AMQP_SESSION_H

#include <azure/amqp/az_amqp_common.h>
#include <azure/amqp/az_amqp_connection.h>
#include <azure/amqp/az_amqp_link.h>

#include <azure/core/az_result.h>

#include <stdbool.h>
#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

/// The default transfer-frame credit this endpoint advertises for inbound transfers.
#define AZ_AMQP_DEFAULT_INCOMING_WINDOW 2048u

/// The default number of outbound transfer-frames this endpoint is prepared to send.
#define AZ_AMQP_DEFAULT_OUTGOING_WINDOW 2048u

/// The default highest link handle usable on the session.
#define AZ_AMQP_DEFAULT_HANDLE_MAX 255u

/**
 * @brief The lifecycle states of an AMQP session.
 *
 * @remark The first enumerator is the error state, per the SDK design guidance.
 */
typedef enum
{
  AZ_AMQP_SESSION_STATE_ERROR = 0, ///< The session failed (e.g. the peer ended it with an error).
  AZ_AMQP_SESSION_STATE_UNMAPPED, ///< Initialized but `begin` has not been sent.
  AZ_AMQP_SESSION_STATE_BEGINNING, ///< `begin` was sent; awaiting the peer's `begin`.
  AZ_AMQP_SESSION_STATE_MAPPED, ///< Both ends have begun; links may be attached.
  AZ_AMQP_SESSION_STATE_ENDING, ///< `end` is being completed.
  AZ_AMQP_SESSION_STATE_UNMAPPED_ENDED, ///< The session has ended.
} az_amqp_session_state;

/**
 * @brief Invoked whenever the session transitions between #az_amqp_session_state values.
 *
 * @param[in] session The session that changed state.
 * @param[in] previous_state The state being left.
 * @param[in] current_state The state being entered.
 * @param[in] error __[nullable]__ Actionable failure detail when entering an error/remote-end state.
 * @param[in] user_data The pointer supplied to #az_amqp_session_set_state_callback.
 */
typedef void (*az_amqp_session_state_changed_fn)(
    az_amqp_session* session,
    az_amqp_session_state previous_state,
    az_amqp_session_state current_state,
    az_amqp_error_detail const* error,
    void* user_data);

/**
 * @brief Tunable options for an AMQP session.
 */
typedef struct
{
  uint32_t incoming_window; ///< Inbound transfer credit. Default #AZ_AMQP_DEFAULT_INCOMING_WINDOW.
  uint32_t outgoing_window; ///< Outbound transfer budget. Default #AZ_AMQP_DEFAULT_OUTGOING_WINDOW.
  uint32_t handle_max; ///< Highest usable link handle. Default #AZ_AMQP_DEFAULT_HANDLE_MAX.
} az_amqp_session_options;

/**
 * @brief Caller-provided storage backing a session: the link registry.
 */
typedef struct
{
  az_amqp_link** links; ///< Array of link-pointer slots the session registers senders/receivers into.
  int32_t links_capacity; ///< The number of slots in #links.
} az_amqp_session_storage;

/**
 * @brief The AMQP session object. Caller-allocated. All fields are visible and library-managed;
 * treat them as read-only after #az_amqp_session_init.
 */
struct az_amqp_session
{
  az_amqp_connection* connection; ///< The owning connection. Library-managed.
  az_amqp_session_storage storage; ///< The caller-provided link registry.
  az_amqp_session_options options; ///< The effective options.
  az_amqp_session_state state; ///< Current state. Library-managed.
  az_amqp_session_state_changed_fn state_changed; ///< State callback. Library-managed.
  void* state_changed_user_data; ///< State callback context. Library-managed.
  az_amqp_error_detail last_error; ///< Most recent failure detail. Library-managed.
  int32_t link_count; ///< Links currently registered. Library-managed.
  uint16_t local_channel; ///< The local channel number. Library-managed.
  uint16_t remote_channel; ///< The peer's channel number. Library-managed.
  uint32_t next_outgoing_id; ///< Next transfer id to send. Library-managed.
  uint32_t next_incoming_id; ///< Next transfer id expected. Library-managed.
  uint32_t remote_incoming_window; ///< The peer's inbound window. Library-managed.
  uint32_t remote_outgoing_window; ///< The peer's outbound window. Library-managed.
};

/**
 * @brief Gets the default (host-grade) session options.
 *
 * @return An initialized #az_amqp_session_options.
 */
AZ_NODISCARD az_amqp_session_options az_amqp_session_options_default(void);

/**
 * @brief Gets session options tuned for a constrained device (smaller transfer windows).
 *
 * @return An initialized #az_amqp_session_options.
 */
AZ_NODISCARD az_amqp_session_options az_amqp_session_options_for_constrained_device(void);

/**
 * @brief Returns session storage backed by a small static link array (room for the CBS pair plus a
 * data link), sized for a constrained device.
 *
 * @remark Aliases a single file-static instance; use for one session.
 */
AZ_NODISCARD az_amqp_session_storage az_amqp_session_storage_for_constrained_device(void);

/**
 * @brief Returns session storage backed by a larger static link array, sized for a host platform.
 *
 * @remark Aliases a single file-static instance; use for one session.
 */
AZ_NODISCARD az_amqp_session_storage az_amqp_session_storage_for_host(void);

/**
 * @brief Initializes a session and registers it with its connection.
 *
 * @details Does not send `begin`. Call #az_amqp_session_begin once the connection is
 * #AZ_AMQP_CONNECTION_STATE_OPENED.
 *
 * @param[out] session The session to initialize. Must not be `NULL`.
 * @param[in] connection The owning, initialized connection. Must outlive @p session.
 * @param[in] storage The link registry. Its array must outlive @p session.
 * @param[in] options __[nullable]__ Session options, or `NULL` for #az_amqp_session_options_default.
 * @return An #az_result.
 * @retval #AZ_OK Success.
 * @retval #AZ_ERROR_ARG A required argument was `NULL` or the link array was empty.
 * @retval #AZ_ERROR_AMQP_NO_STORAGE The connection's session registry is full.
 */
AZ_NODISCARD az_result az_amqp_session_init(
    az_amqp_session* session,
    az_amqp_connection* connection,
    az_amqp_session_storage const* storage,
    az_amqp_session_options const* options);

/**
 * @brief Registers the callback invoked on every session state transition.
 */
void az_amqp_session_set_state_callback(
    az_amqp_session* session,
    az_amqp_session_state_changed_fn state_changed,
    void* user_data);

/**
 * @brief Sends the `begin` performative to map the session onto a channel.
 *
 * @param[in] session The session. Must not be `NULL`.
 * @return #AZ_OK if `begin` was queued; #AZ_ERROR_AMQP_WRONG_STATE if not unmapped or the connection
 * is not opened.
 */
AZ_NODISCARD az_result az_amqp_session_begin(az_amqp_session* session);

/**
 * @brief Sends the `end` performative to tear the session down.
 *
 * @param[in] session The session. Must not be `NULL`.
 * @param[in] error __[nullable]__ An AMQP error to report to the peer, or `NULL`.
 * @return #AZ_OK if `end` was queued; #AZ_ERROR_AMQP_WRONG_STATE if already ended.
 */
AZ_NODISCARD az_result az_amqp_session_end(az_amqp_session* session, az_amqp_error const* error);

/**
 * @brief Returns the current session state.
 */
AZ_NODISCARD az_amqp_session_state az_amqp_session_get_state(az_amqp_session const* session);

/**
 * @brief Returns the actionable detail of the most recent failure on this session.
 */
AZ_NODISCARD az_amqp_error_detail az_amqp_session_get_last_error(az_amqp_session const* session);

#include <azure/core/_az_cfg_suffix.h>

#endif // _az_AMQP_SESSION_H
