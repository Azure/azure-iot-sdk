// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Internal-only API for the connection client. NOT part of the public ABI.
 * Consumers: feature clients (telemetry, twin, direct method), unit tests. */
#ifndef AZ_IOT_CONNECTION_CLIENT_INTERNAL_H
#define AZ_IOT_CONNECTION_CLIENT_INTERNAL_H

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"

#include "internal/dispatch.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* Override the session role used by the next open(). Direct-host opens default
   * to HUB_MQTT_V3 (v3.1.1); DPS calls this with the assigned role before driving
   * the post-provisioning open. */
  az_iot_result az_iot_connection_client__set_session_role(
      az_iot_connection_client* client,
      az_iot_mqtt_role role);

  /** @brief Copies @p iothub_hostname into the client's fixed buffer and points
   * opts.iothub_hostname at it (no heap).
   * @return AZ_IOT_ERR_ALREADY_INITIALIZED unless fully IDLE;
   * AZ_IOT_ERR_INVALID_ARG for a NULL client or a NULL/empty name;
   * AZ_IOT_ERR_NOT_ENOUGH_SPACE if it does not fit. */
  az_iot_result az_iot_connection_client__set_iothub_hostname(
      az_iot_connection_client* client,
      const char* iothub_hostname);

  /** @brief As az_iot_connection_client__set_iothub_hostname(), for
   * opts.device_id. */
  az_iot_result az_iot_connection_client__set_device_id(
      az_iot_connection_client* client,
      const char* device_id);

  /* Seed the jitter PRNG used by the reconnect logic. Tests use this to make
   * backoff timing deterministic. Production builds seed automatically from a
   * monotonic clock during create(). */
  void az_iot_connection_client__seed_rng(az_iot_connection_client* client, uint64_t seed);

  /**
   * @brief Usable SAS token area of a @p buffer_size sas_buffer holding
   * @p key_count keys: the bytes after scratch and key slots, capped at
   * INT32_MAX (az_span sizes are int32_t).
   *
   * @param[in] buffer_size Bytes in sas_buffer; at least
   *                        AZ_IOT_SAS_BUFFER_SIZE(key_count, 0).
   * @param[in] key_count   Distinct keys.
   * @return Token area bytes.
   */
  size_t az_iot_connection_client__sas_token_area(size_t buffer_size, size_t key_count);

  /* Test seam: force any in-flight MQTTv5 presence (birth) handshake to time
   * out on the next do_work(). No-op when no handshake is active. Lets unit tests
   * exercise the SUBACK and birth-ack timeout paths without waiting them out. */
  void az_iot_connection_client__presence_force_timeout(az_iot_connection_client* client);

  /**
   * @brief Replace the random source of defensive-timeout jitter.
   *
   * Test seam: makes az_iot_connection_client__defensive_timeout_ms()
   * deterministic.
   *
   * @param[in,out] client The client.
   * @param[in] source Returns a uniformly distributed value; NULL restores the
   *                   default (the client's PRNG).
   * @param[in] context Passed to @p source.
   */
  void az_iot_connection_client__set_defensive_timeout_random_source(
      az_iot_connection_client* client,
      uint64_t (*source)(void* context),
      void* context);

  /**
   * @brief Replace the defensive-timeout schedule.
   *
   * Test seam: e2e tests use a seconds-scale schedule so a missed birth-ack
   * recovers within their connect windows.
   *
   * @param[in,out] client The client.
   * @param[in] base_ms_by_attempt Base of attempts 1..@p attempt_count; the last
   *                               entry repeats. Must outlive the client. NULL
   *                               restores the spec's schedule.
   * @param[in] attempt_count Entries in @p base_ms_by_attempt; 0 only with NULL.
   * @param[in] jitter_max_ms Largest jitter added to the base.
   * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG for a NULL @p client, or when
   *         exactly one of @p base_ms_by_attempt and @p attempt_count is
   *         NULL / 0.
   */
  az_iot_result az_iot_connection_client__set_defensive_timeout_schedule(
      az_iot_connection_client* client,
      const uint32_t* base_ms_by_attempt,
      size_t attempt_count,
      uint32_t jitter_max_ms);

  /**
   * @brief mqttv5 defensive timeout of 1-based @p attempt, with jitter drawn
   * from the client's random source.
   *
   * Arms the presence birth-ack timeout; for any wait on a service message
   * that must follow the same schedule (see internal/defensive_timeout.h).
   *
   * @param[in,out] client The client; its PRNG advances unless a source is set.
   * @param[in] attempt Attempt number (0 is treated as 1).
   * @return The timeout in milliseconds; 5 to 15 min unless
   *         az_iot_connection_client__set_defensive_timeout_schedule() replaced
   *         the schedule.
   */
  uint32_t az_iot_connection_client__defensive_timeout_ms(
      az_iot_connection_client* client,
      uint32_t attempt);

  /* Authoritative twin versions the service reported on the most recent MQTTv5
   * birth-ack, as of birth admission on the current connection. The twin client
   * uses `reported_version` as the if_match anchor for its next reported patch
   * and `desired_version` as its view of the current desired version. Both are 0
   * before the first birth-ack, on MQTTv3/DPS sessions, and when the service
   * omits the fields. Returns AZ_IOT_ERR_INVALID_ARG on NULL arguments. */
  az_iot_result az_iot_connection_client__presence_twin_versions(
      const az_iot_connection_client* client,
      uint64_t* out_desired_version,
      uint64_t* out_reported_version);

/* Width of the MQTT v5 Correlation Data every MQTTv5 flow uses: a 16-byte UUID,
 * for the connection's birth nonce and for per-request correlation ids alike. */
#define AZ_IOT_CORRELATION_UUID_LEN 16u

  /* Fill `out` with a fresh RFC 4122 version 4 UUID from the client's PRNG.
   * Feature clients use this for the per-attempt Correlation Data that the MQTTv5
   * request/response flows (twin GET, reported patch, ...) require. */
  void az_iot_connection_client__gen_uuid(
      az_iot_connection_client* client,
      uint8_t out[AZ_IOT_CORRELATION_UUID_LEN]);

  /* Copy the current connection's birth nonce into `out`. Backend-initiated
   * dev-bound messages (twin-push, desired-patch) carry it as Correlation Data
   * so the device can tell traffic for this connection from traffic left over
   * from a defunct one. Returns AZ_IOT_ERR_NOT_CONNECTED when no presence
   * handshake has completed on this connection (MQTTv3/DPS sessions included). */
  az_iot_result az_iot_connection_client__presence_nonce(
      const az_iot_connection_client* client,
      uint8_t out[AZ_IOT_CORRELATION_UUID_LEN]);

  /* As az_iot_connection_client__presence_nonce(), but available once the birth is
   * sent: the service may send dev-bound messages before the birth-ack. Not available
   * once close() is requested. */
  az_iot_result az_iot_connection_client__birth_nonce(
      const az_iot_connection_client* client,
      uint8_t out[AZ_IOT_CORRELATION_UUID_LEN]);

  /** @brief Whether options.twin_push.push_desired asks the service to push desired state. */
  bool az_iot_connection_client__twin_push_desired(const az_iot_connection_client* client);

  /** @brief Whether options.twin_push.push_reported asks the service to push reported state. */
  bool az_iot_connection_client__twin_push_reported(const az_iot_connection_client* client);

  /** @brief Whether options.twin_push asks the service for a twin-push on each connect. */
  bool az_iot_connection_client__twin_push_expected(const az_iot_connection_client* client);

  /**
   * @brief Whether @p deadline had passed when the current do_work() pass began. Input that pass
   * dispatches arrived by then or during its waits, which end by the deadline, so it is in time
   * unless this is true.
   */
  bool az_iot_connection_client__deadline_passed_before_pass(
      const az_iot_connection_client* client,
      const az_iot_deadline* deadline);

  /**
   * @brief End the CONNECTED hub session on a feature client's behalf: send
   * DISCONNECT, then reconnect with @p reason, or fault with it without a
   * reconnection policy. Not from inside an MQTT adapter callback (e.g. from a
   * feature observer's TICK).
   *
   * @param[in] reason The failure to report; not AZ_IOT_OK.
   * @return AZ_IOT_OK, also when the session is already being ended;
   *         AZ_IOT_ERR_NOT_CONNECTED when the hub is not CONNECTED or close() was
   *         requested; AZ_IOT_ERR_INVALID_ARG for a NULL @p client or AZ_IOT_OK.
   */
  az_iot_result az_iot_connection_client__end_hub_session(
      az_iot_connection_client* client,
      az_iot_result reason);

  /** @brief Test seam: make a planned hub session end's settle and disconnect bounds pass. */
  void az_iot_connection_client__planned_hub_session_end_force_timeout(
      az_iot_connection_client* client);

  /**
   * @brief Whether new tracked sends wait: the hub session is being ended on purpose, and a SAS
   *        renewal first lets its deliveries settle (see
   *        AZ_IOT_CONNECTION_FEATURE_EVENT_UNSETTLED_QUERY).
   */
  bool az_iot_connection_client__holds_sends_for_session_end(
      const az_iot_connection_client* client);

  /* Test seam: force a pending subscription gate to expire on the next
   * do_work(). No-op when no gate is armed. Lets unit tests exercise the
   * never-acked path without waiting out the configured timeout. */
  void az_iot_connection_client__subscription_gate_force_timeout(az_iot_connection_client* client);

  /* Register an inbound MESSAGE handler. ConnectionClient lazily allocates a
   * dispatch table on the first call. Each registered handler is invoked
   * synchronously from inside do_work() when an inbound MESSAGE topic begins
   * with topic_prefix; longest-prefix wins. */
  az_iot_result az_iot_connection_client__register_inbound_handler(
      az_iot_connection_client* client,
      const char* topic_prefix,
      az_iot_inbound_handler_callback cb,
      void* user_ctx);

  /* Remove every handler whose user_ctx matches. Returns the number removed. */
  size_t az_iot_connection_client__unregister_inbound_handlers(
      az_iot_connection_client* client,
      void* user_ctx);

  /* True when the connection is in CONNECTED state. */
  bool az_iot_connection_client__is_connected(const az_iot_connection_client* client);

  /**
   * @brief Observe @p events (az_iot_connection_feature_event_kind bits) with @p cb.
   *
   * STATE events reach feature observers before the application's. A feature client MUST
   * withdraw before its storage goes away; a callback may withdraw itself or another observer.
   * Adding the same @p cb and @p user_ctx again replaces @p events.
   *
   * @retval AZ_IOT_ERR_INVALID_ARG @p client or @p cb is NULL, or @p events is 0.
   * @retval AZ_IOT_ERR_BUSY Called from inside a state dispatch with STATE in @p events.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE limits.max_feature_observers entries are in use.
   */
  az_iot_result az_iot_connection_client__add_feature_observer(
      az_iot_connection_client* client,
      uint8_t events,
      az_iot_connection_feature_observer_callback cb,
      void* user_ctx);

  /** @brief Withdraw the observer added with @p cb and @p user_ctx; no-op when absent. */
  void az_iot_connection_client__remove_feature_observer(
      az_iot_connection_client* client,
      az_iot_connection_feature_observer_callback cb,
      const void* user_ctx);

  /**
   * @brief Whether an observer with @p cb and @p user_ctx is registered.
   *
   * Lets a feature client check, after calling application code, that it was not deinitialized
   * meanwhile: its deinit withdraws its observer.
   */
  bool az_iot_connection_client__has_feature_observer(
      const az_iot_connection_client* client,
      az_iot_connection_feature_observer_callback cb,
      const void* user_ctx);

  /** @brief Current opts.device_id: the caller's, or the adopted DPS one.
   * @return NULL when none is set (a DPS client before an assignment is
   * adopted, or a direct client whose open() will reject it). */
  const char* az_iot_connection_client__device_id(const az_iot_connection_client* client);

  /**
   * @brief Signs a hub SAS token for an HTTPS request to the hub, with the key
   * the hub session authenticates with: the one a fallback selected (kept while
   * it connects), else the key of its current attempt, else of its last one,
   * else, without a
   * certificate provider, the primary key. The connection's own token area is
   * not used.
   *
   * @param[in]  client   Connection client.
   * @param[out] out      Receives the NUL-terminated token
   *                      (`SharedAccessSignature sr=...&sig=...&se=...`); wiped
   *                      on every failure.
   * @param[in]  out_size Bytes in @p out.
   * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_FOUND when the hub session does not use a
   *         key (X.509 or user-provided tokens); AZ_IOT_ERR_NOT_CONNECTED with
   *         no hub address or device id; AZ_IOT_ERR_BUSY while no Unix time is
   *         known; AZ_IOT_ERR_NOT_ENOUGH_SPACE when @p out_size is below
   *         AZ_IOT_SAS_TOKEN_SIZE() of the two IDs; AZ_IOT_ERR_INVALID_ARG for a
   *         NULL argument; the crypto backend's error otherwise, its
   *         AZ_IOT_ERR_NOT_FOUND as AZ_IOT_ERR_INTERNAL.
   */
  az_iot_result az_iot_connection_client__sign_hub_sas_token(
      const az_iot_connection_client* client,
      char* out,
      size_t out_size);

  /* --- Provisioning-session seam ------------------------------------------- */
  /*
   * The device-update operations ride the device's PROVISIONING session, not the
   * hub session, and the bootstrap check runs BEFORE the device is registered --
   * so az_iot_connection_client__publish() cannot serve them: it publishes on
   * the active hub client and requires CONNECTED.
   *
   * These expose the provisioning session directly, for the one feature that
   * legitimately needs it. Not for general use.
   */

  /* True when a provisioning session exists and is far enough along to carry a
   * publish -- that is, its subscription is established and the registration
   * outcome has not yet torn the session down. */
  bool az_iot_connection_client__dps_session_ready(const az_iot_connection_client* client);

  /* Publish on the provisioning session. Returns ERR_NOT_CONNECTED when no such
   * session is ready. */
  az_iot_result az_iot_connection_client__dps_publish(
      az_iot_connection_client* client,
      const az_iot_mqtt_message* msg);

  /* Hold registration at AZ_IOT_DPS_PHASE_HOLD so a feature client can run a
   * pre-registration exchange on the provisioning session.
   *
   * This exists because the provisioning session is otherwise unusable by a
   * feature client: the registration PUBLISH is issued from the SUBACK handler
   * and the session is torn down on the response, so a caller-driven loop never
   * observes an open session.
   *
   * Must be acquired BEFORE the session reaches its SUBACK (in practice, before
   * az_iot_connection_client_open()); acquiring later has no effect on a
   * registration already in flight, and the call reports that.
   *
   * The hold is ADVISORY: it expires after a deadline and registration then
   * proceeds regardless. A feature client can delay provisioning, never prevent
   * it. Every acquire must be matched by a release; the release is what lets
   * registration continue without waiting out the deadline.
   */
  az_iot_result az_iot_connection_client__dps_hold_acquire(az_iot_connection_client* client);
  void az_iot_connection_client__dps_hold_release(az_iot_connection_client* client);

  /* True while registration is actually being held, i.e. the session is up and
   * waiting on a holder. */
  bool az_iot_connection_client__dps_hold_is_active(const az_iot_connection_client* client);

  /**
   * @brief Take an interest in the provisioning session.
   *
   * The session exists exactly while somebody holds it -- these interests plus
   * the connection client's own registration ref -- so a holder keeps it alive
   * across registration, and the last release ends it. No hub session runs
   * while it exists, so hold an interest only while there is work for the
   * session. Distinct from the pre-registration hold, which delays a
   * registration that is about to happen rather than asking for the session
   * itself.
   *
   * @param[in] client The connection client. Must not be NULL.
   *
   * @return AZ_IOT_OK on success.
   * @retval AZ_IOT_ERR_INVALID_ARG @p client is NULL.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE The interest count is saturated.
   */
  az_iot_result az_iot_connection_client__dps_user_acquire(az_iot_connection_client* client);

  /**
   * @brief Give up an interest taken with
   *        az_iot_connection_client__dps_user_acquire().
   *
   * Does not close the session, even when this is the last interest: this is
   * reachable from inside a message callback, where freeing the adapter would
   * free the object being dispatched on. az_iot_connection_client_do_work()
   * closes it at a safe point, then starts a hub session that waited for it.
   *
   * @param[in] client The connection client. NULL, and an interest count
   *                   already at zero, are both ignored.
   */
  void az_iot_connection_client__dps_user_release(az_iot_connection_client* client);

  /**
   * @brief Whether a registration is pending on the provisioning session.
   *
   * Answers "is there a registration for my pre-registration hold to hold
   * back?". False once registration has reached a terminal outcome, and false
   * on a session no registration was started on.
   *
   * @param[in] client The connection client. NULL reads as false.
   *
   * @return true while the registration ref is held.
   */
  bool az_iot_connection_client__dps_registration_pending(const az_iot_connection_client* client);

  /**
   * @brief Report whether the provisioning session is usable, opening one if
   *        there is none.
   *
   * The caller is not requesting a session so much as asking about the one its
   * interest already entitles it to.
   *
   * Registration is NOT implied: a session opened through this call carries its
   * users' messages, and registers only if the connection client separately
   * holds the registration ref, which az_iot_connection_client_open() raises.
   *
   * The two sessions never coexist: a connected hub session is ended first (a
   * planned end; feature clients see AZ_IOT_CONNECTION_SESSION_END_PLANNED) and
   * waits at HUB:RETRY_PENDING, reason AZ_IOT_OK, until no provisioning session
   * is in use. It then reconnects to the cached assignment, without
   * registering.
   *
   * @param[in] client The connection client. Must not be NULL.
   *
   * @return AZ_IOT_OK when a publish can be made now.
   * @retval AZ_IOT_ERR_INVALID_ARG @p client is NULL.
   * @retval AZ_IOT_ERR_BUSY A session is coming up, the hub session is being
   *         ended or set up, or a retry is scheduled; call again on a later
   *         tick.
   * @retval AZ_IOT_ERR_NOT_SUPPORTED The caller holds no interest, DPS is not
   *         configured, or a lifecycle has settled into FAULTED -- opening a
   *         session from there would drag it out of its terminal state and
   *         hide the fault from the application.
   */
  az_iot_result az_iot_connection_client__dps_session_ensure(az_iot_connection_client* client);

  /* Register the observer for inbound provisioning-session messages the
   * provisioning flow does not claim. At most one: registering a second observer over a live one is
   * refused, so clear it (NULL) before registering a different one. The callback type is declared
   * with the client struct that stores it. */
  void az_iot_connection_client__set_dps_message_observer(
      az_iot_connection_client* client,
      az_iot_dps_message_observer observer,
      void* user_ctx);

  /**
   * @brief Publish through the active adapter.
   *
   * @param owner Feature client publishing. A tracked publish takes a slot from its
   *              reservation, or from the shared pool if it holds none. May be NULL.
   * @param ack_cb QoS 1: called from do_work() with the PUBLISH_ACK result; without one, a
   *               failed PUBLISH_ACK is only logged. QoS 0: called here with AZ_IOT_OK.
   *
   * @retval AZ_IOT_ERR_NOT_CONNECTED Not in CONNECTED state, or the session is being ended on
   *         purpose (DISCONNECT sent).
   * @retval AZ_IOT_ERR_BUSY QoS 1 with @p ack_cb and every slot of the pool it draws from is in
   *         use, or any QoS 1 while a SAS renewal lets the session's deliveries settle (at most
   *         5 s). Nothing was sent.
   * @return Otherwise the adapter's result.
   */
  az_iot_result az_iot_connection_client__publish(
      az_iot_connection_client* client,
      const void* owner,
      const az_iot_mqtt_message* msg,
      az_iot_publish_ack_callback ack_cb,
      void* ack_user_ctx);

  /**
   * @brief One tracked publish awaiting its PUBACK.
   *
   * Stale once the publish completes or is forgotten: ids are unique per client (64-bit, never 0),
   * so a stale handle never names a later publish that reuses the slot.
   */
  typedef struct az_iot_publish_handle
  {
    size_t slot; /**< Index into pending_pubacks. */
    uint64_t id; /**< 0: no tracked publish. */
  } az_iot_publish_handle;

  /**
   * @brief As az_iot_connection_client__publish(), completing through @p result_cb with the
   * PUBACK's reason code and Reason String.
   *
   * @param[out] out_handle Optional. The tracked publish; id 0 when the publish is not
   *             tracked (QoS 0, no @p result_cb) or failed.
   */
  az_iot_result az_iot_connection_client__publish_with_result(
      az_iot_connection_client* client,
      const void* owner,
      const az_iot_mqtt_message* msg,
      az_iot_publish_result_callback result_cb,
      void* user_ctx,
      az_iot_publish_handle* out_handle);

  /**
   * @brief Forget the tracked publish @p handle names, without calling its callback.
   *
   * Frees its pending-PUBACK slot; its late PUBACK is ignored. No-op for a stale handle.
   */
  void az_iot_connection_client__forget_publish(
      az_iot_connection_client* client,
      az_iot_publish_handle handle);

  /**
   * @brief Set aside @p count pending-PUBACK slots for @p owner's tracked publishes.
   *
   * Replaces @p owner's previous reservation; 0 withdraws it. Succeeds only if every pool can
   * hold its publishes already in flight, so a granted reservation is usable at once.
   *
   * @retval AZ_IOT_OK Reserved.
   * @retval AZ_IOT_ERR_INVALID_ARG @p client or @p owner is NULL.
   * @retval AZ_IOT_ERR_NOT_ENOUGH_SPACE All reservations would exceed limits.max_pending_publishes,
   *         or limits.max_publish_reservations owners already hold one. Nothing changes.
   * @retval AZ_IOT_ERR_BUSY Publishes in flight occupy the slots it needs; retry once they are
   *         acknowledged. Nothing changes.
   */
  az_iot_result az_iot_connection_client__reserve_pubacks(
      az_iot_connection_client* client,
      const void* owner,
      size_t count);

  /**
   * @brief Withdraw @p owner's reservation. Its publishes still in flight move to the shared pool.
   */
  void az_iot_connection_client__release_pubacks(
      az_iot_connection_client* client,
      const void* owner);

  /**
   * @brief Forget @p owner's publishes awaiting a PUBACK, without calling their callbacks.
   *
   * For an owner whose callback context is about to go away. Only publishes tracked against
   * @p owner's reservation are affected; their late PUBACKs are then ignored.
   */
  void az_iot_connection_client__abandon_pubacks(
      az_iot_connection_client* client,
      const void* owner);

  /**
   * @brief Whether a tracked QoS 1 publish by @p owner would get a pending-PUBACK slot now.
   *
   * Lets a caller that can live without the ack choose to publish untracked, without
   * mistaking an adapter's AZ_IOT_ERR_BUSY for a full pool.
   *
   * @param owner As for az_iot_connection_client__publish().
   * @return true if a slot is free; false if not, or if @p client is NULL.
   */
  bool az_iot_connection_client__can_track_publish(
      const az_iot_connection_client* client,
      const void* owner);

  /* Subscribe through the active adapter. Returns ERR_NOT_CONNECTED when not in
   * CONNECTED state. Out-arg packet_id is populated on success. */
  az_iot_result az_iot_connection_client__subscribe(
      az_iot_connection_client* client,
      const char* topic_filter,
      az_iot_mqtt_qos qos,
      uint16_t* out_packet_id);

  /* Register a persistent subscription that is (re)issued on every CONNECTED
   * transition. Feature clients call this at create time so the SUBSCRIBE is
   * automatically refreshed across reconnects. The topic_filter string is
   * copied. Returns ERR_NOT_ENOUGH_SPACE if the persistent-subscription registry
   * is full (current bound: 8). If the client is already CONNECTED, the
   * SUBSCRIBE is also issued immediately.
   *
   * `owner` identifies the registering feature client so it can withdraw its
   * own entries later; pass the same pointer used for the inbound handlers.
   *
   * `failure_scope` says what a refusal costs. A feature client's own filter is
   * AZ_IOT_SUBSCRIPTION_FAILS_SESSION: it cannot work without it, so the
   * connection fails rather than coming up with a dead feature. An
   * application-supplied topic is AZ_IOT_SUBSCRIPTION_FAILS_SELF, and
   * `on_failed` is how its owner is told; pass NULL for a gated filter, which
   * reports through the connection state instead. */
  az_iot_result az_iot_connection_client__add_subscription_on_connect(
      az_iot_connection_client* client,
      const char* topic_filter,
      az_iot_mqtt_qos qos,
      const void* owner,
      az_iot_subscription_failure_scope failure_scope,
      az_iot_subscription_failed_callback on_failed);

  /* Withdraw every persistent subscription registered by `owner`. Returns the
   * number removed.
   *
   * Each removed entry is also UNSUBSCRIBEd when connected, on both
   * generations. MQTTv5's device-wide `ih/{device_id}/dev/#` subscription is not
   * at risk from this: the presence handshake takes it out directly rather than
   * through the persistent-subscription registry, so it has no owner and this
   * function can never select it. Withdrawing an entry underneath it does not
   * disturb it either -- the wildcard keeps matching.
   *
   * On MQTTv5 this is now a registry removal in practice, because no feature
   * client registers a filter there any more: the wildcard covers them all. It
   * still issues the UNSUBSCRIBE for anything that is registered, which is what
   * an application custom topic will be.
   *
   * Safe to call when disconnected: the entries are dropped either way, so a
   * later reconnect does not resurrect them. */
  size_t az_iot_connection_client__remove_subscriptions_for(
      az_iot_connection_client* client,
      const void* owner);

  /* Build the topics a feature client needs, once the connection knows the
   * device id and generation it will actually use.
   *
   * A feature client cannot build `devices/{device_id}/...` or
   * `ih/{device_id}/...` at _init(): on a DPS connection the assigned device id
   * is not authoritative until ASSIGNED, and an enrollment may hand back a
   * device id that differs from the registration id. The connection therefore
   * calls this back before each connect attempt, after provisioning has settled.
   *
   * The connection withdraws the owner's previous subscriptions and inbound
   * handlers immediately before the call, so an implementation registers from
   * scratch every time and needs no idempotence of its own. Returning anything
   * other than AZ_IOT_OK fails the connect attempt. */
  typedef az_iot_result (
      *az_iot_feature_client_bind_callback)(void* owner, az_iot_connection_client* client);

  /* Attach a bind callback for `owner`, replacing any previous one. Feature
   * clients call this from _init() and withdraw it in _deinit() --
   * __release_profile only drops the generation refcount and leaves the bind
   * in place, so a client that skips the withdrawal leaves the connection
   * holding a callback into freed storage. */
  az_iot_result az_iot_connection_client__register_feature_client_bind(
      az_iot_connection_client* client,
      void* owner,
      az_iot_feature_client_bind_callback on_bind);

  /* Detach `owner`'s bind callback. Does not withdraw anything the callback
   * registered; feature clients withdraw those in their own _deinit(). */
  void az_iot_connection_client__unregister_feature_client_bind(
      az_iot_connection_client* client,
      const void* owner);

  /* Declare the hub generation a feature client needs, refcounted per client.
   *
   * Records the requirement rather than reading the connection, so a feature
   * client can be constructed before open(); the connection verifies it when the
   * profile becomes authoritative. When it already is -- a direct connect, or a
   * DPS connection past ASSIGNED -- the answer is given here instead.
   *
   * Returns AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH if the connection already
   * carries a requirement for the other generation, or is already known to be
   * the other generation. */
  az_iot_result az_iot_connection_client__require_profile(
      az_iot_connection_client* client,
      az_iot_connection_profile profile);

  /* Drop one requirement taken by __require_profile. Feature clients call this
   * from _deinit(); the pin clears when the last one goes. */
  void az_iot_connection_client__release_profile(az_iot_connection_client* client);

  /* Map session role to required MQTT version (SDK-internal knowledge). */
  static inline az_iot_mqtt_version az_iot_mqtt_required_version_for_role(az_iot_mqtt_role role)
  {
    switch (role)
    {
      case AZ_IOT_MQTT_ROLE_HUB_MQTT_V5:
        return AZ_IOT_MQTT_VERSION_5;
      case AZ_IOT_MQTT_ROLE_DPS:
      case AZ_IOT_MQTT_ROLE_HUB_MQTT_V3:
      default:
        return AZ_IOT_MQTT_VERSION_3_1_1;
    }
  }

  const char* az_iot_mqtt_role_to_string(az_iot_mqtt_role r);

#ifdef __cplusplus
}
#endif

#endif
