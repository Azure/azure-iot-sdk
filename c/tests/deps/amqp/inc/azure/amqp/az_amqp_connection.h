// Copyright (c) Microsoft Corporation. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file
 *
 * @brief The AMQP 1.0 connection: protocol-header negotiation, optional SASL, the `open`/`close`
 * performatives, idle-timeout handling, and the single non-blocking pump that drives all of the
 * above (and every session and link beneath it).
 *
 * @details The connection is the only object the application pumps. After #az_amqp_connection_init
 * and #az_amqp_connection_open, the application calls #az_amqp_connection_process in a loop. Each
 * call performs as much non-blocking work as possible — reading and writing the transport, parsing
 * frames, dispatching them to sessions and links, emitting heartbeats — then reports, via an
 * #az_amqp_connection_process_result, what transport readiness to wait for and when to pump next.
 *
 * No dynamic memory is used. The connection borrows all of its storage — two frame buffers and the
 * session registry — from a caller-provided #az_amqp_connection_storage. Two helpers
 * (#az_amqp_connection_storage_for_constrained_device and #az_amqp_connection_storage_for_host)
 * return ready-made storage sized for a platform class; power users can build their own.
 *
 * @note Every field of every object in this library is visible and library-managed — there is no
 * hidden `_internal` wrapper and no hidden compile-time capacity. Treat fields documented as
 * "library-managed" as read-only after initialization.
 */

#ifndef _az_AMQP_CONNECTION_H
#define _az_AMQP_CONNECTION_H

#include <azure/amqp/az_amqp_common.h>
#include <azure/amqp/az_amqp_sasl.h>
#include <azure/amqp/az_amqp_transport.h>

#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include <stdbool.h>
#include <stdint.h>

#include <azure/core/_az_cfg_prefix.h>

/// The conventional TCP port for AMQP over TLS.
#define AZ_AMQP_PORT_AMQPS 5671

/// The conventional TCP port for plaintext AMQP.
#define AZ_AMQP_PORT_AMQP 5672

/// The default `max-frame-size` advertised in the `open` performative (§2.7.1) for host platforms.
#define AZ_AMQP_DEFAULT_MAX_FRAME_SIZE 65536u

/// A small `max-frame-size` suitable for constrained devices.
#define AZ_AMQP_CONSTRAINED_MAX_FRAME_SIZE 4096u

/// The default `channel-max` (the highest usable session channel number).
#define AZ_AMQP_DEFAULT_CHANNEL_MAX 255u

/**
 * @brief The lifecycle states of an AMQP connection.
 *
 * @remark The first enumerator is the error state, per the SDK design guidance.
 */
typedef enum
{
  AZ_AMQP_CONNECTION_STATE_ERROR = 0, ///< A fatal transport or protocol error occurred.
  AZ_AMQP_CONNECTION_STATE_IDLE, ///< Initialized but not yet opening.
  AZ_AMQP_CONNECTION_STATE_OPENING, ///< Transport connect, TLS, header exchange, SASL, and `open` are in progress.
  AZ_AMQP_CONNECTION_STATE_OPENED, ///< The peer answered `open`; sessions may be begun.
  AZ_AMQP_CONNECTION_STATE_CLOSING, ///< A local or remote `close` is being completed.
  AZ_AMQP_CONNECTION_STATE_CLOSED, ///< The connection is fully closed; the transport is released.
} az_amqp_connection_state;

/**
 * @brief Invoked whenever the connection transitions between #az_amqp_connection_state values.
 *
 * @param[in] connection The connection that changed state (the "sender" of the event).
 * @param[in] previous_state The state being left.
 * @param[in] current_state The state being entered.
 * @param[in] error __[nullable]__ When transitioning to #AZ_AMQP_CONNECTION_STATE_ERROR or a
 * remote-initiated close, the actionable failure detail (result code, AMQP condition, and transport
 * backend status); otherwise `NULL`. Valid only for the duration of the callback.
 * @param[in] user_data The pointer supplied to #az_amqp_connection_set_state_callback.
 */
typedef void (*az_amqp_connection_state_changed_fn)(
    az_amqp_connection* connection,
    az_amqp_connection_state previous_state,
    az_amqp_connection_state current_state,
    az_amqp_error_detail const* error,
    void* user_data);

/**
 * @brief Tunable options for an AMQP connection. Obtain defaults from
 * #az_amqp_connection_options_default and override as needed.
 */
typedef struct
{
  /// The local container-id sent in `open` (§2.7.1). Should be stable and unique for this client.
  az_span container_id;

  /// The `hostname` field of `open` (the virtual host / SNI-equivalent). When empty, the
  /// transport's host name is used.
  az_span hostname;

  /// The largest frame this peer will accept. Must be <= the size of each frame buffer in the
  /// #az_amqp_connection_storage. Default #AZ_AMQP_DEFAULT_MAX_FRAME_SIZE.
  uint32_t max_frame_size;

  /// The highest session channel number this peer will use. Default #AZ_AMQP_DEFAULT_CHANNEL_MAX.
  uint16_t channel_max;

  /// The idle timeout advertised to the peer, in milliseconds. The connection emits empty
  /// heartbeat frames at roughly half this period and fails with #AZ_ERROR_AMQP_TIMEOUT if the peer
  /// goes silent for longer than the value the peer advertises. `0` disables idle handling.
  uint32_t idle_timeout_milliseconds;

  /// Optional SASL configuration. Default mechanism is #AZ_AMQP_SASL_MECHANISM_NONE (no SASL layer).
  az_amqp_sasl_options sasl;
} az_amqp_connection_options;

/**
 * @brief Caller-provided storage backing a connection: the frame buffers and the session registry.
 *
 * @details This is the explicit replacement for hidden, fixed-size internal arrays. The
 * #az_amqp_connection_storage_for_constrained_device and #az_amqp_connection_storage_for_host
 * helpers return an instance backed by static arrays sized for that platform class; a power user
 * can instead fill this struct from their own buffers and session-pointer array.
 */
typedef struct
{
  az_span incoming_buffer; ///< Scratch for assembling inbound frames. Size >= `options.max_frame_size`.
  az_span outgoing_buffer; ///< Scratch for assembling outbound frames. Size >= `options.max_frame_size`.
  az_amqp_session** sessions; ///< Array of session-pointer slots the connection registers sessions into.
  int32_t sessions_capacity; ///< The number of slots in #sessions.
} az_amqp_connection_storage;

/**
 * @brief The AMQP connection object. Caller-allocated (stack or static). All fields are visible and
 * library-managed; treat them as read-only after #az_amqp_connection_init.
 */
struct az_amqp_connection
{
  az_amqp_transport* transport; ///< The pluggable transport. Library-managed.
  az_amqp_connection_storage storage; ///< The caller-provided buffers and session registry.
  az_amqp_connection_options options; ///< The effective options.
  az_amqp_connection_state state; ///< Current state. Library-managed.
  az_amqp_connection_state_changed_fn state_changed; ///< State callback. Library-managed.
  void* state_changed_user_data; ///< State callback context. Library-managed.
  az_amqp_error_detail last_error; ///< Most recent failure detail. Library-managed.
  int32_t phase; ///< Internal open/close sub-state-machine step. Library-managed.
  int32_t session_count; ///< Sessions currently registered. Library-managed.
  int32_t incoming_length; ///< Bytes buffered in `storage.incoming_buffer`. Library-managed.
  int32_t outgoing_length; ///< Bytes pending in `storage.outgoing_buffer`. Library-managed.
  int64_t last_incoming_frame_msec; ///< Clock of the last inbound frame (idle timer). Library-managed.
  int64_t last_outgoing_frame_msec; ///< Clock of the last outbound frame (heartbeat). Library-managed.
  uint32_t remote_max_frame_size; ///< The peer's advertised max-frame-size. Library-managed.
  uint16_t remote_channel_max; ///< The peer's advertised channel-max. Library-managed.
  uint32_t remote_idle_timeout_milliseconds; ///< The peer's advertised idle timeout. Library-managed.
};

/**
 * @brief What the connection needs from the caller's event loop after a #az_amqp_connection_process
 * call.
 */
typedef struct
{
  /// The transport readiness to wait for before pumping again. May be a bitwise OR of
  /// #AZ_AMQP_IO_INTEREST_READ and #AZ_AMQP_IO_INTEREST_WRITE, or #AZ_AMQP_IO_INTEREST_NONE.
  az_amqp_io_interest io_interest;

  /// A suggested upper bound, in milliseconds, on how long to block waiting for #io_interest before
  /// pumping again anyway (so heartbeats and timeouts fire on schedule). `-1` means "no deadline".
  int32_t next_activity_milliseconds;
} az_amqp_connection_process_result;

/**
 * @brief Gets the default connection options (host-grade): empty container-id (the caller SHOULD set
 * one), #AZ_AMQP_DEFAULT_MAX_FRAME_SIZE, #AZ_AMQP_DEFAULT_CHANNEL_MAX, idle timeout `0`, no SASL.
 *
 * @return An initialized #az_amqp_connection_options.
 */
AZ_NODISCARD az_amqp_connection_options az_amqp_connection_options_default(void);

/**
 * @brief Gets connection options tuned for a constrained device (smaller #AZ_AMQP_CONSTRAINED_MAX_FRAME_SIZE
 * frame, a non-zero idle timeout). Use together with #az_amqp_connection_storage_for_constrained_device.
 *
 * @return An initialized #az_amqp_connection_options.
 */
AZ_NODISCARD az_amqp_connection_options az_amqp_connection_options_for_constrained_device(void);

/**
 * @brief Returns connection storage backed by small static arrays sized for a constrained device
 * (one session, #AZ_AMQP_CONSTRAINED_MAX_FRAME_SIZE buffers).
 *
 * @remark The returned spans/array alias a single file-static instance; use it for one connection.
 * For multiple concurrent connections, provide your own #az_amqp_connection_storage.
 * @return A ready-to-use #az_amqp_connection_storage.
 */
AZ_NODISCARD az_amqp_connection_storage az_amqp_connection_storage_for_constrained_device(void);

/**
 * @brief Returns connection storage backed by larger static arrays sized for a host platform
 * (several sessions, #AZ_AMQP_DEFAULT_MAX_FRAME_SIZE buffers).
 *
 * @remark The returned spans/array alias a single file-static instance; use it for one connection.
 * @return A ready-to-use #az_amqp_connection_storage.
 */
AZ_NODISCARD az_amqp_connection_storage az_amqp_connection_storage_for_host(void);

/**
 * @brief Initializes a connection over a pluggable transport using caller-provided storage.
 *
 * @details Does not perform any I/O. Call #az_amqp_connection_open to begin connecting, then pump
 * with #az_amqp_connection_process.
 *
 * @param[out] connection The connection to initialize. Must not be `NULL`.
 * @param[in] transport A transport already initialized via a factory + #az_amqp_transport_init.
 * Must outlive @p connection.
 * @param[in] storage The frame buffers and session registry. Its arrays must outlive @p connection.
 * @param[in] options __[nullable]__ Connection options, or `NULL` for #az_amqp_connection_options_default.
 * @return An #az_result.
 * @retval #AZ_OK Success.
 * @retval #AZ_ERROR_ARG A required argument was `NULL`, or storage arrays were empty.
 * @retval #AZ_ERROR_NOT_ENOUGH_SPACE A frame buffer is smaller than `max_frame_size`.
 */
AZ_NODISCARD az_result az_amqp_connection_init(
    az_amqp_connection* connection,
    az_amqp_transport* transport,
    az_amqp_connection_storage const* storage,
    az_amqp_connection_options const* options);

/**
 * @brief Registers the callback invoked on every connection state transition.
 *
 * @param[in] connection The connection. Must not be `NULL`.
 * @param[in] state_changed __[nullable]__ The callback, or `NULL` to unsubscribe.
 * @param[in] user_data __[nullable]__ Opaque pointer passed back to @p state_changed.
 */
void az_amqp_connection_set_state_callback(
    az_amqp_connection* connection,
    az_amqp_connection_state_changed_fn state_changed,
    void* user_data);

/**
 * @brief Begins opening the connection: transport connect + TLS, AMQP/SASL protocol-header
 * exchange, optional SASL negotiation, and the `open` performative.
 *
 * @details Non-blocking. Progress happens inside #az_amqp_connection_process; completion is
 * signaled by a transition to #AZ_AMQP_CONNECTION_STATE_OPENED.
 *
 * @param[in] connection The connection. Must not be `NULL`.
 * @return #AZ_OK if opening started; #AZ_ERROR_AMQP_WRONG_STATE if not idle.
 */
AZ_NODISCARD az_result az_amqp_connection_open(az_amqp_connection* connection);

/**
 * @brief Begins a graceful close: detaches/ends are the caller's responsibility, then this sends
 * the `close` performative and shuts the transport down.
 *
 * @param[in] connection The connection. Must not be `NULL`.
 * @param[in] error __[nullable]__ An AMQP error to report to the peer in `close`, or `NULL`.
 * @return #AZ_OK if closing started; #AZ_ERROR_AMQP_WRONG_STATE if already closed.
 */
AZ_NODISCARD az_result
az_amqp_connection_close(az_amqp_connection* connection, az_amqp_error const* error);

/**
 * @brief Performs one non-blocking pump of the connection and everything beneath it.
 *
 * @details Call this whenever the transport becomes readable/writable per the previous result's
 * #az_amqp_connection_process_result.io_interest, or when its
 * #az_amqp_connection_process_result.next_activity_milliseconds elapses. Drives the open/close
 * handshakes, frame I/O, session/link state machines, message delivery callbacks, and heartbeats.
 * On a failing return, #az_amqp_connection_get_last_error has the actionable detail.
 *
 * @param[in] connection The connection. Must not be `NULL`.
 * @param[out] out_result Receives the readiness/timeout the caller's loop should honor next.
 * @return An #az_result.
 * @retval #AZ_OK Progress was made (possibly completing the open/close).
 * @retval #AZ_ERROR_AMQP_DISCONNECTED The peer closed the connection or transport.
 * @retval #AZ_ERROR_AMQP_TIMEOUT The idle timeout elapsed without peer activity.
 * @retval #AZ_ERROR_AMQP_PROTOCOL A protocol violation was received.
 * @retval #AZ_ERROR_AMQP_TRANSPORT The transport reported a fatal error.
 */
AZ_NODISCARD az_result az_amqp_connection_process(
    az_amqp_connection* connection,
    az_amqp_connection_process_result* out_result);

/**
 * @brief Returns the current connection state.
 *
 * @param[in] connection The connection. Must not be `NULL`.
 * @return The current #az_amqp_connection_state.
 */
AZ_NODISCARD az_amqp_connection_state az_amqp_connection_get_state(az_amqp_connection const* connection);

/**
 * @brief Returns the actionable detail of the most recent failure on this connection.
 *
 * @details Combines the #az_result, any AMQP error condition the peer reported, and the transport
 * backend's native status code/message (pulled from #az_amqp_transport_vtable.get_last_error). Call
 * this after a failing #az_amqp_connection_process or a transition to #AZ_AMQP_CONNECTION_STATE_ERROR.
 *
 * @param[in] connection The connection. Must not be `NULL`.
 * @return The most recent #az_amqp_error_detail. Its #az_amqp_error_detail.message span is valid
 * until the next operation on @p connection.
 */
AZ_NODISCARD az_amqp_error_detail az_amqp_connection_get_last_error(az_amqp_connection const* connection);

#include <azure/core/_az_cfg_suffix.h>

#endif // _az_AMQP_CONNECTION_H
