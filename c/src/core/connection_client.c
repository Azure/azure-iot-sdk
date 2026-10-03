// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* ConnectionClient core (Phase 2.1 + 2.2 + DPS integration).
 *
 * Owns:
 *   - the adapter registry (factories registered by the application)
 *   - the active MQTT client for the current session
 *   - the lifecycle state machine
 *     (IDLE -> CONNECTING -> CONNECTED -> DISCONNECTING -> IDLE,
 *      with RECONNECTING and FAULTED side branches)
 *   - DPS provisioning (when opts.dps.id_scope is set and opts.host is NULL,
 *     open() internally provisions via DPS before connecting to the hub)
 *
 * Single-threaded contract: every state transition and every user state-callback
 * fires from inside az_iot_connection_client_do_work(). The MQTT adapter's
 * inbound callback is also delivered from process_loop() (which we drive from
 * do_work()); the adapter is responsible for marshalling its own background
 * thread events into that callback.
 *
 * Reconnect (Phase 2.2): when opts.reconnection_policy is enabled
 * (initial_delay_ms > 0), unexpected drops (CONNACK fail, peer DISCONNECT,
 * inbound ERROR) transition to RECONNECTING; do_work() then re-opens after the
 * computed backoff (with jitter). User-initiated close() always goes to IDLE
 * regardless. If max_attempts > 0 is configured and reached, we transition to
 * FAULTED.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_version.h"

#include "internal/cert_util.h"
#include "internal/connection_client_internal.h"
#include "internal/crypto.h"
#include "internal/dispatch.h"
#include "internal/env.h"
#include "internal/log_internal.h"
#include "internal/proto3.h"
#include "internal/mono_time.h"
#include "internal/retry_policy.h"
#include "internal/sas.h"
#include "internal/span_writer.h"

#include <azure/az_core.h>
#include <azure/iot/az_iot_hub_client.h>
#include <azure/iot/az_iot_provisioning_client.h>

/* Local aliases for the enum constants exposed in the public header as
 * anonymous enum values.  Keeps the implementation readable. */
#define DEFER_NONE AZ_IOT_CONN_DEFER_NONE
#define DEFER_FAULT AZ_IOT_CONN_DEFER_FAULT
#define DEFER_RECONNECT AZ_IOT_CONN_DEFER_RECONNECT
#define DEFER_IDLE AZ_IOT_CONN_DEFER_IDLE

#define DPS_PHASE_NONE AZ_IOT_DPS_PHASE_NONE
#define DPS_PHASE_CONNECTING AZ_IOT_DPS_PHASE_CONNECTING
#define DPS_PHASE_SUBSCRIBING AZ_IOT_DPS_PHASE_SUBSCRIBING
#define DPS_PHASE_REGISTERING AZ_IOT_DPS_PHASE_REGISTERING
#define DPS_PHASE_POLLING AZ_IOT_DPS_PHASE_POLLING
#define DPS_PHASE_DONE AZ_IOT_DPS_PHASE_DONE
#define DPS_PHASE_HOLD AZ_IOT_DPS_PHASE_HOLD

/* The certificate provider vtable ABI version that introduced the v2 hooks
 * (sign, get_csr). Every gate on those hooks compares against this, NOT against
 * AZ_IOT_CERTIFICATE_PROVIDER_VTABLE_VERSION: that macro tracks the CURRENT
 * version, so the moment it becomes 3 it would start rejecting the v2 providers
 * these hooks were added for. */
#define CERT_PROVIDER_VTABLE_V2 2u

/* ------------------------------------------------------------------------- */
/* CSR / issued-certificate wire constants. azure-sdk-for-c does not surface   */
/* these DPS/Hub fields, so the JSON field names, the register body and the    */
/* PEM wrapping are defined here rather than inline.                          */
/* ------------------------------------------------------------------------- */

/* DPS registration body members. The body is ONE JSON object carrying only the
 * members this registration actually needs:
 *
 *   {"csr":"<base64 DER>"}                          CSR enrollment only
 *   {"payload":<caller JSON>}                       custom payload only
 *   {"csr":"<base64 DER>","payload":<caller JSON>}  both
 *
 * `payload` is the member the DPS registration request uses for the custom
 * allocation payload; azure-sdk-for-c writes the same member name in
 * az_iot_provisioning_client_register_get_request_payload()
 * (sdk/src/azure/iot/az_iot_provisioning_client.c, `prov_payload_label`). That
 * helper is not used here because it always emits `registrationId` -- which
 * this SDK carries in the DPS username and topic, not the body -- and has no
 * `csr` member, so it could not produce the combined body at all. */
#define DPS_REGISTER_BODY_OPEN "{"
#define DPS_REGISTER_BODY_CLOSE "}"
#define DPS_REGISTER_BODY_SEPARATOR ","
#define DPS_REGISTER_CSR_MEMBER_PREFIX "\"csr\":\""
#define DPS_REGISTER_CSR_MEMBER_SUFFIX "\""
#define DPS_REGISTER_PAYLOAD_MEMBER_PREFIX "\"payload\":"

/* The pinned azure-sdk-for-c helper emits 2019-03-31. Build the CONNECT
 * username here for every DPS session, including CSR and provision-only software updates. */
#define DPS_API_VERSION "2026-11-02-preview"
#define DPS_USERNAME_INFIX "/registrations/"
#define DPS_USERNAME_SUFFIX "/api-version=" DPS_API_VERSION

/* DPS ASSIGNED result fields that carry the issued operational chain. */
#define DPS_JSON_REGISTRATION_STATE "registrationState"
#define DPS_JSON_ISSUED_CERT_CHAIN "issuedCertificateChain"
/* The hub generation the device was assigned to. A string, and an extensible
 * union: absent or null means "classic". New in api-version 2026-11-02-preview. */
#define DPS_JSON_CONNECTION_PROFILE "connectionProfile"
#define CONNECTION_PROFILE_MQTT_V3_STR "classic"
#define CONNECTION_PROFILE_MQTT_V5_STR "mqttV5"
#define DPS_CONNECTION_PROFILE_OVERRIDE_ENV "AZ_IOT_DPS_CONNECTION_PROFILE_OVERRIDE"

/* Max certs in an issued chain (leaf + a few intermediates). The chain is
 * delivered as zero-copy spans into the payload. */
#define CERT_CHAIN_MAX_CERTS 6u

/* ------------------------------------------------------------------------- */
/* MQTTv5 presence (birth) handshake wire constants. Mirrors the .NET          */
/* SDK's ConnectToAzureEventGridIotHubAsync and common/Protos/presence.proto.  */
/* ------------------------------------------------------------------------- */
#define PRESENCE_PHASE_NONE AZ_IOT_PRESENCE_PHASE_NONE
#define PRESENCE_PHASE_SUBSCRIBING AZ_IOT_PRESENCE_PHASE_SUBSCRIBING
#define PRESENCE_PHASE_BIRTH AZ_IOT_PRESENCE_PHASE_BIRTH
#define PRESENCE_PHASE_DONE AZ_IOT_PRESENCE_PHASE_DONE

/* Device publishes the birth message here; the birth-ack arrives on dev/. All
 * three are "ih/" + device id + one of these suffixes. */
#define PRESENCE_TOPIC_PREFIX "ih/"
#define PRESENCE_TOPIC_SRV_SUFFIX "/srv/presence"
#define PRESENCE_TOPIC_DEV_SUFFIX "/dev/presence"
/* The device subscribes to the whole device-bound topic space (per RFC
 * topics.md and the .NET SDK) rather than the narrower dev/presence: one
 * subscription that MQTTv5's topic-space authorization is guaranteed to grant and
 * that also covers the other device-bound feature topics. The birth-ack is
 * still matched by its exact dev/presence topic. */
#define PRESENCE_TOPIC_DEV_SUB_SUFFIX "/dev/#"
/* MQTT v5 User Property key carrying the message type, plus the value we send
 * and the type we match. The service stamps "<type>:<schemaVersion>"; the
 * schema suffix is ignored when matching the birth-ack. */
#define PRESENCE_TYPE_KEY "type"
#define PRESENCE_TYPE_BIRTH "birth:1"
#define PRESENCE_TYPE_BIRTH_ACK "birth-ack"
/* Connection nonce carried as MQTT v5 Correlation Data on the birth PUBLISH and
 * echoed unchanged on the birth-ack. 16 bytes matches the .NET GUID nonce, and
 * is the same width every MQTTv5 correlation id uses. */
#define PRESENCE_NONCE_LEN AZ_IOT_CORRELATION_UUID_LEN

/* The nonce is a UUID, so two of its octets carry RFC 4122 metadata: octet 6
 * holds the version in its high nibble and octet 8 the variant in its high
 * bits. The rest of each octet stays random. */
#define PRESENCE_UUID_VERSION_OCTET 6u
#define PRESENCE_UUID_VARIANT_OCTET 8u
#define PRESENCE_UUID_VERSION_KEEP_MASK 0x0Fu
#define PRESENCE_UUID_VERSION_4 0x40u
#define PRESENCE_UUID_VARIANT_KEEP_MASK 0x3Fu
#define PRESENCE_UUID_VARIANT_RFC4122 0x80u
#define PRESENCE_UUID_STAMP_VERSION_4(octet) \
  ((uint8_t)(((octet) & PRESENCE_UUID_VERSION_KEEP_MASK) | PRESENCE_UUID_VERSION_4))
#define PRESENCE_UUID_STAMP_VARIANT_RFC4122(octet) \
  ((uint8_t)(((octet) & PRESENCE_UUID_VARIANT_KEEP_MASK) | PRESENCE_UUID_VARIANT_RFC4122))

/* Nibble halves of a byte, for rendering the nonce as hex. */
#define PRESENCE_NIBBLE_MASK 0x0Fu
#define PRESENCE_NIBBLE_BITS 4u
#define PRESENCE_HI_NIBBLE(byte) (((byte) >> PRESENCE_NIBBLE_BITS) & PRESENCE_NIBBLE_MASK)
#define PRESENCE_LO_NIBBLE(byte) ((byte) & PRESENCE_NIBBLE_MASK)

/* proto3 field keys for the birth encoder. A record is a varint key -- field
 * number in the high bits, wire type in the low three. Decoding goes through
 * internal/proto3.h; the birth is three fixed bool fields, which is cheaper to
 * emit directly than to frame. */
#define PROTO_WIRE_TYPE_VARINT 0u
#define PROTO_WIRE_TYPE_BITS 3u
/* Single-byte key for a varint field. Valid for field numbers 1..15, which is
 * every field this client encodes. */
#define PROTO_KEY_VARINT(field) \
  ((uint8_t)(((field) << PROTO_WIRE_TYPE_BITS) | PROTO_WIRE_TYPE_VARINT))
#define PROTO_BOOL_TRUE 0x01u

/* Field numbers from presence.proto. */
#define PRESENCE_BIRTH_FIELD_SESSION_PRESENT 1u
#define PRESENCE_BIRTH_FIELD_PUSH_DESIRED 12u
#define PRESENCE_BIRTH_FIELD_PUSH_REPORTED 13u
#define PRESENCE_BIRTH_ACK_FIELD_DESIRED_VERSION 10u
#define PRESENCE_BIRTH_ACK_FIELD_REPORTED_VERSION 11u

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

const char* az_iot_mqtt_role_to_string(az_iot_mqtt_role r)
{
  switch (r)
  {
    case AZ_IOT_MQTT_ROLE_DPS:
      return "DPS";
    case AZ_IOT_MQTT_ROLE_HUB_MQTT_V3:
      return "HUB_MQTT_V3";
    case AZ_IOT_MQTT_ROLE_HUB_MQTT_V5:
      return "HUB_MQTT_V5";
    default:
      return "ROLE?";
  }
}

/* Dispatch one transition to every registered observer.
 *
 * Feature clients first, then the application, so an application observer never
 * sees a connection whose feature clients have not yet reacted to the same
 * event.
 *
 * The event is built once and shared. It is const to the observers and lives on
 * this frame, which is what makes "valid only for the duration of the call"
 * true by construction.
 *
 * `dispatching_state` is held across BOTH passes rather than per pass: an
 * observer in the first pass could otherwise register one that the second pass
 * would walk into.
 *
 * The prior value is SAVED and RESTORED rather than simply cleared, because
 * this can nest: close() is legal from inside an observer, and it transitions
 * the state, which dispatches again. Clearing on the way out of the inner
 * dispatch would drop the guard while the outer one is still walking its
 * arrays, and a later observer in the outer pass could then mutate them. */
static void dispatch_state_event(
    az_iot_connection_client* c,
    const az_iot_connection_state_event* event)
{
  bool was_dispatching = c->dispatching_state;
  c->dispatching_state = true;
  for (size_t i = 0; i < AZ_IOT_MAX_FEATURE_STATE_OBSERVERS; ++i)
  {
    if (c->feature_state_observers[i].cb)
    {
      c->feature_state_observers[i].cb(event, c->feature_state_observers[i].user_ctx);
    }
  }
  for (size_t i = 0; i < AZ_IOT_MAX_APP_STATE_OBSERVERS; ++i)
  {
    if (c->app_state_observers[i].cb)
    {
      c->app_state_observers[i].cb(event, c->app_state_observers[i].user_ctx);
    }
  }
  c->dispatching_state = was_dispatching;
}

static bool have_state_observers(const az_iot_connection_client* c)
{
  for (size_t i = 0; i < AZ_IOT_MAX_FEATURE_STATE_OBSERVERS; ++i)
  {
    if (c->feature_state_observers[i].cb)
    {
      return true;
    }
  }
  for (size_t i = 0; i < AZ_IOT_MAX_APP_STATE_OBSERVERS; ++i)
  {
    if (c->app_state_observers[i].cb)
    {
      return true;
    }
  }
  return false;
}

/**
 * @brief Stage diagnostic detail for the next state event on @p scope.
 *
 * Scoped so a DPS verdict cannot attach to a hub event that runs in between.
 * @p message is not copied: it must stay valid until the pump dispatches, which
 * is the lifetime the public event promises.
 */
static void stage_error(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_connection_error_source source,
    int32_t code,
    az_span message)
{
  c->error_scope = scope;
  c->error_source = source;
  c->error_code = code;

  /* COPIED, not referenced. `message` spans the adapter's inbound buffer, and
   * that buffer is reused or freed the moment the adapter's callback returns --
   * before the pump dispatches the transition that reports this. Holding the
   * span would be a use-after-free that only a memory checker catches, because
   * the bytes usually still look right. */
  c->error_message_len = 0;
  int32_t n = az_span_size(message);
  if (n > 0)
  {
    size_t copy_n = (size_t)n;
    if (copy_n > sizeof(c->error_message))
    {
      copy_n = sizeof(c->error_message);
    }
    memcpy(c->error_message, az_span_ptr(message), copy_n);
    c->error_message_len = copy_n;
  }
}

/**
 * @brief Stage whatever codes an adapter event carried, for @p scope.
 *
 * transport_code wins when both are present: a failure below MQTT is the more
 * specific fact, and a wire code alongside it would be describing the session
 * that never formed. Neither present stages nothing, which is correct -- an
 * adapter that reports no codes is conformant.
 */
static void stage_error_from_event(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    const az_iot_mqtt_event* evt);

/** @brief Discard staged detail; the failure it described is no longer current. */
static void clear_staged_error(az_iot_connection_client* c)
{
  c->error_source = AZ_IOT_CONN_ERR_SRC_NONE;
  c->error_code = 0;
  c->error_message_len = 0;
}

/**
 * @brief Would another attempt at this cause plausibly succeed?
 *
 * A property of the CAUSE, not of SDK intent -- an application that disabled
 * retries owns the ladder and needs this to decide whether to bother. Errs
 * toward retriable: a wrong "do not retry" strands a device that would have
 * recovered, while a wrong "retriable" costs one more attempt.
 */
static void stage_error_from_event(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    const az_iot_mqtt_event* evt)
{
  if (evt == NULL)
  {
    return;
  }
  /* Only a failure-bearing event stages. A SUBACK that SUCCEEDED also carries a
   * protocol_code -- the granted QoS -- and staging that would hand a later
   * failure a code describing something that worked. A server-sent MQTT 5
   * DISCONNECT is the exception that is not flagged by status: the session
   * ended cleanly as far as the transport is concerned, and the reason code is
   * the whole point of the event. */
  bool bears_failure = (evt->status != AZ_IOT_OK)
      || (evt->kind == AZ_IOT_MQTT_EVT_DISCONNECTED && evt->protocol_code != 0);
  if (!bears_failure)
  {
    return;
  }
  if (evt->transport_code != 0)
  {
    stage_error(c, scope, AZ_IOT_CONN_ERR_SRC_TRANSPORT, evt->transport_code, AZ_SPAN_EMPTY);
  }
  else if (evt->protocol_code != 0)
  {
    stage_error(c, scope, AZ_IOT_CONN_ERR_SRC_MQTT, evt->protocol_code, AZ_SPAN_EMPTY);
  }
}

static bool reason_is_retriable(az_iot_result reason)
{
  /* Exhaustive on purpose: -Werror=switch-enum makes a new result code a
   * compile error here, so classifying it is a decision someone has to take
   * rather than one that defaults silently. */
  switch (reason)
  {
    /* The credential or the identity is refused. Retrying with the same inputs
     * returns the same answer; the application has to change something. */
    case AZ_IOT_ERR_AUTH:
    case AZ_IOT_ERR_IDENTITY_REJECTED:
    case AZ_IOT_ERR_CREDENTIAL_INCOMPLETE:
    /* The assigned generation is not one the attached feature clients can use.
     * Re-provisioning returns the same assignment. */
    case AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH:
    case AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED:
    /* The broker refused the filter itself. */
    case AZ_IOT_ERR_SUBSCRIPTION_REFUSED:
    /* Caller, build or programming faults: retrying re-runs the same bad call. */
    case AZ_IOT_ERR_INVALID_ARG:
    case AZ_IOT_ERR_NOT_SUPPORTED:
    case AZ_IOT_ERR_NOT_ENOUGH_SPACE:
    case AZ_IOT_ERR_OUT_OF_MEMORY:
    case AZ_IOT_ERR_NOT_INITIALIZED:
    case AZ_IOT_ERR_ALREADY_INITIALIZED:
    case AZ_IOT_ERR_NOT_FOUND:
    case AZ_IOT_ERR_DETACHED:
    case AZ_IOT_ERR_PROTOCOL:
      return false;

    /* Transport and service conditions that commonly clear on their own. */
    case AZ_IOT_ERR_NOT_CONNECTED:
    case AZ_IOT_ERR_TIMEOUT:
    case AZ_IOT_ERR_TLS:
    case AZ_IOT_ERR_MQTT:
    case AZ_IOT_ERR_BUSY:
    case AZ_IOT_ERR_INTERNAL:
    /* A DPS verdict is retriable as a class: the commonest are a throttle, a
     * server error, or an enrollment that does not exist YET on first boot.
     * The permanent ones are distinguishable through error->code, which is why
     * the code travels. */
    case AZ_IOT_ERR_DPS:
      return true;

    /* Not a failure; set_state_to() never asks about AZ_IOT_OK. */
    case AZ_IOT_OK:
    default:
      return false;
  }
}

static void set_state_to(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_connection_state next,
    az_iot_result reason)
{
  /* One failure produces a SEQUENCE of transitions -- a dying session reports
   * DISCONNECTING, then IDLE, then RECONNECTING or FAULTED -- and they are all
   * reporting the same failure, so the detail rides all of them rather than
   * being consumed by whichever ran first. (It was: the terminal event, the one
   * an application acts on, arrived with nothing.)
   *
   * It is discarded when the scope starts a NEW attempt or succeeds, which is
   * the point at which the old evidence stops describing anything current. */
  if (scope == c->error_scope
      && (next == AZ_IOT_CONN_STATE_CONNECTING || next == AZ_IOT_CONN_STATE_CONNECTED))
  {
    clear_staged_error(c);
  }

  /* Per scope, deliberately. A single comparison here would swallow
   * HUB:CONNECTING straight after DPS:CONNECTING purely because the VALUE
   * matched -- which is exactly why a DPS+hub run used to be reported as one
   * CONNECTING and one CONNECTED, with the whole provisioning phase invisible. */
  if (c->state[scope] == next)
  {
    return;
  }
  c->state[scope] = next;
  /* Bookkeeping that belongs to the transition itself, not to any observer, so
   * it runs whether or not anyone is watching. */
  if (scope == AZ_IOT_CONN_SCOPE_HUB && next == AZ_IOT_CONN_STATE_CONNECTED)
  {
    c->consecutive_hub_connect_failures = 0;
  }
  if (!have_state_observers(c))
  {
    return;
  }
  az_iot_hub_profile profile = {
    .connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V3,
    .connection_profile_raw = NULL,
    .connection_profile_raw_truncated = false,
  };
  az_iot_connection_error_detail detail = {
    .source = AZ_IOT_CONN_ERR_SRC_NONE,
    .code = 0,
    .message = AZ_SPAN_EMPTY,
  };
  az_iot_connection_state_event event = {
    .scope = scope,
    .state = next,
    .reason = reason,
    .profile = NULL,
    .is_retriable = (reason != AZ_IOT_OK) && reason_is_retriable(reason),
    .error = NULL,
    .is_credential_renewal = false,
    .auth_source = c->auth[scope].source,
    .x509_index = c->auth[scope].x509_index,
  };
  /* Detail rides only an event that is actually reporting a failure, and only
   * on the scope it was recorded for. */
  if (reason != AZ_IOT_OK && c->error_source != AZ_IOT_CONN_ERR_SRC_NONE && c->error_scope == scope)
  {
    detail.source = c->error_source;
    detail.code = c->error_code;
    detail.message = (c->error_message_len > 0)
        ? az_span_create((uint8_t*)c->error_message, (int32_t)c->error_message_len)
        : AZ_SPAN_EMPTY;
    event.error = &detail;
  }
  /* The profile rides the events that settle "which hub generation is this?":
   * HUB:CONNECTED, and the two failures that are ABOUT the profile.
   *
   * HUB only. DPS:CONNECTED is the SUBACK, before any assignment exists.
   * connection_profile_resolved cannot stand in for that: it survives close()
   * and a re-provision, so a second run would report a stale profile. */
  if ((next == AZ_IOT_CONN_STATE_CONNECTED && scope == AZ_IOT_CONN_SCOPE_HUB)
      || reason == AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH
      || reason == AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED)
  {
    profile.connection_profile = c->connection_profile;
    profile.connection_profile_raw = c->connection_profile_raw;
    profile.connection_profile_raw_truncated = c->connection_profile_raw_truncated;
    event.profile = &profile;
  }
  dispatch_state_event(c, &event);
}

static bool client_is_fully_idle(const az_iot_connection_client* c)
{
  return c->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_IDLE
      && c->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_IDLE;
}

/* Settle BOTH lifecycles.
 *
 * close() is a statement about the whole client, not about one scope: it
 * cancels a pending retry whichever ladder it belonged to, and it acknowledges
 * a fault on either. Emitting per scope keeps the rule that every state change
 * is observable, and set_state_to() suppresses the ones that did not move. */
static void settle_all_scopes_to_idle(az_iot_connection_client* c)
{
  set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, AZ_IOT_OK);
  set_state_to(c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_IDLE, AZ_IOT_OK);
}

static const az_iot_mqtt_factory* find_factory(
    const az_iot_connection_client* c,
    az_iot_mqtt_version version)
{
  for (size_t i = 0; i < c->factory_count; ++i)
  {
    const az_iot_mqtt_factory* f = &c->factories[i];
    if (f->version != version)
    {
      continue;
    }
    return f;
  }
  return NULL;
}

/* Apply the caller's keep-alive and connect timeout, or the SDK defaults when
 * they were left at 0.
 *
 * Shared by both connect paths on purpose. The DPS bootstrap connect used to
 * carry its own hardcoded copies, so an application that lengthened the
 * keep-alive for a metered link silently got the old value while provisioning
 * -- the one connect that happens on an unattended first boot. */
static void resolve_connect_timings(
    const az_iot_connection_client* c,
    az_iot_mqtt_connect_options* copts)
{
  copts->keep_alive_seconds
      = c->opts.keep_alive_seconds ? c->opts.keep_alive_seconds : AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS;
  copts->connect_timeout_seconds = c->opts.connect_timeout_seconds
      ? c->opts.connect_timeout_seconds
      : AZ_IOT_DEFAULT_CONNECT_TIMEOUT_SECONDS;
}

/* Default broker port for a transport. Every Azure endpoint this client talks
 * to is TLS, so only the TLS ports appear here: 8883 for MQTT over TCP, 443 for
 * MQTT over WebSockets. */
static uint16_t default_port_for_transport(az_iot_mqtt_transport transport)
{
  return transport == AZ_IOT_MQTT_TRANSPORT_WEBSOCKET
      ? (uint16_t)AZ_IOT_MQTT_DEFAULT_PORT_WEBSOCKET_TLS
      : (uint16_t)AZ_IOT_MQTT_DEFAULT_PORT_TCP_TLS;
}

/* Apply the caller's transport, WebSocket path and proxy to one connect, and
 * resolve the port: an explicit @p port wins, 0 means "derive from transport".
 *
 * Shared by the DPS bootstrap connect and the hub connect on purpose. A device
 * that needs a proxy or WebSockets to reach the hub needs them to reach DPS
 * too, so applying this to only one of the two connects would leave the device
 * unable to provision at all. */
static void resolve_connect_transport(
    const az_iot_connection_client* c,
    az_iot_mqtt_connect_options* copts,
    uint16_t port)
{
  copts->transport = c->opts.transport;
  copts->websocket_path = c->opts.websocket_path;
  copts->proxy = c->opts.proxy;
  copts->port = port ? port : default_port_for_transport(c->opts.transport);
}

/* Apply the session semantics for one role: Clean Start / Clean Session, the
 * MQTT 5 Session Expiry Interval, the Last Will, and the reason code the
 * session will be closed with.
 *
 * Every CONNECT this client issues used to go out with whatever the
 * zero-initialized options struct yielded, which is clean_start = false for all
 * three roles -- so DPS and MQTTv5 both asked the broker to resume a session
 * neither of them has any use for. The three roles do not want the same thing,
 * and the choice is made here, in one place, rather than at the two connect
 * sites:
 *
 *  - DPS (v3.1.1): a clean session, and not overridable. The provisioning
 *    service does not implement session persistence at all -- it treats every
 *    session as non-persistent whatever the CONNECT flag says -- so there is no
 *    state for a resumed session to carry and asking for one would promise
 *    something the service does not do. dps_start() re-subscribes
 *    $dps/registrations/res/# on every attempt regardless.
 *
 *    This reason is deliberately a property of the SERVICE, not of how long the
 *    session happens to live. The session is torn down at registration today,
 *    but a feature client (software updates) can hold one open past that, and a longer-lived
 *    or hub-concurrent DPS session does not change the answer: clean_start is
 *    only read at CONNECT, and it is inert at this service whenever it is read.
 *
 *  - HUB_MQTT_V3 (v3.1.1): a PERSISTENT session, which is the behaviour this
 *    role already had and is kept deliberately. MQTTv3 IoT Hub holds a
 *    device's subscriptions, and the cloud-to-device messages that arrived
 *    while it was away, only for a session that is NOT clean; connecting clean
 *    would silently drop whatever was queued during an outage. The SUBSCRIBEs
 *    are re-issued on every connect either way (begin_feature_subscriptions),
 *    so resuming costs nothing and losing the queue costs delivery.
 *
 *  - HUB_MQTT_V5 (v5): a resumed session as well, with a Session Expiry Interval
 *    so there is something left to resume. The presence design is explicit that
 *    this is a TRANSPORT EFFICIENCY choice and not a correctness one: the
 *    backend never reads clean_start or session_present, the device always
 *    publishes birth, and every feature protocol is correct even if each
 *    connect started a fresh session. What resuming buys is the broker's QoS 1
 *    redelivery across a transient drop, and a re-subscribe saved. Both halves
 *    are needed -- a session that expires the instant the connection closes is
 *    gone before any reconnect could resume it -- which is why the expiry is
 *    set here and not left at 0.
 *
 * Either hub role may be overridden by the caller through
 * opts.session_continuity; DPS may not, because the provisioning service does
 * not implement session persistence at all and honouring a request for it
 * would be promising something the service does not do.
 *
 * The v5-only fields are set for the v5 role only. A v3.1.1 broker must never
 * be sent a Session Expiry Interval or a Will Delay Interval -- there is no
 * property field in a v3.1.1 CONNECT to carry them. */
static void resolve_session_options(
    const az_iot_connection_client* c,
    az_iot_mqtt_connect_options* copts,
    az_iot_mqtt_role role)
{
  if (role == AZ_IOT_MQTT_ROLE_DPS)
  {
    copts->clean_start = true;
  }
  else
  {
    copts->clean_start
        = (c->opts.session_continuity == AZ_IOT_SESSION_CONTINUITY_CLEAN) ? true : false;
  }
  copts->session_expiry_seconds = 0;
  copts->disconnect_reason_code = (uint8_t)AZ_IOT_MQTT_DISCONNECT_NORMAL;

  /* Session Expiry is an MQTT 5 property, so it exists only for the v5 hub.
   * A session the caller asked to be clean still carries it: clean_start
   * discards whatever was there at CONNECT, while the expiry governs what
   * happens to THIS session after it ends, and those are independent. */
  if (role == AZ_IOT_MQTT_ROLE_HUB_MQTT_V5)
  {
    copts->session_expiry_seconds = c->opts.session_expiry_seconds
        ? c->opts.session_expiry_seconds
        : (uint32_t)AZ_IOT_DEFAULT_SESSION_EXPIRY_SECONDS;
  }

  /* The Will belongs to the hub session. Nothing consumes a will published from
   * a provisioning session: DPS has no presence or device-lifecycle protocol to
   * deliver one to, and opts.lwt is the application's signal about its DEVICE,
   * which is reachable through the hub. Announcing a departure on a channel
   * with no listener is worse than not announcing it.
   *
   * Stated without reference to how long the session lives, deliberately. It
   * used to read "the DPS session is torn down as soon as the assignment lands,
   * so a will could only fire for a failed attempt" -- true today, but a feature
   * client can hold the session open past registration, which would make that
   * sentence false while the conclusion stayed correct. */
  if (role == AZ_IOT_MQTT_ROLE_DPS || !is_nonempty_cstr(c->opts.lwt.topic))
  {
    return;
  }

  copts->lwt.topic = c->opts.lwt.topic;
  copts->lwt.payload = c->opts.lwt.payload;
  copts->lwt.payload_len = c->opts.lwt.payload_len;
  copts->lwt.qos = c->opts.lwt.qos;
  copts->lwt.retain = c->opts.lwt.retain;

  if (role != AZ_IOT_MQTT_ROLE_HUB_MQTT_V5)
  {
    return; /* v3.1.1: no will delay, no reason codes */
  }

  copts->lwt.will_delay_seconds = c->opts.lwt.will_delay_seconds;
  /* MQTT 5 ends the Will Delay at whichever comes first, the delay or the
   * session expiry, so a delay asked for on a session that expires at once is
   * no delay at all. Carry the session far enough to honour it rather than
   * accepting the option and then ignoring it. */
  if (copts->lwt.will_delay_seconds > copts->session_expiry_seconds)
  {
    copts->session_expiry_seconds = copts->lwt.will_delay_seconds;
  }
  /* An orderly close discards the Will by default. A device that configured one
   * wants its departure announced however it leaves, so close with 0x04. */
  copts->disconnect_reason_code = (uint8_t)AZ_IOT_MQTT_DISCONNECT_WITH_WILL_MESSAGE;
}

static void teardown_active(az_iot_connection_client* c)
{
  if (c->active_client && c->active_client->iface && c->active_client->iface->destroy)
  {
    c->active_client->iface->destroy(c->active_client);
  }
  c->active_client = NULL;
  /* Abandon any in-flight MQTTv5 presence (birth) handshake: it belonged to the
   * now-destroyed session and must restart from CONNACK on the next connect. */
  c->presence.phase = PRESENCE_PHASE_NONE;
  /* Same for the subscription gate. Its packet ids died with the session and
   * will never be acked, so nothing is left for a deadline to wait on. */
  memset(&c->subscription_gate, 0, sizeof(c->subscription_gate));
  /* Complete any pending PUBACK correlation entries with an error. The
   * packet_ids belonged to the now-destroyed session and will never be
   * acknowledged, so silently dropping the callbacks would leave the caller
   * tracking a publish that can no longer finish either way. QoS 1 is
   * at-least-once: the publish must be retried after reconnect, and telling
   * the caller so is what makes that retry deliberate.
   *
   * The slot is released BEFORE the callback runs so a callback that
   * republishes immediately can claim it. */
  for (size_t i = 0; i < AZ_IOT_MAX_PENDING_PUBACKS; ++i)
  {
    az_iot_publish_ack_callback cb = c->pending_pubacks[i].cb;
    void* user_ctx = c->pending_pubacks[i].user_ctx;
    bool was_awaiting_puback = c->pending_pubacks[i].in_use;

    c->pending_pubacks[i].in_use = false;
    c->pending_pubacks[i].cb = NULL;
    c->pending_pubacks[i].user_ctx = NULL;
    c->pending_pubacks[i].packet_id = 0;

    if (was_awaiting_puback && cb)
    {
      cb(AZ_IOT_ERR_NOT_CONNECTED, user_ctx);
    }
  }

  /* Tell the feature clients the session is gone. A twin GET correlated by
   * $rid, or an open certificate renewal, can never be answered now: the
   * response would have travelled on the session that just died. Without this
   * they hold their slot forever and the pool is permanently one entry
   * smaller after every outage.
   *
   * Runs last so a handler that immediately re-issues its request sees a fully
   * torn-down session and gets a clean NOT_CONNECTED rather than publishing
   * into a half-freed adapter. */
  for (size_t i = 0; i < AZ_IOT_MAX_SESSION_HANDLERS; ++i)
  {
    if (c->session_handlers[i].in_use && c->session_handlers[i].cb)
    {
      c->session_handlers[i].cb(c->session_handlers[i].user_ctx);
    }
  }
}

/* Forward decl — used in on_mqtt_event via the deferred-action queue. */
static az_iot_result start_connect_attempt(az_iot_connection_client* c);
static bool dps_configured(const az_iot_connection_client* c);
static az_iot_result run_feature_client_binds(az_iot_connection_client* c);
static void drop_subscriptions_from_other_generations(az_iot_connection_client* c);

/* Forward decls — used in dps_apply_deferred(). */
static az_iot_result check_owned_string(size_t buf_cap, const char* s);
static az_iot_result replace_owned_string(
    char* owned_buf,
    size_t buf_cap,
    const char** opts_slot,
    const char* s);

/* ------------------------------------------------------------------------- */
/* certificate material -> TLS options                                       */
/* ------------------------------------------------------------------------- */

/* Adapts the provider's sign() slot (D8) to the plain callback shape the MQTT
 * interface carries, so az_iot_mqtt_iface.h never has to know the certificate
 * provider ABI. `ctx` is the provider itself. */
static az_iot_result provider_sign_adapter(
    void* ctx,
    const uint8_t* digest,
    size_t digest_len,
    uint8_t* out_sig,
    size_t out_sig_cap,
    size_t* out_sig_len)
{
  az_iot_certificate_provider* p = (az_iot_certificate_provider*)ctx;
  /* The whole chain is re-checked rather than assumed. This runs inside an
   * adapter's TLS callback, where the context came back through a third-party
   * library: a wrong or stale pointer must produce a failed handshake, not a
   * crash in the middle of one. */
  if (!p || !p->vtable || p->vtable->version < CERT_PROVIDER_VTABLE_V2 || !p->vtable->sign)
  {
    /* The only evidence the caller gets is a failed handshake several frames
     * away inside a third-party TLS stack, so say which link of the chain was
     * missing while it is still known. */
    AZ_IOT_LOG_ERRORF(
        "connection: TLS asked the certificate provider to sign, but the provider is unusable "
        "(provider=%s vtable=%s version=%u sign=%s)",
        p ? "set" : "NULL",
        (p && p->vtable) ? "set" : "NULL",
        (p && p->vtable) ? (unsigned)p->vtable->version : 0u,
        (p && p->vtable && p->vtable->sign) ? "set" : "NULL");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* az_iot_mqtt_sign_callback promises that *out_sig_len is untouched on
   * failure. The provider vtable makes no such promise, and this trampoline is
   * the boundary where the MQTT-side contract is made, so it is enforced here
   * rather than assumed of every provider. */
  size_t saved_len = out_sig_len ? *out_sig_len : 0u;
  az_iot_result r = p->vtable->sign(p, digest, digest_len, out_sig, out_sig_cap, out_sig_len);
  if (r != AZ_IOT_OK && out_sig_len)
  {
    *out_sig_len = saved_len;
  }
  return r;
}

/* True when the provider exposes a usable sign() hook. The v2 slot only exists
 * from vtable version 2, so the version is checked before the pointer -- the
 * same gating start_csr()/open() apply to get_csr. */
static bool provider_has_sign(const az_iot_certificate_provider* p)
{
  return p != NULL && p->vtable != NULL && p->vtable->version >= CERT_PROVIDER_VTABLE_V2
      && p->vtable->sign != NULL;
}

/* Can this credential set possibly complete a TLS handshake? A client
 * certificate with no private key in ANY form cannot, and saying so here is the
 * difference between a named error and an unexplained handshake failure.
 *
 * Material carrying no client certificate at all is left alone: that is a
 * server-authentication-only connection, which is legitimate. */
static az_iot_result validate_certificate_material(
    const az_iot_certificate_material* mat,
    bool has_sign)
{
  bool has_cert = mat->client_cert_pem != NULL || mat->client_cert_path != NULL;
  if (!has_cert)
  {
    return AZ_IOT_OK;
  }
  bool has_key = mat->client_key_pem != NULL || mat->client_key_path != NULL
      || mat->client_key_uri != NULL || has_sign;
  if (!has_key)
  {
    AZ_IOT_LOG_ERROR("connection: certificate material carries a client certificate but no private "
                     "key (no PEM, no file, no key URI, no sign() hook)");
    return AZ_IOT_ERR_CREDENTIAL_INCOMPLETE;
  }
  /* A key URI without an engine/provider id names a key nothing can resolve:
   * the URI scheme alone does not say which ENGINE or OpenSSL provider owns
   * it. Adapters would have to guess, so it is rejected here instead. */
  if (mat->client_key_uri != NULL && mat->crypto_engine_id == NULL && !has_sign)
  {
    AZ_IOT_LOG_ERROR("connection: certificate material sets client_key_uri without "
                     "crypto_engine_id and provides no sign() hook");
    return AZ_IOT_ERR_CREDENTIAL_INCOMPLETE;
  }
  return AZ_IOT_OK;
}

/* Single copy site for credential material -> adapter connect options, shared
 * by the DPS/bootstrap path and the operational/reconnect path. It is one
 * function precisely because there are two callers: a field added to only one
 * of them breaks custody silently after the first certificate rotation. */
static az_iot_result apply_certificate_material(
    az_iot_mqtt_connect_options* copts,
    const az_iot_certificate_material* mat,
    az_iot_certificate_provider* prov)
{
  bool has_sign = provider_has_sign(prov);
  az_iot_result vr = validate_certificate_material(mat, has_sign);
  if (vr != AZ_IOT_OK)
  {
    return vr;
  }

  copts->tls.trusted_ca_path = mat->trusted_ca_path;
  copts->tls.client_cert_path = mat->client_cert_path;
  copts->tls.client_key_path = mat->client_key_path;
  copts->tls.client_key_password = mat->client_key_password;
  copts->tls.trusted_ca_pem = mat->trusted_ca_pem;
  copts->tls.client_cert_pem = mat->client_cert_pem;
  copts->tls.client_key_pem = mat->client_key_pem;
  copts->tls.client_key_uri = mat->client_key_uri;
  copts->tls.crypto_engine_id = mat->crypto_engine_id;
  copts->tls.sign = has_sign ? provider_sign_adapter : NULL;
  copts->tls.sign_ctx = has_sign ? (void*)prov : NULL;
  copts->tls.use_tls = true; /* this SDK always connects over TLS */
  return AZ_IOT_OK;
}

/**
 * @brief For a role falling back to SAS: keeps the trust anchors a provider
 * reported alongside AZ_IOT_ERR_NOT_FOUND (it has a CA but no certificate for
 * the role). opts.trusted_ca, applied later, still overrides them.
 */
static void keep_provider_trust(
    az_iot_mqtt_connect_options* copts,
    az_iot_result load_result,
    const az_iot_certificate_material* mat)
{
  if (load_result == AZ_IOT_ERR_NOT_FOUND)
  {
    copts->tls.trusted_ca_pem = mat->trusted_ca_pem;
    copts->tls.trusted_ca_path = mat->trusted_ca_path;
  }
}

/** @brief Replaces the provider's trust anchors with opts.trusted_ca, when set. */
static void apply_trusted_ca(const az_iot_connection_client* c, az_iot_mqtt_connect_options* copts)
{
  if (c->opts.trusted_ca.pem != NULL || c->opts.trusted_ca.path != NULL)
  {
    copts->tls.trusted_ca_pem = c->opts.trusted_ca.pem;
    copts->tls.trusted_ca_path = c->opts.trusted_ca.path;
  }
  copts->tls.use_tls = true;
}

/** @brief Current Unix time from opts.unix_time, else time(); 0 when unknown. */
static uint64_t unix_now(const az_iot_connection_client* c)
{
  if (c->opts.unix_time.get_time != NULL)
  {
    return c->opts.unix_time.get_time(c->opts.unix_time.user_ctx);
  }
  time_t t = time(NULL);
  return t > 0 ? (uint64_t)t : 0u;
}

/**
 * @brief Signs a SAS token with @p scope's primary key and sets it as the
 * CONNECT password, over server-authenticated TLS.
 *
 * @return AZ_IOT_OK; AZ_IOT_ERR_BUSY while no Unix time is known;
 * AZ_IOT_ERR_NOT_ENOUGH_SPACE when the token exceeds AZ_IOT_SAS_TOKEN_BUF;
 * AZ_IOT_ERR_INVALID_ARG when an ID the token needs is missing; the crypto
 * backend's error otherwise.
 */
static az_iot_result apply_sas_key(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_mqtt_connect_options* copts)
{
  bool is_dps = scope == AZ_IOT_CONN_SCOPE_DPS;
  const az_iot_auth* auth = is_dps ? &c->opts.dps_auth : &c->opts.hub_auth;
  uint64_t now = unix_now(c);
  if (now == 0)
  {
    AZ_IOT_LOG_WARN("connection: no Unix time yet; cannot sign a SAS token");
    return AZ_IOT_ERR_BUSY;
  }
  char uri[AZ_IOT_SAS_TOKEN_BUF];
  az_iot_result r = az_iot_sas__resource_uri(
      is_dps,
      is_dps ? c->opts.dps.id_scope : c->opts.host,
      is_dps ? c->opts.dps.registration_id : c->opts.client_id,
      uri,
      sizeof(uri));
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF("connection: SAS resource URI failed (%d)", (int)r);
    return r;
  }
  uint32_t lifetime = auth->sas.token_lifetime_seconds != 0
      ? auth->sas.token_lifetime_seconds
      : (uint32_t)AZ_IOT_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS;
  r = az_iot_sas__build_token(
      c->opts.crypto,
      c->auth[scope].primary_key,
      c->auth[scope].primary_key_len,
      uri,
      is_dps ? "registration" : "",
      now + lifetime,
      c->auth[scope].token,
      sizeof(c->auth[scope].token));
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF("connection: SAS token signing failed (%d)", (int)r);
    return r;
  }
  copts->password = c->auth[scope].token;
  c->auth[scope].source = AZ_IOT_AUTH_SOURCE_PRIMARY_KEY;
  apply_trusted_ca(c, copts);
  return AZ_IOT_OK;
}

/* Schedule a retry.
 *
 * @p failure_scope says WHERE the failure just happened, which is not always
 * the scope of the next attempt: a hub CONNACK that rejects the identity is a
 * HUB failure whose retry is a DPS registration. The two are used for
 * different things -- the failure scope feeds the hub-unreachable counter, the
 * next attempt's scope selects the backoff ladder and its attempt budget. */
static void schedule_reconnect(
    az_iot_connection_client* c,
    az_iot_connection_scope failure_scope,
    az_iot_result reason)
{
  teardown_active(c);

  /* A hub vacated service-side may stop answering rather than rejecting the
   * identity, in which case nothing else would ever send us back to DPS. Only
   * hub failures count -- a failing registration must fall back to an ordinary
   * retry rather than re-arming this and pinning every attempt to DPS.
   *
   * Kept separate from retry_attempt[HUB] on purpose: this counts attempts
   * towards a re-provision DECISION and is reset when that decision is taken,
   * while the ladder counts backoff position and is reset by a successful
   * connect or by open()/close(). Folding them together would make either
   * reset silently move the other. */
  if (failure_scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    c->consecutive_hub_connect_failures++;
  }
  if (!c->needs_reprovision && dps_configured(c) && !c->user_close
      && c->opts.dps.max_hub_connect_attempts_before_reprovision > 0
      && c->consecutive_hub_connect_failures
          >= c->opts.dps.max_hub_connect_attempts_before_reprovision)
  {
    AZ_IOT_LOG_WARN("connection: hub unreachable for the configured number of attempts; "
                    "re-provisioning through DPS");
    c->consecutive_hub_connect_failures = 0;
    c->needs_reprovision = true;
    /* Crossing into provisioning starts the DPS ladder at the beginning: the
     * hub attempts that got us here say nothing about how long DPS will take,
     * so the first registration attempt must wait initial_delay_ms rather than
     * inherit the hub's exhausted backoff. */
    c->retry_attempt[AZ_IOT_CONN_SCOPE_DPS] = 0;
  }

  /* Which ladder this retry climbs: the scope of the attempt about to be
   * scheduled, not of the failure. needs_reprovision is exactly what do_work()
   * reads to decide the next attempt is a registration rather than a hub
   * connect, so it derives the same answer there rather than reading a stored
   * copy of this one. */
  az_iot_connection_scope scope
      = c->needs_reprovision ? AZ_IOT_CONN_SCOPE_DPS : AZ_IOT_CONN_SCOPE_HUB;
  /* Per ladder, so a long hub outage cannot spend the budget a registration
   * that has not been tried yet would need. */
  uint32_t delay = 0;
  if (!az_iot_retry_policy__next(
          &c->opts.reconnection_policy, &c->retry_attempt[scope], &c->rng_state, &delay))
  {
    set_state_to(c, failure_scope, AZ_IOT_CONN_STATE_FAULTED, reason);
    return;
  }
  c->reconnect_due_ms = az_iot_time_mono_ms() + delay;
  /* Reported against the scope that FAILED, not the ladder the retry climbs:
   * a hub CONNACK that rejects the identity is a HUB session going down, even
   * though the recovery attempt is a DPS registration. The ladder scope is
   * separate and lives in retry_attempt[] above. */
  set_state_to(c, failure_scope, AZ_IOT_CONN_STATE_RECONNECTING, reason);
}

/* ------------------------------------------------------------------------- */
/* DPS provisioning (internal, driven from open/do_work)                     */
/* ------------------------------------------------------------------------- */

static bool dps_configured(const az_iot_connection_client* c)
{
  return is_nonempty_cstr(c->opts.dps.id_scope);
}

/* The session exists exactly while this is true. */
/* Reset the user-session ladder. Called wherever the evidence says the
 * provisioning service is reachable again, or the demand has been rebuilt. */
static void dps_user_retry_reset(az_iot_connection_client* c)
{
  az_iot_retry_state__reset(&c->dps_user_retry);
  c->dps_user_retry_blocked = false;
}

/* Space out the next attempt to re-open a session held for its users, after
 * this one failed. `retry_after_secs` is the service's own floor, if it gave
 * one.
 *
 * Distinct from schedule_reconnect(): nothing is torn down and no state change
 * is announced. The DPS scope has already settled at IDLE by the time this
 * runs; all that is recorded is when a holder may ask again. */
static void dps_user_retry_schedule(az_iot_connection_client* c, uint32_t retry_after_secs)
{
  /* 0 disables retrying, and az_iot_retry_policy__delay_ms() would return a 0 ms
   * delay for it -- which is the hot loop, not a fix for it. An application
   * that turned retries off owns the decision to try again, and reaches it by
   * closing the client or by dropping and re-taking the ref. */
  if (!az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy))
  {
    c->dps_user_retry_blocked = true;
    c->dps_user_retry._internal.due_ms = 0;
    return;
  }

  if (!az_iot_retry_state__schedule(
          &c->dps_user_retry, &c->opts.reconnection_policy, &c->rng_state))
  {
    c->dps_user_retry_blocked = true;
    return;
  }

  /* The service's retry-after is a floor, never a ceiling -- the same rule the
   * registration ladder applies. Backing off further than asked is allowed;
   * coming back sooner is not. */
  if (retry_after_secs > 0)
  {
    az_iot_retry_state__defer(
        &c->dps_user_retry, az_iot_time_mono_ms() + (uint64_t)retry_after_secs * 1000ull);
  }
}

static bool dps_refs_held(const az_iot_connection_client* c)
{
  return c->dps_registration_ref || c->dps_user_count > 0 || c->dps_standing_ref;
}

/**
 * @brief Whether anyone may ask for a session through dps_session_ensure(): a
 * feature client holding a user ref, or a provision_only client's standing ref.
 *
 * The registration ref is excluded: a registration starts its own session from
 * open(), and a second entry point would duplicate that.
 */
static bool dps_session_demanded(const az_iot_connection_client* c)
{
  return c->dps_user_count > 0 || c->dps_standing_ref;
}

static void dps_teardown_mqtt(az_iot_connection_client* c)
{
  c->dps_subscription_confirmed = false;
  /* A session that is gone never became ready. */
  c->dps_pending_ready_announce = false;
  /* The live hold belongs to the session. The holder count is the feature
   * client's standing interest and deliberately survives, so a reprovision
   * holds again rather than racing past. */
  c->dps_hold_active = false;
  c->dps_hold_deadline_ms = 0;
  if (c->dps_mqtt && c->dps_mqtt->iface && c->dps_mqtt->iface->destroy)
  {
    c->dps_mqtt->iface->destroy(c->dps_mqtt);
  }
  c->dps_mqtt = NULL;
}

static void dps_finalize(az_iot_connection_client* c, az_iot_result status, bool have_assignment)
{
  if (c->dps_pending_finalize)
  {
    return;
  }
  c->dps_pending_finalize = true;
  c->dps_pending_status = status;
  c->dps_pending_have_assignment = have_assignment;
}

/* Bound on the pre-registration hold. */
static uint64_t dps_hold_timeout_ms(const az_iot_connection_client* c)
{
  return c->opts.dps_hold_timeout_ms ? (uint64_t)c->opts.dps_hold_timeout_ms
                                     : (uint64_t)AZ_IOT_DPS_HOLD_TIMEOUT_MS;
}

/* Where the registration body is built. The dedicated buffer applies only to a
 * body that actually carries the custom payload, which is what its documented
 * contract promises: a CSR-only registration keeps building in
 * csr_payload_buffer exactly where it always did, even when the caller also
 * supplied a registration_body_buffer sized for something else. Without a
 * dedicated buffer the payload body falls back to csr_payload_buffer too, so a
 * device that already provides one only has to enlarge it to add a payload. */
static az_span dps_register_body_buffer(const az_iot_connection_client* c)
{
  bool have_payload = az_span_size(c->opts.dps.registration_payload) > 0;
  return (have_payload && az_span_size(c->opts.dps.registration_body_buffer) > 0)
      ? c->opts.dps.registration_body_buffer
      : c->opts.csr_payload_buffer;
}

/* Shallow but definitive validation of opts.dps.registration_payload: exactly
 * one well-formed JSON object, with nothing after it.
 *
 * The SDK does not interpret the contents -- they belong to the allocation
 * policy -- but it will not embed bytes that cannot be valid JSON either. A
 * malformed payload would otherwise surface as an opaque DPS protocol failure
 * on a device in the field rather than as a configuration error at open(). An
 * OBJECT specifically, because that is what `payload` is on both directions of
 * the DPS contract: azure-sdk-for-c's response parser accepts only an object
 * or null there, so a scalar or array could not even round-trip. */
static az_iot_result dps_validate_registration_payload(az_span payload)
{
  /* Guard the span before handing it to az_core. az_json_reader_init()
   * precondition-checks it, and this project builds with preconditions on and
   * installs no handler, so az_core's default handler would spin this thread
   * forever on a span the caller got wrong (a NULL pointer with a nonzero size
   * is the easy way to produce one). The same reason the registration response
   * is length-checked before it reaches the provisioning parser. */
  if (az_span_ptr(payload) == NULL || az_span_size(payload) <= 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_json_reader jr;
  if (az_result_failed(az_json_reader_init(&jr, payload, NULL))
      || az_result_failed(az_json_reader_next_token(&jr))
      || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT
      || az_result_failed(az_json_reader_skip_children(&jr)))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Trailing content: a second value after the object would splice two JSON
   * documents into one body. az_json_reader reports both "document ended" and
   * "garbage follows" as a failed next_token, so the remainder is inspected
   * directly instead -- only insignificant whitespace may follow. */
  az_span end_token = jr.token.slice;
  uint8_t* begin = az_span_ptr(payload);
  int32_t consumed = (int32_t)(az_span_ptr(end_token) - begin) + az_span_size(end_token);
  for (int32_t i = consumed; i < az_span_size(payload); ++i)
  {
    uint8_t ch = begin[i];
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
  }
  return AZ_IOT_OK;
}

/* True when two spans share any byte. The registration payload is copied INTO
 * the body build buffer, so a caller that points both at the same storage would
 * have the writer overwrite the payload with the `{"payload":` prefix before
 * reading it, and then memcpy overlapping regions -- undefined behaviour, and a
 * malformed body if it survived. Cheap to detect, so it is rejected at open()
 * instead. Plausible rather than theoretical on a no-allocation SDK, where a
 * memory-constrained caller may try to reuse one buffer for both.
 *
 * Compared as integers, and by DIFFERENCE rather than by computing an end
 * address: relational comparison of pointers into different objects is
 * undefined in C (6.5.8p5), and separate arrays are the normal, valid case
 * here, so the check itself must not rely on it. Subtracting the smaller
 * address from the larger cannot overflow either. */
static bool spans_overlap(az_span a, az_span b)
{
  uint8_t* a_ptr = az_span_ptr(a);
  uint8_t* b_ptr = az_span_ptr(b);
  if (a_ptr == NULL || b_ptr == NULL || az_span_size(a) <= 0 || az_span_size(b) <= 0)
  {
    return false;
  }

  uintptr_t a0 = (uintptr_t)a_ptr;
  uintptr_t b0 = (uintptr_t)b_ptr;
  return (a0 >= b0) ? ((a0 - b0) < (uintptr_t)az_span_size(b))
                    : ((b0 - a0) < (uintptr_t)az_span_size(a));
}

/* Build the registration body into @p destination. @p csr_base64 is NULL when
 * this registration carries no CSR, @p payload empty when it carries no custom
 * payload; the result is one JSON object holding whichever members are present.
 * A destination too small latches inside the writer and is reported here, so a
 * body is never published half-built. */
static az_iot_result dps_build_register_body(
    az_span destination,
    const char* csr_base64,
    az_span payload,
    size_t* out_len)
{
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, destination);
  az_iot_span_writer_append_str(&writer, DPS_REGISTER_BODY_OPEN);
  if (csr_base64 != NULL)
  {
    az_iot_span_writer_append_str(&writer, DPS_REGISTER_CSR_MEMBER_PREFIX);
    az_iot_span_writer_append_str(&writer, csr_base64);
    az_iot_span_writer_append_str(&writer, DPS_REGISTER_CSR_MEMBER_SUFFIX);
  }
  if (az_span_size(payload) > 0)
  {
    if (csr_base64 != NULL)
    {
      az_iot_span_writer_append_str(&writer, DPS_REGISTER_BODY_SEPARATOR);
    }
    az_iot_span_writer_append_str(&writer, DPS_REGISTER_PAYLOAD_MEMBER_PREFIX);
    az_iot_span_writer_append_span(&writer, payload);
  }
  az_iot_span_writer_append_str(&writer, DPS_REGISTER_BODY_CLOSE);

  az_span written = AZ_SPAN_EMPTY;
  az_iot_result result = az_iot_span_writer_end(&writer, &written);
  if (result != AZ_IOT_OK)
  {
    return result;
  }
  *out_len = (size_t)az_span_size(written);
  return AZ_IOT_OK;
}

static az_iot_result dps_do_register_publish(az_iot_connection_client* c)
{
  char topic[AZ_IOT_DPS_TOPIC_BUF];
  size_t topic_len = 0;
  az_result ar = az_iot_provisioning_client_register_get_publish_topic(
      &c->dps_prov, topic, sizeof(topic), &topic_len);
  if (az_result_failed(ar))
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = NULL;
  msg.payload_len = 0;
  msg.qos = AZ_IOT_MQTT_QOS_1;
  msg.retain = false;

  /* Body members (both optional, both may be present):
   *   - CSR-based enrollment (D2): the provider's CSR, which asks DPS for an
   *     operational certificate.
   *   - the caller's custom registration payload, which DPS forwards to a
   *     custom-allocation policy.
   * Built into a CALLER-PROVIDED buffer - the SDK declares no payload buffer of
   * its own. With neither configured the registration body stays empty, exactly
   * as before. The registration id travels in the DPS username/topic, not the
   * body. */
  az_span custom_payload
      = (az_span_size(c->opts.dps.registration_payload) > 0 ? c->opts.dps.registration_payload
                                                            : AZ_SPAN_EMPTY);
  if (c->dps_enrolling || az_span_size(custom_payload) > 0)
  {
    az_span body_buffer = dps_register_body_buffer(c);
    if (az_span_ptr(body_buffer) == NULL || az_span_size(body_buffer) <= 0)
    {
      AZ_IOT_LOG_ERROR("dps register: a registration body was configured but neither "
                       "opts.dps.registration_body_buffer nor opts.csr_payload_buffer was "
                       "provided to build it in");
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }

    az_iot_certificate_provider* provider = NULL;
    az_iot_certificate_signing_request csr = { 0 };
    if (c->dps_enrolling)
    {
      provider = c->opts.certificate_provider;
      if (provider == NULL || provider->vtable->get_csr == NULL)
      {
        AZ_IOT_LOG_ERROR("dps register: request_operational_certificate is set but the certificate "
                         "provider does not implement get_csr");
        return AZ_IOT_ERR_NOT_SUPPORTED;
      }

      az_iot_result csr_result
          = provider->vtable->get_csr(provider, c->opts.dps.registration_id, &csr);
      if (csr_result != AZ_IOT_OK || csr.csr_base64 == NULL)
      {
        AZ_IOT_LOG_ERROR("dps register: certificate provider get_csr failed");
        return (csr_result != AZ_IOT_OK) ? csr_result : AZ_IOT_ERR_INTERNAL;
      }
    }

    size_t body_len = 0;
    az_iot_result body_result = dps_build_register_body(
        body_buffer, c->dps_enrolling ? csr.csr_base64 : NULL, custom_payload, &body_len);

    if (provider != NULL && provider->vtable->release_csr != NULL)
    {
      provider->vtable->release_csr(provider, &csr);
    }
    if (body_result != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERROR("dps register: the registration body build buffer is too small for the "
                       "configured CSR and/or custom payload");
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }

    msg.payload = az_span_ptr(body_buffer);
    msg.payload_len = body_len;
  }

  uint16_t pid = 0;
  az_iot_result r = c->dps_mqtt->iface->publish(c->dps_mqtt, &msg, &pid);
  if (r == AZ_IOT_OK)
  {
    c->dps_phase = DPS_PHASE_REGISTERING;
  }
  return r;
}

static az_iot_result dps_do_query_publish(az_iot_connection_client* c)
{
  char topic[AZ_IOT_DPS_TOPIC_BUF];
  size_t topic_len = 0;
  az_span op_id = az_span_create((uint8_t*)c->dps_operation_id, (int32_t)c->dps_operation_id_len);
  az_result ar = az_iot_provisioning_client_query_status_get_publish_topic(
      &c->dps_prov, op_id, topic, sizeof(topic), &topic_len);
  if (az_result_failed(ar))
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.qos = AZ_IOT_MQTT_QOS_1;

  uint16_t pid = 0;
  az_iot_result r = c->dps_mqtt->iface->publish(c->dps_mqtt, &msg, &pid);
  if (r == AZ_IOT_OK)
  {
    c->dps_phase = DPS_PHASE_REGISTERING;
  }
  return r;
}

/* Position `jr` on the BEGIN_OBJECT of registrationState in a DPS response.
 * Both the issued-certificate chain and the connection profile live there, and
 * azure-sdk-for-c surfaces neither, so both walk the raw payload from here. */
static az_iot_result dps_enter_registration_state(az_json_reader* jr, az_span payload)
{
  if (az_result_failed(az_json_reader_init(jr, payload, NULL))
      || az_result_failed(az_json_reader_next_token(jr))
      || jr->token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return AZ_IOT_ERR_PROTOCOL;
  }

  while (az_result_succeeded(az_json_reader_next_token(jr))
         && jr->token.kind != AZ_JSON_TOKEN_END_OBJECT)
  {
    if (jr->token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
    {
      continue;
    }
    bool m = az_json_token_is_text_equal(&jr->token, AZ_SPAN_FROM_STR(DPS_JSON_REGISTRATION_STATE));
    if (az_result_failed(az_json_reader_next_token(jr)))
    {
      return AZ_IOT_ERR_PROTOCOL;
    }
    if (m && jr->token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT)
    {
      return AZ_IOT_OK;
    }
    if (jr->token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr->token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
    {
      if (az_result_failed(az_json_reader_skip_children(jr)))
      {
        return AZ_IOT_ERR_PROTOCOL;
      }
    }
  }
  return AZ_IOT_ERR_NOT_FOUND;
}

/* Record the connection profile from its verbatim wire form. The raw string is
 * always stored -- an unrecognised profile is exactly the case where the text
 * matters -- and only the enum degrades to UNKNOWN. A value too long for the
 * buffer is stored truncated and treated as unknown, which is correct: every
 * profile this SDK recognises is short, so an overlong one cannot be one of
 * them. */
static void connection_profile_set(az_iot_connection_client* c, az_span raw)
{
  int32_t n = az_span_size(raw);
  if (n < 0)
  {
    n = 0;
  }
  bool truncated = false;
  if ((size_t)n >= sizeof(c->connection_profile_raw))
  {
    n = (int32_t)(sizeof(c->connection_profile_raw) - 1);
    truncated = true;
  }
  if (n > 0)
  {
    memcpy(c->connection_profile_raw, az_span_ptr(raw), (size_t)n);
  }
  c->connection_profile_raw[n] = '\0';
  c->connection_profile_raw_truncated = truncated;

  if (truncated)
  {
    AZ_IOT_LOG_ERRORF(
        "connectionProfile is longer than %u bytes and was truncated to \"%s\"",
        (unsigned)sizeof(c->connection_profile_raw),
        c->connection_profile_raw);
    c->connection_profile = AZ_IOT_CONNECTION_PROFILE_UNKNOWN;
  }
  else if (strcmp(c->connection_profile_raw, CONNECTION_PROFILE_MQTT_V3_STR) == 0)
  {
    c->connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V3;
  }
  else if (strcmp(c->connection_profile_raw, CONNECTION_PROFILE_MQTT_V5_STR) == 0)
  {
    c->connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  }
  else
  {
    c->connection_profile = AZ_IOT_CONNECTION_PROFILE_UNKNOWN;
  }
}

/* Development bridge while the DPS api-version that carries connectionProfile
 * is not deployed. It applies only when the property is absent/null; an actual
 * wire value always wins, so enabling this cannot mask service rollout. */
static az_iot_result dps_apply_connection_profile_override(az_iot_connection_client* c)
{
  char value[AZ_IOT_CONNECTION_PROFILE_RAW_BUF];

  if (az_iot_env_read(DPS_CONNECTION_PROFILE_OVERRIDE_ENV, value, sizeof(value)) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("dps: " DPS_CONNECTION_PROFILE_OVERRIDE_ENV " is too long");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (value[0] == '\0')
  {
    return AZ_IOT_OK;
  }

  if (strcmp(value, CONNECTION_PROFILE_MQTT_V3_STR) != 0
      && strcmp(value, CONNECTION_PROFILE_MQTT_V5_STR) != 0)
  {
    AZ_IOT_LOG_ERRORF(
        "dps: %s must be \"classic\" or \"mqttV5\", not \"%s\"",
        DPS_CONNECTION_PROFILE_OVERRIDE_ENV,
        value);
    return AZ_IOT_ERR_INVALID_ARG;
  }

  AZ_IOT_LOG_WARNF(
      "dps: connectionProfile was absent/null; applying development override %s=%s",
      DPS_CONNECTION_PROFILE_OVERRIDE_ENV,
      value);
  connection_profile_set(c, az_span_create_from_str(value));
  return AZ_IOT_OK;
}

/* Read registrationState.connectionProfile from the DPS ASSIGNED payload.
 *
 * Absent or null is NOT an error -- the service contract documents it as
 * meaning "classic" -- so the caller is left with the MQTTv3 default it was
 * seeded with. Only a malformed payload fails here; an unrecognised *value*
 * fails later, at the point the session role is chosen, so the profile is
 * already recorded and readable when it does. */
static az_iot_result dps_read_connection_profile(az_iot_connection_client* c, az_span payload)
{
  az_json_reader jr;
  az_iot_result r = dps_enter_registration_state(&jr, payload);
  if (r != AZ_IOT_OK)
  {
    /* No registrationState at all: nothing to read, keep the default unless a
     * development override was requested. */
    return (r == AZ_IOT_ERR_NOT_FOUND) ? dps_apply_connection_profile_override(c) : r;
  }

  while (az_result_succeeded(az_json_reader_next_token(&jr))
         && jr.token.kind != AZ_JSON_TOKEN_END_OBJECT)
  {
    if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
    {
      continue;
    }
    bool m = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR(DPS_JSON_CONNECTION_PROFILE));
    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      return AZ_IOT_ERR_PROTOCOL;
    }
    if (m)
    {
      if (jr.token.kind == AZ_JSON_TOKEN_STRING)
      {
        connection_profile_set(c, jr.token.slice);
        return AZ_IOT_OK;
      }
      /* null (or any non-string) resolves to the MQTTv3 default unless the
       * development bridge explicitly supplies the profile. */
      return dps_apply_connection_profile_override(c);
    }
    if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
    {
      if (az_result_failed(az_json_reader_skip_children(&jr)))
      {
        return AZ_IOT_ERR_PROTOCOL;
      }
    }
  }
  return dps_apply_connection_profile_override(c);
}

/* Parse registrationState.issuedCertificateChain (an array of base64 DER certs)
 * from the DPS ASSIGNED payload, PEM-wrap each entry, and hand the chain to the
 * certificate_provider to persist as the operational identity. azure-sdk-for-c
 * does not surface this field, so we walk the raw payload with az_json. */
static az_iot_result dps_store_issued_cert(az_iot_connection_client* c, az_span payload)
{
  az_json_reader jr;
  az_iot_result entered = dps_enter_registration_state(&jr, payload);
  if (entered != AZ_IOT_OK)
  {
    return entered;
  }

  /* Find issuedCertificateChain array. */
  bool in_chain = false;
  while (az_result_succeeded(az_json_reader_next_token(&jr))
         && jr.token.kind != AZ_JSON_TOKEN_END_OBJECT)
  {
    if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
    {
      continue;
    }
    bool m = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR(DPS_JSON_ISSUED_CERT_CHAIN));
    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      return AZ_IOT_ERR_PROTOCOL;
    }
    if (m && jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
    {
      in_chain = true;
      break;
    }
    if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
    {
      if (az_result_failed(az_json_reader_skip_children(&jr)))
      {
        return AZ_IOT_ERR_PROTOCOL;
      }
    }
  }
  if (!in_chain)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }

  /* Collect the base64 chain (leaf first) as zero-copy spans into the payload. */
  az_span certs[CERT_CHAIN_MAX_CERTS];
  size_t count = 0;
  az_iot_result rc = az_iot_cert_util_collect_chain_spans(&jr, certs, CERT_CHAIN_MAX_CERTS, &count);
  if (rc != AZ_IOT_OK)
  {
    return rc;
  }
  if (count == 0)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }

  az_iot_certificate_provider* p = c->opts.certificate_provider;
  az_iot_issued_certificate issued;
  issued.certificates = certs;
  issued.count = count;

  /* Persist via the provider (if capable) and/or notify the app (D4). The
   * issued chain must be handled by at least one of the two. */
  bool handled = false;
  if (p && p->vtable && p->vtable->store_issued_certificate)
  {
    rc = p->vtable->store_issued_certificate(p, &issued);
    handled = (rc == AZ_IOT_OK);
  }
  if (rc == AZ_IOT_OK && c->op_cert_cb)
  {
    c->op_cert_cb(&issued, c->op_cert_cb_ctx);
    handled = true;
  }
  if (rc == AZ_IOT_OK && !handled)
  {
    rc = AZ_IOT_ERR_NOT_SUPPORTED;
  }
  return rc;
}

static void on_dps_mqtt_event(const az_iot_mqtt_event* evt, void* user_ctx)
{
  az_iot_connection_client* c = (az_iot_connection_client*)user_ctx;
  if (!c || !evt)
  {
    return;
  }

  /* Before the switch, so every failure route below carries its code without
   * each one having to remember. Staged, not dispatched: the transition that
   * reports it may not run until the pump drains the deferred queue. */
  stage_error_from_event(c, AZ_IOT_CONN_SCOPE_DPS, evt);

  switch (evt->kind)
  {
    case AZ_IOT_MQTT_EVT_CONNECTED:
      if (evt->status != AZ_IOT_OK)
      {
        dps_finalize(c, evt->status, false);
        return;
      }
      {
        uint16_t pid = 0;
        az_iot_result r = c->dps_mqtt->iface->subscribe(
            c->dps_mqtt,
            AZ_IOT_PROVISIONING_CLIENT_REGISTER_SUBSCRIBE_TOPIC,
            AZ_IOT_MQTT_QOS_1,
            &pid);
        if (r != AZ_IOT_OK)
        {
          dps_finalize(c, r, false);
          return;
        }
        c->dps_phase = DPS_PHASE_SUBSCRIBING;
      }
      break;

    case AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK:
      if (c->dps_phase != DPS_PHASE_SUBSCRIBING)
      {
        break;
      }
      if (evt->status != AZ_IOT_OK)
      {
        dps_finalize(c, evt->status, false);
        return;
      }
      c->dps_subscription_confirmed = true;

      /* The SUBACK is what makes the session usable, so it is DPS:CONNECTED.
       * Only RECORDED here: announcing runs observers, close() is legal from
       * one, and it would free this adapter while its process_loop is still on
       * the stack. The pump announces it once process_loop has returned. */
      c->dps_pending_ready_announce = true;

      /* Registration is a task, and it runs only while the registration ref is
       * held. Registering on a session opened for its other users would take
       * the assignment path -- rewriting host and role and reconnecting --
       * and tear down a hub connection this session may sit beside. */
      if (!c->dps_registration_ref)
      {
        AZ_IOT_LOG_DEBUG("dps: session ready for its users");
        /* It came up: whatever the ladder had climbed is spent evidence. */
        dps_user_retry_reset(c);
        break;
      }
      /* A holder wants the session before the device registers. Registration is
       * issued here normally, and the registration response tears the session
       * down, so without this stop there is no point at which a feature client
       * can use it. */
      if (c->dps_hold_count > 0)
      {
        c->dps_phase = DPS_PHASE_HOLD;
        c->dps_hold_active = true;
        c->dps_hold_deadline_ms = az_iot_time_mono_ms() + dps_hold_timeout_ms(c);
        AZ_IOT_LOG_DEBUG("dps: holding registration for a pre-registration exchange");
        break;
      }
      {
        az_iot_result r = dps_do_register_publish(c);
        if (r != AZ_IOT_OK)
        {
          dps_finalize(c, r, false);
          return;
        }
      }
      break;

    case AZ_IOT_MQTT_EVT_MESSAGE:
    {
      if (!evt->message || !evt->message->topic)
      {
        break;
      }
      /* Offer the message to the device-update observer first: it shares this
       * session and its responses arrive on the same subscribed filter, but the
       * provisioning parser below would reject them as malformed registration
       * responses and fault the attempt. */
      if (c->dps_message_observer != NULL)
      {
        if (c->dps_message_observer(
                evt->message->topic,
                evt->message->payload,
                evt->message->payload_len,
                c->dps_message_observer_ctx))
        {
          break;
        }
      }

      if (c->dps_phase != DPS_PHASE_REGISTERING && c->dps_phase != DPS_PHASE_POLLING)
      {
        break;
      }

      /* MQTT permits an empty payload, and an adapter may report that as a NULL
       * pointer with zero length. az_iot_provisioning_client_parse_received_topic_and_payload
       * requires a non-empty one -- _az_PRECONDITION_VALID_SPAN(received_payload, 1, false) --
       * and az_core's precondition handler does not return: it spins forever
       * ("when a precondition fails the calling thread spins forever"). Handing
       * it an empty body would therefore wedge this thread rather than fail the
       * attempt, so the check has to happen before the call -- the same reason
       * the registration id is validated before open() hands it over. An empty
       * registration response has no parseable outcome in any case. */
      if (evt->message->payload == NULL || evt->message->payload_len == 0)
      {
        AZ_IOT_LOG_ERRORF("dps register: empty response body on topic %s", evt->message->topic);
        dps_finalize(c, AZ_IOT_ERR_PROTOCOL, false);
        return;
      }

      az_span topic_span = az_span_create(
          (uint8_t*)(uintptr_t)evt->message->topic, (int32_t)strlen(evt->message->topic));
      az_span payload_span = az_span_create(
          (uint8_t*)(uintptr_t)evt->message->payload, (int32_t)evt->message->payload_len);

      az_iot_provisioning_client_register_response resp = { 0 };
      az_result ar = az_iot_provisioning_client_parse_received_topic_and_payload(
          &c->dps_prov, topic_span, payload_span, &resp);
      if (az_result_failed(ar))
      {
        /* A registration response we cannot parse is not something waiting
         * longer can fix: the operation this session was driving has no
         * knowable outcome. Silently ignoring it left the client in CONNECTING
         * with no fault, no re-poll and no diagnostic -- indistinguishable
         * from a hang. Fail the provisioning attempt instead, and print the
         * body so the cause is recoverable from a log. The reconnection
         * policy (if any) decides whether to try again. */
        AZ_IOT_LOG_ERRORF(
            "dps register: unparsable response on topic %s; body: %.*s",
            evt->message->topic,
            (int)az_span_size(payload_span),
            (const char*)az_span_ptr(payload_span));
        dps_finalize(c, AZ_IOT_ERR_PROTOCOL, false);
        return;
      }

      switch (resp.operation_status)
      {
        case AZ_IOT_PROVISIONING_STATUS_ASSIGNED:
        {
          int32_t hub_n = az_span_size(resp.registration_state.assigned_hub_hostname);
          int32_t dev_n = az_span_size(resp.registration_state.device_id);
          if (hub_n < 0 || (size_t)hub_n + 1 > sizeof(c->dps_assigned_hub) || dev_n < 0
              || (size_t)dev_n + 1 > sizeof(c->dps_assigned_device_id))
          {
            dps_finalize(c, AZ_IOT_ERR_NOT_SUPPORTED, false);
            return;
          }
          memcpy(
              c->dps_assigned_hub,
              az_span_ptr(resp.registration_state.assigned_hub_hostname),
              (size_t)hub_n);
          c->dps_assigned_hub[hub_n] = '\0';
          memcpy(
              c->dps_assigned_device_id,
              az_span_ptr(resp.registration_state.device_id),
              (size_t)dev_n);
          c->dps_assigned_device_id[dev_n] = '\0';
          /* Which hub generation we were assigned to. Read before the issued
           * cert so the profile is recorded even if the cert path fails. */
          az_iot_result pr = dps_read_connection_profile(c, payload_span);
          if (pr != AZ_IOT_OK)
          {
            dps_finalize(c, pr, false);
            return;
          }
          if (c->dps_enrolling)
          {
            az_iot_result sc = dps_store_issued_cert(c, payload_span);
            if (sc != AZ_IOT_OK)
            {
              dps_finalize(c, sc, false);
              return;
            }
            c->dps_have_issued_cert = true;
          }
          /* Surface the assignment's custom payload (registrationState.payload,
           * already parsed by azure-sdk-for-c as a zero-copy span into the
           * inbound message). Delivered only once the assignment is otherwise
           * good, so an application never acts on an allocation result for a
           * provisioning attempt that then fails. The span dies with this
           * callback -- the message buffer is reused. */
          if (c->reg_payload_cb && az_span_size(resp.registration_state.payload) > 0)
          {
            c->reg_payload_cb(resp.registration_state.payload, c->reg_payload_cb_ctx);
          }
          dps_finalize(c, AZ_IOT_OK, true);
          return;
        }
        case AZ_IOT_PROVISIONING_STATUS_FAILED:
        case AZ_IOT_PROVISIONING_STATUS_DISABLED:
        {
          /* Surface the DPS-reported failure (errorCode/errorMessage in
           * the response body) so a provisioning rejection is
           * diagnosable instead of an opaque fault. The formatted text
           * is truncated if the response is long, which is right for a
           * diagnostic: a shortened message still names the cause. */
          /* Both an operation-level failure ("status":"failed"/"disabled") and
           * a request-level one (a 4xx/5xx response topic, whose body carries
           * errorCode/message and no operationId) arrive here -- the parser
           * reports FAILED for both. Keep the service's retry-after: on a
           * throttle or a server error it is the one authoritative statement
           * about when this device may come back. */
          c->dps_pending_retry_after_secs = resp.retry_after_seconds;
          /* The parsed verdict first, on its own line: the raw body below can
           * exceed AZ_IOT_LOG_MESSAGE_MAX and be cut before errorMessage. */
          /* An empty span may carry a NULL pointer, which %.*s must not get. */
          az_span err_msg = resp.registration_state.error_message;
          AZ_IOT_LOG_ERRORF(
              "dps register: errorCode=%ld errorMessage=%.*s",
              (long)resp.registration_state.extended_error_code,
              (int)az_span_size(err_msg),
              az_span_size(err_msg) > 0 ? (const char*)az_span_ptr(err_msg) : "");
          AZ_IOT_LOG_ERRORF(
              "dps register: provisioning failed/disabled; DPS response: %.*s",
              (int)az_span_size(payload_span),
              az_span_size(payload_span) > 0 ? (const char*)az_span_ptr(payload_span) : "");
          /* The service's own verdict -- 401001 "IoTHub not found" is this
           * path. Both are parsed already and were being thrown away, which is
           * what made "registration failed" and "no hub linked" the same
           * opaque AZ_IOT_ERR_DPS. The message spans the inbound buffer and so
           * lives exactly as long as the event that carries it. */
          stage_error(
              c,
              AZ_IOT_CONN_SCOPE_DPS,
              AZ_IOT_CONN_ERR_SRC_DPS,
              (int32_t)resp.registration_state.extended_error_code,
              resp.registration_state.error_message);
          dps_finalize(c, AZ_IOT_ERR_DPS, false);
          return;
        }

        case AZ_IOT_PROVISIONING_STATUS_UNASSIGNED:
        case AZ_IOT_PROVISIONING_STATUS_ASSIGNING:
        default:
        {
          int32_t op_n = az_span_size(resp.operation_id);
          if (op_n < 0 || (size_t)op_n > sizeof(c->dps_operation_id))
          {
            dps_finalize(c, AZ_IOT_ERR_NOT_SUPPORTED, false);
            return;
          }
          memcpy(c->dps_operation_id, az_span_ptr(resp.operation_id), (size_t)op_n);
          c->dps_operation_id_len = (size_t)op_n;
          c->dps_poll_due_ms = az_iot_time_mono_ms() + (uint64_t)resp.retry_after_seconds * 1000ull;
          c->dps_phase = DPS_PHASE_POLLING;
          break;
        }
      }
      break;
    }

    case AZ_IOT_MQTT_EVT_DISCONNECTED:
    case AZ_IOT_MQTT_EVT_ERROR:
    {
      az_iot_result r = (evt->status != AZ_IOT_OK) ? evt->status : AZ_IOT_ERR_NOT_CONNECTED;
      dps_finalize(c, r, false);
      break;
    }

    /* Nothing to do, but listed rather than left to default: so that a new
     * event kind has to be considered here instead of being swallowed. The
     * registration PUBLISH is fire-and-forget as far as this state machine is
     * concerned -- progress is driven by the response on the subscribed topic,
     * not by its ack -- and the unsubscribe ack only arrives during teardown,
     * once the outcome is already decided. */
    case AZ_IOT_MQTT_EVT_PUBLISH_ACK:
    case AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK:
      break;

    default:
      break;
  }
}

/* Start the DPS provisioning flow. Called from _open() when DPS is configured. */
static az_iot_result dps_start(az_iot_connection_client* c)
{
  /* Validate the provisioning identity before handing it to az_core. Empty
   * spans trip an az_core precondition, and this build ships with
   * AZ_NO_PRECONDITION_CHECKING OFF and no handler installed -- the default
   * handler is an infinite loop, so a misconfigured device would hang inside
   * open() instead of getting an error back. */
  if (!is_nonempty_cstr(c->opts.dps.id_scope))
  {
    AZ_IOT_LOG_ERROR("dps_start: dps.id_scope is required for DPS provisioning");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!is_nonempty_cstr(c->opts.dps.registration_id))
  {
    AZ_IOT_LOG_ERROR("dps_start: dps.registration_id is required for DPS provisioning");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  const char* endpoint = c->opts.dps.global_endpoint;
  if (!is_nonempty_cstr(endpoint))
  {
    endpoint = "global.azure-devices-provisioning.net";
  }

  az_span ep_span = az_span_create_from_str((char*)(uintptr_t)endpoint);
  az_span scope_span = az_span_create_from_str((char*)(uintptr_t)c->opts.dps.id_scope);
  az_span reg_span = az_span_create_from_str((char*)(uintptr_t)c->opts.dps.registration_id);
  az_result ar = az_iot_provisioning_client_init(&c->dps_prov, ep_span, scope_span, reg_span, NULL);
  if (az_result_failed(ar))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* All DPS features share the CONNECT API version, including registration,
   * CSR issuance, and update requests on a provision-only session. */
  char dps_username[AZ_IOT_MQTT_USERNAME_BUF];
  const char* username_parts[] = {
    c->opts.dps.id_scope, DPS_USERNAME_INFIX, c->opts.dps.registration_id, DPS_USERNAME_SUFFIX
  };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(dps_username), NULL, username_parts, 4)
      != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  const az_iot_mqtt_factory* f = find_factory(c, AZ_IOT_MQTT_VERSION_3_1_1);
  if (!f)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  az_iot_mqtt_client* mc = f->create(f->factory_ctx);
  if (!mc || !mc->iface)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  mc->iface->set_inbound_cb(mc, on_dps_mqtt_event, c);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = endpoint;
  copts.client_id = c->opts.dps.registration_id;
  resolve_connect_timings(c, &copts);
  resolve_connect_transport(c, &copts, 0);
  resolve_session_options(c, &copts, AZ_IOT_MQTT_ROLE_DPS);

  copts.username = dps_username;
  AZ_IOT_LOG_DEBUGF("dps: connecting with username %s", dps_username);

  /* Credential: the provider's bootstrap X.509 identity first (the
   * operational cert, if any, is issued during this exchange), else a SAS
   * token from dps_auth. Neither fails the attempt; it never goes plaintext. */
  bool dps_has_sas = c->auth[AZ_IOT_CONN_SCOPE_DPS].primary_key_len > 0;
  if (!c->opts.certificate_provider && !dps_has_sas)
  {
    AZ_IOT_LOG_ERROR("dps: no certificate provider and no SAS key; refusing to connect");
    mc->iface->destroy(mc);
    return AZ_IOT_ERR_CREDENTIAL_INCOMPLETE;
  }
  c->auth[AZ_IOT_CONN_SCOPE_DPS].source = AZ_IOT_AUTH_SOURCE_NONE;
  c->auth[AZ_IOT_CONN_SCOPE_DPS].x509_index = 0;
  if (c->opts.certificate_provider)
  {
    az_iot_certificate_material mat = { 0 };
    az_iot_result lr = c->opts.certificate_provider->vtable->load(
        c->opts.certificate_provider, AZ_IOT_CRED_BOOTSTRAP, &mat);
    if ((lr == AZ_IOT_ERR_NOT_FOUND || lr == AZ_IOT_ERR_NOT_INITIALIZED) && dps_has_sas)
    {
      AZ_IOT_LOG_DEBUG("dps: no bootstrap certificate; using SAS");
      keep_provider_trust(&copts, lr, &mat);
    }
    else if (lr == AZ_IOT_OK)
    {
      az_iot_result cr = apply_certificate_material(&copts, &mat, c->opts.certificate_provider);
      AZ_IOT_LOG_DEBUGF(
          "dps: bootstrap TLS ca=%s cert=%s key=%s key_uri=%s engine=%s",
          mat.trusted_ca_path ? mat.trusted_ca_path : "(none)",
          mat.client_cert_path ? mat.client_cert_path : "(none)",
          mat.client_key_path ? mat.client_key_path : "(none)",
          mat.client_key_uri ? mat.client_key_uri : "(none)",
          mat.crypto_engine_id ? mat.crypto_engine_id : "(none)");
      c->opts.certificate_provider->vtable->release(c->opts.certificate_provider, &mat);
      if (cr != AZ_IOT_OK)
      {
        mc->iface->destroy(mc);
        return cr;
      }
      c->auth[AZ_IOT_CONN_SCOPE_DPS].source = AZ_IOT_AUTH_SOURCE_X509;
      apply_trusted_ca(c, &copts);
    }
    else
    {
      AZ_IOT_LOG_ERRORF(
          "dps: certificate provider load() failed for the bootstrap identity (%d)", (int)lr);
      mc->iface->destroy(mc);
      return lr;
    }
  }
  if (c->auth[AZ_IOT_CONN_SCOPE_DPS].source == AZ_IOT_AUTH_SOURCE_NONE)
  {
    az_iot_result sr = apply_sas_key(c, AZ_IOT_CONN_SCOPE_DPS, &copts);
    if (sr != AZ_IOT_OK)
    {
      mc->iface->destroy(mc);
      return sr;
    }
  }

  c->dps_mqtt = mc;
  c->dps_phase = DPS_PHASE_CONNECTING;
  c->dps_pending_finalize = false;
  c->dps_pending_have_assignment = false;
  c->dps_pending_status = AZ_IOT_OK;
  c->dps_pending_retry_after_secs = 0;
  c->dps_enrolling = c->opts.dps.request_operational_certificate;
  c->dps_start_cancelled = false;

  set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_CONNECTING, AZ_IOT_OK);

  /* That announcement ran the application's state callback synchronously, and
   * close() is legal from inside it -- including the branch that cancels a
   * provisioning session, which destroys `mc` and clears dps_mqtt. Touching
   * `mc` afterwards would be a use-after-free, so detect the cancellation and
   * leave: the session the caller asked for no longer exists, and the client
   * is already back in IDLE. */
  if (c->dps_mqtt != mc)
  {
    AZ_IOT_LOG_DEBUG("dps: the session was closed from the state callback; abandoning the start");
    /* Cancellation, not failure -- and it returns the same code as a genuine
     * start failure, so callers that treat a failure as something to retry
     * need to be told which happened. close() has just reset the retry
     * bookkeeping; recreating it here would undo the caller's own close. */
    c->dps_start_cancelled = true;
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  az_iot_result r = mc->iface->connect(mc, &copts);
  if (r != AZ_IOT_OK)
  {
    dps_teardown_mqtt(c);
    c->dps_phase = DPS_PHASE_NONE;
    /* The announcement above already moved the DPS lifecycle to CONNECTING, so
     * a synchronous connect failure has to settle it again here -- every other
     * teardown does. Leaving it pinned at CONNECTING would make the next
     * dps_start() announce nothing (the value would be unchanged) and, for a
     * session the connection client owns, would make open() reject for ever.
     *
     * Settled at the point the session dies rather than in each caller: this is
     * the only place that knows the announcement happened. */
    set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_DISCONNECTING, r);
    set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, r);
  }
  return r;
}

/* Fail an assignment this client cannot use, and make sure the NEXT open()
 * goes back to DPS instead of to whatever assignment is still cached.
 *
 * Declining to adopt the new values is not enough on a re-provision: opts.host
 * and opts.client_id still name the hub from the previous assignment, and
 * open() skips DPS whenever a host is set. Without this the client would
 * connect to the stale hub -- and for a profile mismatch it would do so with
 * session_role already switched to the generation this very response was
 * rejected for.
 *
 * The flag is raised BEFORE the transition because set_state_to() runs the
 * application's state callback synchronously, and close() + open() from inside
 * that callback must already see the demand to re-provision. */
static void reject_assignment(az_iot_connection_client* c, az_iot_result reason)
{
  c->needs_reprovision = true;
  set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_FAULTED, reason);
}

/* Process deferred DPS finalization. Called from _do_work() after process_loop.
 *
 * On success, tears down the DPS MQTT session, validates the assigned profile,
 * adopts host/client_id and starts the hub connect.
 *
 * On failure, retries under the reconnection policy when one is configured and
 * the application has not closed the client, and transitions to FAULTED
 * otherwise. The profile failures (unsupported, or a mismatch with what the
 * attached feature clients require) stay terminal either way: a retry would
 * return the same answer, so they go through reject_assignment() instead.
 *
 * A session held by its USERS rather than by a registration ends here without
 * touching the public connection state at all -- see the guard below. Its
 * retries are paced separately; the application's connection is not involved. */
static void dps_apply_deferred(az_iot_connection_client* c)
{
  if (!c->dps_pending_finalize)
  {
    return;
  }
  bool have_assignment = c->dps_pending_have_assignment;
  az_iot_result status = c->dps_pending_status;
  uint32_t retry_after_secs = c->dps_pending_retry_after_secs;
  c->dps_pending_finalize = false;
  c->dps_pending_have_assignment = false;
  c->dps_pending_status = AZ_IOT_OK;
  c->dps_pending_retry_after_secs = 0;

  /* Registration is over, so its ref goes before the keep/tear decision --
   * that decision is exactly "does anyone still need this session?". */
  bool was_registering = c->dps_registration_ref;
  c->dps_registration_ref = false;

  /* Keep it only if it is still wanted AND still alive. Every terminal outcome
   * other than a successful assignment arrives here because the session died,
   * and a ref cannot resurrect a dead socket. */
  bool device_provisioned = (status == AZ_IOT_OK) && have_assignment;
  if (dps_refs_held(c) && device_provisioned)
  {
    AZ_IOT_LOG_DEBUG("dps: keeping the provisioning session for its users");
  }
  else
  {
    if (c->dps_mqtt && c->dps_mqtt->iface && c->dps_mqtt->iface->disconnect)
    {
      (void)c->dps_mqtt->iface->disconnect(c->dps_mqtt);
    }
    dps_teardown_mqtt(c);

    /* Settle the lifecycle, or the scope sits at CONNECTING for the life of the
     * client and the next dps_start() announces nothing. A failure below
     * overwrites this with RECONNECTING or FAULTED, which is right: "closed,
     * then failed" is two facts. */
    set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_DISCONNECTING, status);
    set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, status);
  }
  c->dps_phase = DPS_PHASE_DONE;

  /* A session nobody registered on can only reach here by failing. That is its
   * users' transport dying, not the application's connection: scheduling a
   * reconnect would tear down a healthy hub session, and faulting would be as
   * wrong. Its users ask again through dps_session_ensure(). */
  if (!was_registering)
  {
    if (status != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERRORF("dps: the provisioning session ended with an error (%d)", (int)status);
      /* Only while a session is still wanted. This runs from the pump, so the
       * last holder can have released since the failure -- and release already
       * cleared the ladder. Pacing here would hand the next holder a latch it
       * did not earn. */
      if (dps_session_demanded(c))
      {
        dps_user_retry_schedule(c, retry_after_secs);
      }
    }
    return;
  }

  if (status != AZ_IOT_OK || !have_assignment)
  {
    /* A registration that failed, or that completed with no assignment, is the
     * most transient failure a device meets: the enrollment may not have been
     * created yet, the DPS may not have a linked IoT Hub yet, or the service
     * may simply have been unavailable. Every other failure path -- hub CONNACK
     * failures, unexpected drops, presence timeouts, subscription-gate timeouts
     * -- consults the reconnection policy first, and this one used to be the
     * single exception: it faulted unconditionally, so a device configured to
     * retry forever still ended terminally on a first boot that ran slightly
     * ahead of its enrollment.
     *
     * needs_reprovision is what makes the retry a re-registration. Without it
     * the scheduled attempt would take the ordinary connect path, which has no
     * host on a DPS client. */
    if (az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy) && !c->user_close)
    {
      c->needs_reprovision = true;
      schedule_reconnect(c, AZ_IOT_CONN_SCOPE_DPS, status);
      /* The service's retry-after wins when it is longer than the policy's
       * backoff. schedule_reconnect() has already set the deadline from the
       * policy; raising it here keeps the two as a floor rather than letting
       * either one alone decide. The policy's max_delay_ms deliberately does
       * NOT cap this: it bounds how long the SDK waits of its own accord, not
       * how long the service asked to be left alone. */
      if (retry_after_secs > 0 && c->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_RECONNECTING)
      {
        uint64_t floor_ms = az_iot_time_mono_ms() + (uint64_t)retry_after_secs * 1000ull;
        if (c->reconnect_due_ms < floor_ms)
        {
          AZ_IOT_LOG_WARNF(
              "dps: the service asked for a %u second retry-after; honoring it over the "
              "reconnection policy",
              (unsigned)retry_after_secs);
          c->reconnect_due_ms = floor_ms;
        }
      }
      return;
    }
    set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_FAULTED, status);
    return;
  }

  /* Apply the assigned hub + device_id and connect to the hub. */
  /* The assigned profile picks the wire protocol for the hub session. An
   * unrecognised value fails the connection instead of guessing an MQTT version
   * -- a device that appears to connect and then misbehaves is far worse to
   * diagnose than one clear error here. The profile stays readable through
   * az_iot_connection_client_get_hub_profile() so the offending value can be
   * logged or reported.
   *
   * Checked BEFORE the assignment is applied to opts.host / opts.client_id,
   * which is what keeps a rejected assignment from being adopted. Refusing to
   * adopt it is only half of the job, though: on a RE-provision the previous
   * assignment is still cached in opts.host, and open() skips DPS whenever a
   * host is set. reject_assignment() therefore also demands that the next
   * open() go back to DPS -- see its comment. */
  az_iot_result r;
  switch (c->connection_profile)
  {
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V3:
      c->session_role = AZ_IOT_MQTT_ROLE_HUB_MQTT_V3;
      break;
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V5:
      c->session_role = AZ_IOT_MQTT_ROLE_HUB_MQTT_V5;
      break;
    case AZ_IOT_CONNECTION_PROFILE_UNKNOWN:
    default:
      AZ_IOT_LOG_ERRORF(
          "dps: assigned an unsupported connectionProfile \"%s\"; this SDK does not know which "
          "protocol to speak",
          c->connection_profile_raw);
      reject_assignment(c, AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED);
      return;
  }
  /* The assignment may have moved the device to a different generation than the
   * one its registered filters were built for. */
  c->connection_profile_resolved = true;
  if (c->required_profile_refs > 0 && c->required_profile != c->connection_profile)
  {
    /* Terminal on purpose: re-provisioning would return this same profile while
     * the feature clients still require the other one, so an immediate retry
     * cannot succeed. The application owns the recovery -- deinit the feature
     * clients and rebuild them for the profile this event carries, then close()
     * this connection client (legal from FAULTED, and it returns it to IDLE)
     * and open() it again. The connection client itself does not have to be
     * deinitialized.
     *
     * The switch above has already moved session_role to the assigned
     * generation. That is harmless only because reject_assignment() forces the
     * next open() through DPS, which settles the role again from whatever that
     * run is assigned. */
    AZ_IOT_LOG_ERRORF(
        "dps: assigned connectionProfile \"%s\", but the attached feature clients require the "
        "other hub generation; deinit them and rebuild for the assigned profile",
        c->connection_profile_raw);
    reject_assignment(c, AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);
    return;
  }

  /* Adopt the assignment. Both halves are checked before EITHER is written:
   * they are only meaningful together, and the response parser accepts an
   * empty device id (it rejects only a negative or oversized one), so
   * committing the hub and then rejecting the device id would leave the client
   * holding the newly assigned hub alongside the previous device id -- which,
   * now that a fault is recoverable, a later open() would connect with. After
   * these checks neither write below can fail. */
  r = check_owned_string(sizeof(c->provisioned_iot_hub_hostname), c->dps_assigned_hub);
  if (r == AZ_IOT_OK)
  {
    r = check_owned_string(sizeof(c->provisioned_device_id), c->dps_assigned_device_id);
  }
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("dps: the assignment did not carry a usable hub hostname and device id");
    reject_assignment(c, r);
    return;
  }
  (void)replace_owned_string(
      c->provisioned_iot_hub_hostname,
      sizeof(c->provisioned_iot_hub_hostname),
      &c->opts.host,
      c->dps_assigned_hub);
  (void)replace_owned_string(
      c->provisioned_device_id,
      sizeof(c->provisioned_device_id),
      &c->opts.client_id,
      c->dps_assigned_device_id);
  drop_subscriptions_from_other_generations(c);
  c->dps_phase = DPS_PHASE_NONE;

  /* Registration succeeded, so BOTH ladders start over: the DPS one because it
   * has done its job, and the hub one because this is a fresh assignment --
   * the attempts that failed against the previous hub say nothing about the
   * one just handed to us, and making the first connect to it wait at the
   * old ladder's cap would be backoff for a failure that never happened. */
  c->retry_attempt[AZ_IOT_CONN_SCOPE_DPS] = 0;
  c->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] = 0;

  /* The service answered, so it is reachable: a user session that failed
   * earlier should not still be serving out a backoff from that. */
  dps_user_retry_reset(c);

  r = start_connect_attempt(c);
  if (r != AZ_IOT_OK)
  {
    set_state_to(c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_FAULTED, r);
  }
}

/* ------------------------------------------------------------------------- */
/* MQTTv5 presence (birth) handshake                                          */
/*                                                                           */
/* On a HUB_MQTT_V5 (MQTT v5) session the connection is not "up" at CONNACK: the  */
/* device must announce presence by publishing a birth message and waiting for */
/* a birth-ack before the SDK reports CONNECTED. MQTTv3/DPS sessions skip all */
/* of this. Sequenced as a small sub-state machine driven from on_mqtt_event:  */
/*   CONNACK  -> SUBSCRIBE ih/{id}/dev/#                  (phase SUBSCRIBING)   */
/*   SUBACK   -> PUBLISH   ih/{id}/srv/presence (birth)   (phase BIRTH)         */
/*   birth-ack MESSAGE on ih/{id}/dev/presence, matching nonce                  */
/*                                     -> announce CONNECTED (phase DONE)       */
/* A stalled handshake is timed out from do_work().                            */
/* ------------------------------------------------------------------------- */

/* Fill `out` with a fresh RFC 4122 version 4 UUID drawn from the client's PRNG.
 * Uses the same LCG as the CSR request-id generator, advanced through rng_state
 * and salted by the attempt count so successive draws never collide. Uniqueness
 * (not cryptographic strength) is what these identifiers need. */
static void gen_uuid_v4(az_iot_connection_client* c, uint8_t out[PRESENCE_NONCE_LEN])
{
  for (size_t i = 0; i < PRESENCE_NONCE_LEN; i += 8)
  {
    uint64_t x = az_iot_time_mono_ms()
        ^ (c->rng_state * 6364136223846793005ull + 1442695040888963407ull)
        ^ ((uint64_t)(c->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] + 1u) << 40);
    c->rng_state = x;
    for (size_t b = 0; b < 8u; ++b)
    {
      out[i + b] = (uint8_t)(x >> (b * 8u));
    }
  }

  /* Stamp the RFC 4122 version (4 = random) and variant (10xx) bits so the
   * nonce is a well-formed UUID, which is what the presence protocol
   * specifies and what the .NET client produces via Guid.NewGuid(). */
  out[PRESENCE_UUID_VERSION_OCTET]
      = PRESENCE_UUID_STAMP_VERSION_4(out[PRESENCE_UUID_VERSION_OCTET]);
  out[PRESENCE_UUID_VARIANT_OCTET]
      = PRESENCE_UUID_STAMP_VARIANT_RFC4122(out[PRESENCE_UUID_VARIANT_OCTET]);
}

/* Fill `out` with the per-connection nonce: a fresh UUID regenerated on every
 * CONNECT attempt (not per successful CONNACK). It is echoed on the birth-ack
 * so the SDK can discard acks from a prior attempt, and it tags the
 * backend-initiated dev-bound traffic belonging to this connection. */
static void presence_gen_nonce(az_iot_connection_client* c, uint8_t out[PRESENCE_NONCE_LEN])
{
  gen_uuid_v4(c, out);
}

/* Build the MQTTv5 CONNECT username. The IoT Hub auth webhook denies a
 * connect with an empty username (WebhookAuthUserNameMissing), so the SDK sends
 * "correlationId=<hex nonce>&clientVersion=c%2F<version>", mirroring the .NET
 * SDK. correlationId is the 32-character lowercase hex of the 16-byte
 * connection nonce; the SAME nonce bytes ride the birth message as raw
 * Correlation Data so the service can correlate the CONNECT with the birth.
 *
 * Returns false if `cap` (AZ_IOT_MQTT_USERNAME_BUF) cannot hold the whole
 * username; `buf` is left unusable and the caller must fail the attempt. */
static bool presence_build_username(const az_iot_connection_client* c, char* buf, size_t cap)
{
  static const char hexdigits[] = "0123456789abcdef";
  char hex[PRESENCE_NONCE_LEN * 2u + 1u];
  for (size_t i = 0; i < PRESENCE_NONCE_LEN; ++i)
  {
    hex[i * 2u] = hexdigits[PRESENCE_HI_NIBBLE(c->presence.nonce[i])];
    hex[i * 2u + 1u] = hexdigits[PRESENCE_LO_NIBBLE(c->presence.nonce[i])];
  }
  hex[(size_t)PRESENCE_NONCE_LEN * 2u] = '\0';

  /* clientVersion is URL-escaped as in the .NET SDK: '/' -> %2F. The version
   * string is percent-encoded too, which leaves today's digits-and-dots form
   * untouched but keeps the pair well-formed if it ever gains a suffix. */
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, az_span_create((uint8_t*)buf, (int32_t)cap));
  az_iot_span_writer_append_str(&writer, "correlationId=");
  az_iot_span_writer_append_str(&writer, hex);
  az_iot_span_writer_append_str(&writer, "&clientVersion=c%2F");
  az_iot_span_writer_append_url_encoded(&writer, az_iot_version_string());

  /* A truncated username is worse than none: it would carry a partial
   * correlationId, so the service could not tie the CONNECT to the birth and
   * the handshake would surface much later as an opaque birth-ack timeout.
   * Report it here so the connect attempt fails with a precise reason. */
  return az_iot_span_writer_end_str(&writer, NULL) == AZ_IOT_OK;
}

/* Encode a proto3 Birth message (common/Protos/presence.proto) into `out`.
 * proto3 omits default-valued fields, matching Google.Protobuf on the .NET
 * side, so a false push bit is simply absent from the payload.
 * push_desired/push_reported advertise which twin traffic the application wants
 * dispatched on this connection (opts.twin_push); reported_version and
 * desired_version stay 0 (the device does not persist twin state yet) and are
 * omitted. Returns the encoded length. */
static size_t presence_encode_birth(
    uint8_t* out,
    size_t cap,
    bool session_present,
    bool push_desired,
    bool push_reported)
{
  size_t n = 0;
  if (session_present && n + 2u <= cap)
  {
    out[n++] = PROTO_KEY_VARINT(PRESENCE_BIRTH_FIELD_SESSION_PRESENT);
    out[n++] = PROTO_BOOL_TRUE;
  }
  if (push_desired && n + 2u <= cap)
  {
    out[n++] = PROTO_KEY_VARINT(PRESENCE_BIRTH_FIELD_PUSH_DESIRED);
    out[n++] = PROTO_BOOL_TRUE;
  }
  if (push_reported && n + 2u <= cap)
  {
    out[n++] = PROTO_KEY_VARINT(PRESENCE_BIRTH_FIELD_PUSH_REPORTED);
    out[n++] = PROTO_BOOL_TRUE;
  }
  return n;
}

/* Decode the twin recovery state the service returns on the birth-ack
 * (presence.proto BirthAck): desired_version (field 10) and reported_version
 * (field 11), both varints. These are the authoritative versions as of birth
 * admission; the device adopts them as its view for this connection. Fields the
 * service omits keep the proto3 default of 0, and unknown fields are skipped so
 * a service-side schema addition does not break the handshake. */
static void presence_decode_birth_ack(az_iot_connection_client* c, const uint8_t* buf, size_t len)
{
  c->presence.desired_version = 0;
  c->presence.reported_version = 0;
  if (!buf)
  {
    return;
  }

  size_t pos = 0;
  while (pos < len)
  {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire))
    {
      return;
    }

    if (wire == AZ_IOT_PROTO3_WIRE_VARINT
        && (field == PRESENCE_BIRTH_ACK_FIELD_DESIRED_VERSION
            || field == PRESENCE_BIRTH_ACK_FIELD_REPORTED_VERSION))
    {
      uint64_t v = 0;
      if (!az_iot_proto3_read_varint(buf, len, &pos, &v))
      {
        return;
      }
      if (field == PRESENCE_BIRTH_ACK_FIELD_DESIRED_VERSION)
      {
        c->presence.desired_version = v;
      }
      else
      {
        c->presence.reported_version = v;
      }
    }
    else if (!az_iot_proto3_skip_field(buf, len, &pos, wire))
    {
      return;
    }
  }
}

/* Announce CONNECTED once the session's persistent subscriptions are live.
 *
 * The ordering matters and the obvious explanation for why is the wrong one.
 * MQTT does preserve ordering: a broker processes one connection's control
 * packets in the order it receives them, so a PUBLISH cannot overtake a
 * SUBSCRIBE already written to that connection. The problem was that ours had
 * not been written yet -- set_state_to() invokes the application callback
 * SYNCHRONOUSLY, so a request published from inside that callback reached the
 * wire ahead of its own SUBSCRIBE, and ordering worked against us.
 *
 * Waiting for the SUBACK rather than merely re-ordering the loop also covers
 * the case where the broker REFUSES a filter, which no amount of local ordering
 * would catch. */
static void announce_connected(az_iot_connection_client* c)
{
  set_state_to(c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_CONNECTED, AZ_IOT_OK);
}

/* Fail the session because a subscription it depends on could not be
 * established. Reported as a connection failure rather than absorbed: a feature
 * whose filter is missing is silently inert, which surfaces later as a request
 * that never gets a response -- far more expensive to diagnose than one error
 * here.
 *
 * A refusal the broker will repeat is terminal even when a reconnection policy
 * is configured. Reconnecting would re-issue the same filter, be refused again,
 * and leave the device cycling forever without ever saying why; the application
 * needs new configuration or a device update, and can only act on that if the
 * SDK stops and tells it. Anything else is transient and reconnects. */
static void fail_subscription_restore(az_iot_connection_client* c, az_iot_result reason)
{
  memset(&c->subscription_gate, 0, sizeof(c->subscription_gate));
  c->deferred = (reason != AZ_IOT_ERR_SUBSCRIPTION_REFUSED
                 && az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy) && !c->user_close)
      ? DEFER_RECONNECT
      : DEFER_FAULT;
  c->deferred_reason = reason;
}

/* A FAILS_SELF filter did not come up. Its owner is told -- that subscription
 * is dead, not optional -- and the entry is dropped so a reconnect cannot
 * silently re-issue it. The entry is released before the callback runs, so a
 * callback that re-registers immediately can claim the slot and, because
 * registration issues the SUBSCRIBE whenever a session is up or a batch is
 * still in flight, that replacement is live in this session rather than
 * waiting for the next one. */
static void report_and_drop_subscription(
    az_iot_connection_client* c,
    size_t index,
    az_iot_result reason,
    int32_t protocol_code)
{
  az_iot_subscription_failed_callback cb = c->persistent_subs[index].on_failed;
  const void* owner = c->persistent_subs[index].owner;
  char topic[AZ_IOT_PERSISTENT_SUB_TOPIC_MAX];
  memcpy(topic, c->persistent_subs[index].topic_filter, sizeof(topic));

  AZ_IOT_LOG_WARNF(
      "connection: subscription '%s' failed (reason_code=%d); dropping it, connection stays up",
      topic,
      (int)protocol_code);
  memset(&c->persistent_subs[index], 0, sizeof(c->persistent_subs[index]));
  if (cb)
  {
    cb(topic, reason, protocol_code, owner);
  }
}

/* Deadline for the SUBACKs of the filters just handed to the adapter. */
static uint64_t subscription_ack_deadline(const az_iot_connection_client* c)
{
  uint32_t secs = c->opts.subscription_ack_timeout_seconds
      ? c->opts.subscription_ack_timeout_seconds
      : AZ_IOT_DEFAULT_SUBSCRIPTION_ACK_TIMEOUT_SECONDS;
  return az_iot_time_mono_ms() + (uint64_t)secs * 1000u;
}

/* Record a SUBSCRIBE that is waiting for its ack. Gated entries hold CONNECTED;
 * the rest are tracked only so a later refusal still reaches its owner. */
static void subscription_gate_track(
    az_iot_connection_client* c,
    uint16_t packet_id,
    size_t sub_index,
    bool gated)
{
  if (c->subscription_gate.pending_count >= AZ_IOT_MAX_PERSISTENT_SUBS)
  {
    return;
  }
  size_t p = c->subscription_gate.pending_count++;
  c->subscription_gate.pending[p].packet_id = packet_id;
  c->subscription_gate.pending[p].sub_index = (uint8_t)sub_index;
  c->subscription_gate.pending[p].gated = gated;
  if (gated)
  {
    c->subscription_gate.gated_outstanding++;
  }
  c->subscription_gate.active = true;
}

/* Forget an ack still pending for a registry slot that has just been cleared.
 * A pending record holds an INDEX, and a withdrawn slot is immediately reusable,
 * so without this a late ack would be applied to whatever took that slot's
 * place -- faulting the session for a filter nobody needs any more, or
 * reporting one owner's failure to another. A withdrawn filter also stops
 * holding CONNECTED, since nothing is waiting on it. */
static void subscription_gate_forget(az_iot_connection_client* c, size_t sub_index)
{
  for (size_t i = 0; i < c->subscription_gate.pending_count; ++i)
  {
    if (c->subscription_gate.pending[i].sub_index != sub_index)
    {
      continue;
    }
    if (c->subscription_gate.pending[i].gated && c->subscription_gate.gated_outstanding > 0)
    {
      c->subscription_gate.gated_outstanding--;
    }
    c->subscription_gate.pending[i]
        = c->subscription_gate.pending[c->subscription_gate.pending_count - 1];
    c->subscription_gate.pending_count--;
    return;
  }
}

/* (Re)issue every persistent subscription and gate CONNECTED on the SUBACKs of
 * those that the session depends on. Announces immediately when nothing gated
 * is registered, which is the common case for a connection whose feature
 * clients are built after open(). */
static void begin_feature_subscriptions(az_iot_connection_client* c)
{
  memset(&c->subscription_gate, 0, sizeof(c->subscription_gate));

  if (!c->active_client || !c->active_client->iface)
  {
    announce_connected(c);
    return;
  }

  for (size_t i = 0; i < AZ_IOT_MAX_PERSISTENT_SUBS; ++i)
  {
    if (!c->persistent_subs[i].in_use)
    {
      continue;
    }
    const bool gated = c->persistent_subs[i].failure_scope == AZ_IOT_SUBSCRIPTION_FAILS_SESSION;
    uint16_t pid = 0;
    az_iot_result r = c->active_client->iface->subscribe(
        c->active_client, c->persistent_subs[i].topic_filter, c->persistent_subs[i].qos, &pid);
    if (r != AZ_IOT_OK)
    {
      /* A SUBSCRIBE that could not even be written is scoped the same way its
       * refusal would be. */
      if (!gated)
      {
        report_and_drop_subscription(c, i, r, 0);
        continue;
      }
      AZ_IOT_LOG_ERRORF(
          "connection: could not re-subscribe '%s' on connect", c->persistent_subs[i].topic_filter);
      fail_subscription_restore(c, r);
      return;
    }
    subscription_gate_track(c, pid, i, gated);
  }

  if (c->subscription_gate.gated_outstanding == 0)
  {
    announce_connected(c);
  }
  /* One deadline for the whole batch, started once the last SUBSCRIBE has been
   * handed over. Ungated acks are still tracked past the transition so a later
   * refusal reaches its owner, but they never hold CONNECTED. */
  if (c->subscription_gate.pending_count > 0)
  {
    c->subscription_gate.deadline_ms = subscription_ack_deadline(c);
  }
}

/* Returns true when `packet_id` was one this gate was waiting on. */
static bool subscription_gate_settle(
    az_iot_connection_client* c,
    uint16_t packet_id,
    az_iot_result status,
    int32_t protocol_code)
{
  if (!c->subscription_gate.active)
  {
    return false;
  }
  for (size_t i = 0; i < c->subscription_gate.pending_count; ++i)
  {
    if (c->subscription_gate.pending[i].packet_id != packet_id)
    {
      continue;
    }
    const bool gated = c->subscription_gate.pending[i].gated;
    const size_t sub_index = c->subscription_gate.pending[i].sub_index;

    c->subscription_gate.pending[i]
        = c->subscription_gate.pending[c->subscription_gate.pending_count - 1];
    c->subscription_gate.pending_count--;

    if (status != AZ_IOT_OK)
    {
      if (!gated)
      {
        report_and_drop_subscription(c, sub_index, status, protocol_code);
        return true;
      }
      AZ_IOT_LOG_ERRORF(
          "connection: broker refused '%s' on connect (reason_code=%d)",
          c->persistent_subs[sub_index].topic_filter,
          (int)protocol_code);
      fail_subscription_restore(c, status);
      return true;
    }

    if (gated && c->subscription_gate.gated_outstanding > 0
        && --c->subscription_gate.gated_outstanding == 0)
    {
      announce_connected(c);
    }
    if (c->subscription_gate.pending_count == 0)
    {
      c->subscription_gate.active = false;
    }
    return true;
  }
  return false;
}

/* Begin the presence handshake after a successful HUB_MQTT_V5 CONNACK: subscribe
 * to the device-bound topic space (dev/#) so the birth-ack can be received. */
static az_iot_result presence_start(az_iot_connection_client* c, bool session_present)
{
  if (!c->active_client || !c->active_client->iface)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  const char* device_id = c->opts.client_id ? c->opts.client_id : "";
  char topic[AZ_IOT_PRESENCE_TOPIC_BUF];
  const char* topic_parts[] = { PRESENCE_TOPIC_PREFIX, device_id, PRESENCE_TOPIC_DEV_SUB_SUFFIX };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, topic_parts, 3) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  c->presence.session_present = session_present;
  /* The connection nonce was generated at CONNECT time (start_connect_attempt)
   * so it could ride the CONNECT username as correlationId; reuse it here as
   * the birth Correlation Data. Do NOT regenerate it, or the username's
   * correlationId and the birth would diverge and the service could not
   * correlate them. */
  c->presence.deadline_ms = az_iot_time_mono_ms() + AZ_IOT_PRESENCE_BIRTH_ACK_TIMEOUT_MS;

  uint16_t pid = 0;
  az_iot_result r
      = c->active_client->iface->subscribe(c->active_client, topic, AZ_IOT_MQTT_QOS_1, &pid);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  c->presence.sub_packet_id = pid;
  c->presence.phase = PRESENCE_PHASE_SUBSCRIBING;
  return AZ_IOT_OK;
}

/* Publish the birth message once the dev/presence SUBSCRIBE is acked. Birth is
 * QoS 0: the service acknowledges it with a full birth-ack PUBLISH rather than
 * a PUBACK. The nonce rides as Correlation Data; "type"="birth:1" as a User
 * Property. */
static az_iot_result presence_publish_birth(az_iot_connection_client* c)
{
  if (!c->active_client || !c->active_client->iface)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  const char* device_id = c->opts.client_id ? c->opts.client_id : "";
  char topic[AZ_IOT_PRESENCE_TOPIC_BUF];
  const char* topic_parts[] = { PRESENCE_TOPIC_PREFIX, device_id, PRESENCE_TOPIC_SRV_SUFFIX };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, topic_parts, 3) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  uint8_t body[8];
  size_t body_len = presence_encode_birth(
      body,
      sizeof(body),
      c->presence.session_present,
      c->opts.twin_push.push_desired,
      c->opts.twin_push.push_reported);

  az_iot_mqtt_user_property type_prop = { PRESENCE_TYPE_KEY, PRESENCE_TYPE_BIRTH };

  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = body;
  msg.payload_len = body_len;
  msg.qos = AZ_IOT_MQTT_QOS_0;
  msg.user_properties = &type_prop;
  msg.user_properties_count = 1;
  msg.correlation_data = c->presence.nonce;
  msg.correlation_data_len = PRESENCE_NONCE_LEN;

  uint16_t pid = 0;
  az_iot_result r = c->active_client->iface->publish(c->active_client, &msg, &pid);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  /* Give the birth-ack its own full window now that the birth is on the wire. */
  c->presence.deadline_ms = az_iot_time_mono_ms() + AZ_IOT_PRESENCE_BIRTH_ACK_TIMEOUT_MS;
  c->presence.phase = PRESENCE_PHASE_BIRTH;
  return AZ_IOT_OK;
}

/* True when `msg` is the birth-ack for the in-flight handshake: it arrives on
 * ih/{deviceId}/dev/presence, carries "type"="birth-ack[:*]", and echoes our
 * connection nonce as Correlation Data. Non-matching acks (e.g. from a prior
 * attempt) are ignored per presence.proto. */
static bool presence_is_birth_ack(const az_iot_connection_client* c, const az_iot_mqtt_message* msg)
{
  if (!msg || !msg->topic)
  {
    return false;
  }

  const char* device_id = c->opts.client_id ? c->opts.client_id : "";
  char topic[AZ_IOT_PRESENCE_TOPIC_BUF];
  const char* topic_parts[] = { PRESENCE_TOPIC_PREFIX, device_id, PRESENCE_TOPIC_DEV_SUFFIX };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, topic_parts, 3) != AZ_IOT_OK)
  {
    return false;
  }
  if (strcmp(msg->topic, topic) != 0)
  {
    return false;
  }

  if (msg->correlation_data_len != PRESENCE_NONCE_LEN || msg->correlation_data == NULL
      || memcmp(msg->correlation_data, c->presence.nonce, PRESENCE_NONCE_LEN) != 0)
  {
    return false;
  }

  for (size_t i = 0; i < msg->user_properties_count; ++i)
  {
    const az_iot_mqtt_user_property* up = &msg->user_properties[i];
    if (!up->key || strcmp(up->key, PRESENCE_TYPE_KEY) != 0)
    {
      continue;
    }
    if (!up->value)
    {
      return false;
    }
    size_t n = strlen(PRESENCE_TYPE_BIRTH_ACK);
    /* Match "birth-ack" exactly or "birth-ack:<schemaVersion>". */
    return strncmp(up->value, PRESENCE_TYPE_BIRTH_ACK, n) == 0
        && (up->value[n] == '\0' || up->value[n] == ':');
  }
  return false;
}

/* Inbound MQTT events are dispatched here (synchronously from process_loop). */
static void on_mqtt_event(const az_iot_mqtt_event* evt, void* user_ctx)
{
  az_iot_connection_client* c = (az_iot_connection_client*)user_ctx;
  if (!c || !evt)
  {
    return;
  }

  /* See the DPS handler: staged here so every failure route below inherits it. */
  stage_error_from_event(c, AZ_IOT_CONN_SCOPE_HUB, evt);

  switch (evt->kind)
  {
    case AZ_IOT_MQTT_EVT_CONNECTED:
      if (evt->status == AZ_IOT_OK)
      {
        /* A CONNACK that arrives after the application asked to close belongs
         * to an attempt it has already abandoned. Announcing CONNECTED here
         * would report a session the caller did not ask for, and anything that
         * publishes on CONNECTED would write into a socket that is already
         * being torn down. The DISCONNECTED event still on its way settles the
         * session to IDLE. */
        if (c->user_close || c->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_DISCONNECTING)
        {
          AZ_IOT_LOG_DEBUG("connack ignored: close already requested");
          break;
        }

        /* Successful CONNACK: clear the HUB ladder. The DPS ladder is left
         * alone -- it is cleared when a REGISTRATION succeeds, not when the
         * hub this device was already assigned to answers. */
        c->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] = 0;
        c->reconnect_due_ms = 0;

        /* MQTTv5 (MQTT v5): the connection is not usable until
         * presence is established. Kick off the birth handshake and
         * defer the CONNECTED announcement until the birth-ack arrives.
         * MQTTv3 (and DPS-assigned MQTTv3) sessions announce now. */
        if (c->session_role == AZ_IOT_MQTT_ROLE_HUB_MQTT_V5)
        {
          az_iot_result pr = presence_start(c, evt->session_present);
          if (pr != AZ_IOT_OK)
          {
            c->presence.phase = PRESENCE_PHASE_NONE;
            c->deferred
                = (az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy) && !c->user_close)
                ? DEFER_RECONNECT
                : DEFER_FAULT;
            c->deferred_reason = pr;
          }
          break;
        }

        begin_feature_subscriptions(c);
      }
      else
      {
        /* An identity rejection is not a transient transport failure: the
         * broker has refused this client id / credential, so retrying the same
         * one cannot succeed. When the device provisions through DPS, ask DPS
         * for a fresh assignment instead -- that is the whole reason the
         * adapter contract maps these CONNACK codes to a distinct result. When
         * a policy is configured the retry is scheduled through it, so backoff
         * and max_attempts continue to bound it (a device whose enrollment has
         * been deleted must not hammer DPS either).
         *
         * NOT gated on az_iot_retry_policy_is_enabled(). It used to be, on the reasoning
         * that only the RECONNECTING branch of do_work() consumed the flag, so
         * with no policy nothing would act on it. That is no longer true:
         * open() consumes it as well, and routes to DPS even when a hub is
         * cached. So with retries disabled the rejection still faults -- the
         * application is still the one that decides -- but the verdict is
         * remembered, and the next open() asks DPS rather than walking back
         * into the hub that just refused this identity.
         *
         * The max_hub_connect_attempts_before_reprovision trigger is NOT
         * decoupled with it, deliberately: that one counts consecutive
         * automatic retries, and with retries disabled there is no such run to
         * count. Each open() is a fresh decision by the application. */
        if (evt->status == AZ_IOT_ERR_IDENTITY_REJECTED && dps_configured(c) && !c->user_close)
        {
          AZ_IOT_LOG_WARN("connack: identity rejected; re-provisioning through DPS");
          c->needs_reprovision = true;
        }
        c->deferred
            = (az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy) && !c->user_close)
            ? DEFER_RECONNECT
            : DEFER_FAULT;
        c->deferred_reason = evt->status;
      }
      break;

    case AZ_IOT_MQTT_EVT_DISCONNECTED:
      if (c->user_close || !az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy))
      {
        c->deferred = DEFER_IDLE;
        c->deferred_reason = evt->status;
      }
      else
      {
        c->deferred = DEFER_RECONNECT;
        c->deferred_reason = (evt->status != AZ_IOT_OK) ? evt->status : AZ_IOT_ERR_NOT_CONNECTED;
      }
      break;

    case AZ_IOT_MQTT_EVT_ERROR:
    {
      az_iot_result r = (evt->status != AZ_IOT_OK) ? evt->status : AZ_IOT_ERR_MQTT;
      c->deferred = (az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy) && !c->user_close)
          ? DEFER_RECONNECT
          : DEFER_FAULT;
      c->deferred_reason = r;
      break;
    }

    /* Phase 2.3: route inbound application messages through the
     * dispatch table; unmatched messages are dropped silently (the same
     * behaviour MQTT brokers rely on for unsubscribed wildcards). */
    case AZ_IOT_MQTT_EVT_MESSAGE:
      if (evt->message)
      {
        /* During the MQTTv5 birth handshake, intercept the birth-ack and
         * complete the connection; everything else routes normally. */
        if (c->presence.phase == PRESENCE_PHASE_BIRTH && presence_is_birth_ack(c, evt->message))
        {
          /* Same reasoning as the CONNACK case above. On the MQTTv5 path it
           * is the birth-ack, not the CONNACK, that completes the connection,
           * so suppressing only the CONNACK would leave this route able to
           * announce CONNECTED for an attempt the application has already
           * abandoned. A birth-ack the broker sent before close() reached it
           * arrives in a later process_loop batch, when user_close is set and
           * the state is DISCONNECTING. */
          if (c->user_close || c->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_DISCONNECTING)
          {
            AZ_IOT_LOG_DEBUG("birth-ack ignored: close already requested");
            break;
          }
          presence_decode_birth_ack(c, evt->message->payload, evt->message->payload_len);
          c->presence.phase = PRESENCE_PHASE_DONE;
          begin_feature_subscriptions(c);
          break;
        }
        (void)az_iot_dispatch_route(&c->dispatch, evt->message);
      }
      break;

    /* Phase 3.1: route PUBACK to the publishing feature client via the
     * correlation table. An unmatched packet_id is a publish sent without an
     * ack callback: success is dropped, failure is logged since no one else
     * will report it. */
    case AZ_IOT_MQTT_EVT_PUBLISH_ACK:
    {
      bool matched = false;
      for (size_t i = 0; i < AZ_IOT_MAX_PENDING_PUBACKS; ++i)
      {
        /* cb == NULL: reserved by a publish still in progress; not yet matchable. */
        if (c->pending_pubacks[i].in_use && c->pending_pubacks[i].cb != NULL
            && c->pending_pubacks[i].packet_id == evt->packet_id)
        {
          az_iot_publish_ack_callback cb = c->pending_pubacks[i].cb;
          void* ctx = c->pending_pubacks[i].user_ctx;
          c->pending_pubacks[i].in_use = false;
          c->pending_pubacks[i].cb = NULL;
          c->pending_pubacks[i].user_ctx = NULL;
          matched = true;
          if (cb)
          {
            cb(evt->status, ctx);
          }
          break;
        }
      }
      if (!matched && evt->status != AZ_IOT_OK)
      {
        AZ_IOT_LOG_ERRORF(
            "publish with packet id %u failed (%s); it had no completion callback",
            (unsigned)evt->packet_id,
            az_iot_result_to_string(evt->status));
      }
      break;
    }

    /* The dev/presence SUBACK advances the MQTTv5 birth handshake: publish the
     * birth message now that the ack topic is subscribed. Other SUBACKs are
     * absorbed (feature clients don't yet need SUBACK correlation). */
    case AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK:
      if (c->presence.phase == PRESENCE_PHASE_SUBSCRIBING
          && evt->packet_id == c->presence.sub_packet_id)
      {
        az_iot_result pr = (evt->status == AZ_IOT_OK) ? presence_publish_birth(c) : evt->status;
        if (pr != AZ_IOT_OK)
        {
          c->presence.phase = PRESENCE_PHASE_NONE;
          c->deferred
              = (az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy) && !c->user_close)
              ? DEFER_RECONNECT
              : DEFER_FAULT;
          c->deferred_reason = pr;
        }
        break;
      }
      if (!subscription_gate_settle(c, evt->packet_id, evt->status, evt->protocol_code))
      {
        /* Not one this connection correlated: an ack for a SUBSCRIBE it never
         * issued, or one left over from a session that has already gone. There
         * is no owner to tell and nothing to release, so record it and move on
         * rather than letting it look like a lost subscription later. */
        AZ_IOT_LOG_DEBUGF(
            "connection: SUBACK for an untracked packet id %u; ignoring", (unsigned)evt->packet_id);
      }
      break;

    /* UNSUBSCRIBE_ACK gets correlation handlers in later Phase 3 slices when
     * feature clients need to know subscriptions are live. For now absorb. */
    case AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK:
    default:
      break;
  }
}

static az_iot_result start_connect_attempt(az_iot_connection_client* c)
{
  az_iot_result br = run_feature_client_binds(c);
  if (br != AZ_IOT_OK)
  {
    return br;
  }

  az_iot_mqtt_version version = az_iot_mqtt_required_version_for_role(c->session_role);
  const az_iot_mqtt_factory* f = find_factory(c, version);
  if (!f)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  az_iot_mqtt_client* mc = f->create(f->factory_ctx);
  if (!mc || !mc->iface)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  mc->iface->set_inbound_cb(mc, on_mqtt_event, c);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = c->opts.host;
  copts.client_id = c->opts.client_id;
  resolve_connect_timings(c, &copts);
  resolve_connect_transport(c, &copts, c->opts.port);
  resolve_session_options(c, &copts, c->session_role);

  /* Build hub MQTT username via azure-sdk-for-c (MQTTv3 only).
   * MQTTv5 does not use the MQTTv3 username format. */
  if (c->session_role != AZ_IOT_MQTT_ROLE_HUB_MQTT_V5 && c->opts.host && c->opts.client_id)
  {
    if (!c->hub_client_initialized)
    {
      az_span host_span = az_span_create_from_str((char*)(uintptr_t)c->opts.host);
      az_span id_span = az_span_create_from_str((char*)(uintptr_t)c->opts.client_id);
      az_iot_hub_client_options hub_opts = az_iot_hub_client_options_default();
      if (is_nonempty_cstr(c->opts.model_id))
      {
        hub_opts.model_id = az_span_create_from_str((char*)(uintptr_t)c->opts.model_id);
      }
      az_result ar = az_iot_hub_client_init(&c->hub_client, host_span, id_span, &hub_opts);
      if (az_result_succeeded(ar))
      {
        c->hub_client_initialized = true;
      }
    }
    if (c->hub_client_initialized)
    {
      size_t ulen = 0;
      az_result ar = az_iot_hub_client_get_user_name(
          &c->hub_client, c->hub_username, sizeof(c->hub_username), &ulen);
      if (az_result_succeeded(ar))
      {
        copts.username = c->hub_username;
      }
    }
  }
  else if (c->session_role == AZ_IOT_MQTT_ROLE_HUB_MQTT_V5 && c->opts.host && c->opts.client_id)
  {
    /* MQTTv5: generate the per-attempt connection nonce now so it
     * rides the CONNECT username (correlationId) and is reused as the birth
     * Correlation Data. The auth webhook denies an empty username. */
    presence_gen_nonce(c, c->presence.nonce);
    if (!presence_build_username(c, c->hub_username, sizeof(c->hub_username)))
    {
      AZ_IOT_LOG_ERROR(
          "connection: AZ_IOT_MQTT_USERNAME_BUF is too small for the MQTTv5 CONNECT username");
      mc->iface->destroy(mc);
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    copts.username = c->hub_username;
  }

  /* Credential: the provider's X.509 identity first -- the issued OPERATIONAL
   * one (from this DPS session, persisted by the provider on a prior run, or
   * supplied for a direct hub connection), else BOOTSTRAP -- then a SAS token
   * from hub_auth. Neither fails the attempt; it never goes plaintext. */
  bool hub_has_sas = c->auth[AZ_IOT_CONN_SCOPE_HUB].primary_key_len > 0;
  if (!c->opts.certificate_provider && !hub_has_sas)
  {
    AZ_IOT_LOG_ERROR("connection: no certificate provider and no SAS key; refusing to connect");
    mc->iface->destroy(mc);
    return AZ_IOT_ERR_CREDENTIAL_INCOMPLETE;
  }
  c->auth[AZ_IOT_CONN_SCOPE_HUB].source = AZ_IOT_AUTH_SOURCE_NONE;
  c->auth[AZ_IOT_CONN_SCOPE_HUB].x509_index = 0;
  if (c->opts.certificate_provider)
  {
    az_iot_certificate_provider* prov = c->opts.certificate_provider;
    az_iot_certificate_material mat = { 0 };
    az_iot_result lr = prov->vtable->load(prov, AZ_IOT_CRED_OPERATIONAL, &mat);
    if (lr == AZ_IOT_ERR_NOT_FOUND || lr == AZ_IOT_ERR_NOT_INITIALIZED)
    {
      lr = prov->vtable->load(prov, AZ_IOT_CRED_BOOTSTRAP, &mat);
    }
    if ((lr == AZ_IOT_ERR_NOT_FOUND || lr == AZ_IOT_ERR_NOT_INITIALIZED) && hub_has_sas)
    {
      AZ_IOT_LOG_DEBUG("connection: no certificate; using SAS");
      keep_provider_trust(&copts, lr, &mat);
    }
    else if (lr != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERRORF("connection: certificate provider load() failed (%d)", (int)lr);
      mc->iface->destroy(mc);
      return lr;
    }
    else
    {
      az_iot_result cr = apply_certificate_material(&copts, &mat, prov);
      prov->vtable->release(prov, &mat);
      if (cr != AZ_IOT_OK)
      {
        mc->iface->destroy(mc);
        return cr;
      }
      c->auth[AZ_IOT_CONN_SCOPE_HUB].source = AZ_IOT_AUTH_SOURCE_X509;
      apply_trusted_ca(c, &copts);
    }
  }
  if (c->auth[AZ_IOT_CONN_SCOPE_HUB].source == AZ_IOT_AUTH_SOURCE_NONE)
  {
    az_iot_result sr = apply_sas_key(c, AZ_IOT_CONN_SCOPE_HUB, &copts);
    if (sr != AZ_IOT_OK)
    {
      mc->iface->destroy(mc);
      return sr;
    }
  }

  set_state_to(c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_CONNECTING, AZ_IOT_OK);
  az_iot_result r = mc->iface->connect(mc, &copts);
  if (r != AZ_IOT_OK)
  {
    mc->iface->destroy(mc);
    return r;
  }
  c->active_client = mc;
  return AZ_IOT_OK;
}

static void apply_deferred(az_iot_connection_client* c)
{
  if (c->deferred == DEFER_NONE)
  {
    return;
  }
  int action = c->deferred;
  az_iot_result reason = c->deferred_reason;
  c->deferred = DEFER_NONE;
  c->deferred_reason = AZ_IOT_OK;

  switch (action)
  {
    case DEFER_FAULT:
      teardown_active(c);
      set_state_to(c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_FAULTED, reason);
      break;
    case DEFER_RECONNECT:
      /* Reached from on_mqtt_event, which serves the HUB session only. */
      schedule_reconnect(c, AZ_IOT_CONN_SCOPE_HUB, reason);
      break;
    case DEFER_IDLE:
      teardown_active(c);
      c->user_close = false;
      c->retry_attempt[AZ_IOT_CONN_SCOPE_DPS] = 0;
      c->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] = 0;
      c->reconnect_due_ms = 0;
      set_state_to(c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_IDLE, reason);
      break;
    default:
      break;
  }
}

/* ------------------------------------------------------------------------- */
/* MQTTv5 mock bypass (env-var-driven, for local dev/test only)            */
/* ------------------------------------------------------------------------- */

/* When AZ_IOT_HUB_MQTT_V5_MOCK_ENDPOINT is set (e.g. "localhost:8883"), skip DPS
 * entirely and connect to the mock MQTTv5 using MQTT v5. The device identity
 * comes from AZ_IOT_DEVICE_ID (must match the cert CN in the mock). This
 * avoids the need for a real DPS service during local development. */
#define MQTT_V5_MOCK_ENDPOINT_ENV "AZ_IOT_HUB_MQTT_V5_MOCK_ENDPOINT"
#define MQTT_V5_MOCK_DEVICE_ID_ENV "AZ_IOT_DEVICE_ID"
/** @brief Capacity for the mock endpoint ("host:port"). */
#define MQTT_V5_MOCK_ENDPOINT_BUF 256

static bool mock_mqtt_v5_configured(void)
{
  char endpoint[MQTT_V5_MOCK_ENDPOINT_BUF];
  /* Too long still counts as set: the bypass then rejects it. */
  return az_iot_env_read(MQTT_V5_MOCK_ENDPOINT_ENV, endpoint, sizeof(endpoint)) != AZ_IOT_OK
      || endpoint[0] != '\0';
}

/* Parse "host:port" into host string and port. Writes host into out_host
 * (up to cap), returns the port, or 0 when the endpoint carries none -- 0 means
 * "derive from the transport" at connect time, so a DPS-assigned hub endpoint
 * does not pin the connection to 8883 when WebSockets were selected. */
static uint16_t parse_host_port(const char* endpoint, char* out_host, size_t cap)
{
  uint16_t port = 0;
  const char* colon = strrchr(endpoint, ':');
  size_t host_len;
  if (colon && colon != endpoint)
  {
    host_len = (size_t)(colon - endpoint);
    unsigned long p = strtoul(colon + 1, NULL, 10);
    if (p > 0 && p <= 65535)
    {
      port = (uint16_t)p;
    }
  }
  else
  {
    host_len = strlen(endpoint);
  }
  if (host_len >= cap)
  {
    host_len = cap - 1;
  }
  memcpy(out_host, endpoint, host_len);
  out_host[host_len] = '\0';
  return port;
}

static az_iot_result apply_mqtt_v5_mock_bypass(az_iot_connection_client* c)
{
  char endpoint[MQTT_V5_MOCK_ENDPOINT_BUF];
  char id_buf[AZ_IOT_DPS_DEVICE_ID_BUF];

  if (az_iot_env_read(MQTT_V5_MOCK_ENDPOINT_ENV, endpoint, sizeof(endpoint)) != AZ_IOT_OK
      || az_iot_env_read(MQTT_V5_MOCK_DEVICE_ID_ENV, id_buf, sizeof(id_buf)) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Falls back to the DPS registration id when AZ_IOT_DEVICE_ID is unset. */
  const char* device_id = id_buf[0] != '\0' ? id_buf : c->opts.dps.registration_id;

  if (endpoint[0] == '\0' || !is_nonempty_cstr(device_id))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  char host[256];
  uint16_t port = parse_host_port(endpoint, host, sizeof(host));

  az_iot_result r = replace_owned_string(
      c->provisioned_iot_hub_hostname,
      sizeof(c->provisioned_iot_hub_hostname),
      &c->opts.host,
      host);
  if (r == AZ_IOT_OK)
  {
    c->opts.port = port;
    r = replace_owned_string(
        c->provisioned_device_id, sizeof(c->provisioned_device_id), &c->opts.client_id, device_id);
  }
  if (r == AZ_IOT_OK)
  {
    c->session_role = AZ_IOT_MQTT_ROLE_HUB_MQTT_V5;
    c->dps_phase = DPS_PHASE_DONE;

    /* Warn, not debug: provisioning was skipped entirely, so anyone reading
     * the log needs to know this session never talked to DPS. */
    AZ_IOT_LOG_WARNF(
        "dps: mqttv5 mock bypass active; host=%s port=%u device=%s",
        host,
        (unsigned)(port ? port : default_port_for_transport(c->opts.transport)),
        device_id);
  }

  return r;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

AZ_NODISCARD az_iot_connection_client_options az_iot_connection_client_options_default(void)
{
  az_iot_connection_client_options opts = { 0 };
  /* 0, not 8883: the port is derived from the transport at connect time, so a
   * caller that selects WebSockets does not also have to remember to change a
   * port that was defaulted for TCP. */
  opts.port = 0;
  /* Retry by default. A zeroed policy has initial_delay_ms == 0, which means
   * reconnection is DISABLED -- every dropped link, every refused CONNACK and
   * every failed registration is then terminal. That is a reasonable meaning
   * for a struct the caller zeroed themselves, but it is the wrong default for
   * the function whose job is to hand back sensible values: an unattended
   * device that stops at the first transient failure is not the behaviour
   * anyone asks for, and it is not what the other Azure IoT SDKs do.
   *
   * Callers who genuinely want a single attempt set
   * reconnection_policy.initial_delay_ms = 0 on the returned struct, or build
   * their options from { 0 } instead. */
  opts.reconnection_policy = az_iot_connection_client_get_default_retry_policy();
  opts.dps.max_hub_connect_attempts_before_reprovision
      = AZ_IOT_DEFAULT_MAX_HUB_CONNECT_ATTEMPTS_BEFORE_REPROVISION;
  return opts;
}

/** @brief Checks one role's SAS options; decoding happens in sas_load_keys(). */
static az_iot_result sas_validate(
    const az_iot_connection_client_options* opts,
    const az_iot_auth* auth)
{
  bool has_primary = is_nonempty_cstr(auth->sas.primary_key_base64);
  if (is_nonempty_cstr(auth->sas.secondary_key_base64) && !has_primary)
  {
    AZ_IOT_LOG_ERROR("connection_client_init: SAS secondary key without a primary key");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (has_primary && opts->crypto == NULL)
  {
    AZ_IOT_LOG_ERROR("connection_client_init: SAS keys need opts.crypto");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (auth->sas.renewal_percent > 99u)
  {
    AZ_IOT_LOG_ERROR("connection_client_init: SAS renewal_percent above 99");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (auth->sas.user_provided_token != NULL)
  {
    AZ_IOT_LOG_ERROR("connection_client_init: user_provided_token is not supported yet");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  return AZ_IOT_OK;
}

/**
 * @brief Decodes @p key_base64 into @p out; for a group key, replaces it with
 * the device key derived for @p id.
 */
static az_iot_result sas_load_key(
    const az_iot_crypto* crypto,
    const char* key_base64,
    bool is_group_key,
    const char* id,
    uint8_t out[AZ_IOT_SAS_KEY_MAX],
    size_t* out_len)
{
  az_iot_result r = az_iot_sas__decode_key(key_base64, out, AZ_IOT_SAS_KEY_MAX, out_len);
  if (r != AZ_IOT_OK || !is_group_key)
  {
    return r;
  }
  if (!is_nonempty_cstr(id))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  uint8_t device_key[AZ_IOT_SHA256_SIZE];
  r = az_iot_sas__derive_device_key(crypto, out, *out_len, id, device_key);
  az_iot_sas__wipe(out, AZ_IOT_SAS_KEY_MAX);
  *out_len = 0;
  if (r == AZ_IOT_OK)
  {
    memcpy(out, device_key, sizeof(device_key));
    *out_len = sizeof(device_key);
  }
  az_iot_sas__wipe(device_key, sizeof(device_key));
  return r;
}

/** @brief Decodes the keys of @p scope into client storage. */
static az_iot_result sas_load_keys(az_iot_connection_client* c, az_iot_connection_scope scope)
{
  const az_iot_auth* auth = scope == AZ_IOT_CONN_SCOPE_DPS ? &c->opts.dps_auth : &c->opts.hub_auth;
  if (!is_nonempty_cstr(auth->sas.primary_key_base64))
  {
    return AZ_IOT_OK;
  }
  /* A group key derives per device: the registration ID, which DPS also makes
   * the device ID, or client_id for a direct hub connection. */
  const char* id = (scope == AZ_IOT_CONN_SCOPE_DPS || dps_configured(c))
      ? c->opts.dps.registration_id
      : c->opts.client_id;
  az_iot_result r = sas_load_key(
      c->opts.crypto,
      auth->sas.primary_key_base64,
      auth->sas.is_enrollment_group_key,
      id,
      c->auth[scope].primary_key,
      &c->auth[scope].primary_key_len);
  if (r == AZ_IOT_OK && is_nonempty_cstr(auth->sas.secondary_key_base64))
  {
    r = sas_load_key(
        c->opts.crypto,
        auth->sas.secondary_key_base64,
        auth->sas.is_enrollment_group_key,
        id,
        c->auth[scope].secondary_key,
        &c->auth[scope].secondary_key_len);
  }
  return r;
}

/** @brief Wipes every SAS key and token the client holds. */
static void sas_wipe(az_iot_connection_client* c) { az_iot_sas__wipe(c->auth, sizeof(c->auth)); }

AZ_NODISCARD az_iot_result az_iot_connection_client_init(
    az_iot_connection_client* client,
    const az_iot_connection_client_options* opts)
{
  if (!client || !opts)
  {
    AZ_IOT_LOG_ERROR("connection_client_init: invalid arguments");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* UNKNOWN only ever comes back FROM the service; a caller cannot meaningfully
   * declare a profile the SDK does not know how to speak. */
  if (opts->connection_profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V3
      && opts->connection_profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    AZ_IOT_LOG_ERROR("connection_client_init: connection_profile is not a profile this SDK speaks");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (opts->crypto != NULL && az_iot_crypto__validate(opts->crypto) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("connection_client_init: crypto backend has another version or lacks SHA-256");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (opts->trusted_ca.pem != NULL && opts->trusted_ca.path != NULL)
  {
    AZ_IOT_LOG_ERROR(
        "connection_client_init: set at most one of trusted_ca.pem and trusted_ca.path");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_result sv = sas_validate(opts, &opts->dps_auth);
  if (sv == AZ_IOT_OK)
  {
    sv = sas_validate(opts, &opts->hub_auth);
  }
  if (sv != AZ_IOT_OK)
  {
    return sv;
  }
  memset(client, 0, sizeof(*client));
  client->opts = *opts;
  sv = sas_load_keys(client, AZ_IOT_CONN_SCOPE_DPS);
  if (sv == AZ_IOT_OK)
  {
    sv = sas_load_keys(client, AZ_IOT_CONN_SCOPE_HUB);
  }
  if (sv != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("connection_client_init: a SAS key is not valid base64, too long, or its "
                     "group-key ID is missing");
    sas_wipe(client);
    return sv;
  }
  client->state[AZ_IOT_CONN_SCOPE_DPS] = AZ_IOT_CONN_STATE_IDLE;
  client->state[AZ_IOT_CONN_SCOPE_HUB] = AZ_IOT_CONN_STATE_IDLE;
  /* Determine session role early so feature clients can query the profile
   * during their init (which happens before open()). When the mqttv5 mock is
   * configured, also resolve the device_id so subscriptions can be built. */
  if (mock_mqtt_v5_configured())
  {
    client->session_role = AZ_IOT_MQTT_ROLE_HUB_MQTT_V5;
    /* Resolve device_id from AZ_IOT_DEVICE_ID or DPS registration_id */
    char id_buf[AZ_IOT_DPS_DEVICE_ID_BUF];
    (void)az_iot_env_read(MQTT_V5_MOCK_DEVICE_ID_ENV, id_buf, sizeof(id_buf));
    const char* dev_id = id_buf[0] != '\0' ? id_buf : client->opts.dps.registration_id;
    if (is_nonempty_cstr(dev_id))
    {
      (void)replace_owned_string(
          client->provisioned_device_id,
          sizeof(client->provisioned_device_id),
          &client->opts.client_id,
          dev_id);
    }
  }
  else if (
      client->opts.host && client->opts.connection_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    /* Direct connect to an MQTTv5 endpoint (MQTT v5). For DPS
     * (host == NULL) the profile is learned during provisioning, so
     * opts.connection_profile is honored only when a direct host is supplied. */
    client->session_role = AZ_IOT_MQTT_ROLE_HUB_MQTT_V5;
  }
  else
  {
    client->session_role = AZ_IOT_MQTT_ROLE_HUB_MQTT_V3;
  }
  /* Seed the reported profile from the role settled above, so a direct connect
   * -- where there is no service to ask -- is answerable from init onward. The
   * DPS path overwrites this when the ASSIGNED payload arrives. */
  connection_profile_set(
      client,
      client->session_role == AZ_IOT_MQTT_ROLE_HUB_MQTT_V5
          ? AZ_SPAN_FROM_STR(CONNECTION_PROFILE_MQTT_V5_STR)
          : AZ_SPAN_FROM_STR(CONNECTION_PROFILE_MQTT_V3_STR));
  /* A direct connect has no service to ask, so the seed above is the answer. */
  client->connection_profile_resolved = !dps_configured(client);
  /* Seed jitter PRNG; tests can overwrite via the internal seed entry point
   * if they need determinism. */
  client->rng_state = az_iot_time_mono_ms() ^ 0xA5A5C3C3DEADBEEFull;
  if (client->rng_state == 0)
  {
    client->rng_state = 1ull;
  }
  return AZ_IOT_OK;
}

void az_iot_connection_client_deinit(az_iot_connection_client* client)
{
  if (!client)
  {
    return;
  }
  /* Abandon pending QoS-1 acknowledgements WITHOUT completing them. On a
   * dropped session the callback is useful -- it tells the caller the publish
   * needs resending. On deinit() it is not: the application is tearing the
   * client down, and the context those callbacks close over may already be
   * gone. Calling back into it here would turn cleanup into a use-after-free.
   * Cleared before teardown_active() so it has nothing left to complete. */
  for (size_t i = 0; i < AZ_IOT_MAX_PENDING_PUBACKS; ++i)
  {
    client->pending_pubacks[i].in_use = false;
    client->pending_pubacks[i].cb = NULL;
    client->pending_pubacks[i].user_ctx = NULL;
    client->pending_pubacks[i].packet_id = 0;
  }
  /* Same reasoning for the session-end handlers: on deinit() the feature
   * clients are being torn down alongside this one, so calling into them is
   * at best pointless and at worst a use-after-free. */
  for (size_t i = 0; i < AZ_IOT_MAX_SESSION_HANDLERS; ++i)
  {
    client->session_handlers[i].in_use = false;
    client->session_handlers[i].cb = NULL;
    client->session_handlers[i].user_ctx = NULL;
  }
  teardown_active(client);
  dps_teardown_mqtt(client);
  sas_wipe(client);
  /* dispatch is embedded; nothing to free. */
  for (size_t i = 0; i < client->factory_count; ++i)
  {
    if (client->factories[i].destroy)
    {
      client->factories[i].destroy(client->factories[i].factory_ctx);
    }
  }
  /* provisioned_iot_hub_hostname / provisioned_device_id are inline fixed buffers; nothing to free.
   */
}

AZ_NODISCARD az_iot_result az_iot_connection_client_register_mqtt_factory(
    az_iot_connection_client* client,
    const az_iot_mqtt_factory* factory)
{
  if (!client || !factory || !factory->create)
  {
    AZ_IOT_LOG_ERROR("register_mqtt_factory: invalid arguments");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* Registering the same factory twice used to append a second entry. It was
   * never reachable -- find_factory() returns the first match for a version --
   * but deinit() walks the whole registry and calls every entry's destroy
   * hook, so the duplicate freed the same factory_ctx a second time. A
   * double free is a disproportionate punishment for a redundant call, and
   * "register the transport" is exactly the kind of setup step an application
   * repeats on a reconfigure path.
   *
   * An exact duplicate is therefore idempotent: the registry already routes
   * this version to this factory, so there is nothing to do. Registering a
   * DIFFERENT factory for a version that already has one still appends, and
   * still loses to the first one at lookup -- that is a separate question
   * about override semantics, not a memory-safety bug. */
  for (size_t i = 0; i < client->factory_count; ++i)
  {
    const az_iot_mqtt_factory* f = &client->factories[i];
    if (f->create == factory->create && f->factory_ctx == factory->factory_ctx
        && f->destroy == factory->destroy && f->version == factory->version)
    {
      return AZ_IOT_OK;
    }
  }

  if (client->factory_count >= AZ_IOT_MAX_MQTT_FACTORIES)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  client->factories[client->factory_count++] = *factory;
  return AZ_IOT_OK;
}

/* Shared by the public and internal registration entry points. The only
 * difference between an application observer and a feature-client one is which
 * pool it lands in -- and that is decided HERE, by which entry point was
 * called, never by an argument. An application cannot register itself into the
 * feature-client pool and take a slot a feature client needs. */
static az_iot_result add_state_observer_to(
    az_iot_connection_client* client,
    az_iot_connection_state_callback cb,
    void* user_ctx,
    bool feature_client)
{
  if (!client || !cb)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (client->dispatching_state)
  {
    AZ_IOT_LOG_ERROR("connection: cannot add a state observer from inside one");
    return AZ_IOT_ERR_BUSY;
  }

  size_t count
      = feature_client ? AZ_IOT_MAX_FEATURE_STATE_OBSERVERS : AZ_IOT_MAX_APP_STATE_OBSERVERS;
  size_t free_slot = count;
  for (size_t i = 0; i < count; ++i)
  {
    az_iot_connection_state_callback slot_cb = feature_client
        ? client->feature_state_observers[i].cb
        : client->app_state_observers[i].cb;
    void* slot_ctx = feature_client ? client->feature_state_observers[i].user_ctx
                                    : client->app_state_observers[i].user_ctx;
    /* Idempotent on the (cb, user_ctx) PAIR, not on cb alone: one callback
     * shared by two owners is two distinct subscriptions and must be delivered
     * twice. */
    if (slot_cb == cb && slot_ctx == user_ctx)
    {
      return AZ_IOT_OK;
    }
    if (!slot_cb && free_slot == count)
    {
      free_slot = i;
    }
  }
  if (free_slot == count)
  {
    AZ_IOT_LOG_ERROR("connection: no free state-observer slot");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  if (feature_client)
  {
    client->feature_state_observers[free_slot].cb = cb;
    client->feature_state_observers[free_slot].user_ctx = user_ctx;
  }
  else
  {
    client->app_state_observers[free_slot].cb = cb;
    client->app_state_observers[free_slot].user_ctx = user_ctx;
  }
  return AZ_IOT_OK;
}

az_iot_result az_iot_connection_client_add_state_observer(
    az_iot_connection_client* client,
    az_iot_connection_state_callback cb,
    void* user_ctx)
{
  return add_state_observer_to(client, cb, user_ctx, false);
}

az_iot_result az_iot_connection_client__add_state_observer(
    az_iot_connection_client* client,
    az_iot_connection_state_callback cb,
    void* user_ctx)
{
  return add_state_observer_to(client, cb, user_ctx, true);
}

/* Withdraw from whichever pool holds the pair. Both are searched rather than
 * requiring the caller to say which, so a feature client's deinit path does not
 * have to name its own pool -- and an application cannot remove a feature
 * client's entry by guessing, because it would have to already hold that
 * client's exact (cb, user_ctx) pair. */
static az_iot_result remove_state_observer_from(
    az_iot_connection_client* client,
    az_iot_connection_state_callback cb,
    void* user_ctx)
{
  if (!client || !cb)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Deliberately NOT refused during a dispatch, unlike adding.
   *
   * The two directions are not symmetric. Adding from inside an observer would
   * deliver the transition being dispatched to a subscriber that was not
   * watching when it happened, so it is refused. Removing is the opposite: a
   * feature client deinitialized from inside an observer -- which is a natural
   * reaction to FAULTED -- MUST be able to withdraw, because the entry holds a
   * raw pointer to storage its deinit path is about to release. Refusing here
   * left the caller with no way to give the seat back, and the next transition
   * called into freed memory.
   *
   * Safe against the walk in dispatch_state_event(): it re-reads each slot and
   * skips a NULL callback, and nothing is compacted, so clearing a slot only
   * means that observer is not called -- which is exactly what withdrawing
   * asks for. An entry already visited in this pass is unaffected. */
  for (size_t i = 0; i < AZ_IOT_MAX_FEATURE_STATE_OBSERVERS; ++i)
  {
    if (client->feature_state_observers[i].cb == cb
        && client->feature_state_observers[i].user_ctx == user_ctx)
    {
      client->feature_state_observers[i].cb = NULL;
      client->feature_state_observers[i].user_ctx = NULL;
      return AZ_IOT_OK;
    }
  }
  for (size_t i = 0; i < AZ_IOT_MAX_APP_STATE_OBSERVERS; ++i)
  {
    if (client->app_state_observers[i].cb == cb
        && client->app_state_observers[i].user_ctx == user_ctx)
    {
      client->app_state_observers[i].cb = NULL;
      client->app_state_observers[i].user_ctx = NULL;
      return AZ_IOT_OK;
    }
  }
  return AZ_IOT_ERR_NOT_FOUND;
}

az_iot_result az_iot_connection_client_remove_state_observer(
    az_iot_connection_client* client,
    az_iot_connection_state_callback cb,
    void* user_ctx)
{
  return remove_state_observer_from(client, cb, user_ctx);
}

az_iot_result az_iot_connection_client__remove_state_observer(
    az_iot_connection_client* client,
    az_iot_connection_state_callback cb,
    void* user_ctx)
{
  return remove_state_observer_from(client, cb, user_ctx);
}

az_iot_result az_iot_connection_client_set_operational_cert_callback(
    az_iot_connection_client* client,
    az_iot_operational_cert_callback cb,
    void* user_ctx)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  client->op_cert_cb = cb;
  client->op_cert_cb_ctx = user_ctx;
  return AZ_IOT_OK;
}

az_iot_result az_iot_connection_client_set_registration_payload_callback(
    az_iot_connection_client* client,
    az_iot_registration_payload_callback cb,
    void* user_ctx)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  client->reg_payload_cb = cb;
  client->reg_payload_cb_ctx = user_ctx;
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result az_iot_connection_client_open(az_iot_connection_client* client)
{
  if (!client)
  {
    AZ_IOT_LOG_ERROR("connection_client_open: NULL client");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* What "already open" means, now that there are two lifecycles.
   *
   * Any non-IDLE HUB value is a refusal. The DPS scope is the application's
   * only while this client drives it, so a session a FEATURE client opened for
   * itself must not block open() -- otherwise a device-update client that
   * pumped first would wedge it at ALREADY_INITIALIZED for ever.
   *
   * A standing ref is an open client whatever the DPS scope says: between a
   * drop and the paced reopen the scope is legitimately IDLE with no session,
   * and a second open() accepted there would reset the ladder and start an
   * unpaced attempt with no close() in between. */
  if (client->state[AZ_IOT_CONN_SCOPE_HUB] != AZ_IOT_CONN_STATE_IDLE
      || (client->state[AZ_IOT_CONN_SCOPE_DPS] != AZ_IOT_CONN_STATE_IDLE
          && client->dps_registration_ref)
      || client->dps_standing_ref)
  {
    AZ_IOT_LOG_ERROR("connection_client_open: client not in IDLE state");
    return AZ_IOT_ERR_ALREADY_INITIALIZED;
  }

  /* Rejected rather than quietly ignored: opts.host names the hub this option
   * says does not exist, and an operational certificate is issued BY a
   * registration, which this device never performs. */
  if (client->opts.dps.provision_only)
  {
    if (!dps_configured(client))
    {
      AZ_IOT_LOG_ERROR("connection_client_open: dps.provision_only requires dps.id_scope");
      return AZ_IOT_ERR_INVALID_ARG;
    }
    if (is_nonempty_cstr(client->opts.host))
    {
      AZ_IOT_LOG_ERROR(
          "connection_client_open: dps.provision_only is set, but opts.host names a hub");
      return AZ_IOT_ERR_INVALID_ARG;
    }
    if (client->opts.dps.request_operational_certificate)
    {
      AZ_IOT_LOG_ERROR("connection_client_open: dps.provision_only cannot be combined with "
                       "request_operational_certificate; the certificate is issued by a "
                       "registration, which this device never performs");
      return AZ_IOT_ERR_INVALID_ARG;
    }
  }

  /* A role the client uses needs X.509 from the provider unless it has a SAS
   * key; CSR enrollment always needs the provider. Checked first, so a missing
   * provider is reported as such rather than as a missing capability below. */
  bool dps_used = dps_configured(client);
  bool hub_used = !client->opts.dps.provision_only;
  if (!client->opts.certificate_provider
      && (client->opts.dps.request_operational_certificate
          || (dps_used && client->auth[AZ_IOT_CONN_SCOPE_DPS].primary_key_len == 0)
          || (hub_used && client->auth[AZ_IOT_CONN_SCOPE_HUB].primary_key_len == 0)))
  {
    AZ_IOT_LOG_ERROR("connection_client_open: opts.certificate_provider is required for a role "
                     "without a SAS key, and for request_operational_certificate");
    return AZ_IOT_ERR_CREDENTIAL_INCOMPLETE;
  }

  /* CSR-based operational-cert enrollment (D2) requires a certificate_provider
   * whose vtable exposes get_csr (ABI version >= 2). Fail fast otherwise. */
  if (client->opts.dps.request_operational_certificate)
  {
    az_iot_certificate_provider* p = client->opts.certificate_provider;
    if (!p || !p->vtable || p->vtable->version < CERT_PROVIDER_VTABLE_V2
        || p->vtable->get_csr == NULL)
    {
      AZ_IOT_LOG_ERROR("connection_client_open: request_operational_certificate set but provider "
                       "does not support CSR enrollment");
      return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    if (az_span_size(client->opts.csr_payload_buffer) <= 0)
    {
      AZ_IOT_LOG_ERROR("connection_client_open: request_operational_certificate requires "
                       "opts.csr_payload_buffer");
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
  }

  /* A custom registration payload is caller-supplied JSON that goes on the wire
   * verbatim. Check it once here, before any socket exists, rather than letting
   * a malformed one become a DPS protocol failure during provisioning. Like
   * every other dps option it is ignored on a direct hub connect, where there
   * is no registration to carry it. */
  if (dps_configured(client) && az_span_size(client->opts.dps.registration_payload) > 0)
  {
    if (dps_validate_registration_payload(client->opts.dps.registration_payload) != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERROR("connection_client_open: opts.dps.registration_payload must be a single "
                       "well-formed JSON object");
      return AZ_IOT_ERR_INVALID_ARG;
    }
    az_span body_buffer = dps_register_body_buffer(client);
    if (az_span_ptr(body_buffer) == NULL || az_span_size(body_buffer) <= 0)
    {
      AZ_IOT_LOG_ERROR("connection_client_open: opts.dps.registration_payload requires "
                       "opts.dps.registration_body_buffer (or opts.csr_payload_buffer) to build "
                       "the registration body in");
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    if (spans_overlap(client->opts.dps.registration_payload, body_buffer))
    {
      AZ_IOT_LOG_ERROR("connection_client_open: opts.dps.registration_payload overlaps the buffer "
                       "the registration body is built in; they must be separate storage");
      return AZ_IOT_ERR_INVALID_ARG;
    }
  }

  /* Pre-flight the credential shape (D8). A client certificate with no private
   * key in any form cannot complete a handshake, and finding that out here --
   * before any socket exists -- is the difference between
   * AZ_IOT_ERR_CREDENTIAL_INCOMPLETE and an opaque TLS failure several seconds
   * later. The material is whatever the provider would actually hand the
   * adapter: the operational identity when it holds one, the bootstrap identity
   * otherwise. A provider that can supply neither yet is not rejected -- it may
   * become able to by the time the connect attempt runs. */
  if (client->opts.certificate_provider)
  {
    az_iot_certificate_provider* p = client->opts.certificate_provider;
    if (p->vtable == NULL || p->vtable->load == NULL)
    {
      /* load() is the one hook every vtable version requires. Without it the
       * connect paths have nothing to ask for credentials, so the client would
       * dereference NULL a few frames later. */
      AZ_IOT_LOG_ERROR("connection_client_open: certificate provider vtable has no load()");
      return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    az_iot_certificate_material mat = { 0 };
    az_iot_result lr = p->vtable->load(p, AZ_IOT_CRED_OPERATIONAL, &mat);
    if (lr != AZ_IOT_OK)
    {
      lr = p->vtable->load(p, AZ_IOT_CRED_BOOTSTRAP, &mat);
    }
    if (lr == AZ_IOT_OK)
    {
      az_iot_result vr = validate_certificate_material(&mat, provider_has_sign(p));
      p->vtable->release(p, &mat);
      if (vr != AZ_IOT_OK)
      {
        return vr;
      }
    }
  }

  client->user_close = false;
  client->retry_attempt[AZ_IOT_CONN_SCOPE_DPS] = 0;
  client->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] = 0;
  client->reconnect_due_ms = 0;
  dps_user_retry_reset(client);
  /* needs_reprovision is deliberately NOT cleared here. It is pending recovery
   * intent -- "the cached assignment is no good, ask DPS again" -- set by an
   * identity rejection, by the unreachable-hub threshold, or by an assignment
   * this client refused. Clearing it would make close() + open() reconnect to
   * exactly the hub that was rejected or unreachable, because the cached host
   * is still set. It is consumed by the DPS route below. */

  /* --- MQTTv5 mock bypass: when AZ_IOT_HUB_MQTT_V5_MOCK_ENDPOINT is set,
   * skip DPS and connect directly to the mock MQTTv5 (MQTT v5). ---
   *
   * Never for provision_only: the bypass makes a hostless client a hub
   * connection, and an environment variable must not override a declared
   * device shape. */
  if (mock_mqtt_v5_configured() && !client->opts.dps.provision_only)
  {
    az_iot_result r = apply_mqtt_v5_mock_bypass(client);
    if (r != AZ_IOT_OK)
    {
      set_state_to(client, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_IDLE, r);
      return r;
    }
    /* host + client_id are set, session_role = HUB_MQTT_V5 → fall through
     * to start_connect_attempt which will resolve the v5 factory. */
    r = start_connect_attempt(client);
    if (r != AZ_IOT_OK)
    {
      set_state_to(client, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_IDLE, r);
    }
    return r;
  }

  /* No hub: bring the provisioning session up and stop there. No registration
   * ref is taken. The standing ref alone gives both behaviours the pump needs
   * -- do not collect it, and re-establish it if it drops.
   *
   * Before the registration route below, which would otherwise claim this
   * client: opts.host is NULL on a provision_only device, so it matches. */
  if (client->opts.dps.provision_only)
  {
    client->dps_standing_ref = true;
    /* BUSY is the ordinary answer: started, and the caller waits for
     * DPS:CONNECTED as a hub client waits for HUB:CONNECTED. */
    az_iot_result r = az_iot_connection_client__dps_session_ensure(client);
    if (r == AZ_IOT_ERR_BUSY || r == AZ_IOT_OK)
    {
      return AZ_IOT_OK;
    }
    client->dps_standing_ref = false;
    AZ_IOT_LOG_ERRORF("connection_client_open: provision_only session did not start (%d)", (int)r);
    return r;
  }

  /* DPS will be used for registration: no hub is cached, or a previous outcome
   * demanded re-provisioning even though one is. */
  if (dps_configured(client) && (!client->opts.host || client->needs_reprovision))
  {
    /* Raised before the start, so a session that is already up is adopted
     * rather than replaced. */
    client->dps_registration_ref = true;

    az_iot_result r;
    if (az_iot_connection_client__dps_session_ready(client))
    {
      AZ_IOT_LOG_DEBUG("dps: registering on the session already open");
      r = dps_do_register_publish(client);
    }
    else if (client->dps_mqtt != NULL)
    {
      r = AZ_IOT_OK; /* coming up; the SUBACK path registers on it */
    }
    else
    {
      client->dps_phase = DPS_PHASE_NONE;
      r = dps_start(client);
    }

    if (r != AZ_IOT_OK)
    {
      client->dps_registration_ref = false;
      set_state_to(client, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, r);
      return r;
    }
    /* Consumed only once provisioning is really under way, so a failed start
     * still leaves the demand standing for the next open(). */
    client->needs_reprovision = false;
    return r;
  }

  if (!client->opts.host || !client->opts.client_id)
  {
    AZ_IOT_LOG_ERROR("connection_client_open: host or client_id not set (and DPS not configured)");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_result r = start_connect_attempt(client);
  if (r != AZ_IOT_OK)
  {
    set_state_to(client, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_IDLE, r);
  }
  return r;
}

/* End the provisioning session, whoever was holding it.
 *
 * Its users' refs SURVIVE: a ref is a standing interest in having a session,
 * not in this one. It is NOT reopened automatically: its users ask again
 * through dps_session_ensure() when they next have something to send. */
static void dps_close_session(az_iot_connection_client* c)
{
  if (!c->dps_mqtt)
  {
    return;
  }
  if (c->dps_mqtt->iface && c->dps_mqtt->iface->disconnect)
  {
    (void)c->dps_mqtt->iface->disconnect(c->dps_mqtt);
  }
  dps_teardown_mqtt(c);
  c->dps_registration_ref = false;
  c->dps_phase = DPS_PHASE_NONE;
  c->dps_pending_finalize = false;
  c->dps_pending_have_assignment = false;
  c->dps_pending_status = AZ_IOT_OK;
  set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_DISCONNECTING, AZ_IOT_OK);
  set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, AZ_IOT_OK);
}

az_iot_result az_iot_connection_client_close(az_iot_connection_client* client)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* Before the idempotency check below, not after it. close() is the
   * documented exit from a settled refusal, and on a DPS-only device both
   * scopes sit at IDLE when the user-session ladder latches -- which is
   * precisely the early return. Clearing it there would be unreachable in the
   * one case that needs it most. Resetting an already-clear ladder is a
   * no-op. */
  dps_user_retry_reset(client);

  /* Before the idempotency check, for the same reason: a provision_only client
   * waiting out a backoff sits at DPS:IDLE with the ref held, which IS that
   * early return, and the next pump tick would reopen what was just closed. */
  client->dps_standing_ref = false;

  /* Idempotent only when BOTH lifecycles are already settled. A client whose
   * hub is IDLE but whose provisioning session is still up has something to
   * close. */
  if (client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_IDLE
      && client->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_IDLE)
  {
    return AZ_IOT_OK; /* idempotent */
  }

  /* Before every branch below, because a provisioning session can be live in
   * ALL of them: a session its users hold outlives registration, so a hub
   * retry, a fault, or an ordinary close can each find one still up. Each
   * branch used to return without touching it, and the pump would not collect
   * it either while a ref was held -- so close() left a connection running. */
  dps_close_session(client);

  /* close() is the documented exit from a settled refusal, so it clears the
   * user-session ladder too -- including the blocked latch. Here rather than in
   * dps_close_session(), which returns early when no session is up: the ladder
   * outlives the session that earned it, and that is the case that most needs
   * clearing. */
  dps_user_retry_reset(client);

  /* Closing while waiting to reconnect: cancel the schedule and go straight
   * to IDLE. There is no live adapter to disconnect at this point.
   *
   * Either scope may be the one waiting -- a registration retry is scheduled on
   * DPS, a hub retry on HUB -- and there is one deadline, so one of them
   * waiting means the client as a whole is waiting. */
  if (client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_RECONNECTING
      || client->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_RECONNECTING)
  {
    client->retry_attempt[AZ_IOT_CONN_SCOPE_DPS] = 0;
    client->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] = 0;
    client->reconnect_due_ms = 0;
    client->user_close = false;
    settle_all_scopes_to_idle(client);
    return AZ_IOT_OK;
  }

  /* Closing from FAULTED: acknowledge the fault and return the client to IDLE,
   * which is what makes open() a supported retry.
   *
   * Every path into FAULTED has already torn the session down, so there is
   * nothing to disconnect -- which is exactly why this has to be handled
   * before the active_client check below, or close() would report
   * NOT_INITIALIZED and leave the client in a state no API could leave. The
   * only escape would then be deinit() plus a full re-init, which also forces
   * the application to rebuild every attached feature client.
   *
   * Same shape as the RECONNECTING branch above: cancel the bookkeeping and
   * transition. The configuration is untouched, so a DPS client re-provisions
   * on the next open() and a client that had already been assigned a hub
   * reconnects to it. */
  if (client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_FAULTED
      || client->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_FAULTED)
  {
    client->retry_attempt[AZ_IOT_CONN_SCOPE_DPS] = 0;
    client->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] = 0;
    client->reconnect_due_ms = 0;
    client->user_close = false;
    /* needs_reprovision survives on purpose: it says the cached assignment is
     * no good, which a close() does not change. Clearing it here would let the
     * following open() reconnect to the very hub that faulted. */
    /* Defensive: no fault path leaves one behind today, but close() must not
     * depend on that to reach IDLE. */
    teardown_active(client);
    settle_all_scopes_to_idle(client);
    return AZ_IOT_OK;
  }

  if (!client->active_client)
  {
    /* Closing with no hub adapter -- during provisioning, or with only a
     * provisioning session up. dps_close_session() above already ended it, so
     * this only has to settle the bookkeeping and report success rather than
     * NOT_INITIALIZED, which would leave the client somewhere no API can
     * leave. */
    client->retry_attempt[AZ_IOT_CONN_SCOPE_DPS] = 0;
    client->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] = 0;
    client->reconnect_due_ms = 0;
    /* needs_reprovision survives, as in the FAULTED branch above. */
    client->user_close = false;
    settle_all_scopes_to_idle(client);
    return AZ_IOT_OK;
  }

  client->user_close = true;
  set_state_to(client, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_DISCONNECTING, AZ_IOT_OK);
  az_iot_result r = client->active_client->iface->disconnect(client->active_client);
  if (r != AZ_IOT_OK && r != AZ_IOT_ERR_NOT_CONNECTED)
  {
    return r;
  }
  return AZ_IOT_OK;
}

az_iot_result az_iot_connection_client_do_work(
    az_iot_connection_client* client,
    uint32_t timeout_ms)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* --- DPS provisioning pump --- */
  /* Close a session nobody needs any more. Done HERE rather than at release
   * time because release is reachable from inside a message callback, where
   * freeing the adapter would free the object it is still dispatching on.
   *
   * Outside the phase check below, and phase-independent: a session kept across
   * registration sits at DPS_PHASE_DONE, and the old placement could never
   * close one. The refs are the only question -- if nobody holds it, it goes. */
  if (client->dps_mqtt != NULL && !dps_refs_held(client))
  {
    AZ_IOT_LOG_DEBUG("dps: closing the provisioning session; nobody is holding it");
    if (client->dps_mqtt->iface && client->dps_mqtt->iface->disconnect)
    {
      (void)client->dps_mqtt->iface->disconnect(client->dps_mqtt);
    }
    dps_teardown_mqtt(client);
    client->dps_phase = DPS_PHASE_DONE;
    /* The session is gone, so its lifecycle settles -- the same rule as any
     * other provisioning-session teardown. The HUB scope is untouched. */
    set_state_to(client, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_DISCONNECTING, AZ_IOT_OK);
    set_state_to(client, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, AZ_IOT_OK);
  }

  /* Pump the provisioning session whenever one EXISTS, not only while a
   * registration is running through its phases. A session kept for the feature
   * clients holding it sits at DPS_PHASE_DONE, and a phase-gated pump would
   * never service it -- so nothing inbound would ever arrive on the very
   * session the refcount kept alive. */
  if (client->dps_mqtt == NULL && client->dps_standing_ref)
  {
    /* A provision_only device has no hub connect to fall back on, so only the
     * pump can bring its session back. Through the same gate a feature client
     * uses, so the pacing applies and a refusing service is not hammered.
     *
     * OK/BUSY mean up or coming up, and NOT_SUPPORTED means the ladder has
     * settled -- all three already visible to the application as DPS states,
     * so none of them is a pump failure. Anything else is unexpected and is
     * reported here rather than being swallowed; do_work() still returns the
     * pump's own result, since one tick failing to start a session is not a
     * reason to stop servicing the rest of the client. */
    az_iot_result sr = az_iot_connection_client__dps_session_ensure(client);
    if (sr != AZ_IOT_OK && sr != AZ_IOT_ERR_BUSY && sr != AZ_IOT_ERR_NOT_SUPPORTED)
    {
      AZ_IOT_LOG_ERRORF("dps: could not re-establish the provisioning session (%d)", (int)sr);
    }
  }

  if (client->dps_mqtt != NULL)
  {
    /* Leave the pre-registration hold once every holder has released, or once
     * the deadline expires. Expiry is not a failure: the hold is advisory, and
     * a feature client must never be able to stop a device provisioning. */
    if (client->dps_phase == DPS_PHASE_HOLD)
    {
      bool expired = az_iot_time_mono_ms() >= client->dps_hold_deadline_ms;
      if (client->dps_hold_count == 0 || expired)
      {
        if (expired && client->dps_hold_count > 0)
        {
          AZ_IOT_LOG_ERROR("dps: pre-registration hold timed out; registering anyway");
        }
        client->dps_hold_active = false;
        az_iot_result hr = dps_do_register_publish(client);
        if (hr != AZ_IOT_OK)
        {
          dps_finalize(client, hr, false);
        }
      }
    }

    /* If polling deadline reached, issue query. */
    if (client->dps_phase == DPS_PHASE_POLLING && az_iot_time_mono_ms() >= client->dps_poll_due_ms)
    {
      az_iot_result r = dps_do_query_publish(client);
      if (r != AZ_IOT_OK)
      {
        dps_finalize(client, r, false);
      }
    }

    az_iot_result r = AZ_IOT_OK;
    if (client->dps_mqtt && client->dps_mqtt->iface && client->dps_mqtt->iface->process_loop)
    {
      /* The hold deadline is only tested on the way into this pump, so an
       * adapter that sleeps for the whole timeout when idle would hold past it:
       * a caller passing a timeout longer than the hold would defer
       * registration until that sleep returned. Cap the wait at the time
       * remaining so the deadline is a real bound rather than a check that
       * happens to run often enough. */
      uint32_t wait_ms = timeout_ms;
      if (client->dps_phase == DPS_PHASE_HOLD)
      {
        uint64_t now = az_iot_time_mono_ms();
        uint64_t remaining
            = (client->dps_hold_deadline_ms > now) ? client->dps_hold_deadline_ms - now : 0;
        if ((uint64_t)wait_ms > remaining)
        {
          wait_ms = (uint32_t)remaining;
        }
      }
      r = client->dps_mqtt->iface->process_loop(client->dps_mqtt, wait_ms);
    }

    /* Deferred out of the SUBACK handler: observers may close() the client,
     * which frees the adapter whose process_loop has only just returned.
     *
     * Before dps_apply_deferred() so that a registration answered in the same
     * batch still reports CONNECTED before DISCONNECTING. */
    if (client->dps_pending_ready_announce)
    {
      client->dps_pending_ready_announce = false;
      if (client->dps_mqtt != NULL)
      {
        set_state_to(client, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_CONNECTED, AZ_IOT_OK);
      }
    }

    dps_apply_deferred(client);

    /* A REGISTRATION run owns the client until it finishes, so it returns here:
     * there is no hub session yet to service.
     *
     * A session held only by feature clients does not: the hub connection is
     * live beside it and must keep being pumped, or a device would stop
     * servicing telemetry, twin and method traffic for as long as a feature
     * client held a provisioning session. Fall through to the hub pump.
     *
     * The DPS wait above already consumed the caller's timeout, so the hub is
     * polled without blocking again rather than doubling the tick. */
    if (client->dps_registration_ref)
    {
      return r;
    }
    if (r != AZ_IOT_OK)
    {
      return r;
    }
    timeout_ms = 0;
  }

  /* --- Normal hub session pump --- */
  az_iot_result r = AZ_IOT_OK;
  if (client->active_client)
  {
    r = client->active_client->iface->process_loop(client->active_client, timeout_ms);
  }

  apply_deferred(client);

  /* Fail an in-flight CSR renewal that never received a terminal response
   * (lost 200/error after a 202, or a hub that went silent) so the one-op
   * slot is not stuck BUSY for the life of the connection. */
  if (client->csr_op.in_use && az_iot_time_mono_ms() >= client->csr_op.deadline_ms)
  {
    az_iot_csr_callback cb = client->csr_op.cb;
    void* uc = client->csr_op.user_ctx;
    client->csr_op.in_use = false;
    az_iot_csr_event evt;
    memset(&evt, 0, sizeof(evt));
    evt.kind = AZ_IOT_CSR_FAILED;
    evt.status = AZ_IOT_ERR_TIMEOUT;
    if (cb)
    {
      cb(&evt, uc);
    }
  }

  /* Fail a stalled MQTTv5 presence (birth) handshake so a missing SUBACK or
   * birth-ack can't wedge the client in CONNECTING forever. Reconnect when a
   * policy is configured (mirrors the .NET SDK, which disconnects and
   * retries), otherwise fault. */
  if ((client->presence.phase == AZ_IOT_PRESENCE_PHASE_SUBSCRIBING
       || client->presence.phase == AZ_IOT_PRESENCE_PHASE_BIRTH)
      && az_iot_time_mono_ms() >= client->presence.deadline_ms)
  {
    client->presence.phase = AZ_IOT_PRESENCE_PHASE_NONE;
    if (az_iot_retry_policy_is_enabled(&client->opts.reconnection_policy) && !client->user_close)
    {
      schedule_reconnect(client, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_ERR_TIMEOUT);
    }
    else
    {
      teardown_active(client);
      set_state_to(client, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_FAULTED, AZ_IOT_ERR_TIMEOUT);
    }
  }

  /* A gated filter withdrawn while its ack was still outstanding stops holding
   * the transition -- but the ack that would have announced CONNECTED is never
   * coming, so the release has to be noticed here. */
  if (client->subscription_gate.active && client->subscription_gate.gated_outstanding == 0
      && client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_CONNECTING)
  {
    announce_connected(client);
  }

  /* Fail a gate whose SUBACKs never arrived, so a broker that accepts the
   * connection and then goes quiet cannot wedge the client in CONNECTING --
   * keep-alive cannot notice, because the link is alive. Silence is not a
   * refusal and the filter may well be granted next time, so this is retryable
   * rather than terminal. Only the gated set is waited on.
   *
   * Restricted to a session that is coming up or up, and to a client the
   * application has not closed. close() transitions to DISCONNECTING but tears
   * down only when the peer's DISCONNECT arrives, so the gate outlives it for a
   * moment; without this the deadline could fault a connection that was
   * shutting down cleanly. */
  if (client->subscription_gate.active && client->subscription_gate.gated_outstanding > 0
      && !client->user_close
      && (client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_CONNECTING
          || client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_CONNECTED)
      && az_iot_time_mono_ms() >= client->subscription_gate.deadline_ms)
  {
    memset(&client->subscription_gate, 0, sizeof(client->subscription_gate));
    if (az_iot_retry_policy_is_enabled(&client->opts.reconnection_policy) && !client->user_close)
    {
      schedule_reconnect(client, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_ERR_TIMEOUT);
    }
    else
    {
      teardown_active(client);
      set_state_to(client, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_FAULTED, AZ_IOT_ERR_TIMEOUT);
    }
  }

  /* If we are waiting to reconnect and the deadline has passed, attempt it.
   *
   * `reconnect_due_ms` is the pending-retry token, and firing CONSUMES it.
   * Gating on the state alone is not enough now that the two scopes move
   * independently: a hub failure whose recovery is a re-registration leaves
   * HUB in RECONNECTING while the attempt runs on DPS, so a state-only test
   * would re-fire on every tick for as long as the hub stayed down. */
  if (client->reconnect_due_ms != 0
      && (client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_RECONNECTING
          || client->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_RECONNECTING)
      && client->active_client == NULL && az_iot_time_mono_ms() >= client->reconnect_due_ms)
  {
    az_iot_result cr;
    az_iot_connection_scope attempted;
    client->reconnect_due_ms = 0;
    if (client->needs_reprovision)
    {
      /* The hub refused this identity, or this device has not registered yet;
       * go to DPS rather than reconnecting to a credential that was rejected.
       *
       * The demand is cleared before the attempt ONLY when there is a cached
       * assignment to fall back to. That fallback exists so a failing
       * dps_start() cannot loop through provisioning forever -- but it is only
       * a fallback if the hub path has somewhere to go. With no assignment
       * (opts.host still NULL) an ordinary retry would hand the adapter a NULL
       * endpoint and the client would never reach DPS again, so the demand
       * survives and the next retry provisions. */
      client->needs_reprovision = (client->opts.host == NULL);
      client->session_role = AZ_IOT_MQTT_ROLE_DPS;
      attempted = AZ_IOT_CONN_SCOPE_DPS;
      /* Adopt a session that is already up rather than building a second one:
       * a session its users hold survives registration, and an unconditional
       * start here would overwrite dps_mqtt, orphan that socket and lose any
       * exchange in flight on it. Same rule as open(). */
      client->dps_registration_ref = true;
      if (az_iot_connection_client__dps_session_ready(client))
      {
        cr = dps_do_register_publish(client);
      }
      else if (client->dps_mqtt != NULL)
      {
        cr = AZ_IOT_OK; /* coming up; the SUBACK path registers on it */
      }
      else
      {
        client->dps_phase = DPS_PHASE_NONE;
        cr = dps_start(client);
      }
    }
    else
    {
      attempted = AZ_IOT_CONN_SCOPE_HUB;
      cr = start_connect_attempt(client);
    }
    if (cr != AZ_IOT_OK)
    {
      /* The failure belongs to whichever attempt was just made. Which ladder
       * its retry climbs is decided inside schedule_reconnect(), from the
       * needs_reprovision left standing above. */
      schedule_reconnect(client, attempted, cr);
    }
  }

  return r;
}

/* ------------------------------------------------------------------------- */
/* internal API                                                              */
/* ------------------------------------------------------------------------- */

az_iot_result az_iot_connection_client__set_session_role(
    az_iot_connection_client* client,
    az_iot_mqtt_role role)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!client_is_fully_idle(client))
  {
    return AZ_IOT_ERR_ALREADY_INITIALIZED;
  }
  client->session_role = role;
  return AZ_IOT_OK;
}

/* Internal helper used by both __set_host and __set_client_id. Copies `s` into
 * the in-struct fixed buffer `owned_buf` (bounded by `buf_cap`) and points
 * `*opts_slot` (the live pointer the rest of the code reads) at it. No heap. */
/* Whether `s` could be adopted into a buffer of `buf_cap` bytes. Split out of
 * replace_owned_string() so a caller adopting more than one string can check
 * them all BEFORE mutating any of them: the two halves of a DPS assignment are
 * only meaningful together, and committing one and then rejecting the other
 * leaves the client holding a hub from the new assignment and a device id from
 * the old one. */
static az_iot_result check_owned_string(size_t buf_cap, const char* s)
{
  if (!is_nonempty_cstr(s))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (strlen(s) + 1 > buf_cap)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  return AZ_IOT_OK;
}

static az_iot_result replace_owned_string(
    char* owned_buf,
    size_t buf_cap,
    const char** opts_slot,
    const char* s)
{
  az_iot_result r = check_owned_string(buf_cap, s);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  size_t n = strlen(s);
  memcpy(owned_buf, s, n + 1);
  *opts_slot = owned_buf;
  return AZ_IOT_OK;
}

az_iot_result az_iot_connection_client__set_host(az_iot_connection_client* client, const char* host)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!client_is_fully_idle(client))
  {
    return AZ_IOT_ERR_ALREADY_INITIALIZED;
  }
  return replace_owned_string(
      client->provisioned_iot_hub_hostname,
      sizeof(client->provisioned_iot_hub_hostname),
      &client->opts.host,
      host);
}

az_iot_result az_iot_connection_client__set_client_id(
    az_iot_connection_client* client,
    const char* client_id)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!client_is_fully_idle(client))
  {
    return AZ_IOT_ERR_ALREADY_INITIALIZED;
  }
  return replace_owned_string(
      client->provisioned_device_id,
      sizeof(client->provisioned_device_id),
      &client->opts.client_id,
      client_id);
}

void az_iot_connection_client__seed_rng(az_iot_connection_client* client, uint64_t seed)
{
  if (!client)
  {
    return;
  }
  client->rng_state = seed ? seed : 1ull;
}

void az_iot_connection_client__presence_force_timeout(az_iot_connection_client* client)
{
  if (!client)
  {
    return;
  }
  if (client->presence.phase == AZ_IOT_PRESENCE_PHASE_SUBSCRIBING
      || client->presence.phase == AZ_IOT_PRESENCE_PHASE_BIRTH)
  {
    client->presence.deadline_ms = 0;
  }
}

az_iot_result az_iot_connection_client__presence_twin_versions(
    const az_iot_connection_client* client,
    uint64_t* out_desired_version,
    uint64_t* out_reported_version)
{
  if (!client || !out_desired_version || !out_reported_version)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  *out_desired_version = client->presence.desired_version;
  *out_reported_version = client->presence.reported_version;
  return AZ_IOT_OK;
}

void az_iot_connection_client__gen_uuid(
    az_iot_connection_client* client,
    uint8_t out[AZ_IOT_CORRELATION_UUID_LEN])
{
  if (!client || !out)
  {
    return;
  }
  gen_uuid_v4(client, out);
}

az_iot_result az_iot_connection_client__presence_nonce(
    const az_iot_connection_client* client,
    uint8_t out[AZ_IOT_CORRELATION_UUID_LEN])
{
  if (!client || !out)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (client->presence.phase != AZ_IOT_PRESENCE_PHASE_DONE)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  memcpy(out, client->presence.nonce, AZ_IOT_CORRELATION_UUID_LEN);
  return AZ_IOT_OK;
}

bool az_iot_connection_client__twin_push_desired(const az_iot_connection_client* client)
{
  return client && client->opts.twin_push.push_desired;
}

void az_iot_connection_client__subscription_gate_force_timeout(az_iot_connection_client* client)
{
  if (client && client->subscription_gate.active)
  {
    client->subscription_gate.deadline_ms = 0;
  }
}

az_iot_result az_iot_connection_client__register_inbound_handler(
    az_iot_connection_client* client,
    const char* topic_prefix,
    az_iot_inbound_handler_callback cb,
    void* user_ctx)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return az_iot_dispatch_register_prefix(&client->dispatch, topic_prefix, cb, user_ctx);
}

size_t az_iot_connection_client__unregister_inbound_handlers(
    az_iot_connection_client* client,
    void* user_ctx)
{
  if (!client)
  {
    return 0;
  }
  return az_iot_dispatch_unregister_by_ctx(&client->dispatch, user_ctx);
}

az_iot_result az_iot_connection_client__register_session_end_handler(
    az_iot_connection_client* client,
    az_iot_session_end_callback cb,
    void* user_ctx)
{
  if (!client || !cb)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  size_t free_slot = AZ_IOT_MAX_SESSION_HANDLERS;
  for (size_t i = 0; i < AZ_IOT_MAX_SESSION_HANDLERS; ++i)
  {
    /* Keyed on user_ctx: a feature client that re-initializes over a live
     * instance replaces its entry instead of consuming a second slot. */
    if (client->session_handlers[i].in_use)
    {
      if (client->session_handlers[i].user_ctx == user_ctx)
      {
        client->session_handlers[i].cb = cb;
        return AZ_IOT_OK;
      }
    }
    else if (free_slot == AZ_IOT_MAX_SESSION_HANDLERS)
    {
      free_slot = i;
    }
  }

  if (free_slot == AZ_IOT_MAX_SESSION_HANDLERS)
  {
    AZ_IOT_LOG_ERROR("connection: session-end handler registry is full");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  client->session_handlers[free_slot].cb = cb;
  client->session_handlers[free_slot].user_ctx = user_ctx;
  client->session_handlers[free_slot].in_use = true;
  return AZ_IOT_OK;
}

size_t az_iot_connection_client__unregister_session_end_handler(
    az_iot_connection_client* client,
    void* user_ctx)
{
  if (!client)
  {
    return 0;
  }
  for (size_t i = 0; i < AZ_IOT_MAX_SESSION_HANDLERS; ++i)
  {
    if (client->session_handlers[i].in_use && client->session_handlers[i].user_ctx == user_ctx)
    {
      client->session_handlers[i].in_use = false;
      client->session_handlers[i].cb = NULL;
      client->session_handlers[i].user_ctx = NULL;
      return 1;
    }
  }
  return 0;
}

az_iot_connection_state az_iot_connection_client_get_state(
    const az_iot_connection_client* client,
    az_iot_connection_scope scope)
{
  /* IDLE is the honest answer for a scope that does not apply -- a direct hub
   * connection never provisions, so its DPS lifecycle genuinely never leaves
   * IDLE -- and it is the safe answer for a NULL client: nothing is usable. */
  if (client == NULL || (scope != AZ_IOT_CONN_SCOPE_DPS && scope != AZ_IOT_CONN_SCOPE_HUB))
  {
    return AZ_IOT_CONN_STATE_IDLE;
  }
  return client->state[scope];
}

bool az_iot_connection_client__is_connected(const az_iot_connection_client* client)
{
  return client && client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_CONNECTED;
}

const char* az_iot_connection_client__device_id(const az_iot_connection_client* client)
{
  return client ? client->opts.client_id : NULL;
}

const char* az_iot_connection_client_get_iothub_address(const az_iot_connection_client* client)
{
  /* opts.host tracks the effective hub: the caller-supplied host for a direct
   * connection, or the DPS-assigned hub (copied into provisioned_iot_hub_hostname
   * and pointed-to here) after provisioning. NULL for a not-yet-provisioned
   * DPS-only client. */
  return client ? client->opts.host : NULL;
}

AZ_NODISCARD az_iot_result az_iot_connection_client_get_hub_profile(
    const az_iot_connection_client* client,
    az_iot_hub_profile* out_profile)
{
  if (!client || !out_profile)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Readable once connected, and also after a profile-driven failure -- that is
   * the case where an application most needs to see what the service said. */
  if (client->state[AZ_IOT_CONN_SCOPE_HUB] != AZ_IOT_CONN_STATE_CONNECTED
      && client->connection_profile != AZ_IOT_CONNECTION_PROFILE_UNKNOWN)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  out_profile->connection_profile = client->connection_profile;
  out_profile->connection_profile_raw = client->connection_profile_raw;
  out_profile->connection_profile_raw_truncated = client->connection_profile_raw_truncated;
  return AZ_IOT_OK;
}

az_iot_result az_iot_connection_client__require_profile(
    az_iot_connection_client* client,
    az_iot_connection_profile profile)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (client->required_profile_refs > 0 && client->required_profile != profile)
  {
    AZ_IOT_LOG_ERROR("connection: a feature client for the other hub generation is already "
                     "attached to this connection");
    return AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH;
  }
  if (client->connection_profile_resolved && client->connection_profile != profile)
  {
    return AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH;
  }
  client->required_profile = profile;
  client->required_profile_refs++;
  return AZ_IOT_OK;
}

void az_iot_connection_client__release_profile(az_iot_connection_client* client)
{
  if (client && client->required_profile_refs > 0)
  {
    client->required_profile_refs--;
  }
}

az_iot_result az_iot_connection_client__register_feature_client_bind(
    az_iot_connection_client* client,
    void* owner,
    az_iot_feature_client_bind_callback on_bind)
{
  if (!client || !owner || !on_bind)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  size_t free_slot = AZ_IOT_MAX_FEATURE_CLIENT_BINDS;
  for (size_t i = 0; i < AZ_IOT_MAX_FEATURE_CLIENT_BINDS; ++i)
  {
    if (client->feature_client_binds[i].owner == owner)
    {
      client->feature_client_binds[i].on_bind = on_bind;
      return AZ_IOT_OK;
    }
    if (!client->feature_client_binds[i].owner && free_slot == AZ_IOT_MAX_FEATURE_CLIENT_BINDS)
    {
      free_slot = i;
    }
  }
  if (free_slot == AZ_IOT_MAX_FEATURE_CLIENT_BINDS)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  client->feature_client_binds[free_slot].owner = owner;
  client->feature_client_binds[free_slot].on_bind = on_bind;
  /* Already connected means the identity is settled and this session's binds
   * have run, so this one has to catch up or it would sit idle until the next
   * connect. Mirrors __add_subscription_on_connect subscribing immediately. */
  if (client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_CONNECTED)
  {
    return on_bind(owner, client);
  }
  return AZ_IOT_OK;
}

void az_iot_connection_client__unregister_feature_client_bind(
    az_iot_connection_client* client,
    const void* owner)
{
  if (!client)
  {
    return;
  }
  for (size_t i = 0; i < AZ_IOT_MAX_FEATURE_CLIENT_BINDS; ++i)
  {
    if (client->feature_client_binds[i].owner == owner)
    {
      client->feature_client_binds[i].owner = NULL;
      client->feature_client_binds[i].on_bind = NULL;
    }
  }
}

/* Rebuild every attached feature client's topics against the identity this
 * attempt will actually use. Their previous registrations are withdrawn first,
 * so a device id or generation that changed during re-provisioning cannot leave
 * a filter behind that the new hub would refuse. */
static az_iot_result run_feature_client_binds(az_iot_connection_client* c)
{
  for (size_t i = 0; i < AZ_IOT_MAX_FEATURE_CLIENT_BINDS; ++i)
  {
    void* owner = c->feature_client_binds[i].owner;
    az_iot_result (*on_bind)(void*, az_iot_connection_client*) = c->feature_client_binds[i].on_bind;
    az_iot_result r;
    if (!owner || !on_bind)
    {
      continue;
    }
    (void)az_iot_connection_client__remove_subscriptions_for(c, owner);
    (void)az_iot_connection_client__unregister_inbound_handlers(c, owner);

    r = on_bind(owner, c);
    if (r != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERROR("connection: a feature client could not bind its topics for this session");
      return r;
    }
  }
  return AZ_IOT_OK;
}

/* --- provisioning-session seam ------------------------------------------- */

az_iot_result az_iot_connection_client__dps_user_acquire(az_iot_connection_client* client)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (client->dps_user_count == UINT8_MAX)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  client->dps_user_count++;
  return AZ_IOT_OK;
}

void az_iot_connection_client__dps_user_release(az_iot_connection_client* client)
{
  if (client == NULL || client->dps_user_count == 0)
  {
    return;
  }
  client->dps_user_count--;
  if (client->dps_user_count == 0)
  {
    /* The demand is gone. A later holder is NEW demand and must not inherit a
     * backoff, or a latched refusal, earned by whoever came before it. */
    dps_user_retry_reset(client);
  }
  /* The session is NOT torn down here even if this was the last ref. Release
   * is reachable from inside a message callback, and freeing the adapter there
   * would free the object it is still dispatching on. The pump notices the
   * count reached zero and closes at a safe point. */
}

bool az_iot_connection_client__dps_registration_pending(const az_iot_connection_client* client)
{
  return client != NULL && client->dps_registration_ref;
}

az_iot_result az_iot_connection_client__dps_session_ensure(az_iot_connection_client* client)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Only a holder may ask: the demand is already recorded, so this asks
   * whether the session it is entitled to is usable yet. */
  if (!dps_session_demanded(client) || !dps_configured(client))
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  /* Usable now -- including a session still on its original provisioning run,
   * which a caller may legitimately use before the device registers. */
  if (az_iot_connection_client__dps_session_ready(client))
  {
    return AZ_IOT_OK;
  }

  /* One exists but is not ready yet. */
  if (client->dps_mqtt != NULL)
  {
    return AZ_IOT_ERR_BUSY;
  }

  /* Never from a settled fault, and never while a retry is pending.
   *
   * FAULTED: starting a session would announce DPS:CONNECTING and drag that
   * lifecycle out of its terminal state, so the application would never see
   * the fault settle.
   *
   * RECONNECTING: a registration retry is already scheduled. Opening a session
   * here moves the DPS lifecycle to CONNECTING without consuming the pending
   * deadline, so the retry gate stops matching and the registration never
   * happens -- the device would stay unregistered indefinitely.
   *
   * A gate, not a latch: close() is the supported exit from FAULTED and the
   * retry fires on its own, so the next attempt lets this through again.
   *
   * Here rather than in each caller: the rule is a property of the connection
   * state, and every holder would otherwise need its own copy. */
  for (size_t i = 0; i < AZ_IOT_CONN_SCOPE_COUNT; ++i)
  {
    if (client->state[i] == AZ_IOT_CONN_STATE_FAULTED)
    {
      return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    if (client->state[i] == AZ_IOT_CONN_STATE_RECONNECTING)
    {
      return AZ_IOT_ERR_BUSY;
    }
  }

  /* A provisioning run is mid-flight and owns the session it is about to
   * open. */
  if (client->dps_phase != DPS_PHASE_NONE && client->dps_phase != DPS_PHASE_DONE)
  {
    return AZ_IOT_ERR_BUSY;
  }

  /* The core's own pacing for a session held by its users. Without it a failed
   * session was re-opened on the very next pump tick, for as long as the
   * application kept pumping. */
  if (client->dps_user_retry_blocked)
  {
    /* Settled, like FAULTED: retries are off or the budget is spent, and the
     * core will not re-open on its own. A holder that keeps asking gets a
     * stable answer it can report, rather than a session attempt per tick. */
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  /* Consumed on firing, so the deadline cannot re-authorize a second attempt. */
  if (!az_iot_retry_state__due(&client->dps_user_retry))
  {
    return AZ_IOT_ERR_BUSY;
  }

  client->dps_phase = DPS_PHASE_NONE;
  az_iot_result r = dps_start(client);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF("dps: could not open a provisioning session (%d)", (int)r);
    /* A SYNCHRONOUS failure never reaches dps_finalize(), so nothing else would
     * pace it: the caller gets the error and its next pump tick asks again
     * immediately. That is the same hot loop, on the path where the adapter
     * cannot even be created -- which is the one least likely to fix itself.
     *
     * But NOT every error here is a failure. dps_start() announces
     * DPS:CONNECTING synchronously, and close() is legal from inside that
     * callback; the cancellation it detects returns the same
     * AZ_IOT_ERR_NOT_CONNECTED as a genuine start failure. close() has just
     * RESET this ladder -- it is the documented escape from a settled refusal
     * -- so pacing that case would recreate the deadline, or the blocked latch,
     * immediately after the caller cleared it, and the escape would not work.
     *
     * dps_start() sets dps_start_cancelled for exactly that case. user_close
     * does NOT work here: close() with no hub adapter -- which is this case,
     * closing during provisioning -- clears it before returning. */
    if (!client->dps_start_cancelled)
    {
      dps_user_retry_schedule(client, 0);
    }
    return r;
  }
  /* Started, not ready: the caller must wait for the SUBACK. */
  return AZ_IOT_ERR_BUSY;
}

az_iot_result az_iot_connection_client__dps_hold_acquire(az_iot_connection_client* client)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* A registration already in flight cannot be held: the request is on the wire
   * and its response will tear the session down. Say so rather than appear to
   * hold something.
   *
   * DONE is allowed, and that distinction matters. It is not "too late", it is
   * "this session is finished" -- and it is the state a provisioned client sits
   * in until it reprovisions. Refusing here would leave a holder unable to
   * reserve the NEXT session, so a reprovision would run from SUBACK straight
   * into registration with no hold. */
  if (client->dps_phase == DPS_PHASE_REGISTERING || client->dps_phase == DPS_PHASE_POLLING)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  if (client->dps_hold_count == UINT8_MAX)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  client->dps_hold_count++;
  return AZ_IOT_OK;
}

void az_iot_connection_client__dps_hold_release(az_iot_connection_client* client)
{
  if (client == NULL || client->dps_hold_count == 0)
  {
    return;
  }
  client->dps_hold_count--;
  /* Registration is issued from the do_work pump, not here: releasing may
   * happen inside a message callback, and publishing from there would reenter
   * the adapter while it is dispatching. */
}

bool az_iot_connection_client__dps_hold_is_active(const az_iot_connection_client* client)
{
  return client != NULL && client->dps_hold_active;
}

bool az_iot_connection_client__dps_session_ready(const az_iot_connection_client* client)
{
  if (client == NULL || client->dps_mqtt == NULL)
  {
    return false;
  }
  /* The SUBACK, not the phase, is what proves a response can come back:
   * SUBSCRIBING is entered when the SUBSCRIBE is sent, so publishing on the
   * phase alone could outrun the route the reply needs. The flag is cleared
   * when the session is torn down. */
  return client->dps_subscription_confirmed;
}

az_iot_result az_iot_connection_client__dps_publish(
    az_iot_connection_client* client,
    const az_iot_mqtt_message* msg)
{
  if (client == NULL || msg == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!az_iot_connection_client__dps_session_ready(client))
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  if (client->dps_mqtt->iface == NULL || client->dps_mqtt->iface->publish == NULL)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  uint16_t pid = 0;
  return client->dps_mqtt->iface->publish(client->dps_mqtt, msg, &pid);
}

void az_iot_connection_client__set_dps_message_observer(
    az_iot_connection_client* client,
    az_iot_dps_message_observer observer,
    void* user_ctx)
{
  if (client == NULL)
  {
    return;
  }
  /* Last writer wins, and there is exactly one slot. Calling this twice with
   * two different observers would silently leave the first one never called,
   * so a second registration over a live one is refused rather than honoured:
   * only the owner may clear its own registration (observer == NULL) before a
   * different one takes over. */
  if (observer != NULL && client->dps_message_observer != NULL
      && client->dps_message_observer != observer)
  {
    AZ_IOT_LOG_ERROR("a provisioning-session message observer is already registered");
    return;
  }
  client->dps_message_observer = observer;
  client->dps_message_observer_ctx = user_ctx;
}

az_iot_result az_iot_connection_client__publish(
    az_iot_connection_client* client,
    const az_iot_mqtt_message* msg,
    az_iot_publish_ack_callback ack_cb,
    void* ack_user_ctx)
{
  if (!client || !msg)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!client->active_client || client->state[AZ_IOT_CONN_SCOPE_HUB] != AZ_IOT_CONN_STATE_CONNECTED)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  /* QoS 1/2 with a callback: reserve the correlation slot BEFORE publishing,
   * so a full table sends nothing and the caller can retry without
   * duplicating. The reserved slot has no cb, so neither the PUBACK match nor
   * teardown_active() acts on it. Adapters deliver events only from
   * process_loop() (az_iot_mqtt_iface), so no PUBACK can arrive before the
   * packet id is filled in below. */
  size_t slot = AZ_IOT_MAX_PENDING_PUBACKS;
  if (msg->qos != AZ_IOT_MQTT_QOS_0 && ack_cb)
  {
    for (size_t i = 0; i < AZ_IOT_MAX_PENDING_PUBACKS; ++i)
    {
      if (!client->pending_pubacks[i].in_use)
      {
        slot = i;
        break;
      }
    }
    if (slot == AZ_IOT_MAX_PENDING_PUBACKS)
    {
      return AZ_IOT_ERR_BUSY;
    }
    client->pending_pubacks[slot].packet_id = 0;
    client->pending_pubacks[slot].cb = NULL;
    client->pending_pubacks[slot].user_ctx = NULL;
    client->pending_pubacks[slot].in_use = true;
  }

  uint16_t pid = 0;
  az_iot_result r = client->active_client->iface->publish(client->active_client, msg, &pid);
  if (slot < AZ_IOT_MAX_PENDING_PUBACKS)
  {
    if (r != AZ_IOT_OK)
    {
      client->pending_pubacks[slot].in_use = false;
      return r;
    }
    client->pending_pubacks[slot].packet_id = pid;
    client->pending_pubacks[slot].cb = ack_cb;
    client->pending_pubacks[slot].user_ctx = ack_user_ctx;
    return AZ_IOT_OK;
  }
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  /* QoS 0: there is no PUBACK on the wire. Fire the cb synchronously. QoS 1/2
   * without a callback: the future PUBACK is silently absorbed. */
  if (msg->qos == AZ_IOT_MQTT_QOS_0 && ack_cb)
  {
    ack_cb(AZ_IOT_OK, ack_user_ctx);
  }
  return AZ_IOT_OK;
}

bool az_iot_connection_client__has_free_puback_slot(const az_iot_connection_client* client)
{
  if (client == NULL)
  {
    return false;
  }
  for (size_t i = 0; i < AZ_IOT_MAX_PENDING_PUBACKS; ++i)
  {
    if (!client->pending_pubacks[i].in_use)
    {
      return true;
    }
  }
  return false;
}

az_iot_result az_iot_connection_client__subscribe(
    az_iot_connection_client* client,
    const char* topic_filter,
    az_iot_mqtt_qos qos,
    uint16_t* out_packet_id)
{
  if (!client || !topic_filter)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!client->active_client || client->state[AZ_IOT_CONN_SCOPE_HUB] != AZ_IOT_CONN_STATE_CONNECTED)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  return client->active_client->iface->subscribe(
      client->active_client, topic_filter, qos, out_packet_id);
}

az_iot_result az_iot_connection_client__add_subscription_on_connect(
    az_iot_connection_client* client,
    const char* topic_filter,
    az_iot_mqtt_qos qos,
    const void* owner,
    az_iot_subscription_failure_scope failure_scope,
    az_iot_subscription_failed_callback on_failed)
{
  if (!client || !topic_filter)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  size_t n = strlen(topic_filter);
  if (n == 0 || n + 1 > AZ_IOT_PERSISTENT_SUB_TOPIC_MAX)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Find a free slot. Duplicates are allowed but pointless; callers are
   * expected to register each filter once. */
  size_t slot = AZ_IOT_MAX_PERSISTENT_SUBS;
  for (size_t i = 0; i < AZ_IOT_MAX_PERSISTENT_SUBS; ++i)
  {
    if (!client->persistent_subs[i].in_use)
    {
      slot = i;
      break;
    }
  }
  if (slot == AZ_IOT_MAX_PERSISTENT_SUBS)
  {
    /* A fixed array -- c/src does not allocate -- so running out is a capacity
     * failure, not an unsupported operation. NOT_SUPPORTED was too vague to act
     * on: it is also what this SDK returns for "that hub flavor cannot do this
     * at all", which no amount of extra registry space would fix.
     *
     * Logged as well as returned, because whoever registers last is the one
     * that fails. The filter named here says more about ordering than about
     * which feature is at fault, so the caller needs to see it to work out that
     * the registry -- not that feature -- is what ran out. */
    AZ_IOT_LOG_ERRORF(
        "connection: cannot register '%s': all %d persistent subscription slots are in use "
        "(raise AZ_IOT_MAX_PERSISTENT_SUBS)",
        topic_filter,
        (int)AZ_IOT_MAX_PERSISTENT_SUBS);
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  memcpy(client->persistent_subs[slot].topic_filter, topic_filter, n + 1);
  client->persistent_subs[slot].qos = qos;
  client->persistent_subs[slot].owner = owner;
  client->persistent_subs[slot].profile = client->connection_profile;
  client->persistent_subs[slot].failure_scope = failure_scope;
  client->persistent_subs[slot].on_failed = on_failed;
  client->persistent_subs[slot].in_use = true;

  /* Issue the SUBSCRIBE now when there is a session to carry it: either the
   * connection is up, or a connect batch is still in flight and this filter can
   * join it. Feature clients rebuilt from the CONNECTED callback take this
   * path, so the ack is correlated like any other -- otherwise a refusal would
   * leave the session reporting CONNECTED with a filter that is dead. */
  if (client->active_client && client->active_client->iface
      && (client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_CONNECTED
          || client->subscription_gate.active))
  {
    uint16_t pid = 0;
    az_iot_result sr
        = client->active_client->iface->subscribe(client->active_client, topic_filter, qos, &pid);
    if (sr != AZ_IOT_OK)
    {
      /* Nothing reached the wire, so roll the registration back and let the
       * caller unwind. Keeping an entry that was never issued would report a
       * subscription the device does not have. */
      memset(&client->persistent_subs[slot], 0, sizeof(client->persistent_subs[slot]));
      AZ_IOT_LOG_ERRORF("connection: could not subscribe '%s'", topic_filter);
      return sr;
    }
    subscription_gate_track(client, pid, slot, failure_scope == AZ_IOT_SUBSCRIPTION_FAILS_SESSION);
    if (client->subscription_gate.deadline_ms == 0)
    {
      client->subscription_gate.deadline_ms = subscription_ack_deadline(client);
    }
  }
  return AZ_IOT_OK;
}

size_t az_iot_connection_client__remove_subscriptions_for(
    az_iot_connection_client* client,
    const void* owner)
{
  if (!client)
  {
    return 0;
  }
  /* Withdraw each entry from the broker too, on both generations. This cannot
   * touch MQTTv5's device-wide ih/{device_id}/dev/# subscription: the presence
   * handshake issues that one directly, not through this registry, so it has no
   * owner and never appears in the loop below. On MQTTv5, entries in this registry
   * are application custom topics; feature delivery uses the wildcard instead.
   * Withdrawing a custom filter leaves the wildcard -- and therefore every
   * feature's delivery -- untouched. */
  const bool unsubscribe_on_the_wire = client->active_client && client->active_client->iface
      && client->active_client->iface->unsubscribe
      && client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_CONNECTED;

  size_t removed = 0;
  for (size_t i = 0; i < AZ_IOT_MAX_PERSISTENT_SUBS; ++i)
  {
    if (!client->persistent_subs[i].in_use || client->persistent_subs[i].owner != owner)
    {
      continue;
    }
    if (unsubscribe_on_the_wire)
    {
      uint16_t pid = 0;
      (void)client->active_client->iface->unsubscribe(
          client->active_client, client->persistent_subs[i].topic_filter, &pid);
    }
    subscription_gate_forget(client, i);
    memset(&client->persistent_subs[i], 0, sizeof(client->persistent_subs[i]));
    ++removed;
  }
  return removed;
}

/* Drop persistent subscriptions that belong to a different hub generation than
 * the one now resolved. Without this, a device reassigned from MQTTv3 to MQTTv5
 * would re-issue its $iothub/... filters at the new hub, which does not grant
 * them -- and once CONNECTED is gated on those SUBACKs, the session could never
 * come up and the application would never get the callback that would have
 * removed them. See docs/eng/connection-c.md section 5.3. */
static void drop_subscriptions_from_other_generations(az_iot_connection_client* c)
{
  for (size_t i = 0; i < AZ_IOT_MAX_PERSISTENT_SUBS; ++i)
  {
    if (!c->persistent_subs[i].in_use || c->persistent_subs[i].profile == c->connection_profile)
    {
      continue;
    }
    AZ_IOT_LOG_WARNF(
        "connection: dropping '%s' -- registered for a different hub generation than the one now "
        "assigned",
        c->persistent_subs[i].topic_filter);
    subscription_gate_forget(c, i);
    memset(&c->persistent_subs[i], 0, sizeof(c->persistent_subs[i]));
  }
}

/* ------------------------------------------------------------------------- */
/* Runtime Hub-side certificate renewal ($iothub/credentials/...)            */
/* ------------------------------------------------------------------------- */

#define CSR_RES_PREFIX "$iothub/credentials/res/"
#define CSR_RES_FILTER "$iothub/credentials/res/#"
#define CSR_MAX_BASE64 8192 /* service cap: CSR <= 8 KB */
#define CSR_OP_TIMEOUT_MS 120000u /* give up on a renewal with no terminal response after 2 min */

/* Hub renewal request: publish topic + body formats. */
#define CSR_RENEW_TOPIC_PREFIX "$iothub/credentials/POST/issueCertificate/?$rid="
#define CSR_RENEW_BODY_ID_PREFIX "{\"id\":\""
#define CSR_RENEW_BODY_CSR_INFIX "\",\"csr\":\""
#define CSR_RENEW_BODY_REPLACE_INFIX "\",\"replace\":\""
#define CSR_RENEW_BODY_SUFFIX "\"}"

/* Hub renewal response JSON fields (issued chain / error body). */
#define CSR_JSON_CERTIFICATES "certificates"
#define CSR_JSON_ERROR_CODE "errorCode"
#define CSR_JSON_RETRY_AFTER "retryAfter"

/* Parse errorCode + retryAfter (both optional) from an error response body. */
static void csr_parse_error(az_span payload, int32_t* out_code, int32_t* out_retry)
{
  *out_code = 0;
  *out_retry = 0;
  az_json_reader jr;
  if (az_result_failed(az_json_reader_init(&jr, payload, NULL))
      || az_result_failed(az_json_reader_next_token(&jr))
      || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return;
  }
  while (az_result_succeeded(az_json_reader_next_token(&jr))
         && jr.token.kind != AZ_JSON_TOKEN_END_OBJECT)
  {
    if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
    {
      continue;
    }
    bool is_code = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR(CSR_JSON_ERROR_CODE));
    bool is_retry = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR(CSR_JSON_RETRY_AFTER));
    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      return;
    }
    if (is_code && jr.token.kind == AZ_JSON_TOKEN_NUMBER)
    {
      if (az_result_failed(az_json_token_get_int32(&jr.token, out_code)))
      {
        *out_code = 0;
      }
    }
    else if (is_retry && jr.token.kind == AZ_JSON_TOKEN_NUMBER)
    {
      if (az_result_failed(az_json_token_get_int32(&jr.token, out_retry)))
      {
        *out_retry = 0;
      }
    }
    else if (
        jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
    {
      if (az_result_failed(az_json_reader_skip_children(&jr)))
      {
        return;
      }
    }
  }
}

/* Inbound handler for $iothub/credentials/res/{status}/?$rid={rid}. */
static void on_csr_response(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_connection_client* c = (az_iot_connection_client*)user_ctx;
  if (!c || !msg || !msg->topic || !c->csr_op.in_use)
  {
    return;
  }

  /* Parse the status code and $rid from the topic. */
  const char* p = msg->topic + (sizeof(CSR_RES_PREFIX) - 1);
  int status = 0;
  while (*p >= '0' && *p <= '9')
  {
    status = status * 10 + (*p - '0');
    ++p;
  }
  const char* rid = strstr(p, "$rid=");
  if (!rid)
  {
    return;
  }
  rid += 5;
  size_t rid_len = strcspn(rid, "&");
  if (rid_len != strlen(c->csr_op.request_id) || strncmp(rid, c->csr_op.request_id, rid_len) != 0)
  {
    return; /* response for a different request */
  }

  az_iot_csr_callback cb = c->csr_op.cb;
  void* uc = c->csr_op.user_ctx;
  az_span payload = az_span_create((uint8_t*)(uintptr_t)msg->payload, (int32_t)msg->payload_len);
  az_iot_csr_event evt;
  memset(&evt, 0, sizeof(evt));

  if (status == 202)
  {
    /* Accepted: signing in progress; keep the op open for the 200/error and
     * extend the deadline so a slow-but-alive signer is not timed out. */
    c->csr_op.deadline_ms = az_iot_time_mono_ms() + CSR_OP_TIMEOUT_MS;
    evt.kind = AZ_IOT_CSR_ACCEPTED;
    evt.status = AZ_IOT_OK;
    if (cb)
    {
      cb(&evt, uc);
    }
    return;
  }

  /* Terminal: the operation completes here regardless of outcome. */
  c->csr_op.in_use = false;

  if (status == 200)
  {
    az_span certs[CERT_CHAIN_MAX_CERTS];
    size_t count = 0;
    az_iot_result rc = AZ_IOT_ERR_PROTOCOL;
    az_json_reader jr;
    if (az_result_succeeded(az_json_reader_init(&jr, payload, NULL))
        && az_result_succeeded(az_json_reader_next_token(&jr))
        && jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT)
    {
      bool in_arr = false;
      while (az_result_succeeded(az_json_reader_next_token(&jr))
             && jr.token.kind != AZ_JSON_TOKEN_END_OBJECT)
      {
        if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
        {
          continue;
        }
        bool m = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR(CSR_JSON_CERTIFICATES));
        if (az_result_failed(az_json_reader_next_token(&jr)))
        {
          break;
        }
        if (m && jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
        {
          in_arr = true;
          break;
        }
        if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT
            || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
        {
          if (az_result_failed(az_json_reader_skip_children(&jr)))
          {
            break;
          }
        }
      }
      if (in_arr)
      {
        rc = az_iot_cert_util_collect_chain_spans(&jr, certs, CERT_CHAIN_MAX_CERTS, &count);
      }
    }

    if (rc == AZ_IOT_OK && count > 0)
    {
      az_iot_issued_certificate issued;
      issued.certificates = certs;
      issued.count = count;
      evt.kind = AZ_IOT_CSR_ISSUED;
      evt.status = AZ_IOT_OK;
      evt.issued = &issued;
      if (cb)
      {
        cb(&evt, uc);
      }
    }
    else
    {
      evt.kind = AZ_IOT_CSR_FAILED;
      evt.status = (rc == AZ_IOT_OK) ? AZ_IOT_ERR_PROTOCOL : rc;
      if (cb)
      {
        cb(&evt, uc);
      }
    }
    return;
  }

  /* Error response (4xx/5xx). */
  {
    int32_t code = 0, retry = 0;
    csr_parse_error(payload, &code, &retry);
    evt.kind = AZ_IOT_CSR_FAILED;
    evt.status = AZ_IOT_ERR_MQTT;
    evt.service_code = code;
    evt.retry_after_s = (uint32_t)(retry > 0 ? retry : 0);
    if (cb)
    {
      cb(&evt, uc);
    }
  }
}

AZ_NODISCARD az_iot_result az_iot_connection_client_send_csr(
    az_iot_connection_client* client,
    const az_iot_certificate_signing_request* csr,
    const char* request_id,
    const char* replace,
    az_iot_csr_callback cb,
    void* user_ctx)
{
  if (!client || !csr || !csr->csr_base64 || !cb)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!az_iot_connection_client__is_connected(client))
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  if (client->session_role != AZ_IOT_MQTT_ROLE_HUB_MQTT_V3)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED; /* MQTTv5 path not defined yet */
  }
  if (client->csr_op.in_use)
  {
    return AZ_IOT_ERR_BUSY;
  }
  if (az_span_size(client->opts.csr_payload_buffer) <= 0)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE; /* caller must provide opts.csr_payload_buffer */
  }

  /* Validate the CSR: base64 and within the 8 KB service cap. */
  size_t csr_len = 0;
  if (!az_iot_cert_util_is_base64(csr->csr_base64, &csr_len) || csr_len > CSR_MAX_BASE64)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* Request id: caller-provided (resubmit) or generated. */
  if (is_nonempty_cstr(request_id))
  {
    size_t n = strlen(request_id);
    if (n + 1 > sizeof(client->csr_op.request_id))
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
    memcpy(client->csr_op.request_id, request_id, n + 1);
  }
  else
  {
    az_iot_cert_util_gen_request_id(
        &client->rng_state, client->csr_op.request_id, sizeof(client->csr_op.request_id));
  }

  /* Subscribe to the response topic + register the handler once. */
  if (!client->csr_op.subscribed)
  {
    az_iot_result r = az_iot_connection_client__register_inbound_handler(
        client, CSR_RES_PREFIX, on_csr_response, client);
    if (r != AZ_IOT_OK)
    {
      return r;
    }
    r = az_iot_connection_client__add_subscription_on_connect(
        client, CSR_RES_FILTER, AZ_IOT_MQTT_QOS_1, client, AZ_IOT_SUBSCRIPTION_FAILS_SESSION, NULL);
    if (r != AZ_IOT_OK)
    {
      (void)az_iot_connection_client__unregister_inbound_handlers(client, client);
      return r;
    }
    client->csr_op.subscribed = true;
  }

  /* Build the request body into the caller-provided payload buffer. */
  const char* device_id = az_iot_connection_client__device_id(client);
  if (!device_id)
  {
    device_id = "";
  }
  char* body = (char*)az_span_ptr(client->opts.csr_payload_buffer);
  size_t body_len = 0;
  const char* body_parts[]
      = { CSR_RENEW_BODY_ID_PREFIX,     device_id, CSR_RENEW_BODY_CSR_INFIX, csr->csr_base64,
          CSR_RENEW_BODY_REPLACE_INFIX, replace,   CSR_RENEW_BODY_SUFFIX };
  /* Without a replacement id the body stops after the csr field, so the last
   * three parts collapse to the closing brace. */
  const bool with_replace = is_nonempty_cstr(replace);
  if (!with_replace)
  {
    body_parts[4] = CSR_RENEW_BODY_SUFFIX;
  }
  if (az_iot_span_writer_build_str(
          client->opts.csr_payload_buffer, &body_len, body_parts, with_replace ? 7u : 5u)
      != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("send_csr: opts.csr_payload_buffer is too small for the request body");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  char topic[128];
  const char* topic_parts[] = { CSR_RENEW_TOPIC_PREFIX, client->csr_op.request_id };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, topic_parts, 2) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.payload = (const uint8_t*)body;
  msg.payload_len = body_len;
  msg.qos = AZ_IOT_MQTT_QOS_1;

  client->csr_op.cb = cb;
  client->csr_op.user_ctx = user_ctx;
  client->csr_op.in_use = true;
  client->csr_op.deadline_ms = az_iot_time_mono_ms() + CSR_OP_TIMEOUT_MS;

  az_iot_result r = az_iot_connection_client__publish(client, &msg, NULL, NULL);
  if (r != AZ_IOT_OK)
  {
    client->csr_op.in_use = false;
    return r;
  }
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result az_iot_connection_client_cancel_csr(az_iot_connection_client* client)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!client->csr_op.in_use)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  /* App-initiated abandon: drop the in-flight operation so a new send_csr()
   * can proceed. No callback fires (the caller already knows). A late hub
   * response for this rid is ignored (in_use is clear). */
  client->csr_op.in_use = false;
  return AZ_IOT_OK;
}

const char* az_iot_connection_state_to_string(az_iot_connection_state s)
{
  switch (s)
  {
    case AZ_IOT_CONN_STATE_IDLE:
      return "AZ_IOT_CONN_STATE_IDLE";
    case AZ_IOT_CONN_STATE_CONNECTING:
      return "AZ_IOT_CONN_STATE_CONNECTING";
    case AZ_IOT_CONN_STATE_CONNECTED:
      return "AZ_IOT_CONN_STATE_CONNECTED";
    case AZ_IOT_CONN_STATE_RECONNECTING:
      return "AZ_IOT_CONN_STATE_RECONNECTING";
    case AZ_IOT_CONN_STATE_DISCONNECTING:
      return "AZ_IOT_CONN_STATE_DISCONNECTING";
    case AZ_IOT_CONN_STATE_FAULTED:
      return "AZ_IOT_CONN_STATE_FAULTED";
    default:
      return "UNKNOWN";
  }
}

AZ_NODISCARD az_iot_result az_iot_connection_client_complete_sas_token(
    az_iot_connection_client* client,
    uint32_t request_id,
    const char* token,
    const az_iot_sas_token_response* response)
{
  (void)client;
  (void)request_id;
  (void)token;
  (void)response;
  /* user_provided_token is rejected by init() until it is implemented, so no
   * request can be pending. */
  return AZ_IOT_ERR_NOT_FOUND;
}
