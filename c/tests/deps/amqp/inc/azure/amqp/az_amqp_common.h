// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief Definitions shared across the AMQP 1.0 client: result codes, protocol enumerations, error
 * details, and small value types that appear in more than one object's API.
 *
 * @details This file is broker-agnostic: it contains only definitions from the OASIS AMQP 1.0
 * specification (http://docs.oasis-open.org/amqp/core/v1.0/os/amqp-core-complete-v1.0-os.html). No
 * dynamic memory is allocated by any function declared here; all buffers are caller-provided
 * #az_span instances.
 *
 * @note The public objects in this library expose their fields directly (there is no hidden
 * `_internal` wrapper). Fields documented as "library-managed" are owned by the implementation and
 * must be treated as read-only by the application after initialization.
 */

#ifndef _az_AMQP_COMMON_H
#define _az_AMQP_COMMON_H

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

/**
 * @brief The facility identifier reserved for AMQP client #az_result values.
 *
 * @remark Chosen to avoid collision with the facilities defined by Azure Core (`az_result.h`).
 */
enum
{
  _az_FACILITY_AMQP = 0x41
};

/// @name Object forward declarations
/// Declared once here so the headers can reference each other (connection ⇄ session ⇄ link) without
/// duplicate `typedef`s. Each object's own header defines the corresponding `struct` body.
/// @{
typedef struct az_amqp_connection az_amqp_connection;
typedef struct az_amqp_session az_amqp_session;
typedef struct az_amqp_link az_amqp_link;
typedef struct az_amqp_cbs az_amqp_cbs;
/// @}

/**
 * @brief AMQP-specific #az_result success and error conditions.
 */
enum az_result_amqp
{
  // === AMQP: Success results ===

  /// The non-blocking operation made progress but is not yet complete; call the relevant
  /// `_process` function again after the transport signals readiness.
  AZ_AMQP_PENDING = _az_RESULT_MAKE_SUCCESS(_az_FACILITY_AMQP, 1),

  // === AMQP: Error results ===

  /// The pluggable transport reported an unrecoverable I/O or TLS failure. Obtain backend detail
  /// from the relevant `_get_last_error` accessor.
  AZ_ERROR_AMQP_TRANSPORT = _az_RESULT_MAKE_ERROR(_az_FACILITY_AMQP, 1),

  /// A received frame or performative violated the AMQP 1.0 protocol.
  AZ_ERROR_AMQP_PROTOCOL = _az_RESULT_MAKE_ERROR(_az_FACILITY_AMQP, 2),

  /// A frame larger than the negotiated `max-frame-size` (or the caller's buffer) was encountered.
  AZ_ERROR_AMQP_FRAME_TOO_LARGE = _az_RESULT_MAKE_ERROR(_az_FACILITY_AMQP, 3),

  /// The AMQP type-system bytes could not be decoded.
  AZ_ERROR_AMQP_DECODE = _az_RESULT_MAKE_ERROR(_az_FACILITY_AMQP, 4),

  /// The peer closed the connection, ended the session, or detached the link unexpectedly.
  AZ_ERROR_AMQP_DISCONNECTED = _az_RESULT_MAKE_ERROR(_az_FACILITY_AMQP, 5),

  /// SASL negotiation failed (bad credentials or unsupported mechanism).
  AZ_ERROR_AMQP_SASL = _az_RESULT_MAKE_ERROR(_az_FACILITY_AMQP, 6),

  /// A claims-based-security (CBS) put-token request was rejected by the peer.
  AZ_ERROR_AMQP_CBS = _az_RESULT_MAKE_ERROR(_az_FACILITY_AMQP, 7),

  /// The operation timed out (for example, the idle timeout elapsed, or an open/begin/attach was
  /// not answered within the configured window).
  AZ_ERROR_AMQP_TIMEOUT = _az_RESULT_MAKE_ERROR(_az_FACILITY_AMQP, 8),

  /// The object is not in a state that permits the requested operation (e.g. send before the
  /// link is attached).
  AZ_ERROR_AMQP_WRONG_STATE = _az_RESULT_MAKE_ERROR(_az_FACILITY_AMQP, 9),

  /// A sender attempted to transfer a message but the link has no available credit.
  AZ_ERROR_AMQP_NO_CREDIT = _az_RESULT_MAKE_ERROR(_az_FACILITY_AMQP, 10),

  /// No free slot remained in caller-provided storage (e.g. too many sessions for the connection's
  /// session array, or too many links for the session's link array). Enlarge the storage.
  AZ_ERROR_AMQP_NO_STORAGE = _az_RESULT_MAKE_ERROR(_az_FACILITY_AMQP, 11),

  /// The peer rejected a transferred message (the delivery reached the `rejected` outcome).
  AZ_ERROR_AMQP_REJECTED = _az_RESULT_MAKE_ERROR(_az_FACILITY_AMQP, 12),
};

/**
 * @brief The directionality of an AMQP link relative to the local node.
 *
 * @remark Matches the boolean `role` field on the `attach` performative (§2.7.3): a sender has
 * role `false`, a receiver has role `true`.
 */
typedef enum
{
  AZ_AMQP_ROLE_SENDER = 0, ///< The local node is the source; it transfers messages to the peer.
  AZ_AMQP_ROLE_RECEIVER = 1, ///< The local node is the target; it receives messages from the peer.
} az_amqp_role;

/**
 * @brief Settlement policy a sender applies to its deliveries (`snd-settle-mode`, §2.6.2).
 */
typedef enum
{
  AZ_AMQP_SENDER_SETTLE_MODE_UNSETTLED = 0, ///< The sender will wait for the receiver's disposition.
  AZ_AMQP_SENDER_SETTLE_MODE_SETTLED = 1, ///< The sender settles immediately ("fire and forget").
  AZ_AMQP_SENDER_SETTLE_MODE_MIXED = 2, ///< The sender may send a mix of settled and unsettled.
} az_amqp_sender_settle_mode;

/**
 * @brief Settlement policy a receiver applies to its deliveries (`rcv-settle-mode`, §2.6.2).
 */
typedef enum
{
  AZ_AMQP_RECEIVER_SETTLE_MODE_FIRST = 0, ///< The receiver settles as soon as it sends a disposition.
  AZ_AMQP_RECEIVER_SETTLE_MODE_SECOND = 1, ///< The receiver waits for the sender to settle first.
} az_amqp_receiver_settle_mode;

/**
 * @brief Durability requirement for a link's source or target terminus (`terminus-durability`,
 * §3.5.5).
 */
typedef enum
{
  AZ_AMQP_TERMINUS_DURABILITY_NONE = 0, ///< No terminus state is retained durably.
  AZ_AMQP_TERMINUS_DURABILITY_CONFIGURATION = 1, ///< Only the terminus configuration is durable.
  AZ_AMQP_TERMINUS_DURABILITY_UNSETTLED_STATE = 2, ///< Configuration and unsettled state are durable.
} az_amqp_terminus_durability;

/**
 * @brief When the contents of a terminus are to be expired (`terminus-expiry-policy`, §3.5.6).
 */
typedef enum
{
  AZ_AMQP_TERMINUS_EXPIRY_POLICY_LINK_DETACH = 0, ///< Expire when the link is detached.
  AZ_AMQP_TERMINUS_EXPIRY_POLICY_SESSION_END = 1, ///< Expire when the owning session ends.
  AZ_AMQP_TERMINUS_EXPIRY_POLICY_CONNECTION_CLOSE = 2, ///< Expire when the connection closes.
  AZ_AMQP_TERMINUS_EXPIRY_POLICY_NEVER = 3, ///< The terminus never expires.
} az_amqp_terminus_expiry_policy;

/**
 * @brief The terminal outcome applied to a delivery (the delivery-state subset that represents an
 * outcome, §3.4).
 */
typedef enum
{
  AZ_AMQP_DELIVERY_OUTCOME_NONE = 0, ///< No terminal outcome yet (the delivery is still in flight).
  AZ_AMQP_DELIVERY_OUTCOME_ACCEPTED = 1, ///< `accepted` (§3.4.2): processed successfully.
  AZ_AMQP_DELIVERY_OUTCOME_REJECTED = 2, ///< `rejected` (§3.4.3): could not be processed; see error.
  AZ_AMQP_DELIVERY_OUTCOME_RELEASED = 3, ///< `released` (§3.4.4): not processed; make re-deliverable.
  AZ_AMQP_DELIVERY_OUTCOME_MODIFIED = 4, ///< `modified` (§3.4.5): not processed; modify annotations.
} az_amqp_delivery_outcome;

/**
 * @brief A bit field describing what transport readiness the AMQP connection is waiting on.
 *
 * @details Returned by #az_amqp_connection_process so the caller's event loop knows whether to
 * wait for the underlying socket to become readable, writable, or both, before pumping again.
 * The values are powers of two and may be combined with bitwise OR.
 */
typedef enum
{
  AZ_AMQP_IO_INTEREST_NONE = 0, ///< No I/O is pending; the caller may pump again after `next_activity`.
  AZ_AMQP_IO_INTEREST_READ = 1, ///< Pump again when the transport is readable.
  AZ_AMQP_IO_INTEREST_WRITE = 2, ///< Pump again when the transport is writable.
} az_amqp_io_interest;

/**
 * @brief An AMQP `error` (§2.8.15): a symbolic condition plus optional human-readable description.
 *
 * @details Both fields are #az_span views into a library decode buffer and remain valid only for
 * the duration of the callback that surfaces them. Copy them if you need to retain them.
 */
typedef struct
{
  az_span condition; ///< The symbolic error condition, e.g. `amqp:link:detach-forced`. Empty if none.
  az_span description; ///< A human-readable explanation, or an empty span.
} az_amqp_error;

/**
 * @brief A consolidated, actionable description of a failure, surfaced to state-changed callbacks
 * and available from each object's `_get_last_error` accessor.
 *
 * @details This is the single place an application looks to understand "what went wrong and what
 * can I do about it":
 *   - #code is the #az_result the operation would return.
 *   - #amqp carries the peer's AMQP error condition/description when the failure came from the
 *     protocol (e.g. the broker detached a link with `amqp:resource-limit-exceeded`).
 *   - #transport_status carries the backend's native code (e.g. an `errno`, a Winsock
 *     `WSAGetLastError`, an OpenSSL error, or a Schannel `SECURITY_STATUS`) when the failure came
 *     from the transport; it is `0` otherwise.
 *   - #message is a short human-readable summary owned by the library, valid only for the duration
 *     of the callback (or until the next call that updates it).
 */
typedef struct
{
  az_result code; ///< The #az_result categorizing the failure (e.g. #AZ_ERROR_AMQP_TRANSPORT).
  az_amqp_error amqp; ///< The AMQP protocol error, when applicable (empty condition otherwise).
  int32_t transport_status; ///< The transport backend's native status code, or `0` if not applicable.
  az_span message; ///< A short, human-readable summary. Empty if none.
} az_amqp_error_detail;

/**
 * @brief The full delivery-state the peer applied to a sent message (§3.4), reported to a sender's
 * completion callback.
 *
 * @details Beyond the terminal #outcome, this exposes the additional information AMQP attaches to
 * the `rejected` and `modified` states so the application can react precisely (retry, dead-letter,
 * re-annotate, etc.).
 */
typedef struct
{
  az_amqp_delivery_outcome outcome; ///< The terminal outcome.
  az_amqp_error error; ///< For #AZ_AMQP_DELIVERY_OUTCOME_REJECTED: the peer's error; empty otherwise.
  bool delivery_failed; ///< `modified`: the peer wants the message's delivery-count incremented.
  bool undeliverable_here; ///< `modified`: the message should not be redelivered to this link.
  az_span message_annotations; ///< `modified`: encoded map of annotations to merge, or empty.
} az_amqp_delivery_state;

#include <azure/core/_az_cfg_suffix.h>

#endif // _az_AMQP_COMMON_H
