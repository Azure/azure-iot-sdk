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
   * to HUB_CLASSIC (v3.1.1); DPS calls this with the assigned role before driving
   * the post-provisioning open. */
  az_iot_result az_iot_connection_client__set_session_role(
      az_iot_connection_client* client,
      az_iot_mqtt_role role);

  /* Override opts.host with a heap-owned copy of `host`. Used by the DPS handoff
   * helper to redirect a freshly-created (and still-IDLE) connection client at
   * the assigned hub. The client owns the duplicate; it is freed in destroy().
   * Rejects the call when state != IDLE so live sessions can never have their
   * target rewritten under them. */
  az_iot_result az_iot_connection_client__set_host(
      az_iot_connection_client* client,
      const char* host);

  /* Override opts.client_id with a heap-owned copy of `client_id`. Same
   * IDLE-only constraint as __set_host. */
  az_iot_result az_iot_connection_client__set_client_id(
      az_iot_connection_client* client,
      const char* client_id);

  /* Seed the jitter PRNG used by the reconnect logic. Tests use this to make
   * backoff timing deterministic. Production builds seed automatically from a
   * monotonic clock during create(). */
  void az_iot_connection_client__seed_rng(az_iot_connection_client* client, uint64_t seed);

  /* Test seam: force any in-flight AEG/Hub-Next presence (birth) handshake to time
   * out on the next do_work(). No-op when no handshake is active. Lets unit tests
   * exercise the birth-ack timeout path without waiting the real
   * AZ_IOT_PRESENCE_BIRTH_ACK_TIMEOUT_MS. */
  void az_iot_connection_client__presence_force_timeout(az_iot_connection_client* client);

  /* Authoritative twin versions the service reported on the most recent AEG
   * birth-ack, as of birth admission on the current connection. The twin client
   * uses `reported_version` as the if_match anchor for its next reported patch
   * and `desired_version` as its view of the current desired version. Both are 0
   * before the first birth-ack, on Classic/DPS sessions, and when the service
   * omits the fields. Returns AZ_IOT_ERR_INVALID_ARG on NULL arguments. */
  az_iot_result az_iot_connection_client__presence_twin_versions(
      const az_iot_connection_client* client,
      uint64_t* out_desired_version,
      uint64_t* out_reported_version);

/* Width of the MQTT v5 Correlation Data every AEG flow uses: a 16-byte UUID,
 * for the connection's birth nonce and for per-request correlation ids alike. */
#define AZ_IOT_CORRELATION_UUID_LEN 16u

  /* Fill `out` with a fresh RFC 4122 version 4 UUID from the client's PRNG.
   * Feature clients use this for the per-attempt Correlation Data that the AEG
   * request/response flows (twin GET, reported patch, ...) require. */
  void az_iot_connection_client__gen_uuid(
      az_iot_connection_client* client,
      uint8_t out[AZ_IOT_CORRELATION_UUID_LEN]);

  /* Copy the current connection's birth nonce into `out`. Backend-initiated
   * dev-bound messages (twin-push, desired-patch) carry it as Correlation Data
   * so the device can tell traffic for this connection from traffic left over
   * from a defunct one. Returns AZ_IOT_ERR_NOT_CONNECTED when no presence
   * handshake has completed on this connection (Classic/DPS sessions included). */
  az_iot_result az_iot_connection_client__presence_nonce(
      const az_iot_connection_client* client,
      uint8_t out[AZ_IOT_CORRELATION_UUID_LEN]);

  /* The twin push advertisement the application configured (options.twin_push).
   * The twin client uses it to decide whether a birth-triggered twin-push is
   * expected on this connection, and therefore whether to arm a defensive
   * timeout for one. Both outputs are false when `client` is NULL. */
  void az_iot_connection_client__twin_push_flags(
      const az_iot_connection_client* client,
      bool* out_push_desired,
      bool* out_push_reported);

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

  /* Register a callback invoked when the MQTT session ends -- a peer
   * disconnect, a transport error, or a user close. A feature client that
   * correlates a request against the session (a twin $rid, a certificate
   * renewal) uses this to complete those requests with
   * AZ_IOT_ERR_NOT_CONNECTED and release their slots, instead of holding them
   * until the pool is exhausted. Registration is keyed on user_ctx: registering
   * the same user_ctx twice replaces the callback rather than consuming a
   * second slot. Returns ERR_NOT_SUPPORTED when the registry is full. */
  az_iot_result az_iot_connection_client__register_session_end_handler(
      az_iot_connection_client* client,
      az_iot_session_end_callback cb,
      void* user_ctx);

  /* Remove the session-end handler registered for user_ctx, if any. Returns the
   * number removed (0 or 1). */
  size_t az_iot_connection_client__unregister_session_end_handler(
      az_iot_connection_client* client,
      void* user_ctx);

  /* True when the connection is in CONNECTED state. */
  bool az_iot_connection_client__is_connected(const az_iot_connection_client* client);

  /* Returns the configured device id (== options.client_id). NULL only when the
   * client was created with a NULL client_id (rejected at create time, so this
   * is effectively never NULL for a live client). */
  const char* az_iot_connection_client__device_id(const az_iot_connection_client* client);

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

  /* Standing interest in the provisioning session, for a feature client that
   * needs it after the device has provisioned.
   *
   * Acquire at initialize, release at destroy. A non-zero count permits a
   * session to be OPENED ON DEMAND -- it does not keep one open, and does not
   * open one by itself. Between polls there is deliberately no session.
   *
   * Separate from the pre-registration hold: the hold delays a registration
   * that is about to happen, this asks for a session once registration is long
   * done. A feature client usually wants both. */
  az_iot_result az_iot_connection_client__dps_user_acquire(az_iot_connection_client* client);
  void az_iot_connection_client__dps_user_release(az_iot_connection_client* client);

  /* True when the session currently up was opened for a feature client rather
   * than by the ordinary provisioning flow. Such a session never registers, so
   * the pre-registration hold does not apply to it. */
  bool az_iot_connection_client__dps_session_is_auxiliary(const az_iot_connection_client* client);

  /* Ensure a provisioning session is up and usable, opening one if needed.
   *
   * Returns AZ_IOT_OK when a publish can be made now, AZ_IOT_ERR_BUSY while one
   * is still coming up (call again on a later tick), AZ_IOT_ERR_NOT_SUPPORTED
   * when the caller holds no interest or DPS is not configured.
   *
   * A session opened this way is AUXILIARY: it runs alongside the hub
   * connection and never registers. Registering would take the assignment path,
   * which rewrites the host and role and reconnects -- destroying the very hub
   * connection this is meant to sit beside.
   *
   * Each call also renews the idle linger, so a caller that is actively using
   * the session keeps it, and one that stops loses it shortly after. */
  az_iot_result az_iot_connection_client__dps_session_ensure(az_iot_connection_client* client);

  /* Register the observer for inbound provisioning-session messages the
   * provisioning flow does not claim. At most one: registering a second observer over a live one is
   * refused, so clear it (NULL) before registering a different one. The callback type is declared
   * with the client struct that stores it. */
  void az_iot_connection_client__set_dps_message_observer(
      az_iot_connection_client* client,
      az_iot_dps_message_observer observer,
      void* user_ctx);

  /* Publish through the active adapter. Returns ERR_NOT_CONNECTED when not in
   * CONNECTED state. For QoS 1, callers may pass a non-NULL ack_cb; it is
   * invoked synchronously from inside do_work() when the matching PUBLISH_ACK
   * arrives. For QoS 0, ack_cb (if any) is invoked synchronously here with
   * AZ_IOT_OK because QoS 0 has no on-the-wire ack. */

  az_iot_result az_iot_connection_client__publish(
      az_iot_connection_client* client,
      const az_iot_mqtt_message* msg,
      az_iot_publish_ack_callback ack_cb,
      void* ack_user_ctx);

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
   * generations. AEG's device-wide `ih/{device_id}/dev/#` subscription is not
   * at risk from this: the presence handshake takes it out directly rather than
   * through the persistent-subscription registry, so it has no owner and this
   * function can never select it. Withdrawing an entry underneath it does not
   * disturb it either -- the wildcard keeps matching.
   *
   * On AEG this is now a registry removal in practice, because no feature
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
   * clients call this from _init() and withdraw it in _destroy() --
   * __release_profile only drops the generation refcount and leaves the bind
   * in place, so a client that skips the withdrawal leaves the connection
   * holding a callback into freed storage. */
  az_iot_result az_iot_connection_client__register_feature_client_bind(
      az_iot_connection_client* client,
      void* owner,
      az_iot_feature_client_bind_callback on_bind);

  /* Detach `owner`'s bind callback. Does not withdraw anything the callback
   * registered; feature clients withdraw those in their own _destroy(). */
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
   * from _destroy(); the pin clears when the last one goes. */
  void az_iot_connection_client__release_profile(az_iot_connection_client* client);

  /* Map session role to required MQTT version (SDK-internal knowledge). */
  static inline az_iot_mqtt_version az_iot_mqtt_required_version_for_role(az_iot_mqtt_role role)
  {
    switch (role)
    {
      case AZ_IOT_MQTT_ROLE_HUB_NEXT:
        return AZ_IOT_MQTT_VERSION_5;
      case AZ_IOT_MQTT_ROLE_DPS:
      case AZ_IOT_MQTT_ROLE_HUB_CLASSIC:
      default:
        return AZ_IOT_MQTT_VERSION_3_1_1;
    }
  }

  const char* az_iot_mqtt_role_to_string(az_iot_mqtt_role r);

#ifdef __cplusplus
}
#endif

#endif
