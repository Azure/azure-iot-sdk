// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief The AMQP 1.0 link: one object for both directions, distinguished by an #az_amqp_role
 * (OASIS AMQP 1.0 §2.6). A link attaches a local terminus to a remote one, transfers messages,
 * applies flow control, and carries dispositions.
 *
 * @details The spec's transport primitive is a single "link with a role", so this client models it
 * as a single #az_amqp_link. A *sender-role* link transfers messages (#az_amqp_link_send) and is
 * told its delivery outcomes; a *receiver-role* link issues credit and surfaces incoming messages
 * to a callback, which the application settles with accept/reject/release/modify. Role-specific
 * options are filled ergonomically by #az_amqp_link_sender_options_default /
 * #az_amqp_link_receiver_options_default. A link is attached on a mapped session and driven by the
 * owning connection's #az_amqp_connection_process pump. All storage is caller-provided; no dynamic
 * memory is used. Every field is visible and library-managed (no hidden `_internal` wrapper).
 */

#ifndef _az_AMQP_LINK_H
#define _az_AMQP_LINK_H

#include <azure/amqp/az_amqp_common.h>
#include <azure/amqp/az_amqp_message.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

/**
 * @brief The `source` terminus of a link (§3.5.3): the node messages are read from.
 */
typedef struct
{
  az_span address; ///< The node address (string). Empty for an anonymous or dynamic source.
  az_amqp_terminus_durability durable; ///< Terminus durability. Default NONE.
  az_amqp_terminus_expiry_policy expiry_policy; ///< Expiry policy. Default SESSION_END.
  uint32_t timeout_seconds; ///< Duration the terminus survives after expiry begins.
  bool dynamic; ///< Request that the peer create a dynamic node and report its address.
  az_span dynamic_node_properties; ///< Encoded map of properties for the dynamic node, or empty.
  az_span distribution_mode; ///< Symbol `move` or `copy`, or empty for the peer default.
  az_span filter; ///< Encoded filter-set map (e.g. a selector), or empty.
  az_amqp_delivery_outcome default_outcome; ///< Outcome applied if none is stated. Default NONE.
  az_span capabilities; ///< Encoded symbol/array of extension capabilities, or empty.
} az_amqp_source;

/**
 * @brief The `target` terminus of a link (§3.5.4): the node messages are written to.
 */
typedef struct
{
  az_span address; ///< The node address (string). Empty for an anonymous or dynamic target.
  az_amqp_terminus_durability durable; ///< Terminus durability. Default NONE.
  az_amqp_terminus_expiry_policy expiry_policy; ///< Expiry policy. Default SESSION_END.
  uint32_t timeout_seconds; ///< Duration the terminus survives after expiry begins.
  bool dynamic; ///< Request that the peer create a dynamic node and report its address.
  az_span dynamic_node_properties; ///< Encoded map of properties for the dynamic node, or empty.
  az_span capabilities; ///< Encoded symbol/array of extension capabilities, or empty.
} az_amqp_target;

/**
 * @brief The lifecycle states of a link.
 *
 * @remark The first enumerator is the error state, per the SDK design guidance.
 */
typedef enum
{
  AZ_AMQP_LINK_STATE_ERROR = 0, ///< The link failed (e.g. the peer detached it with an error).
  AZ_AMQP_LINK_STATE_DETACHED, ///< Initialized or fully detached; not attached to the peer.
  AZ_AMQP_LINK_STATE_ATTACHING, ///< `attach` sent; awaiting the peer's `attach`.
  AZ_AMQP_LINK_STATE_ATTACHED, ///< Both ends attached; transfers and flow may occur.
  AZ_AMQP_LINK_STATE_DETACHING, ///< `detach` is being completed.
} az_amqp_link_state;

/**
 * @brief Identifies a single delivery on a link, as surfaced to callbacks and disposition calls.
 */
typedef struct
{
  uint32_t number; ///< The delivery-id assigned by the sending end.
  az_span tag; ///< The delivery-tag bytes (alias a library buffer; valid only during the callback).
  bool settled; ///< Whether the sending end pre-settled this delivery.
} az_amqp_delivery;

/**
 * @brief One slot of caller-provided storage for tracking a single unsettled (sender-role) delivery.
 *
 * @details Provide an array of these via #az_amqp_link_options.unsettled_storage on a sender-role
 * link. A sender configured for #AZ_AMQP_SENDER_SETTLE_MODE_SETTLED needs no slots. Library-managed.
 */
typedef struct
{
  uint32_t delivery_id; ///< The delivery-id assigned to this transfer. Library-managed.
  az_span delivery_tag; ///< The caller's delivery tag. Library-managed.
  void* on_complete; ///< Completion callback (an #az_amqp_link_send_complete_callback). Library-managed.
  void* user_data; ///< Completion callback context. Library-managed.
  bool in_use; ///< Whether this slot is occupied. Library-managed.
} az_amqp_link_unsettled;

/// @name Link callbacks (all receive the #az_amqp_link as the first argument)
/// @{

/**
 * @brief Invoked whenever the link transitions between #az_amqp_link_state values.
 *
 * @param[in] error __[nullable]__ Actionable detail when entering an error/remote-detach state.
 */
typedef void (*az_amqp_link_state_changed_callback)(
    az_amqp_link* link,
    az_amqp_link_state previous_state,
    az_amqp_link_state current_state,
    az_amqp_error_detail const* error,
    void* user_data);

/**
 * @brief (Sender role) Invoked when a sent delivery reaches a terminal delivery-state.
 *
 * @param[in] delivery_tag The tag supplied to #az_amqp_link_send (aliases caller memory).
 * @param[in] delivery_state The full terminal state (outcome + rejected error / modified flags).
 */
typedef void (*az_amqp_link_send_complete_callback)(
    az_amqp_link* link,
    az_span delivery_tag,
    az_amqp_delivery_state const* delivery_state,
    void* user_data);

/**
 * @brief (Sender role) Invoked when the peer grants the link additional credit to send.
 */
typedef void (*az_amqp_link_credit_available_callback)(
    az_amqp_link* link,
    uint32_t available_credit,
    void* user_data);

/**
 * @brief (Receiver role) Invoked when a complete message has been received and decoded.
 *
 * @details @p message and @p delivery view the link's reassembly buffer and are valid only for the
 * duration of the callback; copy out anything you need to retain, then settle the delivery.
 */
typedef void (*az_amqp_link_message_received_callback)(
    az_amqp_link* link,
    az_amqp_message const* message,
    az_amqp_delivery const* delivery,
    void* user_data);

/// @}

/**
 * @brief Tunable options for a link. Role-specific fields are read only when relevant to the
 * #role. Use #az_amqp_link_sender_options_default / #az_amqp_link_receiver_options_default to fill
 * the common cases in one call.
 */
typedef struct
{
  az_amqp_role role; ///< Whether this is a sending or receiving link.
  az_span name; ///< Link name, unique per role on the session. Empty to auto-derive.
  az_amqp_source source; ///< Receiver: the remote source. Sender: local (often anonymous).
  az_amqp_target target; ///< Sender: the remote target. Receiver: local (often anonymous).
  az_amqp_sender_settle_mode sender_settle_mode; ///< Sender settlement policy.
  az_amqp_receiver_settle_mode receiver_settle_mode; ///< Receiver settlement policy.
  uint64_t max_message_size; ///< Largest message for the link (0 = no explicit limit).

  /// (Sender role) Caller-provided array tracking in-flight unsettled deliveries. Required unless
  /// the sender uses #AZ_AMQP_SENDER_SETTLE_MODE_SETTLED.
  az_amqp_link_unsettled* unsettled_storage;
  int32_t unsettled_capacity; ///< (Sender role) The number of slots in #unsettled_storage.

  /// (Receiver role) Caller-owned buffer used to reassemble incoming (possibly multi-frame)
  /// messages before they are decoded and surfaced. Required for a receiver.
  az_span message_buffer;
  /// (Receiver role) When non-zero, the library keeps link credit topped up to this value. `0`
  /// means manual flow via #az_amqp_link_add_credit.
  uint32_t prefetch_credit;
} az_amqp_link_options;

/**
 * @brief The AMQP link object. Caller-allocated. The owning session keeps an array of
 * `az_amqp_link*`. All fields are visible and library-managed; treat them as read-only after init.
 */
struct az_amqp_link
{
  az_amqp_session* session; ///< The owning session. Library-managed.
  az_amqp_link_options options; ///< The effective options.
  az_amqp_link_state state; ///< Current lifecycle state. Library-managed.
  az_amqp_link_state_changed_callback state_changed; ///< State callback. Library-managed.
  void* state_changed_user_data; ///< State callback context. Library-managed.
  az_amqp_link_credit_available_callback credit_available; ///< (Sender) credit callback. Library-managed.
  void* credit_available_user_data; ///< (Sender) credit callback context. Library-managed.
  az_amqp_link_message_received_callback message_received; ///< (Receiver) message callback. Library-managed.
  void* message_received_user_data; ///< (Receiver) message callback context. Library-managed.
  uint32_t handle; ///< The local link handle. Library-managed.
  uint32_t remote_handle; ///< The peer's link handle. Library-managed.
  uint32_t delivery_count; ///< The link delivery-count. Library-managed.
  uint32_t link_credit; ///< Current link credit. Library-managed.
  int32_t partial_length; ///< (Receiver) bytes accumulated for the in-progress message. Library-managed.
  uint32_t current_delivery_id; ///< (Receiver) the delivery-id being assembled. Library-managed.
  az_amqp_error_detail last_error; ///< The most recent failure detail. Library-managed.
};

/// @name Terminus constructors
/// @{
AZ_NODISCARD az_amqp_source az_amqp_source_default(void);
AZ_NODISCARD az_amqp_source az_amqp_source_from_address(az_span address);
AZ_NODISCARD az_amqp_target az_amqp_target_default(void);
AZ_NODISCARD az_amqp_target az_amqp_target_from_address(az_span address);
/// @}

/// @name Option constructors
/// @{

/**
 * @brief Returns role-agnostic default link options (empty termini, unsettled/first settle modes,
 * no storage). Set #az_amqp_link_options.role and the role's fields, or use a role helper below.
 */
AZ_NODISCARD az_amqp_link_options az_amqp_link_options_default(void);

/**
 * @brief Returns sender-role link options filled from the common parameters.
 *
 * @param[in] name The link name (e.g. `AZ_SPAN_FROM_STR("my-sender")`).
 * @param[in] target The remote target the messages are sent to.
 * @param[in] sender_settle_mode The settlement policy.
 * @param[in] unsettled_storage Caller array tracking in-flight deliveries (may be `NULL` for a
 * settled sender).
 * @param[in] unsettled_capacity The number of slots in @p unsettled_storage.
 * @return An initialized #az_amqp_link_options with role #AZ_AMQP_ROLE_SENDER.
 */
AZ_NODISCARD az_amqp_link_options az_amqp_link_sender_options_default(
    az_span name,
    az_amqp_target target,
    az_amqp_sender_settle_mode sender_settle_mode,
    az_amqp_link_unsettled* unsettled_storage,
    int32_t unsettled_capacity);

/**
 * @brief Returns receiver-role link options filled from the common parameters.
 *
 * @param[in] name The link name.
 * @param[in] source The remote source the messages are read from.
 * @param[in] receiver_settle_mode The settlement policy.
 * @param[in] message_buffer Caller buffer used to reassemble incoming messages.
 * @param[in] prefetch_credit Auto-maintained credit (`0` for manual flow).
 * @return An initialized #az_amqp_link_options with role #AZ_AMQP_ROLE_RECEIVER.
 */
AZ_NODISCARD az_amqp_link_options az_amqp_link_receiver_options_default(
    az_span name,
    az_amqp_source source,
    az_amqp_receiver_settle_mode receiver_settle_mode,
    az_span message_buffer,
    uint32_t prefetch_credit);

/// @}

/**
 * @brief Initializes a link and registers it with its session.
 *
 * @param[out] link The link to initialize. Must not be `NULL`.
 * @param[in] session The owning, initialized session. Must outlive @p link.
 * @param[in] options Link options (role required; see the role helpers).
 * @return #AZ_OK; #AZ_ERROR_ARG on a `NULL`/invalid argument; #AZ_ERROR_AMQP_NO_STORAGE if the
 * session's link registry is full.
 */
AZ_NODISCARD az_result az_amqp_link_init(
    az_amqp_link* link,
    az_amqp_session* session,
    az_amqp_link_options const* options);

/// @name Callback registration
/// @{
void az_amqp_link_set_state_callback(
    az_amqp_link* link,
    az_amqp_link_state_changed_callback state_changed,
    void* user_data);
/// (Sender role) Invoked when the peer grants credit.
void az_amqp_link_set_credit_callback(
    az_amqp_link* link,
    az_amqp_link_credit_available_callback credit_available,
    void* user_data);
/// (Receiver role) Invoked for each received message.
void az_amqp_link_set_message_callback(
    az_amqp_link* link,
    az_amqp_link_message_received_callback message_received,
    void* user_data);
/// @}

/**
 * @brief Sends the `attach` performative to attach the link.
 *
 * @return #AZ_OK if `attach` was queued; #AZ_ERROR_AMQP_WRONG_STATE if not detached or the session
 * is not mapped.
 */
AZ_NODISCARD az_result az_amqp_link_attach(az_amqp_link* link);

/**
 * @brief Sends the `detach` performative to detach the link.
 *
 * @param[in] error __[nullable]__ An AMQP error to report to the peer, or `NULL`.
 * @return #AZ_OK if `detach` was queued; #AZ_ERROR_AMQP_WRONG_STATE if not attached.
 */
AZ_NODISCARD az_result az_amqp_link_detach(az_amqp_link* link, az_amqp_error const* error);

/**
 * @brief Returns the current link state.
 */
AZ_NODISCARD az_amqp_link_state az_amqp_link_get_state(az_amqp_link const* link);

/**
 * @brief Returns the actionable detail of the most recent failure on this link.
 */
AZ_NODISCARD az_amqp_error_detail az_amqp_link_get_last_error(az_amqp_link const* link);

/**
 * @brief (Sender role) Returns the credit currently available to send.
 */
AZ_NODISCARD uint32_t az_amqp_link_get_credit(az_amqp_link const* link);

/**
 * @brief (Sender role) Transfers a message to the peer.
 *
 * @details Requires available credit (see #az_amqp_link_get_credit). For a settled sender,
 * @p on_complete (if any) fires with #AZ_AMQP_DELIVERY_OUTCOME_ACCEPTED once written; for an
 * unsettled sender it fires later with the peer's full delivery-state.
 *
 * @param[in] link A #AZ_AMQP_ROLE_SENDER link in state #AZ_AMQP_LINK_STATE_ATTACHED.
 * @param[in] message The message to send.
 * @param[in] delivery_tag A short, link-unique tag (1–32 bytes). Referenced, not copied.
 * @param[in] on_complete __[nullable]__ Invoked with the terminal delivery-state.
 * @param[in] user_data __[nullable]__ Opaque pointer passed back to @p on_complete.
 * @return #AZ_OK; #AZ_ERROR_AMQP_NO_CREDIT; #AZ_ERROR_AMQP_WRONG_STATE (not attached, or a receiver
 * link); #AZ_ERROR_AMQP_NO_STORAGE; #AZ_ERROR_NOT_ENOUGH_SPACE.
 */
AZ_NODISCARD az_result az_amqp_link_send(
    az_amqp_link* link,
    az_amqp_message const* message,
    az_span delivery_tag,
    az_amqp_link_send_complete_callback on_complete,
    void* user_data);

/**
 * @brief (Receiver role) Grants additional link credit to the peer (issues a `flow`).
 *
 * @return #AZ_OK; #AZ_ERROR_AMQP_WRONG_STATE if not attached or not a receiver link.
 */
AZ_NODISCARD az_result az_amqp_link_add_credit(az_amqp_link* link, uint32_t credit);

/// @name Dispositions (receiver role)
/// Settle a previously received delivery by its #az_amqp_delivery.number.
/// @{
AZ_NODISCARD az_result az_amqp_link_accept(az_amqp_link* link, uint32_t delivery_number);
/// @param[in] error __[nullable]__ The error condition to report, or `NULL`.
AZ_NODISCARD az_result
az_amqp_link_reject(az_amqp_link* link, uint32_t delivery_number, az_amqp_error const* error);
AZ_NODISCARD az_result az_amqp_link_release(az_amqp_link* link, uint32_t delivery_number);
/// @param[in] message_annotations __[nullable]__ Encoded map of annotations to merge, or empty.
AZ_NODISCARD az_result az_amqp_link_modify(
    az_amqp_link* link,
    uint32_t delivery_number,
    bool delivery_failed,
    bool undeliverable_here,
    az_span message_annotations);
/// @}

#include <azure/core/_az_cfg_suffix.h>

#endif // _az_AMQP_LINK_H
