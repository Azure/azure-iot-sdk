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
#include "internal/protocol_profile.h"

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

  /* Return the protocol profile selected by the current session_role. May be
   * NULL when the role has no profile yet (e.g. HUB_NEXT in Phase 2.3). */
  const az_iot_protocol_profile* az_iot_connection_client__profile(
      const az_iot_connection_client* client);

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
   * copied. Returns ERR_NOT_SUPPORTED if the persistent-subscription registry
   * is full (current bound: 8). If the client is already CONNECTED, the
   * SUBSCRIBE is also issued immediately. */
  az_iot_result az_iot_connection_client__add_subscription_on_connect(
      az_iot_connection_client* client,
      const char* topic_filter,
      az_iot_mqtt_qos qos);

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
