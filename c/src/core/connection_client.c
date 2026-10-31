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
 *      with RETRY_PENDING and FAULTED side branches)
 *   - DPS provisioning (when opts.dps.id_scope is set and opts.iothub_hostname is NULL,
 *     open() internally provisions via DPS before connecting to the hub; with
 *     opts.iothub_hostname set too, open() connects to that saved hub and DPS is the
 *     fallback)
 *
 * Single-threaded contract: every state transition and every user state-callback
 * fires from inside az_iot_connection_client_do_work(). The MQTT adapter's
 * inbound callback is also delivered from process_loop() (which we drive from
 * do_work()); the adapter is responsible for marshalling its own background
 * thread events into that callback.
 *
 * Reconnect (Phase 2.2): when opts.reconnection_policy is enabled
 * (initial_delay_ms > 0), unexpected drops (CONNACK fail, peer DISCONNECT,
 * inbound ERROR) transition to RETRY_PENDING; do_work() then re-opens after the
 * computed backoff (with jitter). User-initiated close() always goes to IDLE
 * regardless. If max_attempts > 0 is configured and reached, we transition to
 * FAULTED. A hub that refuses the identity is retried on the separate
 * opts.identity_recovery ladder instead.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_version.h"

#include "internal/cert_util.h"
#include "internal/connection_client_internal.h"
#include "internal/crypto.h"
#include "internal/deadline.h"
#include "internal/defensive_timeout.h"
#include "internal/dispatch.h"
#include "internal/log_internal.h"
#include "internal/proto3.h"
#include "internal/mono_time.h"
#include "internal/retry_policy.h"
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
#define DEFER_FALLBACK AZ_IOT_CONN_DEFER_FALLBACK
#define DEFER_PLANNED_END AZ_IOT_CONN_DEFER_PLANNED_END

/** @brief How long a SAS renewal waits for its disconnect to complete before
 * reconnecting anyway. */
#define PLANNED_DISCONNECT_TIMEOUT_MS 5000u

/** @brief When to ask again for a renewal token that could not be asked for. */
#define SAS_TOKEN_RENEWAL_RETRY_MS 30000u

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

/** @brief mqttv3 hub CONNECT api-version. Hub certificate management
 * ($iothub/credentials) requires 2025-08-01-preview; the pinned azure-sdk-for-c
 * username helper emits 2020-09-30, so the username is built here. */
#define HUB_API_VERSION "2025-08-01-preview"
/** @brief DeviceClientType value, unchanged from the azure-sdk-for-c default. */
#define HUB_DEVICE_CLIENT_TYPE "azsdk-c%2F" AZ_SDK_VERSION_STRING

/* The pinned azure-sdk-for-c helper emits 2019-03-31. Build the CONNECT
 * username here for every DPS session, including CSR and provision-only software updates. */
#define DPS_API_VERSION "2026-11-02-preview"
#define DPS_USERNAME_INFIX "/registrations/"
#define DPS_USERNAME_SUFFIX "/api-version=" DPS_API_VERSION
#define DPS_DEFAULT_GLOBAL_ENDPOINT "global.azure-devices-provisioning.net"
/* SAS key name (`skn`) DPS requires in device registration tokens. */
#define DPS_SAS_KEY_NAME "registration"

/* DPS ASSIGNED result fields that carry the issued operational chain. */
#define DPS_JSON_REGISTRATION_STATE "registrationState"
#define DPS_JSON_ISSUED_CERT_CHAIN "issuedCertificateChain"
/* The hub generation the device was assigned to. A string, and an extensible
 * union: absent or null means "classic". New in api-version 2026-11-02-preview. */
#define DPS_JSON_CONNECTION_PROFILE "connectionProfile"
#define CONNECTION_PROFILE_MQTT_V3_STR "classic"
#define CONNECTION_PROFILE_MQTT_V5_STR "mqttV5"

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
 * this can nest: an observer can start a transition (a feature client starting
 * its session), which dispatches again. Clearing on the way out of the inner
 * dispatch would drop the guard while the outer one is still walking its
 * arrays, and a later observer in the outer pass could then mutate them. */
/** @brief Marks the start of a call into application or feature-client code; open() and
 * close() return AZ_IOT_ERR_BUSY until the matching leave_callback(). */
static void enter_callback(az_iot_connection_client* c) { c->callback_depth++; }

/** @brief Marks the end of the call enter_callback() started. */
static void leave_callback(az_iot_connection_client* c) { c->callback_depth--; }

/**
 * @brief Call every feature observer of @p event's kind.
 *
 * Each entry is copied before its call: a callback may withdraw itself or another observer, and
 * nothing is compacted.
 */
static void notify_feature_observers(
    az_iot_connection_client* c,
    const az_iot_connection_feature_event* event)
{
  for (size_t i = 0; i < c->opts.limits.max_feature_observers; ++i)
  {
    az_iot_connection_feature_observer const observer = c->feature_observers[i];
    if (observer.cb != NULL && (observer.events & (uint8_t)event->kind) != 0u)
    {
      enter_callback(c);
      observer.cb(event, observer.user_ctx);
      leave_callback(c);
    }
  }
}

/** @brief notify_feature_observers() for an event that carries no data. */
static void notify_feature_observers_of(
    az_iot_connection_client* c,
    az_iot_connection_feature_event_kind kind)
{
  az_iot_connection_feature_event const event
      = { kind, NULL, AZ_IOT_CONNECTION_SESSION_END_LOST, NULL };
  notify_feature_observers(c, &event);
}

static void dispatch_state_event(
    az_iot_connection_client* c,
    const az_iot_connection_state_event* event)
{
  bool was_dispatching = c->dispatching_state;
  c->dispatching_state = true;
  az_iot_connection_feature_event const feature_event
      = { AZ_IOT_CONNECTION_FEATURE_EVENT_STATE, event, AZ_IOT_CONNECTION_SESSION_END_LOST, NULL };
  notify_feature_observers(c, &feature_event);
  for (size_t i = 0; i < c->opts.limits.max_application_state_observers; ++i)
  {
    if (c->app_state_observers[i].cb)
    {
      enter_callback(c);
      c->app_state_observers[i].cb(event, c->app_state_observers[i].user_ctx);
      leave_callback(c);
    }
  }
  c->dispatching_state = was_dispatching;
}

static bool have_state_observers(const az_iot_connection_client* c)
{
  for (size_t i = 0; i < c->opts.limits.max_feature_observers; ++i)
  {
    if (c->feature_observers[i].cb
        && (c->feature_observers[i].events & (uint8_t)AZ_IOT_CONNECTION_FEATURE_EVENT_STATE))
    {
      return true;
    }
  }
  for (size_t i = 0; i < c->opts.limits.max_application_state_observers; ++i)
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

/**
 * @brief Stage AZ_IOT_CONN_ERR_SRC_LOCAL detail for a step of the current
 * attempt on @p scope that failed on the device.
 *
 * Detail already staged for @p scope is kept: SETTING_UP cleared the previous
 * attempt's, so it came from an adapter event in this one, which is more
 * specific.
 *
 * @param step Static text naming the step; copied.
 */
static void stage_local_error(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_result result,
    const char* step)
{
  if (c->error_scope == scope && c->error_source != AZ_IOT_CONN_ERR_SRC_NONE)
  {
    return;
  }
  stage_error(
      c,
      scope,
      AZ_IOT_CONN_ERR_SRC_LOCAL,
      (int32_t)result,
      az_span_create_from_str((char*)(uintptr_t)step));
}

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
    /* The broker refused the filter, or the publish, itself. */
    case AZ_IOT_ERR_SUBSCRIPTION_REFUSED:
    case AZ_IOT_ERR_PUBLISH_REFUSED:
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

/** @brief Log name of @p scope. */
static const char* scope_name(az_iot_connection_scope scope)
{
  return scope == AZ_IOT_CONN_SCOPE_DPS ? "dps" : "hub";
}

/** @brief Log name of @p profile. */
static const char* profile_name(az_iot_connection_profile profile)
{
  switch (profile)
  {
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V3:
      return "mqttv3";
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V5:
      return "mqttv5";
    case AZ_IOT_CONNECTION_PROFILE_UNKNOWN:
    default:
      return "unknown";
  }
}

/** @brief @p s, or "(none)" when NULL or empty. */
static const char* text_or_none(const char* s)
{
  return (s != NULL && s[0] != '\0') ? s : "(none)";
}

/** @brief Log name of @p source. */
static const char* error_source_name(az_iot_connection_error_source source)
{
  switch (source)
  {
    case AZ_IOT_CONN_ERR_SRC_TRANSPORT:
      return "transport";
    case AZ_IOT_CONN_ERR_SRC_MQTT:
      return "mqtt";
    case AZ_IOT_CONN_ERR_SRC_DPS:
      return "dps";
    case AZ_IOT_CONN_ERR_SRC_LOCAL:
      return "local";
    case AZ_IOT_CONN_ERR_SRC_NONE:
    default:
      return "none";
  }
}

/** @brief One INFO line per state change; WARN when it reports a failure. */
static void log_state_transition(
    const az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_connection_state next,
    az_iot_result reason)
{
  az_iot_log_level level = reason == AZ_IOT_OK ? AZ_IOT_LOG_LEVEL_INFO : AZ_IOT_LOG_LEVEL_WARN;
  if (!az_iot_log_is_enabled(level))
  {
    return;
  }
  if (reason == AZ_IOT_OK)
  {
    az_iot_log_emitf(
        level,
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        __FILE__,
        __LINE__,
        "%s state=%s",
        scope_name(scope),
        az_iot_connection_state_to_string(next));
    return;
  }
  if (c->error_source != AZ_IOT_CONN_ERR_SRC_NONE && c->error_scope == scope)
  {
    az_iot_log_emitf(
        level,
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        __FILE__,
        __LINE__,
        "%s state=%s reason=%s source=%s code=%ld%s%.*s",
        scope_name(scope),
        az_iot_connection_state_to_string(next),
        az_iot_result_to_string(reason),
        error_source_name(c->error_source),
        (long)c->error_code,
        c->error_message_len > 0 ? " message=" : "",
        (int)c->error_message_len,
        c->error_message);
    return;
  }
  az_iot_log_emitf(
      level,
      AZ_IOT_LOG_COMPONENT_CONNECTION,
      __FILE__,
      __LINE__,
      "%s state=%s reason=%s",
      scope_name(scope),
      az_iot_connection_state_to_string(next),
      az_iot_result_to_string(reason));
}

/** @brief Whether @p reason is the service refusing the presented identity. */
static bool reason_is_identity_refusal(az_iot_result reason)
{
  return reason == AZ_IOT_ERR_IDENTITY_REJECTED || reason == AZ_IOT_ERR_AUTH;
}

static az_iot_connection_failure_class classify_failure(
    az_iot_connection_scope scope,
    az_iot_result reason)
{
  if (reason == AZ_IOT_OK)
  {
    return AZ_IOT_CONN_FAILURE_NONE;
  }
  /* Only a hub refusal is recovered on the identity ladder. */
  if (scope == AZ_IOT_CONN_SCOPE_HUB && reason_is_identity_refusal(reason))
  {
    return AZ_IOT_CONN_FAILURE_IDENTITY;
  }
  return reason_is_retriable(reason) ? AZ_IOT_CONN_FAILURE_TRANSIENT : AZ_IOT_CONN_FAILURE_TERMINAL;
}

static const char* dps_endpoint(const az_iot_connection_client* c)
{
  return is_nonempty_cstr(c->opts.dps.global_endpoint) ? c->opts.dps.global_endpoint
                                                       : DPS_DEFAULT_GLOBAL_ENDPOINT;
}

/** @brief Restart the identity recovery ladder and its duration clock. */
static void reset_identity_recovery(az_iot_connection_client* c)
{
  c->identity_retry_attempt = 0;
  c->identity_recovery_started_ms = 0;
  c->identity_recovery_active = false;
}

static void clear_sas_token_request(az_iot_connection_client* c, az_iot_connection_scope scope);

/**
 * @brief Whether the transition of @p scope to @p new_state with @p reason is
 * part of a SAS token renewal in progress, without a failure: the hub's
 * RETRY_PENDING through CONNECTED, or the provisioning session's DISCONNECTING
 * through CONNECTED.
 */
static bool is_sas_token_renewal_transition(
    const az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_connection_state new_state,
    az_iot_result reason)
{
  az_iot_connection_state first = scope == AZ_IOT_CONN_SCOPE_HUB ? AZ_IOT_CONN_STATE_RETRY_PENDING
                                                                 : AZ_IOT_CONN_STATE_DISCONNECTING;
  return c->sas_token_renewal_in_progress[scope] && reason == AZ_IOT_OK
      && (new_state == first || new_state == AZ_IOT_CONN_STATE_SETTING_UP
          || new_state == AZ_IOT_CONN_STATE_CONNECTING || new_state == AZ_IOT_CONN_STATE_CONNECTED);
}

static void set_state_to(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_connection_state new_state,
    az_iot_result reason)
{
  /* One failure produces a SEQUENCE of transitions -- a dying session reports
   * DISCONNECTING, then IDLE, then RETRY_PENDING or FAULTED -- and they are all
   * reporting the same failure, so the detail rides all of them rather than
   * being consumed by whichever ran first. (It was: the terminal event, the one
   * an application acts on, arrived with nothing.)
   *
   * It is discarded when the scope starts a NEW attempt or succeeds, which is
   * the point at which the old evidence stops describing anything current.
   * Except a DPS:CONNECTED announced after its registration already failed: the
   * SUBACK path registers before the deferred announcement, and the evidence is
   * for the settle that follows. */
  bool dps_failure_pending = scope == AZ_IOT_CONN_SCOPE_DPS && c->dps_pending_finalize
      && c->dps_pending_status != AZ_IOT_OK;
  if (scope == c->error_scope
      && (new_state == AZ_IOT_CONN_STATE_SETTING_UP || new_state == AZ_IOT_CONN_STATE_CONNECTING
          || (new_state == AZ_IOT_CONN_STATE_CONNECTED && !dps_failure_pending)))
  {
    clear_staged_error(c);
  }

  /* Retry progress staged for this event is consumed here even when the
   * transition is suppressed, so it cannot attach to a later one. */
  az_iot_connection_recovery_info recovery = {
    .classification = AZ_IOT_CONN_FAILURE_NONE,
    .endpoint = NULL,
    .attempt = 0,
    .next_attempt_delay_ms = 0,
    .next_attempt_reprovisions = false,
  };
  bool reports_recovery = false;
  if (new_state == AZ_IOT_CONN_STATE_RETRY_PENDING || new_state == AZ_IOT_CONN_STATE_FAULTED)
  {
    reports_recovery = (reason != AZ_IOT_OK);
    if (reports_recovery && c->recovery_report.staged)
    {
      recovery.classification = c->recovery_report.classification;
      recovery.attempt = c->recovery_report.attempt;
      recovery.next_attempt_delay_ms = c->recovery_report.delay_ms;
      recovery.next_attempt_reprovisions = c->recovery_report.reprovisions;
    }
    c->recovery_report.staged = false;
  }

  /* Per scope, deliberately. A single comparison here would swallow
   * HUB:CONNECTING straight after DPS:CONNECTING purely because the VALUE
   * matched -- which is exactly why a DPS+hub run used to be reported as one
   * CONNECTING and one CONNECTED, with the whole provisioning phase invisible. */
  if (c->state[scope] == new_state)
  {
    return;
  }
  c->state[scope] = new_state;
  log_state_transition(c, scope, new_state, reason);
  bool is_token_renewal = is_sas_token_renewal_transition(c, scope, new_state, reason);
  /* CONNECTED completes a renewal; any other transition of the scope ends it. */
  if (!is_token_renewal || new_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    c->sas_token_renewal_in_progress[scope] = false;
  }
  /* A planned end lasts while its session is up or being closed; any other
   * transition ends it. */
  if (scope == AZ_IOT_CONN_SCOPE_HUB && new_state != AZ_IOT_CONN_STATE_CONNECTING
      && new_state != AZ_IOT_CONN_STATE_CONNECTED && new_state != AZ_IOT_CONN_STATE_DISCONNECTING)
  {
    az_iot_deadline__clear(&c->planned_hub_session_end.disconnect_deadline);
  }
  /* Bookkeeping that belongs to the transition itself, not to any observer, so
   * it runs whether or not anyone is watching. A settled scope has no attempt
   * left to take a user-provided token. */
  if (new_state == AZ_IOT_CONN_STATE_IDLE || new_state == AZ_IOT_CONN_STATE_FAULTED)
  {
    clear_sas_token_request(c, scope);
  }
  if (scope == AZ_IOT_CONN_SCOPE_HUB && new_state == AZ_IOT_CONN_STATE_CONNECTED)
  {
    c->consecutive_hub_connect_failures = 0;
    /* The identity was accepted end to end (birth-ack included on mqttv5). */
    reset_identity_recovery(c);
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
    .state = new_state,
    .reason = reason,
    .profile = NULL,
    .is_retriable = (reason != AZ_IOT_OK) && reason_is_retriable(reason),
    .error = NULL,
    .recovery = NULL,
    .is_credential_renewal = is_token_renewal,
    .auth_source = c->auth[scope].source,
    .x509_index = c->auth[scope].x509_index,
  };
  if (reports_recovery)
  {
    if (recovery.classification == AZ_IOT_CONN_FAILURE_NONE)
    {
      recovery.classification = classify_failure(scope, reason);
    }
    recovery.endpoint
        = (scope == AZ_IOT_CONN_SCOPE_DPS) ? dps_endpoint(c) : c->opts.iothub_hostname;
    event.recovery = &recovery;
  }
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
  if ((new_state == AZ_IOT_CONN_STATE_CONNECTED && scope == AZ_IOT_CONN_SCOPE_HUB)
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
  /* Callers have torn the provisioning session down, so one that exists now
   * was started from a callback by a feature client: not IDLE. */
  if (c->dps_mqtt == NULL)
  {
    set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, AZ_IOT_OK);
  }
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

/* Apply the caller's transport, WebSocket path and proxy to one connect for
 * @p role, and resolve the port: an explicit @p port wins, 0 means "derive from
 * transport".
 *
 * Shared by the DPS bootstrap connect and the hub connect on purpose. A device
 * that needs a proxy or WebSockets to reach the hub needs them to reach DPS
 * too, so applying this to only one of the two connects would leave the device
 * unable to provision at all. */
static void resolve_connect_transport(
    const az_iot_connection_client* c,
    az_iot_mqtt_connect_options* copts,
    az_iot_mqtt_role role,
    uint16_t port)
{
  copts->transport = c->opts.transport;
  copts->websocket_path = c->opts.websocket_path;
  /* The mqttv5 hub serves WebSockets on its own path; DPS and the mqttv3 hub keep
   * the adapter default. */
  if (copts->websocket_path == NULL && role == AZ_IOT_MQTT_ROLE_HUB_MQTT_V5)
  {
    copts->websocket_path = AZ_IOT_MQTT_HUB_V5_WEBSOCKET_PATH;
  }
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

/**
 * @brief Complete a tracked publish through the callback it registered.
 *
 * @param ack The PUBLISH_ACK event, for its reason code and string; NULL when none arrived.
 */
static void complete_publish(
    az_iot_connection_client* c,
    az_iot_publish_ack_callback cb,
    az_iot_publish_result_callback result_cb,
    void* user_ctx,
    az_iot_result status,
    const az_iot_mqtt_event* ack)
{
  if (result_cb)
  {
    az_iot_publish_result result = { 0 };
    result.status = status;
    if (ack)
    {
      result.reason_code = ack->protocol_code;
      result.reason_string = ack->reason_string;
      result.reason_string_len = ack->reason_string != NULL ? ack->reason_string_len : 0;
    }
    enter_callback(c);
    result_cb(&result, user_ctx);
    leave_callback(c);
  }
  else if (cb)
  {
    enter_callback(c);
    cb(status, user_ctx);
    leave_callback(c);
  }
}

/** @brief Destroys the hub session, completes its pending PUBACKs and tells feature observers
 * it ended for @p reason. */
static void end_session(az_iot_connection_client* c, az_iot_connection_session_end_reason reason)
{
  if (c->active_client && c->active_client->iface && c->active_client->iface->destroy)
  {
    c->active_client->iface->destroy(c->active_client);
  }
  c->active_client = NULL;
  /* A birth-ack wait torn down at or past its deadline is a miss, whatever
   * ended the session (timeout, DISCONNECTED or ERROR processed first). */
  if (c->presence.phase == PRESENCE_PHASE_BIRTH
      && az_iot_deadline__has_passed(&c->presence.deadline, az_iot_time_mono_ms()))
  {
    az_iot_defensive_timeout__count_miss(&c->presence.consecutive_birth_ack_timeouts);
  }
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
  for (size_t i = 0; i < c->opts.limits.max_pending_publishes; ++i)
  {
    az_iot_publish_ack_callback cb = c->pending_pubacks[i].cb;
    az_iot_publish_result_callback result_cb = c->pending_pubacks[i].result_cb;
    void* user_ctx = c->pending_pubacks[i].user_ctx;
    bool was_awaiting_puback = c->pending_pubacks[i].in_use;

    c->pending_pubacks[i].in_use = false;
    c->pending_pubacks[i].cb = NULL;
    c->pending_pubacks[i].result_cb = NULL;
    c->pending_pubacks[i].user_ctx = NULL;
    c->pending_pubacks[i].packet_id = 0;

    if (was_awaiting_puback)
    {
      complete_publish(c, cb, result_cb, user_ctx, AZ_IOT_ERR_NOT_CONNECTED, NULL);
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
  az_iot_connection_feature_event const event
      = { AZ_IOT_CONNECTION_FEATURE_EVENT_SESSION_END, NULL, reason, NULL };
  notify_feature_observers(c, &event);
}

/** @brief end_session(): planned after close(), lost otherwise. */
static void teardown_active(az_iot_connection_client* c)
{
  end_session(
      c,
      c->user_close ? AZ_IOT_CONNECTION_SESSION_END_PLANNED : AZ_IOT_CONNECTION_SESSION_END_LOST);
}

static void puback_abandon(az_iot_connection_client* c, const void* owner);

/* Forward decl — used in on_mqtt_event via the deferred-action queue. */
static az_iot_result start_connect_attempt(az_iot_connection_client* c);
static bool planned_hub_session_end_in_progress(const az_iot_connection_client* c);
static bool dps_configured(const az_iot_connection_client* c);
static az_iot_result run_feature_client_binds(az_iot_connection_client* c);
static void drop_subscriptions_from_other_generations(az_iot_connection_client* c);
static void csr_event_init(az_iot_csr_event* evt, az_iot_csr_event_kind kind);
static void report_csr_event(
    az_iot_connection_client* c,
    az_iot_csr_callback cb,
    const az_iot_csr_event* evt,
    void* user_ctx);

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
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "TLS asked the certificate provider to sign, but the provider is unusable "
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

/**
 * @brief True when @p p makes CSRs and stores the issued chains: the only
 * source of CSRs for DPS enrollment and hub renewal.
 */
static bool provider_owns_csr(const az_iot_certificate_provider* p)
{
  return p != NULL && p->vtable != NULL && p->vtable->version >= CERT_PROVIDER_VTABLE_V2
      && p->vtable->get_csr != NULL && p->vtable->release_csr != NULL
      && p->vtable->store_issued_certificate != NULL;
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
      || mat->client_key_uri != NULL || mat->client_key_psa_id != 0 || has_sign;
  if (!has_key)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "certificate material carries a client certificate but no private "
        "key (no PEM, no file, no key URI, no PSA key, no sign() hook)");
    return AZ_IOT_ERR_CREDENTIAL_INCOMPLETE;
  }
  /* A key URI without an engine/provider id names a key nothing can resolve:
   * the URI scheme alone does not say which ENGINE or OpenSSL provider owns
   * it. Adapters would have to guess, so it is rejected here instead. */
  if (mat->client_key_uri != NULL && mat->crypto_engine_id == NULL && !has_sign)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "certificate material sets client_key_uri without "
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
  copts->tls.client_key_psa_id = mat->client_key_psa_id;
  copts->tls.sign = has_sign ? provider_sign_adapter : NULL;
  copts->tls.sign_ctx = has_sign ? (void*)prov : NULL;
  copts->tls.use_tls = true; /* this SDK always connects over TLS */
  return AZ_IOT_OK;
}

/**
 * @brief For a role using SAS: keeps the provider's trust anchors from
 * @p mat (its certificate, if any, is not used). opts.trusted_ca, applied
 * later, still overrides them.
 */
static void keep_provider_trust(
    az_iot_mqtt_connect_options* copts,
    const az_iot_certificate_material* mat)
{
  copts->tls.trusted_ca_pem = mat->trusted_ca_pem;
  copts->tls.trusted_ca_path = mat->trusted_ca_path;
}

static void release_material(az_iot_connection_client* c, az_iot_connection_scope scope);

/**
 * @brief Holds @p mat, from a successful load(), for @p scope's attempt until
 * release_material(). The adapter's TLS options point into it.
 */
static void hold_material(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    const az_iot_certificate_material* mat)
{
  release_material(c, scope);
  c->held_material[scope] = *mat;
  c->material_held[scope] = true;
}

/**
 * @brief Releases the material @p scope holds, if any. Called once the
 * adapter's connect() has copied it, when the attempt ends first, and from
 * close() and deinit().
 */
static void release_material(az_iot_connection_client* c, az_iot_connection_scope scope)
{
  az_iot_certificate_provider* p = c->opts.certificate_provider;
  if (c->material_held[scope])
  {
    c->material_held[scope] = false;
    if (p != NULL)
    {
      p->vtable->release(p, &c->held_material[scope]);
    }
    memset(&c->held_material[scope], 0, sizeof(c->held_material[scope]));
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
  return az_iot_time_unix_s(c->opts.unix_time.get_time, c->opts.unix_time.user_ctx);
}

/** @brief Bytes of sas_buffer before the key slots: HMAC, then its base64. */
#define SAS_SCRATCH_MAC_SIZE AZ_IOT_SHA256_SIZE
#define SAS_SCRATCH_SIG_SIZE 48u /* base64 of AZ_IOT_SHA256_SIZE bytes: 44 */

/** @brief Wipes the token once the adapter has the CONNECT. */
static void sas_wipe_token(az_iot_connection_client* c)
{
  /* A delivered user-provided token waits for its attempt. */
  if (c->sas_token != NULL && c->sas_token_holder == 0)
  {
    c->sas_token_in_use = 0;
    az_iot_crypto__wipe(c->sas_token, c->sas_token_size);
  }
}

/**
 * @brief Whether @p s can become an az_span: non-empty and at most INT32_MAX
 * bytes. az_core checks the length with a precondition, whose default handler
 * never returns, so strings are checked here first.
 */
static bool is_span_safe_cstr(const char* s)
{
  return is_nonempty_cstr(s) && strlen(s) <= (size_t)INT32_MAX;
}

/**
 * @brief (Re)initializes c->hub_client from the current opts.iothub_hostname /
 * opts.device_id. Not cached: DPS (re)provisioning replaces both, and the
 * client holds spans of their old lengths. Cheap: it only stores spans.
 */
static bool ensure_hub_client(az_iot_connection_client* c)
{
  c->hub_client_initialized = false;
  if (is_span_safe_cstr(c->opts.iothub_hostname) && is_span_safe_cstr(c->opts.device_id)
      && (c->opts.model_id == NULL || strlen(c->opts.model_id) <= (size_t)INT32_MAX))
  {
    az_span host_span = az_span_create_from_str((char*)(uintptr_t)c->opts.iothub_hostname);
    az_span id_span = az_span_create_from_str((char*)(uintptr_t)c->opts.device_id);
    az_iot_hub_client_options hub_opts = az_iot_hub_client_options_default();
    if (is_nonempty_cstr(c->opts.model_id))
    {
      hub_opts.model_id = az_span_create_from_str((char*)(uintptr_t)c->opts.model_id);
    }
    c->hub_client_initialized = az_result_succeeded(
        az_iot_hub_client_init(&c->hub_client, host_span, id_span, &hub_opts));
  }
  return c->hub_client_initialized;
}

/**
 * @brief Builds the mqttv3 hub CONNECT username
 * "{host}/{client_id}/?api-version=HUB_API_VERSION&DeviceClientType=...[&model-id=...]",
 * the azure-sdk-for-c layout with HUB_API_VERSION.
 * @return AZ_IOT_ERR_NOT_ENOUGH_SPACE if it does not fit @p buf.
 */
static az_iot_result hub_build_username(const az_iot_connection_client* c, char* buf, size_t cap)
{
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, az_span_create((uint8_t*)buf, (int32_t)cap));
  az_iot_span_writer_append_str(&writer, c->opts.iothub_hostname);
  az_iot_span_writer_append_str(&writer, "/");
  az_iot_span_writer_append_str(&writer, c->opts.device_id);
  az_iot_span_writer_append_str(
      &writer, "/?api-version=" HUB_API_VERSION "&DeviceClientType=" HUB_DEVICE_CLIENT_TYPE);
  if (is_nonempty_cstr(c->opts.model_id))
  {
    az_iot_span_writer_append_str(&writer, "&model-id=");
    az_iot_span_writer_append_url_encoded(&writer, c->opts.model_id);
  }
  return az_iot_span_writer_end_str(&writer, NULL) == AZ_IOT_OK ? AZ_IOT_OK
                                                                : AZ_IOT_ERR_NOT_ENOUGH_SPACE;
}

/**
 * @brief Whether the largest token for @p scope fits the token area,
 * every ID byte URL-encoded to 3. azure-sdk-for-c's SAS helpers check sizes
 * with preconditions, whose default handler never returns, so oversize input
 * must be refused before they run.
 */
static bool sas_token_fits(const az_iot_connection_client* c, az_iot_connection_scope scope)
{
  bool is_dps = scope == AZ_IOT_CONN_SCOPE_DPS;
  const char* first = is_dps ? c->opts.dps.id_scope : c->opts.iothub_hostname;
  const char* second = is_dps ? c->opts.dps.registration_id : c->opts.device_id;
  if (first == NULL || second == NULL)
  {
    return false;
  }
  size_t ids = strlen(first) + strlen(second);
  if (ids > c->sas_token_size)
  {
    return false;
  }
  return AZ_IOT_SAS_TOKEN_SIZE(ids) <= c->sas_token_size;
}

static void claim_sas_token_area(az_iot_connection_client* c, az_iot_connection_scope scope);

/**
 * @brief Milliseconds from signing to renewing a token valid for
 * @p lifetime_seconds: az_iot_auth::sas::renewal_percent of it.
 */
static uint64_t get_sas_token_renewal_delay_ms(const az_iot_auth* auth, uint32_t lifetime_seconds)
{
  uint32_t percent = auth->sas.renewal_percent != 0u ? auth->sas.renewal_percent
                                                     : (uint32_t)AZ_IOT_DEFAULT_SAS_RENEWAL_PERCENT;
  /* At most UINT32_MAX * 99 * 10: no overflow. */
  return (uint64_t)lifetime_seconds * percent * 10u;
}

/**
 * @brief Signs a SAS token with @p scope's @p key (primary or secondary) and sets it as the
 * CONNECT password, over server-authenticated TLS. The token format comes
 * from azure-sdk-for-c (c->dps_prov / c->hub_client); every buffer is in
 * opts.sas_buffer.
 *
 * @return AZ_IOT_OK; AZ_IOT_ERR_BUSY while no Unix time is known;
 * AZ_IOT_ERR_NOT_ENOUGH_SPACE when the token exceeds the token area;
 * AZ_IOT_ERR_INVALID_ARG when the hub host or client ID is missing; the
 * crypto backend's error otherwise.
 */
static az_iot_result apply_sas_key(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_auth_source key,
    az_iot_mqtt_connect_options* copts)
{
  bool secondary = key == AZ_IOT_AUTH_SOURCE_SECONDARY_KEY;
  bool is_dps = scope == AZ_IOT_CONN_SCOPE_DPS;
  const az_iot_auth* auth = is_dps ? &c->opts.dps_auth : &c->opts.hub_auth;
  /* The other role's CONNECT has not taken its token yet. */
  if (c->sas_token_in_use != 0 && c->sas_token_in_use != (uint8_t)(scope + 1))
  {
    return AZ_IOT_ERR_BUSY;
  }
  claim_sas_token_area(c, scope);
  uint64_t now = unix_now(c);
  if (now == 0)
  {
    AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_CONNECTION, "no Unix time yet; cannot sign a SAS token");
    return AZ_IOT_ERR_BUSY;
  }
  if (!is_dps && !ensure_hub_client(c))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!sas_token_fits(c, scope))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION, "IDs too long for the SAS token area of opts.sas_buffer");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  uint32_t lifetime = auth->sas.token_lifetime_seconds != 0
      ? auth->sas.token_lifetime_seconds
      : (uint32_t)AZ_IOT_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS;
  if (now > UINT64_MAX - lifetime)
  {
    /* The expiry would wrap to a time long past: treat as no valid time. */
    AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_CONNECTION, "Unix time too large to sign a SAS token");
    return AZ_IOT_ERR_BUSY;
  }
  uint64_t expiry = now + lifetime;
  char* token = c->sas_token;
  az_span token_span = az_span_create((uint8_t*)token, (int32_t)c->sas_token_size);
  uint8_t* mac = c->opts.sas_buffer.buffer;
  uint8_t* signature = mac + SAS_SCRATCH_MAC_SIZE;

  /* `<resource URI>\n<expiry>` is built in the token buffer, then replaced by
   * the token. */
  az_span to_sign = AZ_SPAN_EMPTY;
  az_result ar = is_dps
      ? az_iot_provisioning_client_sas_get_signature(&c->dps_prov, expiry, token_span, &to_sign)
      : az_iot_hub_client_sas_get_signature(&c->hub_client, expiry, token_span, &to_sign);
  az_iot_result r = az_result_succeeded(ar) ? AZ_IOT_OK : AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  if (r == AZ_IOT_OK)
  {
    r = az_iot_crypto__hmac_sha256(
        c->opts.crypto,
        secondary ? c->auth[scope].secondary_key : c->auth[scope].primary_key,
        secondary ? c->auth[scope].secondary_key_len : c->auth[scope].primary_key_len,
        az_span_ptr(to_sign),
        (size_t)az_span_size(to_sign),
        mac);
  }
  int32_t sig_len = 0;
  if (r == AZ_IOT_OK
      && az_result_failed(az_base64_encode(
          az_span_create(signature, (int32_t)SAS_SCRATCH_SIG_SIZE),
          az_span_create(mac, (int32_t)SAS_SCRATCH_MAC_SIZE),
          &sig_len)))
  {
    r = AZ_IOT_ERR_INTERNAL;
  }
  if (r == AZ_IOT_OK)
  {
    az_span sig = az_span_create(signature, sig_len);
    ar = is_dps ? az_iot_provisioning_client_sas_get_password(
                      &c->dps_prov,
                      sig,
                      expiry,
                      AZ_SPAN_FROM_STR(DPS_SAS_KEY_NAME),
                      token,
                      c->sas_token_size,
                      NULL)
                : az_iot_hub_client_sas_get_password(
                      &c->hub_client, expiry, sig, AZ_SPAN_EMPTY, token, c->sas_token_size, NULL);
    r = az_result_succeeded(ar) ? AZ_IOT_OK : AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  az_iot_crypto__wipe(mac, SAS_SCRATCH_MAC_SIZE + SAS_SCRATCH_SIG_SIZE);
  if (r != AZ_IOT_OK)
  {
    sas_wipe_token(c);
    AZ_IOT_LOG_ERRORF(AZ_IOT_LOG_COMPONENT_CONNECTION, "SAS token signing failed (%d)", (int)r);
    return r;
  }
  copts->password = token;
  c->sas_token_in_use = (uint8_t)(scope + 1);
  c->auth[scope].source
      = secondary ? AZ_IOT_AUTH_SOURCE_SECONDARY_KEY : AZ_IOT_AUTH_SOURCE_PRIMARY_KEY;
  /* Both clocks: the monotonic one may stop while the device is suspended. */
  uint64_t delay_ms = get_sas_token_renewal_delay_ms(auth, lifetime);
  c->sas_token_renewal_due_ms[scope] = az_iot_time_mono_ms() + delay_ms;
  c->sas_token_expiry_ms[scope] = 0;
  c->sas_token_expiry_unix_seconds[scope] = 0;
  /* Rounded up: a truncated deadline would renew at once, every time. */
  c->sas_token_renewal_due_unix_seconds[scope] = now + (delay_ms + 999u) / 1000u;
  apply_trusted_ca(c, copts);
  return AZ_IOT_OK;
}

az_iot_result az_iot_connection_client__sign_hub_sas_token(
    const az_iot_connection_client* client,
    char* out,
    size_t out_size)
{
  if (out == NULL || out_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Every failure leaves nothing of an earlier token in the caller's buffer. */
  az_iot_crypto__wipe(out, out_size);
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  const az_iot_connection_client* c = client;
  /* first: the source a fallback selected, kept while it connects; it
   * replaces a rejected source before the next attempt starts. */
  az_iot_auth_source source = c->auth[AZ_IOT_CONN_SCOPE_HUB].first;
  if (source == AZ_IOT_AUTH_SOURCE_NONE)
  {
    source = c->auth[AZ_IOT_CONN_SCOPE_HUB].source;
  }
  if (source == AZ_IOT_AUTH_SOURCE_NONE)
  {
    source = c->auth[AZ_IOT_CONN_SCOPE_HUB].last_source;
  }
  if (source == AZ_IOT_AUTH_SOURCE_NONE && c->opts.certificate_provider == NULL)
  {
    source = AZ_IOT_AUTH_SOURCE_PRIMARY_KEY;
  }
  const uint8_t* key = NULL;
  size_t key_len = 0;
  if (source == AZ_IOT_AUTH_SOURCE_PRIMARY_KEY)
  {
    key = c->auth[AZ_IOT_CONN_SCOPE_HUB].primary_key;
    key_len = c->auth[AZ_IOT_CONN_SCOPE_HUB].primary_key_len;
  }
  else if (source == AZ_IOT_AUTH_SOURCE_SECONDARY_KEY)
  {
    key = c->auth[AZ_IOT_CONN_SCOPE_HUB].secondary_key;
    key_len = c->auth[AZ_IOT_CONN_SCOPE_HUB].secondary_key_len;
  }
  if (key == NULL || key_len == 0)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  if (!is_span_safe_cstr(c->opts.iothub_hostname) || !is_span_safe_cstr(c->opts.device_id))
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  /* Checked before azure-sdk-for-c runs: its size preconditions never return. */
  size_t ids = strlen(c->opts.iothub_hostname) + strlen(c->opts.device_id);
  size_t size = out_size > (size_t)INT32_MAX ? (size_t)INT32_MAX : out_size;
  if (ids > size || AZ_IOT_SAS_TOKEN_SIZE(ids) > size)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  uint64_t now = unix_now(c);
  const az_iot_auth* auth = &c->opts.hub_auth;
  uint32_t lifetime = auth->sas.token_lifetime_seconds != 0
      ? auth->sas.token_lifetime_seconds
      : (uint32_t)AZ_IOT_DEFAULT_SAS_TOKEN_LIFETIME_SECONDS;
  if (now == 0 || now > UINT64_MAX - lifetime)
  {
    return AZ_IOT_ERR_BUSY;
  }
  uint64_t expiry = now + lifetime;

  /* A local hub client: the connection's is left as its CONNECT set it. */
  az_iot_hub_client hub;
  az_iot_hub_client_options hub_opts = az_iot_hub_client_options_default();
  if (az_result_failed(az_iot_hub_client_init(
          &hub,
          az_span_create_from_str((char*)(uintptr_t)c->opts.iothub_hostname),
          az_span_create_from_str((char*)(uintptr_t)c->opts.device_id),
          &hub_opts)))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  uint8_t mac[SAS_SCRATCH_MAC_SIZE];
  uint8_t signature[SAS_SCRATCH_SIG_SIZE];
  az_span to_sign = AZ_SPAN_EMPTY;
  az_result ar = az_iot_hub_client_sas_get_signature(
      &hub, expiry, az_span_create((uint8_t*)out, (int32_t)size), &to_sign);
  az_iot_result r = az_result_succeeded(ar) ? AZ_IOT_OK : AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  if (r == AZ_IOT_OK)
  {
    r = az_iot_crypto__hmac_sha256(
        c->opts.crypto, key, key_len, az_span_ptr(to_sign), (size_t)az_span_size(to_sign), mac);
    /* NOT_FOUND is reserved for "no key in use": callers then send no token. */
    if (r == AZ_IOT_ERR_NOT_FOUND)
    {
      r = AZ_IOT_ERR_INTERNAL;
    }
  }
  int32_t sig_len = 0;
  if (r == AZ_IOT_OK
      && az_result_failed(az_base64_encode(
          az_span_create(signature, (int32_t)sizeof(signature)),
          az_span_create(mac, (int32_t)sizeof(mac)),
          &sig_len)))
  {
    r = AZ_IOT_ERR_INTERNAL;
  }
  if (r == AZ_IOT_OK)
  {
    ar = az_iot_hub_client_sas_get_password(
        &hub, expiry, az_span_create(signature, sig_len), AZ_SPAN_EMPTY, out, size, NULL);
    r = az_result_succeeded(ar) ? AZ_IOT_OK : AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  az_iot_crypto__wipe(mac, sizeof(mac));
  az_iot_crypto__wipe(signature, sizeof(signature));
  if (r != AZ_IOT_OK)
  {
    az_iot_crypto__wipe(out, out_size);
    AZ_IOT_LOG_ERRORF(AZ_IOT_LOG_COMPONENT_CONNECTION, "hub SAS token signing failed (%d)", (int)r);
  }
  return r;
}

/* Schedule a retry.
 *
 * @p failure_scope says WHERE the failure just happened, which is not always
 * the scope of the next attempt: a hub failure can be retried as a DPS
 * registration. The two are used for different things -- the failure scope
 * feeds the hub-unreachable counter, the next attempt's scope selects the
 * backoff ladder and its attempt budget. A hub identity refusal climbs the
 * identity recovery ladder instead; see schedule_identity_recovery(). */
static void schedule_identity_recovery(az_iot_connection_client* c, az_iot_result reason);
static uint64_t identity_recovery_deadline_ms(const az_iot_connection_client* c);

/** @brief Give up the single pending retry: FAULTED on @p scope, and on the
 * other scope if it is still RETRY_PENDING, since nothing will retry it. Both
 * events carry the staged recovery report. */
static void fault_retry_scopes(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_result reason)
{
  az_iot_connection_scope other
      = (scope == AZ_IOT_CONN_SCOPE_HUB) ? AZ_IOT_CONN_SCOPE_DPS : AZ_IOT_CONN_SCOPE_HUB;
  bool staged = c->recovery_report.staged;
  c->reconnect_due_ms = 0;
  set_state_to(c, scope, AZ_IOT_CONN_STATE_FAULTED, reason);
  /* A callback may have closed the client, which settles both scopes. */
  if (c->state[other] == AZ_IOT_CONN_STATE_RETRY_PENDING)
  {
    c->recovery_report.staged = staged;
    set_state_to(c, other, AZ_IOT_CONN_STATE_FAULTED, reason);
  }
}

static void schedule_reconnect(
    az_iot_connection_client* c,
    az_iot_connection_scope failure_scope,
    az_iot_result reason)
{
  teardown_active(c);

  if (failure_scope == AZ_IOT_CONN_SCOPE_HUB && reason_is_identity_refusal(reason))
  {
    schedule_identity_recovery(c, reason);
    return;
  }

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
  /* A missing user-provided token says nothing about the hub. */
  if (failure_scope == AZ_IOT_CONN_SCOPE_HUB && !c->sas_token_attempt_failed)
  {
    c->consecutive_hub_connect_failures++;
  }
  if (!c->needs_reprovision && dps_configured(c) && !c->user_close
      && c->opts.dps.max_hub_connect_attempts_before_reprovision > 0
      && c->consecutive_hub_connect_failures
          >= c->opts.dps.max_hub_connect_attempts_before_reprovision)
  {
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "hub unreachable for the configured number of attempts; "
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
  /* A disabled policy faults: an immediate credential fallback can reach here
   * without one, and its zero delay would retry at once forever. */
  bool retry = az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy)
      && az_iot_retry_policy__next(
                   &c->opts.reconnection_policy, &c->retry_attempt[scope], &c->rng_state, &delay);
  uint64_t delay_ms = delay;
  c->recovery_report.classification = classify_failure(failure_scope, reason);
  c->recovery_report.attempt = c->retry_attempt[scope];
  /* An identity recovery episode bounds every retry until HUB:CONNECTED: one
   * that would start at or past the deadline is not scheduled, and the fault
   * reports the refusal that started the episode. */
  uint64_t now = az_iot_time_mono_ms();
  uint64_t deadline = identity_recovery_deadline_ms(c);
  if (retry && deadline != 0 && now + delay_ms >= deadline)
  {
    AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_CONNECTION, "identity recovery duration spent; stopping");
    retry = false;
    reason = c->identity_recovery_reason;
    c->recovery_report.classification = AZ_IOT_CONN_FAILURE_IDENTITY;
  }
  c->recovery_report.delay_ms = retry ? delay : 0u;
  c->recovery_report.reprovisions = retry && c->needs_reprovision;
  c->recovery_report.staged = true;
  if (!retry)
  {
    fault_retry_scopes(c, failure_scope, reason);
    return;
  }
  c->reconnect_due_ms = now + delay_ms;
  AZ_IOT_LOG_INFOF(
      AZ_IOT_LOG_COMPONENT_CONNECTION,
      "%s retry %u in %llu ms",
      scope_name(scope),
      (unsigned)c->retry_attempt[scope],
      (unsigned long long)delay_ms);
  /* Reported against the scope that FAILED, not the ladder the retry climbs:
   * a hub failure retried as a registration is still a HUB session going
   * down. The ladder scope is separate and lives in retry_attempt[] above. */
  set_state_to(c, failure_scope, AZ_IOT_CONN_STATE_RETRY_PENDING, reason);
}

/** @brief Retry schedule for identity recovery: its own policy, or
 * reconnection_policy when that is zeroed. */
static const az_iot_retry_policy* identity_recovery_policy(const az_iot_connection_client* c)
{
  return az_iot_retry_policy_is_enabled(&c->opts.identity_recovery.policy)
      ? &c->opts.identity_recovery.policy
      : &c->opts.reconnection_policy;
}

/** @brief End of the active episode's max_duration_seconds; 0 when unbounded
 * or no episode is active. */
static uint64_t identity_recovery_deadline_ms(const az_iot_connection_client* c)
{
  uint32_t s = c->opts.identity_recovery.max_duration_seconds;
  return (c->identity_recovery_active && s != 0)
      ? c->identity_recovery_started_ms + (uint64_t)s * 1000u
      : 0u;
}

/** @brief Whether the active episode's max_duration_seconds has passed. */
static bool identity_recovery_expired(const az_iot_connection_client* c)
{
  uint64_t deadline = identity_recovery_deadline_ms(c);
  return deadline != 0 && az_iot_time_mono_ms() >= deadline;
}

/** @brief End identity recovery: FAULTED on @p scope, and on the other scope
 * if it is waiting, with the refusal that started the episode as the reason. */
static void stop_identity_recovery(az_iot_connection_client* c, az_iot_connection_scope scope)
{
  AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_CONNECTION, "identity recovery duration spent; stopping");
  c->reconnect_due_ms = 0;
  c->recovery_report.classification = AZ_IOT_CONN_FAILURE_IDENTITY;
  c->recovery_report.attempt = c->identity_retry_attempt;
  c->recovery_report.delay_ms = 0;
  c->recovery_report.reprovisions = false;
  c->recovery_report.staged = true;
  fault_retry_scopes(c, scope, c->identity_recovery_reason);
}

/* Retry after the hub refused the identity.
 *
 * A refusal does not say whether the device is disabled, its credential
 * revoked or its assignment moved. In RETRY_HUB mode the cached hub is retried:
 * no DPS registration and no new certificate. In REPROVISION mode
 * on_mqtt_event() has already raised needs_reprovision for a CONNACK refusal,
 * and the same ladder paces the registrations. The ladder is not reset by a
 * successful registration, so DPS-accept / hub-reject cycles stay bounded by
 * max_attempts and max_duration_seconds. */
static void schedule_identity_recovery(az_iot_connection_client* c, az_iot_result reason)
{
  uint64_t now = az_iot_time_mono_ms();

  /* The hub answered, so it is not unreachable. */
  c->consecutive_hub_connect_failures = 0;
  if (!c->identity_recovery_active)
  {
    c->identity_recovery_active = true;
    c->identity_recovery_started_ms = now;
    c->identity_recovery_reason = reason;
  }

  uint32_t delay = 0;
  bool retry = !c->user_close && az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy)
      && c->opts.identity_recovery.mode != AZ_IOT_IDENTITY_RECOVERY_NONE;
  uint64_t deadline = identity_recovery_deadline_ms(c);
  if (retry
      && !az_iot_retry_policy__next(
          identity_recovery_policy(c), &c->identity_retry_attempt, &c->rng_state, &delay))
  {
    retry = false;
  }
  /* No attempt is scheduled to start at or past the deadline. */
  if (retry && deadline != 0 && now + delay >= deadline)
  {
    AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_CONNECTION, "identity recovery duration spent; stopping");
    retry = false;
  }

  c->recovery_report.classification = AZ_IOT_CONN_FAILURE_IDENTITY;
  c->recovery_report.attempt = c->identity_retry_attempt;
  c->recovery_report.delay_ms = retry ? delay : 0u;
  c->recovery_report.reprovisions = retry && c->needs_reprovision;
  c->recovery_report.staged = true;
  if (!retry)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "hub refused the identity (%s); identity recovery stopped after %u attempt(s)",
        az_iot_result_to_string(reason),
        (unsigned)c->identity_retry_attempt);
    fault_retry_scopes(c, AZ_IOT_CONN_SCOPE_HUB, reason);
    return;
  }
  AZ_IOT_LOG_WARNF(
      AZ_IOT_LOG_COMPONENT_CONNECTION,
      "hub refused the identity (%s); %s in %u ms (attempt %u)",
      az_iot_result_to_string(reason),
      c->needs_reprovision ? "re-provisioning" : "retrying the hub",
      (unsigned)delay,
      (unsigned)c->identity_retry_attempt);
  c->reconnect_due_ms = now + delay;
  set_state_to(c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING, reason);
}

/* In REPROVISION mode a CONNACK refusal makes the next attempt (or open())
 * a DPS registration. An MQTT 5 DISCONNECT refusal does not, as before. */
static void note_identity_refusal(az_iot_connection_client* c, az_iot_result status)
{
  if (status == AZ_IOT_ERR_IDENTITY_REJECTED
      && c->opts.identity_recovery.mode == AZ_IOT_IDENTITY_RECOVERY_REPROVISION && dps_configured(c)
      && !c->user_close)
  {
    c->needs_reprovision = true;
  }
}

/** @brief az_iot_auth of @p scope. */
static const az_iot_auth* auth_of(const az_iot_connection_client* c, az_iot_connection_scope scope)
{
  return scope == AZ_IOT_CONN_SCOPE_DPS ? &c->opts.dps_auth : &c->opts.hub_auth;
}

#if AZ_IOT_MAX_CERTS_PER_ROLE < 1 || AZ_IOT_MAX_CERTS_PER_ROLE > 255
#error "AZ_IOT_MAX_CERTS_PER_ROLE must be 1 to 255"
#endif

/** @brief Credential sources in pass order: the X.509 certificates by index,
 * then the primary key, the secondary key and the user-provided token. */
#define AUTH_POSITIONS ((unsigned)AZ_IOT_MAX_CERTS_PER_ROLE + 3u)

/** @brief Position of source @p s (certificate @p index for X.509) in pass order. */
static unsigned auth_position(az_iot_auth_source s, uint8_t index)
{
  switch (s)
  {
    case AZ_IOT_AUTH_SOURCE_X509:
      return index;
    case AZ_IOT_AUTH_SOURCE_PRIMARY_KEY:
      return (unsigned)AZ_IOT_MAX_CERTS_PER_ROLE;
    case AZ_IOT_AUTH_SOURCE_SECONDARY_KEY:
      return (unsigned)AZ_IOT_MAX_CERTS_PER_ROLE + 1u;
    case AZ_IOT_AUTH_SOURCE_USER_PROVIDED:
    case AZ_IOT_AUTH_SOURCE_NONE:
    default:
      return (unsigned)AZ_IOT_MAX_CERTS_PER_ROLE + 2u;
  }
}

/**
 * @brief Loads @p scope's certificate at @p index: for DPS the bootstrap
 * identity; for the hub at index 0 the operational identity, else the
 * bootstrap one, and at later indexes the role index 0 resolved to.
 */
static az_iot_result load_x509(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    uint8_t index,
    az_iot_certificate_material* mat)
{
  az_iot_certificate_provider* prov = c->opts.certificate_provider;
  if (scope == AZ_IOT_CONN_SCOPE_DPS)
  {
    c->auth[scope].x509_role = (uint8_t)AZ_IOT_CRED_BOOTSTRAP;
    return prov->vtable->load(prov, AZ_IOT_CRED_BOOTSTRAP, index, mat);
  }
  if (index != 0)
  {
    az_iot_cert_role role = c->auth[scope].x509_role == (uint8_t)AZ_IOT_CRED_OPERATIONAL
        ? AZ_IOT_CRED_OPERATIONAL
        : AZ_IOT_CRED_BOOTSTRAP;
    return prov->vtable->load(prov, role, index, mat);
  }
  c->auth[scope].x509_role = (uint8_t)AZ_IOT_CRED_OPERATIONAL;
  az_iot_result op_lr = prov->vtable->load(prov, AZ_IOT_CRED_OPERATIONAL, 0, mat);
  az_iot_result lr = op_lr;
  if (lr == AZ_IOT_ERR_NOT_FOUND || lr == AZ_IOT_ERR_NOT_INITIALIZED)
  {
    c->auth[scope].x509_role = (uint8_t)AZ_IOT_CRED_BOOTSTRAP;
    lr = prov->vtable->load(prov, AZ_IOT_CRED_BOOTSTRAP, 0, mat);
    /* A bootstrap NOT_FOUND must not hide the operational error. */
    if (lr == AZ_IOT_ERR_NOT_FOUND && op_lr != AZ_IOT_ERR_NOT_FOUND)
    {
      lr = op_lr;
    }
  }
  return lr;
}

/**
 * @brief For an attempt about to reuse a kept certificate past index 0
 * (@p *first X.509, @p *x509_index > 0): checks index 0 again. When it is gone,
 * or (hub) its role changed -- e.g. DPS issued an operational certificate --
 * the pass restarts from the first source (@p *first NONE, index 0).
 */
static void recheck_kept_certificate(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_auth_source* first,
    uint8_t* x509_index)
{
  if (*first != AZ_IOT_AUTH_SOURCE_X509 || *x509_index == 0)
  {
    return;
  }
  az_iot_certificate_provider* prov = c->opts.certificate_provider;
  uint8_t role = c->auth[scope].x509_role;
  az_iot_certificate_material mat = { 0 };
  az_iot_result lr = load_x509(c, scope, 0, &mat);
  c->auth[scope].x509_available = lr == AZ_IOT_OK;
  if (lr == AZ_IOT_OK)
  {
    prov->vtable->release(prov, &mat);
  }
  if (lr != AZ_IOT_OK || c->auth[scope].x509_role != role)
  {
    c->auth[scope].first = AZ_IOT_AUTH_SOURCE_NONE;
    c->auth[scope].pass_from = AZ_IOT_AUTH_SOURCE_NONE;
    *first = AZ_IOT_AUTH_SOURCE_NONE;
    *x509_index = 0;
  }
}

/** @brief Whether @p s can be tried for @p scope: X.509 when the last load()
 * at index 0 returned a certificate, a key or token callback when it is set. */
static bool auth_source_available(
    const az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_auth_source s)
{
  if (s == AZ_IOT_AUTH_SOURCE_X509)
  {
    return c->opts.certificate_provider != NULL && c->auth[scope].x509_available;
  }
  if (s == AZ_IOT_AUTH_SOURCE_PRIMARY_KEY)
  {
    return c->auth[scope].primary_key_len > 0;
  }
  if (s == AZ_IOT_AUTH_SOURCE_USER_PROVIDED)
  {
    return auth_of(c, scope)->sas.on_sas_token_required != NULL;
  }
  return s == AZ_IOT_AUTH_SOURCE_SECONDARY_KEY && c->auth[scope].secondary_key_len > 0;
}

/**
 * @brief Whether position @p p (see auth_position()) can be tried for
 * @p scope. A certificate past index 0 is probed with load(); the first index
 * that fails ends the certificates (@p x509_end).
 */
static bool auth_position_available(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    unsigned p,
    unsigned* x509_end)
{
  if (p >= (unsigned)AZ_IOT_MAX_CERTS_PER_ROLE)
  {
    az_iot_auth_source s = p == (unsigned)AZ_IOT_MAX_CERTS_PER_ROLE ? AZ_IOT_AUTH_SOURCE_PRIMARY_KEY
        : p == (unsigned)AZ_IOT_MAX_CERTS_PER_ROLE + 1u ? AZ_IOT_AUTH_SOURCE_SECONDARY_KEY
                                                        : AZ_IOT_AUTH_SOURCE_USER_PROVIDED;
    return auth_source_available(c, scope, s);
  }
  /* No certificate at index 0: none past it either. */
  if (p == 0 || p >= *x509_end || !auth_source_available(c, scope, AZ_IOT_AUTH_SOURCE_X509))
  {
    return p == 0 && auth_source_available(c, scope, AZ_IOT_AUTH_SOURCE_X509);
  }
  az_iot_certificate_material mat = { 0 };
  az_iot_result lr = load_x509(c, scope, (uint8_t)p, &mat);
  if (lr != AZ_IOT_OK)
  {
    if (lr != AZ_IOT_ERR_NOT_FOUND)
    {
      AZ_IOT_LOG_WARNF(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "certificate provider load() of index %u failed (%s); no further certificates",
          p,
          az_iot_result_to_string(lr));
    }
    *x509_end = p;
    return false;
  }
  c->opts.certificate_provider->vtable->release(c->opts.certificate_provider, &mat);
  return true;
}

/**
 * @brief After the service rejected the credential of the last attempt on
 * @p scope: selects the next source of the pass and returns true, or ends the
 * pass and returns false.
 *
 * A pass tries each available source once, from the one it began with, in the
 * order X.509 (index 0, 1, ...), primary key, secondary key, user-provided
 * token, wrapping. After a full pass the next attempt starts at the first
 * source.
 */
static bool auth_next_source(az_iot_connection_client* c, az_iot_connection_scope scope)
{
  az_iot_auth_source s = c->auth[scope].source;
  az_iot_auth_source from_s = c->auth[scope].pass_from;
  if (s != AZ_IOT_AUTH_SOURCE_NONE && from_s != AZ_IOT_AUTH_SOURCE_NONE)
  {
    unsigned from = auth_position(from_s, c->auth[scope].pass_from_x509_index);
    unsigned pos = auth_position(s, c->auth[scope].x509_index);
    unsigned x509_end = (unsigned)AZ_IOT_MAX_CERTS_PER_ROLE;
    for (unsigned i = 1; i < AUTH_POSITIONS; ++i)
    {
      unsigned p = (pos + i) % AUTH_POSITIONS;
      if (p == from)
      {
        break;
      }
      if (auth_position_available(c, scope, p, &x509_end))
      {
        c->auth[scope].first = p < (unsigned)AZ_IOT_MAX_CERTS_PER_ROLE ? AZ_IOT_AUTH_SOURCE_X509
            : p == (unsigned)AZ_IOT_MAX_CERTS_PER_ROLE      ? AZ_IOT_AUTH_SOURCE_PRIMARY_KEY
            : p == (unsigned)AZ_IOT_MAX_CERTS_PER_ROLE + 1u ? AZ_IOT_AUTH_SOURCE_SECONDARY_KEY
                                                            : AZ_IOT_AUTH_SOURCE_USER_PROVIDED;
        c->auth[scope].first_x509_index = p < (unsigned)AZ_IOT_MAX_CERTS_PER_ROLE ? (uint8_t)p : 0u;
        return true;
      }
    }
  }
  c->auth[scope].first = AZ_IOT_AUTH_SOURCE_NONE;
  c->auth[scope].pass_from = AZ_IOT_AUTH_SOURCE_NONE;
  return false;
}

/** @brief Retries @p scope at once with the source auth_next_source()
 * selected: no reconnection_policy delay, no policy attempt. */
static void retry_with_next_source(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_result reason)
{
  uint64_t now = az_iot_time_mono_ms();
  AZ_IOT_LOG_WARNF(
      AZ_IOT_LOG_COMPONENT_CONNECTION,
      "%s rejected the credential (%s); trying the next one",
      scope_name(scope),
      az_iot_result_to_string(reason));
  if (scope == AZ_IOT_CONN_SCOPE_DPS)
  {
    c->needs_reprovision = true;
  }
  else
  {
    c->consecutive_hub_connect_failures = 0; /* the hub answered */
  }
  c->reconnect_due_ms = now != 0 ? now : 1u;
  c->recovery_report.classification = AZ_IOT_CONN_FAILURE_IDENTITY;
  c->recovery_report.attempt = 0;
  c->recovery_report.delay_ms = 0;
  c->recovery_report.reprovisions = scope == AZ_IOT_CONN_SCOPE_DPS;
  c->recovery_report.staged = true;
  set_state_to(c, scope, AZ_IOT_CONN_STATE_RETRY_PENDING, reason);
}

/** @brief DPS registration error code for a rejected credential. */
#define DPS_ERROR_UNAUTHORIZED 401000

/** @brief Whether a failed registration is DPS rejecting the credential: a
 * CONNACK refusal, or registration error DPS_ERROR_UNAUTHORIZED. */
static bool dps_rejected_credential(const az_iot_connection_client* c, az_iot_result status)
{
  return reason_is_identity_refusal(status)
      || (status == AZ_IOT_ERR_DPS && c->error_scope == AZ_IOT_CONN_SCOPE_DPS
          && c->error_source == AZ_IOT_CONN_ERR_SRC_DPS && c->error_code == DPS_ERROR_UNAUTHORIZED);
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
  uint64_t now = az_iot_time_mono_ms();
  uint64_t due = c->dps_user_retry._internal.due_ms;
  AZ_IOT_LOG_INFOF(
      AZ_IOT_LOG_COMPONENT_CONNECTION,
      "dps session retry %u in %llu ms",
      (unsigned)c->dps_user_retry._internal.attempt,
      (unsigned long long)(due > now ? due - now : 0u));
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

/** @brief Outcome of calling the on_sas_token_required callback. */
typedef enum
{
  USER_TOKEN_READY, /**< Supplied from the callback; held in the token area. */
  USER_TOKEN_PENDING, /**< Awaiting az_iot_connection_client_update_sas_token(). */
  USER_TOKEN_ABANDONED, /**< The request ended while the callback ran. */
  USER_TOKEN_FAILED /**< See the returned error. */
} user_token_outcome;

/** @brief Ends @p scope's user-provided token request, and drops its token. */
static void clear_sas_token_request(az_iot_connection_client* c, az_iot_connection_scope scope)
{
  memset(&c->sas_token_request[scope], 0, sizeof(c->sas_token_request[scope]));
  if (c->sas_token_holder == (uint8_t)(scope + 1))
  {
    c->sas_token_holder = 0;
    sas_wipe_token(c);
  }
}

/**
 * @brief Makes the token area free for @p scope. A token held for the other
 * scope's request is dropped, and that request asks again; one held for
 * @p scope itself, or supplied unasked, is dropped with its request.
 */
static void claim_sas_token_area(az_iot_connection_client* c, az_iot_connection_scope scope)
{
  if (c->sas_token_holder == 0)
  {
    return;
  }
  az_iot_connection_scope holder
      = c->sas_token_holder == 1u ? AZ_IOT_CONN_SCOPE_DPS : AZ_IOT_CONN_SCOPE_HUB;
  if (holder == scope || c->sas_token_request[holder].held)
  {
    clear_sas_token_request(c, holder);
    return;
  }
  c->sas_token_request[holder].asked = false;
  c->sas_token_request[holder].ready = false;
  c->sas_token_holder = 0;
  sas_wipe_token(c);
}

/**
 * @brief Places @p scope's token resource URI (`sr`), NUL-terminated, at the
 * end of the token area; the token goes before it.
 *
 * @return AZ_IOT_OK; AZ_IOT_ERR_INVALID_ARG when the hub host or client ID is
 * missing; AZ_IOT_ERR_NOT_ENOUGH_SPACE when the URI leaves no room for a token.
 */
static az_iot_result place_sas_resource_uri(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    const char** uri,
    size_t* uri_len)
{
  bool is_dps = scope == AZ_IOT_CONN_SCOPE_DPS;
  if (!is_dps && !ensure_hub_client(c))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* `<resource URI>\n<expiry>` from azure-sdk-for-c, which encodes the URI
   * and reports, rather than asserts, a buffer too small for it.
   * Only the URI is kept; any expiry above 0 passes its precondition. */
  az_span area = az_span_create((uint8_t*)c->sas_token, (int32_t)c->sas_token_size);
  az_span signed_text = AZ_SPAN_EMPTY;
  az_result ar = is_dps
      ? az_iot_provisioning_client_sas_get_signature(&c->dps_prov, 1, area, &signed_text)
      : az_iot_hub_client_sas_get_signature(&c->hub_client, 1, area, &signed_text);
  int32_t len = az_result_succeeded(ar) ? az_span_find(signed_text, AZ_SPAN_FROM_STR("\n")) : -1;
  /* Room for the URI, its terminator, and a token of at least one byte plus its own. */
  if (len <= 0 || (size_t)len + 3u > c->sas_token_size)
  {
    sas_wipe_token(c);
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  char* tail = c->sas_token + c->sas_token_size - (size_t)len - 1u;
  memmove(tail, c->sas_token, (size_t)len);
  tail[len] = '\0';
  memset(c->sas_token, 0, c->sas_token_size - (size_t)len - 1u);
  *uri = tail;
  *uri_len = (size_t)len;
  return AZ_IOT_OK;
}

/**
 * @brief Calls @p scope's on_sas_token_required callback for its request.
 * From do_work() only. The callback may supply the token at once.
 *
 * @param[out] error For USER_TOKEN_FAILED: place_sas_resource_uri()'s error.
 */
static user_token_outcome ask_user_token(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_result* error)
{
  const az_iot_auth* auth = auth_of(c, scope);
  uint32_t id = c->sas_token_request[scope].request_id;
  claim_sas_token_area(c, scope);
  const char* uri = NULL;
  size_t uri_len = 0;
  *error = place_sas_resource_uri(c, scope, &uri, &uri_len);
  if (*error != AZ_IOT_OK)
  {
    return USER_TOKEN_FAILED;
  }
  c->sas_token_request[scope].asked = true;
  c->sas_token_request[scope].ready = false;
  az_iot_sas_token_request request = {
    .scope = scope,
    .profile
    = scope == AZ_IOT_CONN_SCOPE_HUB ? c->connection_profile : AZ_IOT_CONNECTION_PROFILE_MQTT_V3,
    .resource_uri = uri,
    .key_name = scope == AZ_IOT_CONN_SCOPE_DPS ? DPS_SAS_KEY_NAME : "",
    .is_renewal = c->sas_token_request[scope].for_renewal,
  };
  /* A token supplied meanwhile goes before the URI: its own terminator and the URI's. */
  c->sas_token_capacity = c->sas_token_size - uri_len - 2u;
  c->sas_token_asking = (uint8_t)(scope + 1);
  enter_callback(c);
  auth->sas.on_sas_token_required(&request, auth->sas.user_ctx);
  leave_callback(c);
  c->sas_token_asking = 0;
  memset((char*)(uintptr_t)uri, 0, uri_len + 1u);
  c->sas_token_capacity = c->sas_token_size - 1u;
  if (c->sas_token_request[scope].request_id != id)
  {
    sas_wipe_token(c);
    return USER_TOKEN_ABANDONED;
  }
  return c->sas_token_request[scope].ready ? USER_TOKEN_READY : USER_TOKEN_PENDING;
}

/** @brief Opens a user-provided token request for @p scope; do_work() asks. */
static void open_sas_token_request(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    bool for_renewal,
    uint64_t deadline_ms)
{
  if (++c->sas_token_last_request_id == 0)
  {
    c->sas_token_last_request_id = 1;
  }
  memset(&c->sas_token_request[scope], 0, sizeof(c->sas_token_request[scope]));
  c->sas_token_request[scope].request_id = c->sas_token_last_request_id;
  c->sas_token_request[scope].for_renewal = for_renewal;
  c->sas_token_request[scope].deadline_ms = deadline_ms;
}

/** @brief @p unix_seconds + @p add_seconds; 0 (none: the monotonic deadline
 * alone applies) when the time is unknown or the sum would wrap. */
static uint64_t unix_deadline_seconds(uint64_t unix_seconds, uint64_t add_seconds)
{
  return unix_seconds != 0 && unix_seconds <= UINT64_MAX - add_seconds ? unix_seconds + add_seconds
                                                                       : 0;
}

/**
 * @brief Age of @p scope's supplied token, by whichever clock shows more time
 * passed: the monotonic one may stop in suspend. Capped just past its lifetime.
 */
static uint64_t user_token_age_ms(
    const az_iot_connection_client* c,
    az_iot_connection_scope scope,
    uint64_t now_ms,
    uint64_t unix_seconds)
{
  uint32_t valid_seconds = c->sas_token_request[scope].valid_seconds;
  uint64_t age_ms = now_ms - c->sas_token_request[scope].delivered_ms;
  uint64_t delivered_unix = c->sas_token_request[scope].delivered_unix_seconds;
  if (delivered_unix != 0 && unix_seconds > delivered_unix)
  {
    uint64_t unix_age_seconds = unix_seconds - delivered_unix;
    uint64_t unix_age_ms = unix_age_seconds > valid_seconds
        ? (uint64_t)valid_seconds * 1000u + 1000u
        : unix_age_seconds * 1000u;
    if (unix_age_ms > age_ms)
    {
      age_ms = unix_age_ms;
    }
  }
  return age_ms;
}

/**
 * @brief Selects a user-provided token for @p scope's attempt: the one
 * delivered for its request, or a wait for one. The callback is not called
 * here but from do_work().
 *
 * @param[out] pending The attempt waits; nothing connects yet.
 */
static void apply_user_token(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_mqtt_connect_options* copts,
    bool* pending)
{
  *pending = false;
  uint64_t now_ms = az_iot_time_mono_ms();
  uint64_t unix_seconds = unix_now(c);
  uint32_t valid_seconds = c->sas_token_request[scope].valid_seconds;
  uint64_t valid_ms = (uint64_t)valid_seconds * 1000u;
  uint64_t age_ms = user_token_age_ms(c, scope, now_ms, unix_seconds);
  if (c->sas_token_request[scope].request_id != 0 && c->sas_token_request[scope].ready
      && c->sas_token_holder == (uint8_t)(scope + 1) && age_ms >= valid_ms)
  {
    /* Expired before use: ask again. */
    c->sas_token_request[scope].asked = false;
    c->sas_token_request[scope].ready = false;
    c->sas_token_holder = 0;
    sas_wipe_token(c);
  }
  if (c->sas_token_request[scope].request_id != 0 && c->sas_token_request[scope].ready
      && c->sas_token_holder == (uint8_t)(scope + 1))
  {
    copts->password = c->sas_token;
    c->sas_token_in_use = (uint8_t)(scope + 1);
    c->auth[scope].source = AZ_IOT_AUTH_SOURCE_USER_PROVIDED;
    /* valid_seconds counts from delivery, not from this use. */
    uint64_t delay_ms = get_sas_token_renewal_delay_ms(auth_of(c, scope), valid_seconds);
    uint64_t left_ms = delay_ms > age_ms ? delay_ms - age_ms : 0;
    uint64_t expiry_ms = now_ms + (valid_ms - age_ms);
    c->sas_token_renewal_due_ms[scope] = now_ms + left_ms;
    c->sas_token_renewal_due_unix_seconds[scope]
        = unix_deadline_seconds(unix_seconds, (left_ms + 999u) / 1000u);
    c->sas_token_expiry_ms[scope] = expiry_ms;
    /* Rounded down: never later than the token's real expiry. */
    c->sas_token_expiry_unix_seconds[scope]
        = unix_deadline_seconds(unix_seconds, (expiry_ms - now_ms) / 1000u);
    /* The transport takes the token; it is wiped after connect(). */
    memset(&c->sas_token_request[scope], 0, sizeof(c->sas_token_request[scope]));
    c->sas_token_holder = 0;
    apply_trusted_ca(c, copts);
    return;
  }
  az_iot_mqtt_connect_options timings = { 0 };
  resolve_connect_timings(c, &timings);
  /* Counted from when the request may be asked. */
  uint64_t ask_ms
      = c->sas_token_ask_after_ms[scope] > now_ms ? c->sas_token_ask_after_ms[scope] : now_ms;
  uint64_t deadline_ms = ask_ms + (uint64_t)timings.connect_timeout_seconds * 1000u;
  if (c->sas_token_request[scope].request_id != 0)
  {
    /* A renewal request, or one supplied unasked, becomes this attempt's. */
    c->sas_token_request[scope].for_renewal = false;
    c->sas_token_request[scope].held = false;
    if (c->sas_token_request[scope].deadline_ms == 0)
    {
      c->sas_token_request[scope].deadline_ms = deadline_ms;
    }
  }
  else
  {
    open_sas_token_request(c, scope, false, deadline_ms);
  }
  *pending = true;
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
    stage_local_error(
        c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_ERR_INTERNAL, "registration topic could not be built");
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
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_DPS,
          "register: a registration body was configured but neither "
          "opts.dps.registration_body_buffer nor opts.csr_payload_buffer was "
          "provided to build it in");
      stage_local_error(
          c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_ERR_NOT_ENOUGH_SPACE, "no registration body buffer");
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }

    az_iot_certificate_provider* provider = NULL;
    az_iot_certificate_signing_request csr = { 0 };
    if (c->dps_enrolling)
    {
      provider = c->opts.certificate_provider;
      if (!provider_owns_csr(provider))
      {
        AZ_IOT_LOG_ERROR(
            AZ_IOT_LOG_COMPONENT_DPS,
            "register: request_operational_certificate is set but the certificate "
            "provider does not implement get_csr, release_csr and store_issued_certificate");
        stage_local_error(
            c,
            AZ_IOT_CONN_SCOPE_DPS,
            AZ_IOT_ERR_NOT_SUPPORTED,
            "certificate provider cannot make CSRs and store chains");
        return AZ_IOT_ERR_NOT_SUPPORTED;
      }

      az_iot_result csr_result
          = provider->vtable->get_csr(provider, c->opts.dps.registration_id, &csr);
      if (csr_result != AZ_IOT_OK || csr.csr_base64 == NULL)
      {
        if (csr_result == AZ_IOT_OK)
        {
          AZ_IOT_LOG_ERROR(
              AZ_IOT_LOG_COMPONENT_DPS, "register: certificate provider get_csr returned no CSR");
          provider->vtable->release_csr(provider, &csr);
          csr_result = AZ_IOT_ERR_INTERNAL;
        }
        else
        {
          AZ_IOT_LOG_ERROR(
              AZ_IOT_LOG_COMPONENT_DPS, "register: certificate provider get_csr failed");
        }
        stage_local_error(
            c, AZ_IOT_CONN_SCOPE_DPS, csr_result, "certificate provider get_csr() failed");
        return csr_result;
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
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_DPS,
          "register: the registration body build buffer is too small for the "
          "configured CSR and/or custom payload");
      stage_local_error(
          c,
          AZ_IOT_CONN_SCOPE_DPS,
          AZ_IOT_ERR_NOT_ENOUGH_SPACE,
          "registration body buffer too small");
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
  else
  {
    stage_local_error(c, AZ_IOT_CONN_SCOPE_DPS, r, "registration publish() failed");
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
    stage_local_error(
        c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_ERR_INTERNAL, "status query topic could not be built");
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
  else
  {
    stage_local_error(c, AZ_IOT_CONN_SCOPE_DPS, r, "status query publish() failed");
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

/** @brief Whether the current token is the property name @p name. */
static bool dps_json_key_is(const az_json_reader* jr, const char* name)
{
  return jr->token.kind == AZ_JSON_TOKEN_PROPERTY_NAME
      && az_json_token_is_text_equal(&jr->token, az_span_create_from_str((char*)(uintptr_t)name));
}

/**
 * @brief Read an operation-level "failed" or "disabled" registration verdict.
 *
 * Fallback for the one body shape azure-sdk-for-c rejects that is still a
 * verdict: a registrationState with deviceId but no assignedHub, which is how
 * the service reports e.g. a failed reprovisioning. Every field that parser
 * type-checks is checked the same way here, so any other malformed body is
 * still rejected.
 *
 * @param payload Response body (non-empty).
 * @param[out] out On success, operation_id, operation_status and
 * registration_state (extended_error_code, error_message) are set; spans point
 * into @p payload.
 * @return true if @p payload is exactly one JSON object with a string
 * operationId, a "failed" or "disabled" status, and a registrationState object
 * with a string deviceId and no assignedHub.
 */
static bool dps_parse_operation_refusal(
    az_span payload,
    az_iot_provisioning_client_register_response* out)
{
  az_json_reader jr;
  if (az_result_failed(az_json_reader_init(&jr, payload, NULL))
      || az_result_failed(az_json_reader_next_token(&jr))
      || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return false;
  }

  az_span operation_id = AZ_SPAN_EMPTY;
  bool have_status = false;
  bool have_device_id = false;
  az_iot_provisioning_client_operation_status status = AZ_IOT_PROVISIONING_STATUS_FAILED;
  /* registrationState values take precedence over top-level ones. */
  uint32_t error_code = 0;
  uint32_t state_error_code = 0;
  bool have_state_error_code = false;
  az_span error_message = AZ_SPAN_EMPTY;
  az_span state_error_message = AZ_SPAN_EMPTY;
  bool have_state_error_message = false;

  for (;;)
  {
    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      return false;
    }
    if (jr.token.kind == AZ_JSON_TOKEN_END_OBJECT)
    {
      break;
    }
    bool is_operation_id = dps_json_key_is(&jr, "operationId");
    bool is_status = dps_json_key_is(&jr, "status");
    bool is_state = dps_json_key_is(&jr, DPS_JSON_REGISTRATION_STATE);
    bool is_code = dps_json_key_is(&jr, "errorCode");
    bool is_message = dps_json_key_is(&jr, "message");
    bool is_text
        = is_message || dps_json_key_is(&jr, "trackingId") || dps_json_key_is(&jr, "timestampUtc");
    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      return false;
    }
    if (is_operation_id)
    {
      if (jr.token.kind != AZ_JSON_TOKEN_STRING)
      {
        return false;
      }
      operation_id = jr.token.slice;
    }
    else if (is_status)
    {
      if (jr.token.kind != AZ_JSON_TOKEN_STRING)
      {
        return false;
      }
      if (az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("failed")))
      {
        status = AZ_IOT_PROVISIONING_STATUS_FAILED;
      }
      else if (az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("disabled")))
      {
        status = AZ_IOT_PROVISIONING_STATUS_DISABLED;
      }
      else
      {
        return false;
      }
      have_status = true;
    }
    else if (is_code)
    {
      if (az_result_failed(az_json_token_get_uint32(&jr.token, &error_code)))
      {
        return false;
      }
    }
    else if (is_text)
    {
      if (jr.token.kind != AZ_JSON_TOKEN_STRING)
      {
        return false;
      }
      if (is_message)
      {
        error_message = jr.token.slice;
      }
    }
    else if (is_state)
    {
      if (jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
      {
        return false;
      }
      for (;;)
      {
        if (az_result_failed(az_json_reader_next_token(&jr)))
        {
          return false;
        }
        if (jr.token.kind == AZ_JSON_TOKEN_END_OBJECT)
        {
          break;
        }
        if (dps_json_key_is(&jr, "assignedHub"))
        {
          return false;
        }
        bool is_device_id = dps_json_key_is(&jr, "deviceId");
        bool is_state_code = dps_json_key_is(&jr, "errorCode");
        bool is_state_message = dps_json_key_is(&jr, "errorMessage");
        bool is_state_text = dps_json_key_is(&jr, "lastUpdatedDateTimeUtc");
        bool is_payload = dps_json_key_is(&jr, "payload");
        if (az_result_failed(az_json_reader_next_token(&jr)))
        {
          return false;
        }
        if (is_state_code)
        {
          if (az_result_failed(az_json_token_get_uint32(&jr.token, &state_error_code)))
          {
            return false;
          }
          have_state_error_code = true;
        }
        else if (is_device_id || is_state_message || is_state_text)
        {
          if (jr.token.kind != AZ_JSON_TOKEN_STRING)
          {
            return false;
          }
          have_device_id = have_device_id || is_device_id;
          if (is_state_message)
          {
            state_error_message = jr.token.slice;
            have_state_error_message = true;
          }
        }
        else if (
            is_payload && jr.token.kind != AZ_JSON_TOKEN_NULL
            && jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
        {
          return false;
        }
        else if (az_result_failed(az_json_reader_skip_children(&jr)))
        {
          return false;
        }
      }
    }
    else if (az_result_failed(az_json_reader_skip_children(&jr)))
    {
      return false;
    }
  }
  if (!have_status || !have_device_id || az_span_size(operation_id) <= 0)
  {
    return false;
  }
  /* Only insignificant whitespace may follow the object. */
  az_span end_token = jr.token.slice;
  uint8_t* begin = az_span_ptr(payload);
  int32_t consumed = (int32_t)(az_span_ptr(end_token) - begin) + az_span_size(end_token);
  for (int32_t i = consumed; i < az_span_size(payload); ++i)
  {
    uint8_t ch = begin[i];
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
    {
      return false;
    }
  }

  out->operation_id = operation_id;
  out->operation_status = status;
  out->registration_state = (az_iot_provisioning_client_registration_state){ 0 };
  out->registration_state.extended_error_code
      = have_state_error_code ? state_error_code : error_code;
  out->registration_state.error_message
      = have_state_error_message ? state_error_message : error_message;
  return true;
}

/* Record the connection profile from its verbatim wire form. The raw string is
 * always stored -- an unrecognised profile is exactly the case where the text
 * matters -- and only the enum degrades to UNKNOWN. A value too long for the
 * buffer is stored truncated and treated as unknown, which is correct: every
 * profile this SDK recognises is short, so an overlong one cannot be one of
 * them. */
static void profile_record(
    az_span raw,
    az_iot_connection_profile* profile,
    char* raw_buf,
    size_t raw_cap,
    bool* raw_truncated)
{
  int32_t n = az_span_size(raw);
  if (n < 0)
  {
    n = 0;
  }
  bool truncated = false;
  if ((size_t)n >= raw_cap)
  {
    n = (int32_t)(raw_cap - 1);
    truncated = true;
  }
  if (n > 0)
  {
    memcpy(raw_buf, az_span_ptr(raw), (size_t)n);
  }
  raw_buf[n] = '\0';
  *raw_truncated = truncated;

  if (truncated)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_DPS,
        "connectionProfile is longer than %u bytes and was truncated to \"%s\"",
        (unsigned)raw_cap,
        raw_buf);
    *profile = AZ_IOT_CONNECTION_PROFILE_UNKNOWN;
  }
  else if (strcmp(raw_buf, CONNECTION_PROFILE_MQTT_V3_STR) == 0)
  {
    *profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V3;
  }
  else if (strcmp(raw_buf, CONNECTION_PROFILE_MQTT_V5_STR) == 0)
  {
    *profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5;
  }
  else
  {
    *profile = AZ_IOT_CONNECTION_PROFILE_UNKNOWN;
  }
}

/** @brief Records @p raw as the profile of the hub @p c is bound to. */
static void connection_profile_set(az_iot_connection_client* c, az_span raw)
{
  profile_record(
      raw,
      &c->connection_profile,
      c->connection_profile_raw,
      sizeof(c->connection_profile_raw),
      &c->connection_profile_raw_truncated);
}

/** @brief Records @p raw as the pending assignment's profile, adopted with its hub. */
static void dps_assigned_profile_set(az_iot_connection_client* c, az_span raw)
{
  profile_record(
      raw,
      &c->dps_assigned_profile,
      c->dps_assigned_profile_raw,
      sizeof(c->dps_assigned_profile_raw),
      &c->dps_assigned_profile_raw_truncated);
}

/** @brief Records the pending assignment's profile, as it is adopted. */
static void dps_assigned_profile_commit(az_iot_connection_client* c)
{
  c->connection_profile = c->dps_assigned_profile;
  memcpy(c->connection_profile_raw, c->dps_assigned_profile_raw, sizeof(c->connection_profile_raw));
  c->connection_profile_raw_truncated = c->dps_assigned_profile_raw_truncated;
}

/* Read registrationState.connectionProfile from the DPS ASSIGNED payload.
 *
 * Absent or null is NOT an error -- the service contract documents it as
 * meaning "classic" -- so it leaves the MQTTv3 default set on entry. Only a malformed payload fails
 * here; an unrecognised *value* fails later, at the point the session role is chosen, once the
 * profile is adopted and readable. Written to the pending dps_assigned_profile* fields. */
static az_iot_result dps_read_connection_profile(az_iot_connection_client* c, az_span payload)
{
  /* Each assignment starts from the default: an absent profile must not keep
   * the one an earlier, failed assignment recorded. */
  dps_assigned_profile_set(c, AZ_SPAN_FROM_STR(CONNECTION_PROFILE_MQTT_V3_STR));

  az_json_reader jr;
  az_iot_result r = dps_enter_registration_state(&jr, payload);
  if (r != AZ_IOT_OK)
  {
    /* No registrationState at all: nothing to read, keep the default. */
    return (r == AZ_IOT_ERR_NOT_FOUND) ? AZ_IOT_OK : r;
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
        dps_assigned_profile_set(c, jr.token.slice);
        return AZ_IOT_OK;
      }
      /* null (or any non-string) resolves to the MQTTv3 default. */
      return AZ_IOT_OK;
    }
    if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
    {
      if (az_result_failed(az_json_reader_skip_children(&jr)))
      {
        return AZ_IOT_ERR_PROTOCOL;
      }
    }
  }
  return AZ_IOT_OK;
}

/**
 * @brief Whether dps_apply_deferred() would adopt the parsed assignment: a
 * supported profile the attached feature clients accept, and a hub and device id.
 *
 * @param[in] c Client, with dps_assigned_* already parsed.
 * @return true if the assignment would be adopted.
 */
static bool dps_assignment_acceptable(const az_iot_connection_client* c)
{
  return (c->dps_assigned_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V3
          || c->dps_assigned_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
      && (c->required_profile_refs == 0 || c->required_profile == c->dps_assigned_profile)
      && is_nonempty_cstr(c->dps_assigned_hub) && is_nonempty_cstr(c->dps_assigned_device_id);
}

/* Parse registrationState.issuedCertificateChain (an array of base64 DER or PEM
 * certs) from the DPS ASSIGNED payload and hand the chain to the
 * certificate_provider to persist as the operational identity. azure-sdk-for-c
 * does not surface this field, so we walk the raw payload with az_json. */
static az_iot_result dps_store_issued_cert(az_iot_connection_client* c, az_span payload)
{
  /* A truncated or malformed payload never reaches the provider. */
  if (!az_iot_cert_util_json_is_complete(payload))
  {
    return AZ_IOT_ERR_PROTOCOL;
  }
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

  /* Store in the provider (open() guarantees it can), then notify the app with
   * the outcome (D4). A failed store fails the registration. */
  rc = p->vtable->store_issued_certificate(p, &issued);
  if (rc != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_DPS, "storing the issued certificate failed (%d)", (int)rc);
  }
  if (c->op_cert_cb)
  {
    enter_callback(c);
    c->op_cert_cb(&issued, rc, c->op_cert_cb_ctx);
    leave_callback(c);
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
      /* Without a registration, CONNACK is the service's verdict: the source
       * is kept and a later rejection starts a new pass. */
      if (!c->dps_registration_ref)
      {
        c->auth[AZ_IOT_CONN_SCOPE_DPS].pass_from = AZ_IOT_AUTH_SOURCE_NONE;
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
          stage_local_error(c, AZ_IOT_CONN_SCOPE_DPS, r, "provisioning subscribe() failed");
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
       * Only RECORDED here, so observers do not run inside this adapter's
       * process_loop. The pump announces it once process_loop has returned. */
      c->dps_pending_ready_announce = true;

      /* Registration is a task, and it runs only while the registration ref is
       * held. Registering on a session opened for its other users would take
       * the assignment path -- rewriting host and role and reconnecting --
       * and tear down a hub connection this session may sit beside. */
      if (!c->dps_registration_ref)
      {
        AZ_IOT_LOG_DEBUG(AZ_IOT_LOG_COMPONENT_DPS, "session ready for its users");
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
        AZ_IOT_LOG_DEBUG(
            AZ_IOT_LOG_COMPONENT_DPS, "holding registration for a pre-registration exchange");
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
        enter_callback(c);
        bool consumed = c->dps_message_observer(
            evt->message->topic,
            evt->message->payload,
            evt->message->payload_len,
            c->dps_message_observer_ctx);
        leave_callback(c);
        if (consumed)
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
        AZ_IOT_LOG_ERRORF(
            AZ_IOT_LOG_COMPONENT_DPS,
            "register: empty response body on topic %s",
            evt->message->topic);
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
      /* ITEM_NOT_FOUND only comes from the body, so the topic (status,
       * retry-after) was parsed. A failed/disabled verdict the SDK parser
       * rejects is still a verdict. */
      if (ar == AZ_ERROR_ITEM_NOT_FOUND && dps_parse_operation_refusal(payload_span, &resp))
      {
        ar = AZ_OK;
      }
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
            AZ_IOT_LOG_COMPONENT_DPS,
            "register: unparsable response on topic %s; body: %.*s",
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
          /* An issued chain is stored only from a complete payload; check it
           * before any assignment field is read or cached, so a rejected
           * payload leaves nothing (hub, profile) behind for the retry. */
          if (c->dps_enrolling && !az_iot_cert_util_json_is_complete(payload_span))
          {
            dps_finalize(c, AZ_IOT_ERR_PROTOCOL, false);
            return;
          }
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
          /* Which hub generation we were assigned to. Held pending and recorded
           * only when the assignment is adopted (dps_apply_deferred()): until
           * then the cached or saved hub, and its profile, still apply. */
          az_iot_result pr = dps_read_connection_profile(c, payload_span);
          if (pr != AZ_IOT_OK)
          {
            dps_finalize(c, pr, false);
            return;
          }
          /* An assignment dps_apply_deferred() will reject keeps the stored
           * certificate: it must stay paired with the adopted assignment. */
          if (c->dps_enrolling && !dps_assignment_acceptable(c))
          {
            AZ_IOT_LOG_WARN(
                AZ_IOT_LOG_COMPONENT_DPS,
                "the assignment will be rejected; not storing its issued certificate");
            c->dps_cert_withheld = true;
          }
          else if (c->dps_enrolling)
          {
            /* Before the store: it runs the operational-certificate callback. */
            c->dps_cert_awaits_adoption = true;
            az_iot_result sc = dps_store_issued_cert(c, payload_span);
            if (sc != AZ_IOT_OK)
            {
              c->dps_cert_awaits_adoption = false;
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
          AZ_IOT_LOG_INFOF(
              AZ_IOT_LOG_COMPONENT_DPS,
              "assigned hub=%s device_id=%s profile=%s",
              c->dps_assigned_hub,
              c->dps_assigned_device_id,
              profile_name(c->dps_assigned_profile));
          if (c->reg_payload_cb && az_span_size(resp.registration_state.payload) > 0)
          {
            enter_callback(c);
            c->reg_payload_cb(resp.registration_state.payload, c->reg_payload_cb_ctx);
            leave_callback(c);
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
              AZ_IOT_LOG_COMPONENT_DPS,
              "register: errorCode=%ld errorMessage=%.*s operationId=%.*s",
              (long)resp.registration_state.extended_error_code,
              (int)az_span_size(err_msg),
              az_span_size(err_msg) > 0 ? (const char*)az_span_ptr(err_msg) : "",
              (int)az_span_size(resp.operation_id),
              az_span_size(resp.operation_id) > 0 ? (const char*)az_span_ptr(resp.operation_id)
                                                  : "");
          AZ_IOT_LOG_ERRORF(
              AZ_IOT_LOG_COMPONENT_DPS,
              "register: provisioning failed/disabled; DPS response: %.*s",
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

/**
 * @brief Build and connect the provisioning session; dps_start() owns the
 * SETTING_UP announcement.
 *
 * A failure before DPS:CONNECTING stages AZ_IOT_CONN_ERR_SRC_LOCAL detail and
 * leaves no session and no settle: the caller reports it. A connect() failure
 * after DPS:CONNECTING settles the session it announced (DISCONNECTING, IDLE).
 */
static az_iot_result dps_connect_session(az_iot_connection_client* c)
{
  /* Validate the provisioning identity before handing it to az_core. Empty
   * spans trip an az_core precondition, and this build ships with
   * AZ_NO_PRECONDITION_CHECKING OFF and no handler installed -- the default
   * handler is an infinite loop, so a misconfigured device would hang inside
   * open() instead of getting an error back. */
  if (!is_nonempty_cstr(c->opts.dps.id_scope))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_DPS, "start: dps.id_scope is required for DPS provisioning");
    stage_local_error(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_ERR_INVALID_ARG, "dps.id_scope is not set");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!is_nonempty_cstr(c->opts.dps.registration_id))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_DPS, "start: dps.registration_id is required for DPS provisioning");
    stage_local_error(
        c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_ERR_INVALID_ARG, "dps.registration_id is not set");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* The default endpoint is a short literal; only caller strings need the
   * length check. */
  bool custom_endpoint = is_nonempty_cstr(c->opts.dps.global_endpoint);
  if ((custom_endpoint && !is_span_safe_cstr(c->opts.dps.global_endpoint))
      || !is_span_safe_cstr(c->opts.dps.id_scope)
      || !is_span_safe_cstr(c->opts.dps.registration_id))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_DPS, "start: a DPS endpoint or ID is longer than INT32_MAX bytes");
    stage_local_error(
        c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_ERR_INVALID_ARG, "DPS endpoint or ID too long");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  const char* endpoint = dps_endpoint(c);

  az_span ep_span = az_span_create_from_str((char*)(uintptr_t)endpoint);
  az_span scope_span = az_span_create_from_str((char*)(uintptr_t)c->opts.dps.id_scope);
  az_span reg_span = az_span_create_from_str((char*)(uintptr_t)c->opts.dps.registration_id);
  az_result ar = az_iot_provisioning_client_init(&c->dps_prov, ep_span, scope_span, reg_span, NULL);
  if (az_result_failed(ar))
  {
    stage_local_error(
        c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_ERR_INVALID_ARG, "provisioning client init failed");
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
    stage_local_error(
        c,
        AZ_IOT_CONN_SCOPE_DPS,
        AZ_IOT_ERR_NOT_ENOUGH_SPACE,
        "DPS CONNECT username does not fit AZ_IOT_MQTT_USERNAME_BUF");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  const az_iot_mqtt_factory* f = find_factory(c, AZ_IOT_MQTT_VERSION_3_1_1);
  if (!f)
  {
    stage_local_error(
        c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_ERR_NOT_SUPPORTED, "no MQTT 3.1.1 factory registered");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  az_iot_mqtt_client* mc = f->create(f->factory_ctx);
  if (!mc || !mc->iface)
  {
    stage_local_error(
        c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_ERR_INTERNAL, "MQTT factory create() failed");
    return AZ_IOT_ERR_INTERNAL;
  }

  mc->iface->set_inbound_cb(mc, on_dps_mqtt_event, c);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = endpoint;
  copts.client_id = c->opts.dps.registration_id;
  resolve_connect_timings(c, &copts);
  resolve_connect_transport(c, &copts, AZ_IOT_MQTT_ROLE_DPS, 0);
  resolve_session_options(c, &copts, AZ_IOT_MQTT_ROLE_DPS);

  copts.username = dps_username;
  AZ_IOT_LOG_DEBUGF(AZ_IOT_LOG_COMPONENT_DPS, "connecting with username %s", dps_username);

  /* Credential, from c->auth[DPS].first (see auth_next_source()): the
   * provider's bootstrap X.509 identity (the operational cert, if any, is
   * issued during this exchange), then the primary and the secondary key of
   * dps_auth, then its on_sas_token_required. It never goes plaintext. */
  az_iot_auth_source first = c->auth[AZ_IOT_CONN_SCOPE_DPS].first;
  bool dps_has_key = c->auth[AZ_IOT_CONN_SCOPE_DPS].primary_key_len > 0;
  bool dps_has_sas
      = dps_has_key || auth_of(c, AZ_IOT_CONN_SCOPE_DPS)->sas.on_sas_token_required != NULL;
  if (!c->opts.certificate_provider && !dps_has_sas)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_DPS,
        "no certificate provider and no SAS key or token callback; refusing to connect");
    mc->iface->destroy(mc);
    stage_local_error(
        c,
        AZ_IOT_CONN_SCOPE_DPS,
        AZ_IOT_ERR_CREDENTIAL_INCOMPLETE,
        "no certificate provider and no SAS key or token callback");
    return AZ_IOT_ERR_CREDENTIAL_INCOMPLETE;
  }
  if (c->opts.certificate_provider)
  {
    az_iot_certificate_material mat = { 0 };
    uint8_t x509_index
        = first == AZ_IOT_AUTH_SOURCE_X509 ? c->auth[AZ_IOT_CONN_SCOPE_DPS].first_x509_index : 0u;
    recheck_kept_certificate(c, AZ_IOT_CONN_SCOPE_DPS, &first, &x509_index);
    az_iot_result lr = load_x509(c, AZ_IOT_CONN_SCOPE_DPS, x509_index, &mat);
    if (x509_index == 0)
    {
      c->auth[AZ_IOT_CONN_SCOPE_DPS].x509_available = lr == AZ_IOT_OK;
    }
    /* Only an absent certificate selects SAS; other failures fail the attempt. */
    if (lr == AZ_IOT_ERR_NOT_FOUND && first == AZ_IOT_AUTH_SOURCE_X509)
    {
      /* The certificate selected (by fallback, or kept after connecting) is
       * gone: the pass ends, failing this attempt, rather than trying the
       * sources before it again unpaced. */
      AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_DPS, "the certificate selected for fallback is gone");
      c->auth[AZ_IOT_CONN_SCOPE_DPS].first = AZ_IOT_AUTH_SOURCE_NONE;
      c->auth[AZ_IOT_CONN_SCOPE_DPS].pass_from = AZ_IOT_AUTH_SOURCE_NONE;
      mc->iface->destroy(mc);
      stage_local_error(c, AZ_IOT_CONN_SCOPE_DPS, lr, "certificate selected for fallback is gone");
      return lr;
    }
    if (lr == AZ_IOT_ERR_NOT_FOUND && dps_has_sas)
    {
      AZ_IOT_LOG_DEBUG(AZ_IOT_LOG_COMPONENT_DPS, "no bootstrap certificate; using SAS");
      keep_provider_trust(&copts, &mat);
    }
    else if (lr == AZ_IOT_OK && first > AZ_IOT_AUTH_SOURCE_X509)
    {
      /* X.509 was rejected in this pass: SAS, with the provider's trust anchors. */
      keep_provider_trust(&copts, &mat);
      hold_material(c, AZ_IOT_CONN_SCOPE_DPS, &mat);
    }
    else if (lr == AZ_IOT_OK)
    {
      az_iot_result cr = apply_certificate_material(&copts, &mat, c->opts.certificate_provider);
      /* A key URI's query may carry a PIN (pin-value): log only what precedes it. */
      const char* key_uri = text_or_none(mat.client_key_uri);
      const char* key_query = strchr(key_uri, '?');
      AZ_IOT_LOG_DEBUGF(
          AZ_IOT_LOG_COMPONENT_DPS,
          "bootstrap TLS index=%u ca=%s cert=%s key=%s key_uri=%.*s%s engine=%s",
          (unsigned)x509_index,
          text_or_none(mat.trusted_ca_path),
          text_or_none(mat.client_cert_path),
          text_or_none(mat.client_key_path),
          (int)(key_query != NULL ? (size_t)(key_query - key_uri) : strlen(key_uri)),
          key_uri,
          key_query != NULL ? "?<redacted>" : "",
          text_or_none(mat.crypto_engine_id));
      hold_material(c, AZ_IOT_CONN_SCOPE_DPS, &mat);
      if (cr != AZ_IOT_OK)
      {
        release_material(c, AZ_IOT_CONN_SCOPE_DPS);
        mc->iface->destroy(mc);
        stage_local_error(c, AZ_IOT_CONN_SCOPE_DPS, cr, "certificate material rejected");
        return cr;
      }
      c->auth[AZ_IOT_CONN_SCOPE_DPS].source = AZ_IOT_AUTH_SOURCE_X509;
      c->auth[AZ_IOT_CONN_SCOPE_DPS].x509_index = x509_index;
      apply_trusted_ca(c, &copts);
    }
    else
    {
      AZ_IOT_LOG_ERRORF(
          AZ_IOT_LOG_COMPONENT_DPS,
          "certificate provider load() failed for the bootstrap identity (%d)",
          (int)lr);
      mc->iface->destroy(mc);
      stage_local_error(c, AZ_IOT_CONN_SCOPE_DPS, lr, "certificate provider load() failed");
      return lr;
    }
  }
  if (c->auth[AZ_IOT_CONN_SCOPE_DPS].source == AZ_IOT_AUTH_SOURCE_NONE
      && (first == AZ_IOT_AUTH_SOURCE_USER_PROVIDED || !dps_has_key))
  {
    bool pending = false;
    apply_user_token(c, AZ_IOT_CONN_SCOPE_DPS, &copts, &pending);
    if (pending)
    {
      /* Waiting for the token: the attempt stays in SETTING_UP. */
      release_material(c, AZ_IOT_CONN_SCOPE_DPS);
      mc->iface->destroy(mc);
      return AZ_IOT_OK;
    }
  }
  else if (c->auth[AZ_IOT_CONN_SCOPE_DPS].source == AZ_IOT_AUTH_SOURCE_NONE)
  {
    az_iot_result sr = apply_sas_key(
        c,
        AZ_IOT_CONN_SCOPE_DPS,
        first == AZ_IOT_AUTH_SOURCE_SECONDARY_KEY ? first : AZ_IOT_AUTH_SOURCE_PRIMARY_KEY,
        &copts);
    if (sr != AZ_IOT_OK)
    {
      release_material(c, AZ_IOT_CONN_SCOPE_DPS);
      mc->iface->destroy(mc);
      stage_local_error(c, AZ_IOT_CONN_SCOPE_DPS, sr, "SAS token signing failed");
      return sr;
    }
  }
  /* Another source was selected: a token request for this scope is moot. */
  if (c->auth[AZ_IOT_CONN_SCOPE_DPS].source != AZ_IOT_AUTH_SOURCE_USER_PROVIDED
      && c->sas_token_request[AZ_IOT_CONN_SCOPE_DPS].request_id != 0)
  {
    clear_sas_token_request(c, AZ_IOT_CONN_SCOPE_DPS);
  }
  /* An attempt from the start of the order begins the pass, even after a
   * failure that was not a rejection: the available sources may have changed. */
  if (c->auth[AZ_IOT_CONN_SCOPE_DPS].pass_from == AZ_IOT_AUTH_SOURCE_NONE
      || first == AZ_IOT_AUTH_SOURCE_NONE)
  {
    c->auth[AZ_IOT_CONN_SCOPE_DPS].pass_from = c->auth[AZ_IOT_CONN_SCOPE_DPS].source;
    c->auth[AZ_IOT_CONN_SCOPE_DPS].pass_from_x509_index = c->auth[AZ_IOT_CONN_SCOPE_DPS].x509_index;
  }

  c->dps_mqtt = mc;
  c->dps_phase = DPS_PHASE_CONNECTING;
  c->dps_cert_awaits_adoption = false;
  c->dps_cert_withheld = false;
  c->dps_pending_finalize = false;
  c->dps_pending_have_assignment = false;
  c->dps_pending_status = AZ_IOT_OK;
  c->dps_pending_retry_after_secs = 0;
  c->dps_enrolling = c->opts.dps.request_operational_certificate;

  set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_CONNECTING, AZ_IOT_OK);

  az_iot_result r = mc->iface->connect(mc, &copts);
  sas_wipe_token(c);
  release_material(c, AZ_IOT_CONN_SCOPE_DPS);
  if (r != AZ_IOT_OK)
  {
    dps_teardown_mqtt(c);
    c->dps_phase = DPS_PHASE_NONE;
    stage_local_error(c, AZ_IOT_CONN_SCOPE_DPS, r, "MQTT adapter connect() failed");
    /* The announcement above already moved the DPS lifecycle to CONNECTING, so
     * a synchronous connect failure has to settle it again here -- every other
     * teardown does. Leaving it pinned at CONNECTING would make the next
     * dps_start() announce nothing (the value would be unchanged) and, for a
     * session the connection client owns, would make open() reject for ever.
     *
     * Settled at the point the session dies rather than in each caller: this is
     * the only place that knows the announcement happened. */
    set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_DISCONNECTING, r);
    if (c->dps_mqtt == NULL)
    {
      set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, r);
    }
    /* A feature client may have started a newer session from those callbacks:
     * report this start as replaced, so no caller settles or paces over it. */
    if (c->dps_mqtt != NULL)
    {
      c->dps_start_cancelled = true;
    }
  }
  return r;
}

/**
 * @brief Start the provisioning session.
 *
 * Announces DPS:SETTING_UP before any step that can fail. A failure before
 * the session exists leaves the scope in SETTING_UP for the caller to settle:
 * RETRY_PENDING or FAULTED from a retry, IDLE otherwise.
 *
 * @return AZ_IOT_OK when the session is connecting. AZ_IOT_ERR_NOT_CONNECTED
 * with dps_start_cancelled set when a feature client started a session from
 * the announcement. Otherwise the failed step's result.
 */
static az_iot_result dps_start(az_iot_connection_client* c)
{
  c->dps_start_cancelled = false;
  /* A new session: no credential is selected yet, so none is reported. */
  c->auth[AZ_IOT_CONN_SCOPE_DPS].source = AZ_IOT_AUTH_SOURCE_NONE;
  c->auth[AZ_IOT_CONN_SCOPE_DPS].x509_index = 0;
  set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_SETTING_UP, AZ_IOT_OK);
  /* A feature client may have started a session from the state callback. */
  if (c->dps_mqtt != NULL)
  {
    AZ_IOT_LOG_DEBUG(
        AZ_IOT_LOG_COMPONENT_DPS, "session started from the state callback; abandoned");
    c->dps_start_cancelled = true;
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  /* An attempt already waiting for its user-provided token continues; a
   * token held for the next attempt is this one's to take. */
  if (c->sas_token_request[AZ_IOT_CONN_SCOPE_DPS].request_id != 0
      && !c->sas_token_request[AZ_IOT_CONN_SCOPE_DPS].held)
  {
    return AZ_IOT_OK;
  }
  return dps_connect_session(c);
}

/**
 * @brief Register on a provisioning session that is already up.
 *
 * DPS:SETTING_UP, then DPS:CONNECTED once the request is published, so every
 * registration attempt moves the scope, including one retried on a session a
 * feature client keeps up.
 *
 * @return As dps_do_register_publish().
 */
static az_iot_result dps_register_on_ready_session(az_iot_connection_client* c)
{
  c->dps_start_cancelled = false;
  set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_SETTING_UP, AZ_IOT_OK);
  /* The registration's pass starts from the source the session connected with. */
  if (c->auth[AZ_IOT_CONN_SCOPE_DPS].pass_from == AZ_IOT_AUTH_SOURCE_NONE)
  {
    c->auth[AZ_IOT_CONN_SCOPE_DPS].pass_from = c->auth[AZ_IOT_CONN_SCOPE_DPS].source;
    c->auth[AZ_IOT_CONN_SCOPE_DPS].pass_from_x509_index = c->auth[AZ_IOT_CONN_SCOPE_DPS].x509_index;
  }
  az_iot_result r = dps_do_register_publish(c);
  if (r == AZ_IOT_OK)
  {
    set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_CONNECTED, AZ_IOT_OK);
  }
  return r;
}

/* Fail an assignment this client cannot use, and make sure the NEXT open()
 * goes back to DPS instead of to whatever assignment is still cached.
 *
 * Declining to adopt the new values is not enough on a re-provision: opts.iothub_hostname
 * and opts.device_id still name the hub from the previous assignment, and
 * open() skips DPS whenever a host is set. Without this the client would
 * connect to the stale hub -- and for a profile mismatch it would do so with
 * session_role already switched to the generation this very response was
 * rejected for.
 *
 * The flag is raised BEFORE the transition because set_state_to() runs the
 * application's state callback synchronously, and that callback must already
 * see the demand to re-provision.
 *
 * A hub waiting on this registration as its retry is faulted too: nothing else
 * would retry it. */
static void reject_assignment(az_iot_connection_client* c, az_iot_result reason)
{
  c->dps_cert_awaits_adoption = false;
  c->needs_reprovision = true;
  fault_retry_scopes(c, AZ_IOT_CONN_SCOPE_DPS, reason);
}

/* Process deferred DPS finalization. Called from _do_work() after process_loop.
 *
 * On success, tears down the DPS MQTT session, validates the assigned profile,
 * adopts iothub_hostname/device_id and starts the hub connect.
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
  bool cert_withheld = c->dps_cert_withheld;
  c->dps_cert_withheld = false;
  az_iot_result status = c->dps_pending_status;
  if (status != AZ_IOT_OK || !have_assignment)
  {
    c->dps_cert_awaits_adoption = false;
  }
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
  /* Read before the transitions below run callbacks. */
  bool credential_rejected = !device_provisioned && dps_rejected_credential(c, status);
  /* The transitions below run state callbacks, in which feature clients may
   * release or acquire their demand for the session. */
  uint32_t demand_epoch = c->dps_demand_epoch;
  if (device_provisioned)
  {
    c->auth[AZ_IOT_CONN_SCOPE_DPS].pass_from = AZ_IOT_AUTH_SOURCE_NONE;
  }
  if (dps_refs_held(c) && device_provisioned)
  {
    AZ_IOT_LOG_DEBUG(AZ_IOT_LOG_COMPONENT_DPS, "keeping the provisioning session for its users");
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
     * overwrites this with RETRY_PENDING or FAULTED, which is right: "closed,
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
    /* A rejected credential: the next ensure() reopens at once with the next
     * source, policy or not; no pacing, as for a registration. */
    /* Demand released (and maybe re-acquired) in the callbacks above is new
     * demand: it starts at the first source, unpaced. */
    if (c->dps_demand_epoch != demand_epoch)
    {
      return;
    }
    /* Demand is checked again: the IDLE callback above may have released it. */
    if (credential_rejected && !c->user_close && dps_session_demanded(c)
        && auth_next_source(c, AZ_IOT_CONN_SCOPE_DPS))
    {
      AZ_IOT_LOG_WARNF(
          AZ_IOT_LOG_COMPONENT_DPS,
          "the provisioning session's credential was rejected (%s); trying the next one",
          az_iot_result_to_string(status));
      return;
    }
    if (status != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERRORF(
          AZ_IOT_LOG_COMPONENT_DPS,
          "the provisioning session ended with an error (%d)",
          (int)status);
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
    /* A rejected credential moves to the next source at once, policy or not. */
    if (credential_rejected && !c->user_close && auth_next_source(c, AZ_IOT_CONN_SCOPE_DPS))
    {
      retry_with_next_source(c, AZ_IOT_CONN_SCOPE_DPS, status);
      return;
    }
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
      if (retry_after_secs > 0
          && c->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_RETRY_PENDING)
      {
        uint64_t floor_ms = az_iot_time_mono_ms() + (uint64_t)retry_after_secs * 1000ull;
        uint64_t identity_deadline = identity_recovery_deadline_ms(c);
        if (identity_deadline != 0 && floor_ms >= identity_deadline)
        {
          /* Honoring the retry-after would start past max_duration_seconds. */
          stop_identity_recovery(c, AZ_IOT_CONN_SCOPE_DPS);
          return;
        }
        if (c->reconnect_due_ms < floor_ms)
        {
          AZ_IOT_LOG_WARNF(
              AZ_IOT_LOG_COMPONENT_DPS,
              "the service asked for a %u second retry-after; honoring it over the "
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
  dps_assigned_profile_commit(c);
  /* The assigned profile picks the wire protocol for the hub session. An
   * unrecognised value fails the connection instead of guessing an MQTT version
   * -- a device that appears to connect and then misbehaves is far worse to
   * diagnose than one clear error here. The profile stays readable through
   * az_iot_connection_client_get_hub_profile() so the offending value can be
   * logged or reported.
   *
   * Checked BEFORE the assignment is applied to opts.iothub_hostname / opts.device_id,
   * which is what keeps a rejected assignment from being adopted. Refusing to
   * adopt it is only half of the job, though: on a RE-provision the previous
   * assignment is still cached in opts.iothub_hostname, and open() skips DPS whenever a
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
          AZ_IOT_LOG_COMPONENT_DPS,
          "assigned an unsupported connectionProfile \"%s\"; this SDK does not know which "
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
        AZ_IOT_LOG_COMPONENT_DPS,
        "assigned connectionProfile \"%s\", but the attached feature clients require the "
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
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_DPS,
        "the assignment did not carry a usable hub hostname and device id");
    reject_assignment(c, r);
    return;
  }

  /* The issued chain was withheld for an assignment that was to be rejected;
   * the profile pins changed since. Adopting it would pair the assignment
   * with the previous certificate. */
  if (cert_withheld)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_DPS,
        "the attached feature clients changed while the assignment was pending; registering "
        "again");
    reject_assignment(c, AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);
    return;
  }

  /* Only a replaced identity has state to discard; the first assignment has none. */
  bool const identity_changed = c->opts.iothub_hostname != NULL && c->opts.device_id != NULL
      && (strcmp(c->opts.iothub_hostname, c->dps_assigned_hub) != 0
          || strcmp(c->opts.device_id, c->dps_assigned_device_id) != 0);
  (void)replace_owned_string(
      c->provisioned_iot_hub_hostname,
      sizeof(c->provisioned_iot_hub_hostname),
      &c->opts.iothub_hostname,
      c->dps_assigned_hub);
  (void)replace_owned_string(
      c->provisioned_device_id,
      sizeof(c->provisioned_device_id),
      &c->opts.device_id,
      c->dps_assigned_device_id);
  c->dps_cert_awaits_adoption = false;
  drop_subscriptions_from_other_generations(c);
  /* A hub token asked for, or held, before this assignment may be for
   * another hub or device: the next attempt asks again. */
  clear_sas_token_request(c, AZ_IOT_CONN_SCOPE_HUB);
  c->dps_phase = DPS_PHASE_NONE;
  /* This registration satisfies any re-provision asked for while it ran. */
  c->needs_reprovision = false;

  /* Registration succeeded, so BOTH ladders start over: the DPS one because it
   * has done its job, and the hub one because this is a fresh assignment --
   * the attempts that failed against the previous hub say nothing about the
   * one just handed to us, and making the first connect to it wait at the
   * old ladder's cap would be backoff for a failure that never happened.
   *
   * The identity recovery ladder is NOT reset: a hub that keeps refusing the
   * device after every registration must still exhaust it. */
  c->retry_attempt[AZ_IOT_CONN_SCOPE_DPS] = 0;
  c->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] = 0;

  /* The service answered, so it is reachable: a user session that failed
   * earlier should not still be serving out a backoff from that. */
  dps_user_retry_reset(c);

  /* Before the application learns of the assignment and before the hub connect, so feature
   * clients drop what belongs to the previous identity first. */
  if (identity_changed)
  {
    ++c->identity_changes;
    notify_feature_observers_of(c, AZ_IOT_CONNECTION_FEATURE_EVENT_IDENTITY_CHANGED);
  }

  if (c->assignment_cb)
  {
    az_iot_hub_profile assigned_profile = {
      .connection_profile = c->connection_profile,
      .connection_profile_raw = c->connection_profile_raw,
      .connection_profile_raw_truncated = c->connection_profile_raw_truncated,
    };
    az_iot_dps_assignment assignment = {
      .hub_hostname = c->opts.iothub_hostname,
      .device_id = c->opts.device_id,
      .profile = &assigned_profile,
    };
    enter_callback(c);
    c->assignment_cb(&assignment, c->assignment_cb_ctx);
    leave_callback(c);
  }
  /* The assignment is kept, but no hub attempt starts past the bound. */
  if (identity_recovery_expired(c))
  {
    stop_identity_recovery(c, AZ_IOT_CONN_SCOPE_HUB);
    return;
  }

  r = start_connect_attempt(c);
  if (r != AZ_IOT_OK)
  {
    /* The same rule as every other hub failure: retried under the policy. */
    if (az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy) && !c->user_close)
    {
      schedule_reconnect(c, AZ_IOT_CONN_SCOPE_HUB, r);
    }
    else
    {
      set_state_to(c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_FAULTED, r);
    }
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
 * a service-side schema addition does not break the handshake.
 * @return false for a malformed ack (bad encoding, or a known field on the wrong wire type). */
static bool presence_decode_birth_ack(az_iot_connection_client* c, const uint8_t* buf, size_t len)
{
  c->presence.desired_version = 0;
  c->presence.reported_version = 0;
  if (!buf)
  {
    return true;
  }

  size_t pos = 0;
  while (pos < len)
  {
    uint32_t field = 0;
    uint8_t wire = 0;
    if (!az_iot_proto3_read_tag(buf, len, &pos, &field, &wire))
    {
      return false;
    }
    bool const known = field == PRESENCE_BIRTH_ACK_FIELD_DESIRED_VERSION
        || field == PRESENCE_BIRTH_ACK_FIELD_REPORTED_VERSION;
    if (known && wire != AZ_IOT_PROTO3_WIRE_VARINT)
    {
      return false;
    }

    if (known)
    {
      uint64_t v = 0;
      if (!az_iot_proto3_read_varint(buf, len, &pos, &v))
      {
        return false;
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
      return false;
    }
  }
  return true;
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
      AZ_IOT_LOG_COMPONENT_CONNECTION,
      "subscription '%s' failed (reason_code=%d); dropping it, connection stays up",
      topic,
      (int)protocol_code);
  memset(&c->persistent_subs[index], 0, sizeof(c->persistent_subs[index]));
  if (cb)
  {
    enter_callback(c);
    cb(topic, reason, protocol_code, owner);
    leave_callback(c);
  }
}

/* Deadline for the SUBACKs of the filters just handed to the adapter. */
static uint32_t subscription_ack_timeout_seconds(const az_iot_connection_client* c)
{
  return c->opts.subscription_ack_timeout_seconds ? c->opts.subscription_ack_timeout_seconds
                                                  : AZ_IOT_DEFAULT_SUBSCRIPTION_ACK_TIMEOUT_SECONDS;
}

static uint64_t subscription_ack_deadline(const az_iot_connection_client* c)
{
  return az_iot_time_mono_ms() + (uint64_t)subscription_ack_timeout_seconds(c) * 1000u;
}

/* Record a SUBSCRIBE that is waiting for its ack. Gated entries hold CONNECTED;
 * the rest are tracked only so a later refusal still reaches its owner. */
static void subscription_gate_track(
    az_iot_connection_client* c,
    uint16_t packet_id,
    size_t sub_index,
    bool gated)
{
  if (c->subscription_gate.pending_count >= c->opts.limits.max_persistent_subscriptions)
  {
    return;
  }
  size_t p = c->subscription_gate.pending_count++;
  c->subscription_acks[p].packet_id = packet_id;
  c->subscription_acks[p].sub_index = (uint8_t)sub_index;
  c->subscription_acks[p].gated = gated;
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
    if (c->subscription_acks[i].sub_index != sub_index)
    {
      continue;
    }
    if (c->subscription_acks[i].gated && c->subscription_gate.gated_outstanding > 0)
    {
      c->subscription_gate.gated_outstanding--;
    }
    c->subscription_acks[i] = c->subscription_acks[c->subscription_gate.pending_count - 1];
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

  for (size_t i = 0; i < c->opts.limits.max_persistent_subscriptions; ++i)
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
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "could not re-subscribe '%s' on connect",
          c->persistent_subs[i].topic_filter);
      stage_local_error(c, AZ_IOT_CONN_SCOPE_HUB, r, "re-subscribe() failed on connect");
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
    if (c->subscription_acks[i].packet_id != packet_id)
    {
      continue;
    }
    const bool gated = c->subscription_acks[i].gated;
    const size_t sub_index = c->subscription_acks[i].sub_index;

    c->subscription_acks[i] = c->subscription_acks[c->subscription_gate.pending_count - 1];
    c->subscription_gate.pending_count--;

    if (status != AZ_IOT_OK)
    {
      if (!gated)
      {
        report_and_drop_subscription(c, sub_index, status, protocol_code);
        return true;
      }
      AZ_IOT_LOG_ERRORF(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "broker refused '%s' on connect (reason_code=%d)",
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

  const char* device_id = c->opts.device_id ? c->opts.device_id : "";
  char topic[AZ_IOT_PRESENCE_TOPIC_BUF];
  const char* topic_parts[] = { PRESENCE_TOPIC_PREFIX, device_id, PRESENCE_TOPIC_DEV_SUB_SUFFIX };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, topic_parts, 3) != AZ_IOT_OK)
  {
    stage_local_error(
        c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_ERR_NOT_ENOUGH_SPACE, "presence topic does not fit");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  c->presence.session_present = session_present;
  /* The connection nonce was generated at CONNECT time (start_connect_attempt)
   * so it could ride the CONNECT username as correlationId; reuse it here as
   * the birth Correlation Data. Do NOT regenerate it, or the username's
   * correlationId and the birth would diverge and the service could not
   * correlate them. The broker answers the SUBSCRIBE, so it gets the SUBACK
   * window; the birth-ack timeout is armed once the birth is published. */
  az_iot_deadline__arm_after_seconds(
      &c->presence.deadline, az_iot_time_mono_ms(), subscription_ack_timeout_seconds(c));

  uint16_t pid = 0;
  az_iot_result r
      = c->active_client->iface->subscribe(c->active_client, topic, AZ_IOT_MQTT_QOS_1, &pid);
  if (r != AZ_IOT_OK)
  {
    stage_local_error(c, AZ_IOT_CONN_SCOPE_HUB, r, "presence subscribe() failed");
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

  const char* device_id = c->opts.device_id ? c->opts.device_id : "";
  char topic[AZ_IOT_PRESENCE_TOPIC_BUF];
  const char* topic_parts[] = { PRESENCE_TOPIC_PREFIX, device_id, PRESENCE_TOPIC_SRV_SUFFIX };
  if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(topic), NULL, topic_parts, 3) != AZ_IOT_OK)
  {
    stage_local_error(
        c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_ERR_NOT_ENOUGH_SPACE, "presence topic does not fit");
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
  msg.user_properties = az_iot_mqtt_user_properties_from_array(&type_prop, 1);
  msg.correlation_data = c->presence.nonce;
  msg.correlation_data_len = PRESENCE_NONCE_LEN;

  uint16_t pid = 0;
  az_iot_result r = c->active_client->iface->publish(c->active_client, &msg, &pid);
  if (r != AZ_IOT_OK)
  {
    stage_local_error(c, AZ_IOT_CONN_SCOPE_HUB, r, "presence birth publish() failed");
    return r;
  }

  /* The birth is on the wire: arm the escalating birth-ack timeout. Its attempt
   * is one past the birth-acks missed in a row. */
  uint32_t attempt
      = az_iot_defensive_timeout__next_attempt(c->presence.consecutive_birth_ack_timeouts);
  c->presence.birth_ack_timeout_ms = az_iot_connection_client__defensive_timeout_ms(c, attempt);
  az_iot_deadline__arm_after_ms(
      &c->presence.deadline, az_iot_time_mono_ms(), c->presence.birth_ack_timeout_ms);
  c->presence.phase = PRESENCE_PHASE_BIRTH;
  AZ_IOT_LOG_DEBUGF(
      AZ_IOT_LOG_COMPONENT_CONNECTION,
      "birth published; birth-ack timeout %lu ms (attempt %lu)",
      (unsigned long)c->presence.birth_ack_timeout_ms,
      (unsigned long)attempt);
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

  const char* device_id = c->opts.device_id ? c->opts.device_id : "";
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

  az_iot_mqtt_user_property_view type;
  if (az_iot_mqtt_user_properties_find(&msg->user_properties, PRESENCE_TYPE_KEY, &type)
      != AZ_IOT_OK)
  {
    return false;
  }
  size_t n = strlen(PRESENCE_TYPE_BIRTH_ACK);
  /* Match "birth-ack" exactly or "birth-ack:<schemaVersion>". */
  return type.value_len >= n && memcmp(type.value, PRESENCE_TYPE_BIRTH_ACK, n) == 0
      && (type.value_len == n || type.value[n] == ':');
}

/* Inbound MQTT events are dispatched here (synchronously from process_loop). */
/**
 * @brief Defer finishing the planned hub session end with its own reason, unless this batch
 * already deferred an identity refusal, which keeps its recovery path.
 */
static void defer_planned_hub_session_end_completion(az_iot_connection_client* c)
{
  if (c->deferred != DEFER_NONE && reason_is_identity_refusal(c->deferred_reason))
  {
    return;
  }
  c->deferred = DEFER_PLANNED_END;
  c->deferred_reason = c->planned_hub_session_end.reason;
}

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
          AZ_IOT_LOG_DEBUG(
              AZ_IOT_LOG_COMPONENT_CONNECTION, "connack ignored: close already requested");
          break;
        }

        /* Successful CONNACK: clear the HUB ladder. The DPS ladder is left
         * alone -- it is cleared when a REGISTRATION succeeds, not when the
         * hub this device was already assigned to answers. */
        c->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] = 0;
        c->reconnect_due_ms = 0;
        /* The credential was accepted: kept; a later rejection starts a new pass. */
        c->auth[AZ_IOT_CONN_SCOPE_HUB].pass_from = AZ_IOT_AUTH_SOURCE_NONE;

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
        /* An identity rejection is not a transient transport failure, and it
         * does not say why the identity was refused. apply_deferred() routes
         * it to schedule_identity_recovery() and the identity ladder. In
         * REPROVISION mode the retry is a DPS registration.
         *
         * That is recorded here rather than in the scheduler so that, with
         * retries disabled, the next open() still honours it.
         *
         * First, a rejected credential moves to the next source at once,
         * policy or not; only a pass with every source rejected gets here. */
        if (reason_is_identity_refusal(evt->status) && !c->user_close
            && auth_next_source(c, AZ_IOT_CONN_SCOPE_HUB))
        {
          c->deferred = DEFER_FALLBACK;
          c->deferred_reason = evt->status;
          break;
        }
        note_identity_refusal(c, evt->status);
        c->deferred
            = (az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy) && !c->user_close)
            ? DEFER_RECONNECT
            : DEFER_FAULT;
        c->deferred_reason = evt->status;
      }
      break;

    case AZ_IOT_MQTT_EVT_DISCONNECTED:
      /* An mqttv5 Not authorized DISCONNECT is an identity refusal too. */
      note_identity_refusal(c, evt->status);
      if (c->user_close)
      {
        c->deferred = DEFER_IDLE;
        c->deferred_reason = evt->status;
      }
      else if (planned_hub_session_end_in_progress(c) && !reason_is_identity_refusal(evt->status))
      {
        /* The disconnect a planned session end sent. */
        defer_planned_hub_session_end_completion(c);
      }
      else if (!az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy))
      {
        /* A refusal is a failure, not a clean end of session. */
        c->deferred = reason_is_identity_refusal(evt->status) ? DEFER_FAULT : DEFER_IDLE;
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
      if (c->user_close && planned_hub_session_end_in_progress(c))
      {
        /* close() took over a planned end: it settles IDLE. */
        c->deferred = DEFER_IDLE;
        c->deferred_reason = AZ_IOT_OK;
        break;
      }
      if (planned_hub_session_end_in_progress(c) && c->planned_hub_session_end.reason != AZ_IOT_OK
          && !reason_is_identity_refusal(r))
      {
        /* A failure was already ending the session (e.g. a timeout): it ends with that reason.
         * A SAS renewal's end (AZ_IOT_OK) takes the error like any other session. */
        defer_planned_hub_session_end_completion(c);
        break;
      }
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
          /* The session is already being ended (e.g. its handshake timed out). */
          if (planned_hub_session_end_in_progress(c))
          {
            break;
          }
          /* Same reasoning as the CONNACK case above. On the MQTTv5 path it
           * is the birth-ack, not the CONNACK, that completes the connection,
           * so suppressing only the CONNACK would leave this route able to
           * announce CONNECTED for an attempt the application has already
           * abandoned. A birth-ack the broker sent before close() reached it
           * arrives in a later process_loop batch, when user_close is set and
           * the state is DISCONNECTING. */
          if (c->user_close || c->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_DISCONNECTING)
          {
            AZ_IOT_LOG_DEBUG(
                AZ_IOT_LOG_COMPONENT_CONNECTION, "birth-ack ignored: close already requested");
            break;
          }
          /* An ack whose deadline had passed when this pass began is a miss, left
           * to the timeout path to reconnect and escalate. A pass's waits end by
           * the deadline, but one may dispatch late (e.g. after a DPS wait). */
          if (az_iot_connection_client__deadline_passed_before_pass(c, &c->presence.deadline))
          {
            AZ_IOT_LOG_DEBUG(
                AZ_IOT_LOG_COMPONENT_CONNECTION, "birth-ack ignored: birth-ack timeout expired");
            break;
          }
          if (!presence_decode_birth_ack(c, evt->message->payload, evt->message->payload_len))
          {
            /* Its versions cannot be trusted; the birth-ack timeout ends the session. */
            AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_CONNECTION, "malformed birth-ack ignored");
            break;
          }
          c->presence.phase = PRESENCE_PHASE_DONE;
          /* Arrived in time: the next birth starts the schedule over. */
          c->presence.consecutive_birth_ack_timeouts = 0;
          begin_feature_subscriptions(c);
          break;
        }
        enter_callback(c);
        (void)az_iot_dispatch_route(&c->dispatch, evt->message);
        leave_callback(c);
      }
      break;

    /* Phase 3.1: route PUBACK to the publishing feature client via the
     * correlation table. An unmatched packet_id is a publish sent without an
     * ack callback: success is dropped, failure is logged since no one else
     * will report it. */
    case AZ_IOT_MQTT_EVT_PUBLISH_ACK:
    {
      bool matched = false;
      az_iot_publish_ack_callback cb = NULL;
      az_iot_publish_result_callback result_cb = NULL;
      void* ctx = NULL;
      for (size_t i = 0; i < c->opts.limits.max_pending_publishes; ++i)
      {
        /* No callback yet: reserved by a publish still in progress; not yet matchable. */
        if (c->pending_pubacks[i].in_use
            && (c->pending_pubacks[i].cb != NULL || c->pending_pubacks[i].result_cb != NULL)
            && c->pending_pubacks[i].packet_id == evt->packet_id)
        {
          cb = c->pending_pubacks[i].cb;
          result_cb = c->pending_pubacks[i].result_cb;
          ctx = c->pending_pubacks[i].user_ctx;
          c->pending_pubacks[i].in_use = false;
          c->pending_pubacks[i].cb = NULL;
          c->pending_pubacks[i].result_cb = NULL;
          c->pending_pubacks[i].user_ctx = NULL;
          matched = true;
          break;
        }
      }
      if (evt->status != AZ_IOT_OK)
      {
        /* protocol_code 0 is "not reported": printed as "none" (precision 0 prints no digit). */
        bool const has_code = evt->protocol_code != 0;
        int const reason_len = evt->reason_string != NULL && evt->reason_string_len <= INT32_MAX
            ? (int)evt->reason_string_len
            : 0;
        const char* const reason = reason_len > 0 ? evt->reason_string : "";
        if (matched)
        {
          AZ_IOT_LOG_WARNF(
              AZ_IOT_LOG_COMPONENT_CONNECTION,
              "publish with packet id %u failed (%s, protocol code %s%.*X, reason \"%.*s\")",
              (unsigned)evt->packet_id,
              az_iot_result_to_string(evt->status),
              has_code ? "0x" : "none",
              has_code ? 2 : 0,
              (unsigned)evt->protocol_code,
              reason_len,
              reason);
        }
        else
        {
          AZ_IOT_LOG_ERRORF(
              AZ_IOT_LOG_COMPONENT_CONNECTION,
              "publish with packet id %u failed (%s, protocol code %s%.*X, reason \"%.*s\"); it "
              "had no completion callback",
              (unsigned)evt->packet_id,
              az_iot_result_to_string(evt->status),
              has_code ? "0x" : "none",
              has_code ? 2 : 0,
              (unsigned)evt->protocol_code,
              reason_len,
              reason);
        }
      }
      if (matched)
      {
        complete_publish(c, cb, result_cb, ctx, evt->status, evt);
      }
      /* Any PUBACK frees adapter capacity, whoever published. A callback may withdraw itself. */
      notify_feature_observers_of(c, AZ_IOT_CONNECTION_FEATURE_EVENT_PUBLISH_COMPLETED);
      break;
    }

    /* The dev/presence SUBACK advances the MQTTv5 birth handshake: publish the
     * birth message now that the ack topic is subscribed. Other SUBACKs are
     * absorbed (feature clients don't yet need SUBACK correlation). */
    case AZ_IOT_MQTT_EVT_SUBSCRIBE_ACK:
      if (c->presence.phase == PRESENCE_PHASE_SUBSCRIBING
          && evt->packet_id == c->presence.sub_packet_id && !planned_hub_session_end_in_progress(c))
      {
        /* As for the birth-ack: late only if the deadline passed before this pass. */
        if (az_iot_connection_client__deadline_passed_before_pass(c, &c->presence.deadline))
        {
          AZ_IOT_LOG_DEBUG(
              AZ_IOT_LOG_COMPONENT_CONNECTION, "presence SUBACK ignored: its timeout expired");
          break;
        }
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
            AZ_IOT_LOG_COMPONENT_CONNECTION,
            "SUBACK for an untracked packet id %u; ignoring",
            (unsigned)evt->packet_id);
      }
      break;

    /* UNSUBSCRIBE_ACK gets correlation handlers in later Phase 3 slices when
     * feature clients need to know subscriptions are live. For now absorb. */
    case AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK:
    default:
      break;
  }
}

/**
 * @brief Start a hub attempt.
 *
 * Announces HUB:SETTING_UP before any step that can fail, and HUB:CONNECTING
 * just before the adapter's connect(), so every attempt moves the scope and
 * the caller's failure transition is reported. Each failure stages
 * AZ_IOT_CONN_ERR_SRC_LOCAL detail naming the step.
 *
 * @return AZ_IOT_OK when started, or when close() from inside either
 * announcement cancelled the attempt (the scope is then settled and there is
 * no failure to report); otherwise the failed step's result.
 */
static az_iot_result start_connect_attempt(az_iot_connection_client* c)
{
  /* A new session: no credential is selected yet, so none is reported. */
  if (c->auth[AZ_IOT_CONN_SCOPE_HUB].source != AZ_IOT_AUTH_SOURCE_NONE)
  {
    c->auth[AZ_IOT_CONN_SCOPE_HUB].last_source = c->auth[AZ_IOT_CONN_SCOPE_HUB].source;
  }
  c->auth[AZ_IOT_CONN_SCOPE_HUB].source = AZ_IOT_AUTH_SOURCE_NONE;
  c->auth[AZ_IOT_CONN_SCOPE_HUB].x509_index = 0;
  set_state_to(c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_SETTING_UP, AZ_IOT_OK);

  az_iot_result br = run_feature_client_binds(c);
  if (br != AZ_IOT_OK)
  {
    stage_local_error(c, AZ_IOT_CONN_SCOPE_HUB, br, "feature client bind failed");
    return br;
  }

  az_iot_mqtt_version version = az_iot_mqtt_required_version_for_role(c->session_role);
  const az_iot_mqtt_factory* f = find_factory(c, version);
  if (!f)
  {
    stage_local_error(
        c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_ERR_NOT_SUPPORTED, "no MQTT factory for the hub protocol");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  az_iot_mqtt_client* mc = f->create(f->factory_ctx);
  if (!mc || !mc->iface)
  {
    stage_local_error(
        c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_ERR_INTERNAL, "MQTT factory create() failed");
    return AZ_IOT_ERR_INTERNAL;
  }

  mc->iface->set_inbound_cb(mc, on_mqtt_event, c);

  az_iot_mqtt_connect_options copts = { 0 };
  copts.host = c->opts.iothub_hostname;
  copts.client_id = c->opts.device_id;
  resolve_connect_timings(c, &copts);
  resolve_connect_transport(c, &copts, c->session_role, c->opts.port);
  resolve_session_options(c, &copts, c->session_role);

  /* mqttv3 hub username (see hub_build_username()). MQTTv5 uses its own format. */
  if (c->session_role != AZ_IOT_MQTT_ROLE_HUB_MQTT_V5 && c->opts.iothub_hostname
      && c->opts.device_id)
  {
    /* IoT Hub requires the username: fail locally rather than CONNECT without it. */
    if (!ensure_hub_client(c))
    {
      AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CONNECTION, "invalid host, client ID or model ID");
      mc->iface->destroy(mc);
      stage_local_error(
          c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_ERR_INVALID_ARG, "invalid host, client ID or model ID");
      return AZ_IOT_ERR_INVALID_ARG;
    }
    if (hub_build_username(c, c->hub_username, sizeof(c->hub_username)) != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "AZ_IOT_MQTT_USERNAME_BUF is too small for the MQTTv3 CONNECT username");
      mc->iface->destroy(mc);
      stage_local_error(
          c,
          AZ_IOT_CONN_SCOPE_HUB,
          AZ_IOT_ERR_NOT_ENOUGH_SPACE,
          "MQTTv3 CONNECT username does not fit AZ_IOT_MQTT_USERNAME_BUF");
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    copts.username = c->hub_username;
  }
  else if (
      c->session_role == AZ_IOT_MQTT_ROLE_HUB_MQTT_V5 && c->opts.iothub_hostname
      && c->opts.device_id)
  {
    /* MQTTv5: generate the per-attempt connection nonce now so it
     * rides the CONNECT username (correlationId) and is reused as the birth
     * Correlation Data. The auth webhook denies an empty username. */
    presence_gen_nonce(c, c->presence.nonce);
    if (!presence_build_username(c, c->hub_username, sizeof(c->hub_username)))
    {
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "AZ_IOT_MQTT_USERNAME_BUF is too small for the MQTTv5 CONNECT username");
      mc->iface->destroy(mc);
      stage_local_error(
          c,
          AZ_IOT_CONN_SCOPE_HUB,
          AZ_IOT_ERR_NOT_ENOUGH_SPACE,
          "MQTTv5 CONNECT username does not fit AZ_IOT_MQTT_USERNAME_BUF");
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    copts.username = c->hub_username;
  }

  /* Credential, from c->auth[HUB].first (see auth_next_source()): the
   * provider's X.509 identity -- the issued OPERATIONAL one (from this DPS
   * session, persisted by the provider on a prior run, or supplied for a direct
   * hub connection), else BOOTSTRAP -- then the primary and the secondary key
   * of hub_auth, then its on_sas_token_required. It never goes plaintext. */
  az_iot_auth_source first = c->auth[AZ_IOT_CONN_SCOPE_HUB].first;
  bool hub_has_key = c->auth[AZ_IOT_CONN_SCOPE_HUB].primary_key_len > 0;
  bool hub_has_sas
      = hub_has_key || auth_of(c, AZ_IOT_CONN_SCOPE_HUB)->sas.on_sas_token_required != NULL;
  if (!c->opts.certificate_provider && !hub_has_sas)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "no certificate provider and no SAS key or token callback; refusing to connect");
    mc->iface->destroy(mc);
    stage_local_error(
        c,
        AZ_IOT_CONN_SCOPE_HUB,
        AZ_IOT_ERR_CREDENTIAL_INCOMPLETE,
        "no certificate provider and no SAS key or token callback");
    return AZ_IOT_ERR_CREDENTIAL_INCOMPLETE;
  }
  if (c->opts.certificate_provider)
  {
    az_iot_certificate_provider* prov = c->opts.certificate_provider;
    az_iot_certificate_material mat = { 0 };
    uint8_t x509_index
        = first == AZ_IOT_AUTH_SOURCE_X509 ? c->auth[AZ_IOT_CONN_SCOPE_HUB].first_x509_index : 0u;
    recheck_kept_certificate(c, AZ_IOT_CONN_SCOPE_HUB, &first, &x509_index);
    az_iot_result lr = load_x509(c, AZ_IOT_CONN_SCOPE_HUB, x509_index, &mat);
    if (x509_index == 0)
    {
      c->auth[AZ_IOT_CONN_SCOPE_HUB].x509_available = lr == AZ_IOT_OK;
    }
    /* Only an absent certificate selects SAS; other failures fail the attempt. */
    if (lr == AZ_IOT_ERR_NOT_FOUND && first == AZ_IOT_AUTH_SOURCE_X509)
    {
      /* The certificate selected (by fallback, or kept after connecting) is
       * gone: the pass ends, failing this attempt, rather than trying the
       * sources before it again unpaced. */
      AZ_IOT_LOG_WARN(
          AZ_IOT_LOG_COMPONENT_CONNECTION, "the certificate selected for fallback is gone");
      c->auth[AZ_IOT_CONN_SCOPE_HUB].first = AZ_IOT_AUTH_SOURCE_NONE;
      c->auth[AZ_IOT_CONN_SCOPE_HUB].pass_from = AZ_IOT_AUTH_SOURCE_NONE;
      mc->iface->destroy(mc);
      stage_local_error(c, AZ_IOT_CONN_SCOPE_HUB, lr, "certificate selected for fallback is gone");
      return lr;
    }
    if (lr == AZ_IOT_ERR_NOT_FOUND && hub_has_sas)
    {
      AZ_IOT_LOG_DEBUG(AZ_IOT_LOG_COMPONENT_CONNECTION, "no certificate; using SAS");
      keep_provider_trust(&copts, &mat);
    }
    else if (lr != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERRORF(
          AZ_IOT_LOG_COMPONENT_CONNECTION, "certificate provider load() failed (%d)", (int)lr);
      mc->iface->destroy(mc);
      stage_local_error(c, AZ_IOT_CONN_SCOPE_HUB, lr, "certificate provider load() failed");
      return lr;
    }
    else if (first > AZ_IOT_AUTH_SOURCE_X509)
    {
      /* X.509 was rejected in this pass: SAS, with the provider's trust anchors. */
      keep_provider_trust(&copts, &mat);
      hold_material(c, AZ_IOT_CONN_SCOPE_HUB, &mat);
    }
    else
    {
      az_iot_result cr = apply_certificate_material(&copts, &mat, prov);
      hold_material(c, AZ_IOT_CONN_SCOPE_HUB, &mat);
      if (cr != AZ_IOT_OK)
      {
        release_material(c, AZ_IOT_CONN_SCOPE_HUB);
        mc->iface->destroy(mc);
        stage_local_error(c, AZ_IOT_CONN_SCOPE_HUB, cr, "certificate material rejected");
        return cr;
      }
      c->auth[AZ_IOT_CONN_SCOPE_HUB].source = AZ_IOT_AUTH_SOURCE_X509;
      c->auth[AZ_IOT_CONN_SCOPE_HUB].x509_index = x509_index;
      apply_trusted_ca(c, &copts);
    }
  }
  if (c->auth[AZ_IOT_CONN_SCOPE_HUB].source == AZ_IOT_AUTH_SOURCE_NONE
      && (first == AZ_IOT_AUTH_SOURCE_USER_PROVIDED || !hub_has_key))
  {
    bool pending = false;
    apply_user_token(c, AZ_IOT_CONN_SCOPE_HUB, &copts, &pending);
    if (pending)
    {
      /* Waiting for the token: the attempt stays in SETTING_UP. */
      release_material(c, AZ_IOT_CONN_SCOPE_HUB);
      mc->iface->destroy(mc);
      return AZ_IOT_OK;
    }
  }
  else if (c->auth[AZ_IOT_CONN_SCOPE_HUB].source == AZ_IOT_AUTH_SOURCE_NONE)
  {
    az_iot_result sr = apply_sas_key(
        c,
        AZ_IOT_CONN_SCOPE_HUB,
        first == AZ_IOT_AUTH_SOURCE_SECONDARY_KEY ? first : AZ_IOT_AUTH_SOURCE_PRIMARY_KEY,
        &copts);
    if (sr != AZ_IOT_OK)
    {
      release_material(c, AZ_IOT_CONN_SCOPE_HUB);
      mc->iface->destroy(mc);
      stage_local_error(c, AZ_IOT_CONN_SCOPE_HUB, sr, "SAS token signing failed");
      return sr;
    }
  }
  /* Another source was selected: a token request for this scope is moot. */
  if (c->auth[AZ_IOT_CONN_SCOPE_HUB].source != AZ_IOT_AUTH_SOURCE_USER_PROVIDED
      && c->sas_token_request[AZ_IOT_CONN_SCOPE_HUB].request_id != 0)
  {
    clear_sas_token_request(c, AZ_IOT_CONN_SCOPE_HUB);
  }
  /* An attempt from the start of the order begins the pass, even after a
   * failure that was not a rejection: the available sources may have changed. */
  if (c->auth[AZ_IOT_CONN_SCOPE_HUB].pass_from == AZ_IOT_AUTH_SOURCE_NONE
      || first == AZ_IOT_AUTH_SOURCE_NONE)
  {
    c->auth[AZ_IOT_CONN_SCOPE_HUB].pass_from = c->auth[AZ_IOT_CONN_SCOPE_HUB].source;
    c->auth[AZ_IOT_CONN_SCOPE_HUB].pass_from_x509_index = c->auth[AZ_IOT_CONN_SCOPE_HUB].x509_index;
  }

  set_state_to(c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_CONNECTING, AZ_IOT_OK);
  if (c->state[AZ_IOT_CONN_SCOPE_HUB] != AZ_IOT_CONN_STATE_CONNECTING || c->active_client != NULL)
  {
    AZ_IOT_LOG_DEBUG(
        AZ_IOT_LOG_COMPONENT_CONNECTION, "attempt closed from the state callback; abandoned");
    sas_wipe_token(c);
    release_material(c, AZ_IOT_CONN_SCOPE_HUB);
    mc->iface->destroy(mc);
    return AZ_IOT_OK;
  }
  az_iot_result r = mc->iface->connect(mc, &copts);
  sas_wipe_token(c);
  release_material(c, AZ_IOT_CONN_SCOPE_HUB);
  if (r != AZ_IOT_OK)
  {
    mc->iface->destroy(mc);
    stage_local_error(c, AZ_IOT_CONN_SCOPE_HUB, r, "MQTT adapter connect() failed");
    return r;
  }
  c->active_client = mc;
  return AZ_IOT_OK;
}

/** @brief Ends the hub session with @p reason: reconnects under the
 * reconnection policy, or faults without one or once close() is requested. */
static void end_hub_session_with_failure(az_iot_connection_client* c, az_iot_result reason)
{
  if (az_iot_retry_policy_is_enabled(&c->opts.reconnection_policy) && !c->user_close)
  {
    schedule_reconnect(c, AZ_IOT_CONN_SCOPE_HUB, reason);
  }
  else
  {
    teardown_active(c);
    set_state_to(c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_FAULTED, reason);
  }
}

/** @brief Whether a planned hub session end awaits its disconnect. */
static bool planned_hub_session_end_in_progress(const az_iot_connection_client* c)
{
  return az_iot_deadline__is_set(&c->planned_hub_session_end.disconnect_deadline);
}

/**
 * @brief Ends the hub session a planned end disconnected. A failure reason ends
 * it with end_hub_session_with_failure(); AZ_IOT_OK (SAS renewal) reconnects at
 * once with a new token: RETRY_PENDING with reason AZ_IOT_OK, no policy delay
 * or attempt.
 */
static void finish_planned_hub_session_end(az_iot_connection_client* c)
{
  az_iot_result reason = c->planned_hub_session_end.reason;
  az_iot_deadline__clear(&c->planned_hub_session_end.disconnect_deadline);
  if (reason != AZ_IOT_OK)
  {
    end_hub_session_with_failure(c, reason);
    return;
  }
  end_session(c, AZ_IOT_CONNECTION_SESSION_END_PLANNED);
  uint64_t now = az_iot_time_mono_ms();
  c->reconnect_due_ms = now != 0 ? now : 1u;
  set_state_to(c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_RETRY_PENDING, AZ_IOT_OK);
}

/**
 * @brief Ends the live hub session with a DISCONNECT. It is finished by
 * finish_planned_hub_session_end() when the adapter reports the disconnect, or
 * after PLANNED_DISCONNECT_TIMEOUT_MS.
 */
static void start_planned_hub_session_end(az_iot_connection_client* c, az_iot_result reason)
{
  c->planned_hub_session_end.reason = reason;
  az_iot_deadline__arm_after_ms(
      &c->planned_hub_session_end.disconnect_deadline,
      az_iot_time_mono_ms(),
      PLANNED_DISCONNECT_TIMEOUT_MS);
  if (c->active_client->iface->disconnect(c->active_client) != AZ_IOT_OK)
  {
    finish_planned_hub_session_end(c);
  }
}

/** @brief Starts renewing the hub's SAS token. MQTT 3.1.1 cannot re-authenticate
 * a live session, so the session is ended and reconnected with a new token. */
static void start_sas_token_renewal(az_iot_connection_client* c)
{
  AZ_IOT_LOG_INFO(AZ_IOT_LOG_COMPONENT_CONNECTION, "renewing the SAS token; reconnecting");
  c->sas_token_renewal_due_ms[AZ_IOT_CONN_SCOPE_HUB] = 0;
  c->sas_token_renewal_due_unix_seconds[AZ_IOT_CONN_SCOPE_HUB] = 0;
  c->sas_token_renewal_in_progress[AZ_IOT_CONN_SCOPE_HUB] = true;
  start_planned_hub_session_end(c, AZ_IOT_OK);
}

/** @brief Whether @p source authenticates with a SAS token. */
static bool is_sas_token_source(az_iot_auth_source source)
{
  return source == AZ_IOT_AUTH_SOURCE_PRIMARY_KEY || source == AZ_IOT_AUTH_SOURCE_SECONDARY_KEY
      || source == AZ_IOT_AUTH_SOURCE_USER_PROVIDED;
}

/** @brief Asks again for @p scope's renewal token after
 * SAS_TOKEN_RENEWAL_RETRY_MS. The session stays up meanwhile. */
static void retry_sas_token_renewal_later(
    az_iot_connection_client* c,
    az_iot_connection_scope scope)
{
  c->sas_token_renewal_due_ms[scope] = az_iot_time_mono_ms() + (uint64_t)SAS_TOKEN_RENEWAL_RETRY_MS;
  c->sas_token_renewal_due_unix_seconds[scope] = 0;
  /* Held even if the token expires first and the session reconnects. */
  c->sas_token_ask_after_ms[scope] = c->sas_token_renewal_due_ms[scope];
}

/** @brief Opens a request for @p scope's renewal token, or makes a held one
 * it; do_work() asks for it while the session stays up, and the renewal starts
 * once it is READY. */
static void request_sas_token_for_renewal(
    az_iot_connection_client* c,
    az_iot_connection_scope scope)
{
  c->sas_token_renewal_due_ms[scope] = 0;
  c->sas_token_renewal_due_unix_seconds[scope] = 0;
  if (c->sas_token_request[scope].request_id == 0)
  {
    open_sas_token_request(c, scope, true, 0);
  }
  else if (c->sas_token_request[scope].held)
  {
    /* Supplied while the session could not be renewed (e.g. registering). */
    c->sas_token_request[scope].held = false;
    c->sas_token_request[scope].for_renewal = true;
  }
}

/** @brief Whether @p scope's renewal token was delivered and awaits its renewal. */
static bool sas_token_renewal_ready(
    const az_iot_connection_client* c,
    az_iot_connection_scope scope)
{
  return c->sas_token_request[scope].request_id != 0 && c->sas_token_request[scope].for_renewal
      && c->sas_token_request[scope].ready && c->sas_token_holder == (uint8_t)(scope + 1)
      && c->auth[scope].source == AZ_IOT_AUTH_SOURCE_USER_PROVIDED;
}

/**
 * @brief Whether the provisioning session can be renewed now: up on a SAS
 * token, wanted, and not registering (a registration ends or keeps it first).
 */
static bool dps_session_renewable(const az_iot_connection_client* c)
{
  return az_iot_connection_client__dps_session_ready(c) && !c->dps_pending_finalize
      && !c->dps_registration_ref && dps_session_demanded(c)
      && is_sas_token_source(c->auth[AZ_IOT_CONN_SCOPE_DPS].source);
}

/** @brief Whether @p scope's session is up on a SAS token it may renew. */
static bool sas_session_renewable(const az_iot_connection_client* c, az_iot_connection_scope scope)
{
  if (scope == AZ_IOT_CONN_SCOPE_DPS)
  {
    return dps_session_renewable(c);
  }
  return c->active_client != NULL && !c->user_close
      && c->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_CONNECTED
      && is_sas_token_source(c->auth[AZ_IOT_CONN_SCOPE_HUB].source);
}

/** @brief Whether @p scope's user-provided token reached its expiry. */
static bool user_sas_token_expired(
    const az_iot_connection_client* c,
    az_iot_connection_scope scope,
    uint64_t now)
{
  return c->auth[scope].source == AZ_IOT_AUTH_SOURCE_USER_PROVIDED
      && ((c->sas_token_expiry_ms[scope] != 0 && now >= c->sas_token_expiry_ms[scope])
          || (c->sas_token_expiry_unix_seconds[scope] != 0
              && unix_now(c) >= c->sas_token_expiry_unix_seconds[scope]));
}

/** @brief Whether @p scope's renewal is due. */
static bool sas_token_renewal_due(
    const az_iot_connection_client* c,
    az_iot_connection_scope scope,
    uint64_t now)
{
  return (c->sas_token_renewal_due_ms[scope] != 0 && now >= c->sas_token_renewal_due_ms[scope])
      || (c->sas_token_renewal_due_unix_seconds[scope] != 0
          && unix_now(c) >= c->sas_token_renewal_due_unix_seconds[scope]);
}

/**
 * @brief Whether @p scope's delivered renewal token expired before its use;
 * if so, it is asked for again and the session stays up.
 */
static bool renewal_token_expired_before_use(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    uint64_t now)
{
  if (user_token_age_ms(c, scope, now, unix_now(c))
      < (uint64_t)c->sas_token_request[scope].valid_seconds * 1000u)
  {
    return false;
  }
  c->sas_token_request[scope].asked = false;
  c->sas_token_request[scope].ready = false;
  c->sas_token_holder = 0;
  sas_wipe_token(c);
  return true;
}

/**
 * @brief Renews the provisioning session's SAS token: MQTT 3.1.1 cannot
 * re-authenticate, so the session is closed and opened again at once, with no
 * policy delay: DISCONNECTING, SETTING_UP, CONNECTING, then CONNECTED, flagged
 * as a renewal. A delivered user-provided token is used; otherwise the new
 * session waits for one, as any attempt.
 */
static void renew_dps_session(az_iot_connection_client* c)
{
  AZ_IOT_LOG_INFO(AZ_IOT_LOG_COMPONENT_DPS, "renewing the SAS token; reconnecting");
  az_iot_connection_scope dps = AZ_IOT_CONN_SCOPE_DPS;
  c->sas_token_renewal_due_ms[dps] = 0;
  c->sas_token_renewal_due_unix_seconds[dps] = 0;
  c->sas_token_expiry_ms[dps] = 0;
  c->sas_token_expiry_unix_seconds[dps] = 0;
  if (c->sas_token_request[dps].request_id != 0)
  {
    /* The renewal request becomes the new attempt's. */
    c->sas_token_request[dps].for_renewal = false;
    c->sas_token_request[dps].held
        = c->sas_token_request[dps].ready && c->sas_token_holder == (uint8_t)(dps + 1);
    if (!c->sas_token_request[dps].held)
    {
      az_iot_mqtt_connect_options timings = { 0 };
      resolve_connect_timings(c, &timings);
      uint64_t now = az_iot_time_mono_ms();
      uint64_t ask = c->sas_token_ask_after_ms[dps] > now ? c->sas_token_ask_after_ms[dps] : now;
      c->sas_token_request[dps].deadline_ms
          = ask + (uint64_t)timings.connect_timeout_seconds * 1000u;
    }
  }
  c->sas_token_renewal_in_progress[dps] = true;
  if (c->dps_mqtt->iface && c->dps_mqtt->iface->disconnect)
  {
    (void)c->dps_mqtt->iface->disconnect(c->dps_mqtt);
  }
  dps_teardown_mqtt(c);
  /* As dps_close_session(): a session asked for from the callback starts anew. */
  c->dps_phase = DPS_PHASE_NONE;
  set_state_to(c, dps, AZ_IOT_CONN_STATE_DISCONNECTING, AZ_IOT_OK);
  /* A session the callback started continues the renewal (set_state_to()
   * keeps the flag through CONNECTED). */
  if (c->state[dps] != AZ_IOT_CONN_STATE_DISCONNECTING)
  {
    return;
  }
  if (!dps_session_demanded(c))
  {
    set_state_to(c, dps, AZ_IOT_CONN_STATE_IDLE, AZ_IOT_OK);
    return;
  }
  az_iot_result r = dps_start(c);
  if (r != AZ_IOT_OK && !c->dps_start_cancelled)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_DPS, "could not reopen the provisioning session (%d)", (int)r);
    /* As a failed dps_session_ensure() start. */
    dps_user_retry_schedule(c, 0);
    set_state_to(c, dps, AZ_IOT_CONN_STATE_IDLE, r);
  }
}

/**
 * @brief Called from do_work(): renews the provisioning session's SAS token
 * when due, when its user-provided token is delivered, or when that token
 * expired with its replacement still pending.
 */
static void process_dps_sas_token_renewal(az_iot_connection_client* c)
{
  az_iot_connection_scope dps = AZ_IOT_CONN_SCOPE_DPS;
  if (!dps_session_renewable(c))
  {
    return;
  }
  uint64_t now = az_iot_time_mono_ms();
  if (sas_token_renewal_ready(c, dps))
  {
    if (!renewal_token_expired_before_use(c, dps, now))
    {
      renew_dps_session(c);
    }
    return;
  }
  if (user_sas_token_expired(c, dps, now))
  {
    renew_dps_session(c);
    return;
  }
  if (sas_token_renewal_due(c, dps, now))
  {
    if (c->auth[dps].source == AZ_IOT_AUTH_SOURCE_USER_PROVIDED)
    {
      request_sas_token_for_renewal(c, dps);
    }
    else
    {
      renew_dps_session(c);
    }
  }
}

/**
 * @brief Called from do_work(): starts a SAS token renewal of the connected
 * hub when due or when its user-provided token is delivered, reconnects one
 * whose disconnect was not reported in time.
 */
static void process_sas_token_renewal(az_iot_connection_client* c)
{
  az_iot_connection_scope hub = AZ_IOT_CONN_SCOPE_HUB;
  if (!sas_session_renewable(c, hub))
  {
    return;
  }
  uint64_t now = az_iot_time_mono_ms();
  /* A session already ending gets its new token on the reconnect. */
  if (c->sas_token_renewal_in_progress[hub] || planned_hub_session_end_in_progress(c))
  {
    return;
  }
  if (sas_token_renewal_ready(c, hub))
  {
    if (!renewal_token_expired_before_use(c, hub, now))
    {
      start_sas_token_renewal(c);
    }
    return;
  }
  /* The token expired while its replacement is still pending: the session
   * ends, and the reconnect waits for the token. */
  if (user_sas_token_expired(c, hub, now))
  {
    c->sas_token_expiry_ms[hub] = 0;
    c->sas_token_expiry_unix_seconds[hub] = 0;
    start_sas_token_renewal(c);
    return;
  }
  if (sas_token_renewal_due(c, hub, now))
  {
    if (c->auth[hub].source == AZ_IOT_AUTH_SOURCE_USER_PROVIDED)
    {
      request_sas_token_for_renewal(c, hub);
    }
    else
    {
      start_sas_token_renewal(c);
    }
  }
}

/**
 * @brief Fails @p scope's attempt that waited for a user-provided token, as a
 * start failure: retried under reconnection_policy.
 */
static void fail_sas_token_attempt(
    az_iot_connection_client* c,
    az_iot_connection_scope scope,
    az_iot_result reason)
{
  if (scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    if (c->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_SETTING_UP && c->active_client == NULL)
    {
      c->sas_token_attempt_failed = true;
      schedule_reconnect(c, AZ_IOT_CONN_SCOPE_HUB, reason);
      c->sas_token_attempt_failed = false;
    }
  }
  else if (c->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_SETTING_UP && c->dps_mqtt == NULL)
  {
    if (c->dps_registration_ref)
    {
      /* As any registration that failed to start: re-registered, or the
       * cached hub tried; see do_work(). */
      c->dps_registration_ref = false;
      c->needs_reprovision = c->needs_reprovision || c->opts.iothub_hostname == NULL;
      schedule_reconnect(c, AZ_IOT_CONN_SCOPE_DPS, reason);
    }
    else
    {
      if (dps_session_demanded(c))
      {
        dps_user_retry_schedule(c, 0);
      }
      set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, reason);
    }
  }
}

/**
 * @brief Resumes @p scope's attempt with its delivered user-provided token. A
 * failure to start is retried as in do_work().
 */
static void resume_sas_token_attempt(az_iot_connection_client* c, az_iot_connection_scope scope)
{
  az_iot_result r;
  if (scope == AZ_IOT_CONN_SCOPE_HUB)
  {
    if (c->state[AZ_IOT_CONN_SCOPE_HUB] != AZ_IOT_CONN_STATE_SETTING_UP || c->active_client != NULL)
    {
      clear_sas_token_request(c, scope);
      return;
    }
    if (identity_recovery_expired(c))
    {
      clear_sas_token_request(c, scope);
      stop_identity_recovery(c, scope);
      return;
    }
    r = start_connect_attempt(c);
    if (r != AZ_IOT_OK)
    {
      schedule_reconnect(c, AZ_IOT_CONN_SCOPE_HUB, r);
    }
    return;
  }
  if (c->state[AZ_IOT_CONN_SCOPE_DPS] != AZ_IOT_CONN_STATE_SETTING_UP || c->dps_mqtt != NULL)
  {
    clear_sas_token_request(c, scope);
    return;
  }
  if (c->dps_registration_ref && identity_recovery_expired(c))
  {
    clear_sas_token_request(c, scope);
    c->dps_registration_ref = false;
    stop_identity_recovery(c, scope);
    return;
  }
  c->dps_start_cancelled = false;
  r = dps_connect_session(c);
  if (r == AZ_IOT_OK)
  {
    if (c->dps_mqtt != NULL && c->dps_registration_ref)
    {
      /* Provisioning is under way now; see open(). */
      c->needs_reprovision = false;
    }
    return;
  }
  if (c->dps_start_cancelled)
  {
    return;
  }
  if (c->dps_registration_ref)
  {
    c->dps_registration_ref = false;
    c->needs_reprovision = c->needs_reprovision || c->opts.iothub_hostname == NULL;
    schedule_reconnect(c, AZ_IOT_CONN_SCOPE_DPS, r);
    return;
  }
  if (dps_session_demanded(c))
  {
    dps_user_retry_schedule(c, 0);
  }
  if (c->state[AZ_IOT_CONN_SCOPE_DPS] != AZ_IOT_CONN_STATE_IDLE)
  {
    set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, r);
  }
}

/** @brief Whether @p scope's request for an attempt is past its deadline. */
static bool sas_token_request_expired(
    const az_iot_connection_client* c,
    az_iot_connection_scope scope)
{
  return c->sas_token_request[scope].request_id != 0 && !c->sas_token_request[scope].for_renewal
      && c->sas_token_request[scope].deadline_ms != 0
      && az_iot_time_mono_ms() >= c->sas_token_request[scope].deadline_ms;
}

/** @brief Fails @p scope's attempt whose token was not there in time. */
static void fail_expired_sas_token_request(
    az_iot_connection_client* c,
    az_iot_connection_scope scope)
{
  AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_CONNECTION, "no SAS token supplied in time");
  if (c->sas_token_request[scope].ready && c->sas_token_holder == (uint8_t)(scope + 1))
  {
    /* Supplied after the deadline: kept for the retry. */
    c->sas_token_request[scope].held = true;
    c->sas_token_request[scope].deadline_ms = 0;
  }
  else
  {
    clear_sas_token_request(c, scope);
  }
  stage_local_error(c, scope, AZ_IOT_ERR_TIMEOUT, "no SAS token supplied in time");
  fail_sas_token_attempt(c, scope, AZ_IOT_ERR_TIMEOUT);
}

/**
 * @brief Moves @p scope's user-provided token request on: calls the callback
 * for a new one. A token resumes the attempt waiting for it, or is left for
 * process_sas_token_renewal(). No token within connect_timeout_seconds fails
 * the attempt; a renewal waits while the session stays up. A token supplied
 * unasked waits for the next attempt.
 */
static void process_sas_token_request_of(az_iot_connection_client* c, az_iot_connection_scope scope)
{
  if (c->sas_token_request[scope].request_id == 0)
  {
    return;
  }
  bool for_renewal = c->sas_token_request[scope].for_renewal;
  if (c->sas_token_request[scope].held)
  {
    /* Waits for the next attempt, unless its token was replaced. */
    if (c->sas_token_holder != (uint8_t)(scope + 1))
    {
      clear_sas_token_request(c, scope);
    }
    return;
  }
  if (c->sas_token_request[scope].ready && c->sas_token_holder != (uint8_t)(scope + 1))
  {
    /* Its token was replaced: ask again. */
    c->sas_token_request[scope].asked = false;
    c->sas_token_request[scope].ready = false;
  }
  if (!for_renewal && scope == AZ_IOT_CONN_SCOPE_DPS && !dps_refs_held(c))
  {
    /* Nobody wants the provisioning session any more. */
    clear_sas_token_request(c, scope);
    set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, AZ_IOT_OK);
    return;
  }
  /* Past its connect_timeout_seconds, the attempt fails whatever the
   * outcome, before the callback and after it. */
  if (sas_token_request_expired(c, scope))
  {
    fail_expired_sas_token_request(c, scope);
    return;
  }
  if (!c->sas_token_request[scope].asked && c->sas_token_ask_after_ms[scope] != 0)
  {
    if (az_iot_time_mono_ms() < c->sas_token_ask_after_ms[scope])
    {
      return;
    }
    c->sas_token_ask_after_ms[scope] = 0;
  }
  if (!c->sas_token_request[scope].asked)
  {
    az_iot_result error = AZ_IOT_OK;
    user_token_outcome outcome = ask_user_token(c, scope, &error);
    if (outcome == USER_TOKEN_ABANDONED)
    {
      return;
    }
    if (outcome == USER_TOKEN_FAILED)
    {
      clear_sas_token_request(c, scope);
      if (for_renewal)
      {
        retry_sas_token_renewal_later(c, scope);
        return;
      }
      stage_local_error(c, scope, error, "SAS token request failed");
      fail_sas_token_attempt(c, scope, error);
      return;
    }
    if (sas_token_request_expired(c, scope))
    {
      fail_expired_sas_token_request(c, scope);
      return;
    }
  }
  if (!c->sas_token_request[scope].ready)
  {
    return;
  }
  if (for_renewal)
  {
    /* Started by process_sas_token_renewal(), after process_loop() has
     * drained events already queued; otherwise held for the next attempt. */
    return;
  }
  resume_sas_token_attempt(c, scope);
}

/** @brief Called from do_work(): see process_sas_token_request_of(). */
static void process_sas_token_request(az_iot_connection_client* c)
{
  process_sas_token_request_of(c, AZ_IOT_CONN_SCOPE_DPS);
  process_sas_token_request_of(c, AZ_IOT_CONN_SCOPE_HUB);
}

/**
 * @brief @p timeout_ms, capped so a process_loop() wait ends by the next
 * connection deadline: a SAS token request's bound (none while one waits to be
 * asked, as do_work() asks it), a session's SAS token renewal, a planned hub
 * session end's disconnect bound, the presence SUBACK or birth-ack timeout, or a
 * feature observer's NEXT_DEADLINE.
 */
static uint32_t limit_wait_to_connection_deadlines(az_iot_connection_client* c, uint32_t timeout_ms)
{
  uint64_t now = az_iot_time_mono_ms();
  uint64_t remaining = UINT64_MAX;
  for (int i = 0; i < (int)AZ_IOT_CONN_SCOPE_COUNT; ++i)
  {
    if (c->sas_token_request[i].request_id == 0 || c->sas_token_request[i].for_renewal)
    {
      continue;
    }
    uint64_t due = c->sas_token_request[i].asked ? c->sas_token_request[i].deadline_ms : now;
    if (!c->sas_token_request[i].asked && c->sas_token_ask_after_ms[i] > now)
    {
      due = c->sas_token_ask_after_ms[i];
    }
    if (due != 0)
    {
      uint64_t left = due > now ? due - now : 0;
      remaining = left < remaining ? left : remaining;
    }
  }
  const az_iot_connection_scope scopes[] = { AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_SCOPE_HUB };
  for (size_t i = 0; i < sizeof(scopes) / sizeof(scopes[0]); ++i)
  {
    az_iot_connection_scope scope = scopes[i];
    if (!sas_session_renewable(c, scope))
    {
      continue;
    }
    /* A planned end in progress holds the hub's renewal; its bound is counted below. */
    bool in_progress = c->sas_token_renewal_in_progress[scope]
        || (scope == AZ_IOT_CONN_SCOPE_HUB && planned_hub_session_end_in_progress(c));
    uint64_t deadline = in_progress ? 0 : c->sas_token_renewal_due_ms[scope];
    if (!in_progress && c->sas_token_expiry_ms[scope] != 0
        && (deadline == 0 || c->sas_token_expiry_ms[scope] < deadline))
    {
      deadline = c->sas_token_expiry_ms[scope];
    }
    if (deadline != 0)
    {
      uint64_t left = deadline > now ? deadline - now : 0;
      remaining = left < remaining ? left : remaining;
    }
    if (in_progress)
    {
      continue;
    }
    if (sas_token_renewal_ready(c, scope))
    {
      remaining = 0;
    }
    uint64_t unix_seconds = unix_now(c);
    const uint64_t unix_deadlines[]
        = { c->sas_token_renewal_due_unix_seconds[scope], c->sas_token_expiry_unix_seconds[scope] };
    for (size_t k = 0; k < sizeof(unix_deadlines) / sizeof(unix_deadlines[0]); ++k)
    {
      if (unix_deadlines[k] != 0)
      {
        uint64_t left
            = unix_deadlines[k] > unix_seconds ? (unix_deadlines[k] - unix_seconds) * 1000u : 0;
        remaining = left < remaining ? left : remaining;
      }
    }
  }
  uint64_t planned_end_left
      = az_iot_deadline__remaining_ms(&c->planned_hub_session_end.disconnect_deadline, now);
  remaining = planned_end_left < remaining ? planned_end_left : remaining;
  /* Once a planned end is under way the presence deadline has done its job; its bound counts. */
  if ((c->presence.phase == PRESENCE_PHASE_SUBSCRIBING || c->presence.phase == PRESENCE_PHASE_BIRTH)
      && !planned_hub_session_end_in_progress(c))
  {
    uint64_t presence_left = az_iot_deadline__remaining_ms(&c->presence.deadline, now);
    remaining = presence_left < remaining ? presence_left : remaining;
  }
  az_iot_connection_feature_event const deadline_query
      = { AZ_IOT_CONNECTION_FEATURE_EVENT_NEXT_DEADLINE,
          NULL,
          AZ_IOT_CONNECTION_SESSION_END_LOST,
          &remaining };
  notify_feature_observers(c, &deadline_query);
  return (uint64_t)timeout_ms > remaining ? (uint32_t)remaining : timeout_ms;
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
  /* close() took the planned end over before its DISCONNECTED arrived. */
  if (action == DEFER_PLANNED_END && c->user_close)
  {
    action = DEFER_IDLE;
    reason = AZ_IOT_OK;
  }

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
    case DEFER_FALLBACK:
      teardown_active(c);
      retry_with_next_source(c, AZ_IOT_CONN_SCOPE_HUB, reason);
      break;
    case DEFER_PLANNED_END:
      finish_planned_hub_session_end(c);
      break;
    case DEFER_IDLE:
    {
      teardown_active(c);
      c->user_close = false;
      c->retry_attempt[AZ_IOT_CONN_SCOPE_DPS] = 0;
      c->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] = 0;
      c->reconnect_due_ms = 0;
      reset_identity_recovery(c);
      set_state_to(c, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_IDLE, reason);
      break;
    }
    default:
      break;
  }
}

/** @brief Ends a hub session close() is ending, without its DISCONNECTED event: IDLE. */
static void settle_closed_hub_session(az_iot_connection_client* c)
{
  c->deferred = DEFER_IDLE;
  c->deferred_reason = AZ_IOT_OK;
  apply_deferred(c);
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
  opts.identity_recovery.policy = az_iot_connection_client_get_default_identity_recovery_policy();
  opts.identity_recovery.mode = AZ_IOT_IDENTITY_RECOVERY_RETRY_HUB;
  opts.max_unconfirmed_delivery_timeout_ms = AZ_IOT_DEFAULT_MAX_UNCONFIRMED_DELIVERY_TIMEOUT_MS;
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
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION, "init: SAS secondary key without a primary key");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (has_primary && opts->crypto == NULL)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CONNECTION, "init: SAS keys need opts.crypto");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (auth->sas.renewal_percent > 99u)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CONNECTION, "init: SAS renewal_percent above 99");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  return AZ_IOT_OK;
}

/** @brief One configured SAS key, before it is placed in sas_buffer. */
typedef struct
{
  az_iot_connection_scope scope;
  bool secondary;
  const char* base64;
  bool is_group;
  const char* id; /* group-key derivation ID */
} sas_key_ref;

/**
 * @brief Decodes @p key_base64 into @p out; for a group key, replaces it with
 * the device key HMAC-SHA256(group key, @p id). @p scratch (at least
 * AZ_IOT_SAS_KEY_MAX bytes) holds the group key meanwhile and is always wiped;
 * @p out is wiped on failure.
 */
static az_iot_result sas_load_key(
    const az_iot_crypto* crypto,
    const char* key_base64,
    bool is_group_key,
    const char* id,
    uint8_t* out,
    size_t* out_len,
    uint8_t* scratch)
{
  *out_len = 0;
  uint8_t* dest = is_group_key ? scratch : out;
  int32_t written = 0;
  size_t n = strlen(key_base64);
  az_iot_result r = AZ_IOT_ERR_INVALID_ARG;
  if (n <= (size_t)az_base64_get_max_encoded_size(AZ_IOT_SAS_KEY_MAX)
      && az_result_succeeded(az_base64_decode(
          az_span_create(dest, AZ_IOT_SAS_KEY_MAX),
          az_span_create((uint8_t*)(uintptr_t)key_base64, (int32_t)n),
          &written))
      && written > 0)
  {
    r = AZ_IOT_OK;
  }
  if (r == AZ_IOT_OK && is_group_key)
  {
    r = is_nonempty_cstr(id)
        ? az_iot_crypto__hmac_sha256(
              crypto, scratch, (size_t)written, (const uint8_t*)id, strlen(id), out)
        : AZ_IOT_ERR_INVALID_ARG;
    written = AZ_IOT_SHA256_SIZE;
  }
  az_iot_crypto__wipe(scratch, AZ_IOT_SAS_KEY_MAX);
  if (r != AZ_IOT_OK)
  {
    az_iot_crypto__wipe(out, AZ_IOT_SAS_KEY_MAX);
    return r;
  }
  *out_len = (size_t)written;
  return AZ_IOT_OK;
}

/** @brief Lists the configured keys; returns how many (at most 4). */
static size_t sas_key_refs(const az_iot_connection_client* c, sas_key_ref refs[4])
{
  size_t n = 0;
  for (int i = 0; i < (int)AZ_IOT_CONN_SCOPE_COUNT; ++i)
  {
    az_iot_connection_scope scope = (az_iot_connection_scope)i;
    const az_iot_auth* auth
        = scope == AZ_IOT_CONN_SCOPE_DPS ? &c->opts.dps_auth : &c->opts.hub_auth;
    /* A group key derives per device: the registration ID, which DPS also
     * makes the device ID, or device_id for a direct hub connection. */
    const char* id = (scope == AZ_IOT_CONN_SCOPE_DPS || dps_configured(c))
        ? c->opts.dps.registration_id
        : c->opts.device_id;
    const char* keys[2] = { auth->sas.primary_key_base64, auth->sas.secondary_key_base64 };
    for (int k = 0; k < 2 && is_nonempty_cstr(keys[k]); ++k)
    {
      refs[n++] = (sas_key_ref){ .scope = scope,
                                 .secondary = k == 1,
                                 .base64 = keys[k],
                                 .is_group = auth->sas.is_enrollment_group_key,
                                 .id = id };
    }
  }
  return n;
}

/** @brief Whether two key settings decode to the same key. */
static bool sas_same_key(const sas_key_ref* a, const sas_key_ref* b)
{
  return strcmp(a->base64, b->base64) == 0 && a->is_group == b->is_group
      && (!a->is_group
          || (is_nonempty_cstr(a->id) && is_nonempty_cstr(b->id) && strcmp(a->id, b->id) == 0));
}

/**
 * @brief Lays out opts.sas_buffer -- scratch, one slot per distinct key, then
 * the token area -- and decodes the keys into it.
 *
 * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_ENOUGH_SPACE when a key is set and the
 * buffer is missing or too small; AZ_IOT_ERR_INVALID_ARG for a bad key or a
 * group key without an ID.
 */
static az_iot_result sas_load_keys(az_iot_connection_client* c)
{
  sas_key_ref refs[4];
  size_t count = sas_key_refs(c, refs);
  bool user_tokens = c->opts.dps_auth.sas.on_sas_token_required != NULL
      || c->opts.hub_auth.sas.on_sas_token_required != NULL;
  if (count == 0 && !user_tokens)
  {
    return AZ_IOT_OK;
  }
  const uint8_t* slot_of[4] = { NULL, NULL, NULL, NULL };
  size_t len_of[4] = { 0, 0, 0, 0 };
  size_t distinct = 0;
  for (size_t i = 0; i < count; ++i)
  {
    bool shared = false;
    for (size_t j = 0; j < i && !shared; ++j)
    {
      shared = sas_same_key(&refs[i], &refs[j]);
    }
    distinct += shared ? 0u : 1u;
  }
  uint8_t* buf = c->opts.sas_buffer.buffer;
  size_t size = c->opts.sas_buffer.size;
  if (buf == NULL || size < AZ_IOT_SAS_BUFFER_SIZE(distinct, AZ_IOT_SAS_KEY_MAX))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "init: SAS needs opts.sas_buffer of at least "
        "AZ_IOT_SAS_BUFFER_SIZE(keys, AZ_IOT_SAS_KEY_MAX) bytes");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  az_iot_crypto__wipe(buf, size);
  uint8_t* next = buf + SAS_SCRATCH_MAC_SIZE + SAS_SCRATCH_SIG_SIZE;
  uint8_t* token_area = buf + AZ_IOT_SAS_BUFFER_SIZE(distinct, 0);
  for (size_t i = 0; i < count; ++i)
  {
    for (size_t j = 0; j < i && slot_of[i] == NULL; ++j)
    {
      if (sas_same_key(&refs[i], &refs[j]))
      {
        slot_of[i] = slot_of[j];
        len_of[i] = len_of[j];
      }
    }
    if (slot_of[i] == NULL)
    {
      az_iot_result r = sas_load_key(
          c->opts.crypto,
          refs[i].base64,
          refs[i].is_group,
          refs[i].id,
          next,
          &len_of[i],
          token_area);
      if (r != AZ_IOT_OK)
      {
        return r;
      }
      slot_of[i] = next;
      next += AZ_IOT_SAS_KEY_MAX;
    }
    if (refs[i].secondary)
    {
      c->auth[refs[i].scope].secondary_key = slot_of[i];
      c->auth[refs[i].scope].secondary_key_len = len_of[i];
    }
    else
    {
      c->auth[refs[i].scope].primary_key = slot_of[i];
      c->auth[refs[i].scope].primary_key_len = len_of[i];
    }
  }
  c->sas_token = (char*)token_area;
  c->sas_token_size = az_iot_connection_client__sas_token_area(size, distinct);
  c->sas_token_capacity = c->sas_token_size - 1u;
  return AZ_IOT_OK;
}

size_t az_iot_connection_client__sas_token_area(size_t buffer_size, size_t key_count)
{
  size_t area = buffer_size - AZ_IOT_SAS_BUFFER_SIZE(key_count, 0);
  /* Bytes past INT32_MAX are never used; deinit() still wipes them. */
  return area > (size_t)INT32_MAX ? (size_t)INT32_MAX : area;
}

/** @brief Wipes every SAS key, token and scratch byte the client holds. */
static void sas_wipe(az_iot_connection_client* c)
{
  if (c->opts.sas_buffer.buffer != NULL)
  {
    az_iot_crypto__wipe(c->opts.sas_buffer.buffer, c->opts.sas_buffer.size);
  }
  az_iot_crypto__wipe(c->auth, sizeof(c->auth));
}

/* Bound on the counts with no tighter maximum, so the size arithmetic cannot overflow. */
#define LIMIT_MAX_ENTRIES 65535u

/**
 * @brief Writes @p limits with every 0 replaced by its default to @p out.
 * @return false when a value is above its maximum.
 */
static bool limits_resolve(
    const az_iot_connection_client_limits* limits,
    az_iot_connection_client_limits* out)
{
  *out = *limits;
  size_t* const value[] = {
    &out->max_pending_publishes,
    &out->max_publish_reservations,
    &out->max_persistent_subscriptions,
    &out->max_feature_observers,
    &out->max_application_state_observers,
    &out->max_feature_clients,
    &out->max_inbound_handlers,
  };
  static const size_t fallback[] = {
    AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PENDING_PUBLISHES,
    AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PUBLISH_RESERVATIONS,
    AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_PERSISTENT_SUBSCRIPTIONS,
    AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_OBSERVERS,
    AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_APPLICATION_STATE_OBSERVERS,
    AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_FEATURE_CLIENTS,
    AZ_IOT_CONNECTION_CLIENT_DEFAULT_MAX_INBOUND_HANDLERS,
  };
  for (size_t i = 0; i < sizeof(fallback) / sizeof(fallback[0]); ++i)
  {
    if (*value[i] == 0)
    {
      *value[i] = fallback[i];
    }
    if (*value[i] > LIMIT_MAX_ENTRIES)
    {
      return false;
    }
  }
  /* Reservation and subscription indexes are stored in a uint8_t. */
  return out->max_publish_reservations < UINT8_MAX
      && out->max_persistent_subscriptions <= UINT8_MAX;
}

/** @brief Workspace bytes for resolved @p l. */
static size_t workspace_size_of(const az_iot_connection_client_limits* l)
{
  return AZ_IOT_CONNECTION_CLIENT_WORKSPACE_SIZE(
      l->max_pending_publishes,
      l->max_publish_reservations,
      l->max_persistent_subscriptions,
      l->max_feature_observers,
      l->max_application_state_observers,
      l->max_feature_clients,
      l->max_inbound_handlers);
}

size_t az_iot_connection_client_get_workspace_size(const az_iot_connection_client_limits* limits)
{
  az_iot_connection_client_limits resolved;
  return limits != NULL && limits_resolve(limits, &resolved) ? workspace_size_of(&resolved) : 0;
}

/** @brief Takes @p count entries of @p entry_size at @p *cursor, which moves past them. */
static void* workspace_take(uint8_t** cursor, size_t count, size_t entry_size)
{
  void* const entries = *cursor;
  *cursor += AZ_IOT_CONNECTION_CLIENT_WORKSPACE_ALIGN(count * entry_size);
  return entries;
}

/** @brief Points @p c's entries into its cleared workspace, laid out as workspace_size_of(). */
static void workspace_assign(az_iot_connection_client* c, uint8_t* workspace)
{
  const az_iot_connection_client_limits* l = &c->opts.limits;
  uint8_t* cursor = (uint8_t*)(((uintptr_t)workspace + 15u) & ~(uintptr_t)15u);
  c->pending_pubacks
      = workspace_take(&cursor, l->max_pending_publishes, sizeof(*c->pending_pubacks));
  c->puback_reservations
      = workspace_take(&cursor, l->max_publish_reservations, sizeof(*c->puback_reservations));
  c->persistent_subs
      = workspace_take(&cursor, l->max_persistent_subscriptions, sizeof(*c->persistent_subs));
  c->subscription_acks
      = workspace_take(&cursor, l->max_persistent_subscriptions, sizeof(*c->subscription_acks));
  c->feature_observers
      = workspace_take(&cursor, l->max_feature_observers, sizeof(*c->feature_observers));
  c->app_state_observers = workspace_take(
      &cursor, l->max_application_state_observers, sizeof(*c->app_state_observers));
  c->feature_client_binds
      = workspace_take(&cursor, l->max_feature_clients, sizeof(*c->feature_client_binds));
  az_iot_dispatch_init(
      &c->dispatch,
      workspace_take(&cursor, l->max_inbound_handlers, sizeof(az_iot_dispatch_entry)),
      l->max_inbound_handlers);
}

AZ_NODISCARD az_iot_result az_iot_connection_client_init(
    az_iot_connection_client* client,
    const az_iot_connection_client_options* opts)
{
  if (!client || !opts)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CONNECTION, "init: invalid arguments");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* UNKNOWN only ever comes back FROM the service; a caller cannot meaningfully
   * declare a profile the SDK does not know how to speak. */
  if (opts->connection_profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V3
      && opts->connection_profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "init: connection_profile is not a profile this SDK speaks");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (opts->crypto != NULL && az_iot_crypto__validate(opts->crypto) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "init: crypto backend has another version or lacks SHA-256");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (opts->trusted_ca.pem != NULL && opts->trusted_ca.path != NULL)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "init: set at most one of trusted_ca.pem and trusted_ca.path");
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
  az_iot_connection_client_limits limits;
  if (!limits_resolve(&opts->limits, &limits))
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CONNECTION, "init: a limit is above its maximum");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  const size_t workspace_size = workspace_size_of(&limits);
  if (opts->workspace == NULL)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "init: no workspace; %lu bytes needed",
        (unsigned long)workspace_size);
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (opts->workspace_size < workspace_size)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "init: the workspace has %lu bytes; %lu needed",
        (unsigned long)opts->workspace_size,
        (unsigned long)workspace_size);
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  memset(client, 0, sizeof(*client));
  client->opts = *opts;
  if (client->opts.max_unconfirmed_delivery_timeout_ms == 0u)
  {
    client->opts.max_unconfirmed_delivery_timeout_ms
        = AZ_IOT_DEFAULT_MAX_UNCONFIRMED_DELIVERY_TIMEOUT_MS;
  }
  client->opts.limits = limits;
  memset(client->opts.workspace, 0, workspace_size);
  workspace_assign(client, client->opts.workspace);
  sv = sas_load_keys(client);
  if (sv != AZ_IOT_OK)
  {
    if (sv == AZ_IOT_ERR_INVALID_ARG)
    {
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "init: a SAS key is not valid base64, too long, or its "
          "group-key ID is missing");
    }
    sas_wipe(client);
    return sv;
  }
  client->state[AZ_IOT_CONN_SCOPE_DPS] = AZ_IOT_CONN_STATE_IDLE;
  client->state[AZ_IOT_CONN_SCOPE_HUB] = AZ_IOT_CONN_STATE_IDLE;
  /* Determine session role early so feature clients can query the profile
   * during their init (which happens before open()). */
  if (client->opts.iothub_hostname
      && client->opts.connection_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
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
  /* A direct connect has no service to ask, and a saved hub (host with DPS
   * configured) declares its profile, so the seed above is the answer until a
   * registration replaces it. */
  client->connection_profile_resolved
      = !dps_configured(client) || is_nonempty_cstr(client->opts.iothub_hostname);
  /* send_csr() needs opts.csr_payload_buffer, so its presence is what marks a device that may
   * renew. Reserving now, with nothing in flight, cannot fail and keeps every renewal tracked. */
  if (az_span_size(client->opts.csr_payload_buffer) > 0)
  {
    sv = az_iot_connection_client__reserve_pubacks(client, &client->csr_op, 1);
    if (sv != AZ_IOT_OK)
    {
      sas_wipe(client);
      return sv;
    }
  }
  /* Seed jitter PRNG; tests can overwrite via the internal seed entry point
   * if they need determinism. */
  client->next_publish_id = 1u;
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
  for (size_t i = 0; i < client->opts.limits.max_pending_publishes; ++i)
  {
    client->pending_pubacks[i].in_use = false;
    client->pending_pubacks[i].cb = NULL;
    client->pending_pubacks[i].result_cb = NULL;
    client->pending_pubacks[i].user_ctx = NULL;
    client->pending_pubacks[i].packet_id = 0;
  }
  /* Same reasoning for the feature observers: on deinit() the feature clients are being torn
   * down alongside this one, so the teardown below must not call into them. */
  for (size_t i = 0; i < client->opts.limits.max_feature_observers; ++i)
  {
    memset(&client->feature_observers[i], 0, sizeof(client->feature_observers[i]));
  }
  release_material(client, AZ_IOT_CONN_SCOPE_DPS);
  release_material(client, AZ_IOT_CONN_SCOPE_HUB);
  teardown_active(client);
  dps_teardown_mqtt(client);
  sas_wipe(client);
  /* Registered factories are borrowed; the application destroys them after this returns. */
  az_iot_crypto__wipe(client->opts.workspace, client->opts.workspace_size);
  /* provisioned_iot_hub_hostname / provisioned_device_id are inline fixed buffers; nothing to free.
   */
}

AZ_NODISCARD az_iot_result az_iot_connection_client_register_mqtt_factory(
    az_iot_connection_client* client,
    const az_iot_mqtt_factory* factory)
{
  if (!client || !factory || !factory->create)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CONNECTION, "register_mqtt_factory: invalid arguments");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* An identical entry is redundant: find_factory() returns the first match
   * for a version, so registering the same factory again is a no-op. A
   * different factory for an already-registered version still appends and
   * loses to the first one at lookup. */
  for (size_t i = 0; i < client->factory_count; ++i)
  {
    const az_iot_mqtt_factory* f = &client->factories[i];
    if (f->create == factory->create && f->factory_ctx == factory->factory_ctx
        && f->version == factory->version)
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

az_iot_result az_iot_connection_client_add_state_observer(
    az_iot_connection_client* client,
    az_iot_connection_state_callback cb,
    void* user_ctx)
{
  if (!client || !cb)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (client->dispatching_state)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION, "cannot add a state observer from inside one");
    return AZ_IOT_ERR_BUSY;
  }
  size_t const count = client->opts.limits.max_application_state_observers;
  size_t free_slot = count;
  for (size_t i = 0; i < count; ++i)
  {
    /* Idempotent on the (cb, user_ctx) PAIR, not on cb alone: one callback
     * shared by two owners is two distinct subscriptions and must be delivered
     * twice. */
    if (client->app_state_observers[i].cb == cb
        && client->app_state_observers[i].user_ctx == user_ctx)
    {
      return AZ_IOT_OK;
    }
    if (!client->app_state_observers[i].cb && free_slot == count)
    {
      free_slot = i;
    }
  }
  if (free_slot == count)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CONNECTION, "no free state-observer slot");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  client->app_state_observers[free_slot].cb = cb;
  client->app_state_observers[free_slot].user_ctx = user_ctx;
  return AZ_IOT_OK;
}

az_iot_result az_iot_connection_client_remove_state_observer(
    az_iot_connection_client* client,
    az_iot_connection_state_callback cb,
    void* user_ctx)
{
  if (!client || !cb)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Allowed during a dispatch, unlike adding: an owner torn down in reaction to a
   * transition must be able to withdraw before its storage goes away. The walk in
   * dispatch_state_event() re-reads each slot and skips a NULL callback. */
  for (size_t i = 0; i < client->opts.limits.max_application_state_observers; ++i)
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

az_iot_result az_iot_connection_client_set_dps_assignment_callback(
    az_iot_connection_client* client,
    az_iot_dps_assignment_callback cb,
    void* user_ctx)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  client->assignment_cb = cb;
  client->assignment_cb_ctx = user_ctx;
  return AZ_IOT_OK;
}

/**
 * @brief The configuration support needs first, once per open(). Identifiers only, no secrets.
 *
 * @param[in] c Client.
 */
static void log_open_summary(const az_iot_connection_client* c)
{
  const char* transport
      = c->opts.transport == AZ_IOT_MQTT_TRANSPORT_WEBSOCKET ? "websocket" : "tcp";
  if (dps_configured(c) && (!is_nonempty_cstr(c->opts.iothub_hostname) || c->needs_reprovision))
  {
    AZ_IOT_LOG_INFOF(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "open: sdk=%s route=dps endpoint=%s id_scope=%s registration_id=%s csr=%s "
        "provision_only=%s transport=%s",
        az_iot_version_string(),
        is_nonempty_cstr(c->opts.dps.global_endpoint) ? c->opts.dps.global_endpoint
                                                      : "global.azure-devices-provisioning.net",
        text_or_none(c->opts.dps.id_scope),
        text_or_none(c->opts.dps.registration_id),
        c->opts.dps.request_operational_certificate ? "yes" : "no",
        c->opts.dps.provision_only ? "yes" : "no",
        transport);
    return;
  }
  AZ_IOT_LOG_INFOF(
      AZ_IOT_LOG_COMPONENT_CONNECTION,
      "open: sdk=%s route=hub host=%s device_id=%s profile=%s model_id=%s transport=%s",
      az_iot_version_string(),
      text_or_none(c->opts.iothub_hostname),
      text_or_none(c->opts.device_id),
      profile_name(c->connection_profile),
      text_or_none(c->opts.model_id),
      transport);
}

AZ_NODISCARD az_iot_result az_iot_connection_client_open(az_iot_connection_client* client)
{
  if (!client)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CONNECTION, "open: NULL client");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (client->callback_depth > 0)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "open: not callable from a callback; call after do_work()");
    return AZ_IOT_ERR_BUSY;
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
   * unpaced attempt with no close() in between. A registration retry waiting
   * in DPS:RETRY_PENDING holds no ref, for the same reason. */
  if (client->state[AZ_IOT_CONN_SCOPE_HUB] != AZ_IOT_CONN_STATE_IDLE
      || (client->state[AZ_IOT_CONN_SCOPE_DPS] != AZ_IOT_CONN_STATE_IDLE
          && client->dps_registration_ref)
      || client->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_RETRY_PENDING
      || client->dps_standing_ref)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CONNECTION, "open: client not in IDLE state");
    return AZ_IOT_ERR_ALREADY_INITIALIZED;
  }

  /* A set iothub_hostname names a hub only when non-empty and paired with a
   * device id, whichever route this open() takes. */
  if (client->opts.iothub_hostname
      && (!is_nonempty_cstr(client->opts.iothub_hostname)
          || !is_nonempty_cstr(client->opts.device_id)))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION, "open: iothub_hostname is empty or set without device_id");
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* Rejected rather than quietly ignored: opts.iothub_hostname names the hub this option
   * says does not exist, and an operational certificate is issued BY a
   * registration, which this device never performs. */
  if (client->opts.dps.provision_only)
  {
    if (!dps_configured(client))
    {
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_CONNECTION, "open: dps.provision_only requires dps.id_scope");
      return AZ_IOT_ERR_INVALID_ARG;
    }
    if (is_nonempty_cstr(client->opts.iothub_hostname))
    {
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "open: dps.provision_only is set, but opts.iothub_hostname names a hub");
      return AZ_IOT_ERR_INVALID_ARG;
    }
    if (client->opts.dps.request_operational_certificate)
    {
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "open: dps.provision_only cannot be combined with "
          "request_operational_certificate; the certificate is issued by a "
          "registration, which this device never performs");
      return AZ_IOT_ERR_INVALID_ARG;
    }
  }

  /* A role the client uses needs X.509 from the provider unless it has a SAS
   * key or token callback; CSR enrollment always needs the provider. Checked
   * first, so a missing provider is reported as such rather than as a missing
   * capability below. */
  bool dps_used = dps_configured(client);
  bool hub_used = !client->opts.dps.provision_only;
  bool dps_has_sas = client->auth[AZ_IOT_CONN_SCOPE_DPS].primary_key_len > 0
      || client->opts.dps_auth.sas.on_sas_token_required != NULL;
  bool hub_has_sas = client->auth[AZ_IOT_CONN_SCOPE_HUB].primary_key_len > 0
      || client->opts.hub_auth.sas.on_sas_token_required != NULL;
  if (!client->opts.certificate_provider
      && (client->opts.dps.request_operational_certificate || (dps_used && !dps_has_sas)
          || (hub_used && !hub_has_sas)))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "open: opts.certificate_provider is required for a role without a SAS key or token "
        "callback, and for request_operational_certificate");
    return AZ_IOT_ERR_CREDENTIAL_INCOMPLETE;
  }

  /* CSR-based operational-cert enrollment (D2) requires a certificate_provider
   * that makes the CSR and stores the issued chain. Fail fast otherwise. */
  if (client->opts.dps.request_operational_certificate)
  {
    if (!provider_owns_csr(client->opts.certificate_provider))
    {
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "open: request_operational_certificate needs a certificate provider with "
          "get_csr, release_csr and store_issued_certificate");
      return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    if (az_span_size(client->opts.csr_payload_buffer) <= 0)
    {
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "open: request_operational_certificate requires "
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
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "open: opts.dps.registration_payload must be a single "
          "well-formed JSON object");
      return AZ_IOT_ERR_INVALID_ARG;
    }
    az_span body_buffer = dps_register_body_buffer(client);
    if (az_span_ptr(body_buffer) == NULL || az_span_size(body_buffer) <= 0)
    {
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "open: opts.dps.registration_payload requires "
          "opts.dps.registration_body_buffer (or opts.csr_payload_buffer) to build "
          "the registration body in");
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    if (spans_overlap(client->opts.dps.registration_payload, body_buffer))
    {
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "open: opts.dps.registration_payload overlaps the buffer "
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
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_CONNECTION, "open: certificate provider vtable has no load()");
      return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    az_iot_certificate_material mat = { 0 };
    az_iot_result lr = p->vtable->load(p, AZ_IOT_CRED_OPERATIONAL, 0, &mat);
    if (lr != AZ_IOT_OK)
    {
      lr = p->vtable->load(p, AZ_IOT_CRED_BOOTSTRAP, 0, &mat);
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
  /* A fresh connect cycle starts the birth-ack timeout schedule over. */
  client->presence.consecutive_birth_ack_timeouts = 0;
  reset_identity_recovery(client);
  dps_user_retry_reset(client);
  /* open() starts again at the first credential source. */
  for (size_t i = 0; i < AZ_IOT_CONN_SCOPE_COUNT; ++i)
  {
    client->auth[i].first = AZ_IOT_AUTH_SOURCE_NONE;
    client->auth[i].pass_from = AZ_IOT_AUTH_SOURCE_NONE;
  }
  /* needs_reprovision is deliberately NOT cleared here. It is pending recovery
   * intent -- "the cached assignment is no good, ask DPS again" -- set by an
   * identity rejection in REPROVISION mode, by the unreachable-hub
   * threshold, by an assignment this client refused, or by
   * az_iot_connection_client_request_reprovision(). Clearing it would make
   * close() + open() reconnect to the hub that was given up on, because the
   * cached host is still set. It is consumed by the DPS route below. */

  /* As in close(): a reopen abandons the stored chain's assignment. */
  if (client->dps_cert_awaits_adoption)
  {
    client->needs_reprovision = true;
    client->dps_cert_awaits_adoption = false;
  }

  log_open_summary(client);

  /* No hub: bring the provisioning session up and stop there. No registration
   * ref is taken. The standing ref alone gives both behaviours the pump needs
   * -- do not collect it, and re-establish it if it drops.
   *
   * Before the registration route below, which would otherwise claim this
   * client: opts.iothub_hostname is NULL on a provision_only device, so it matches. */
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
    /* A feature client started a newer session from the start's announcement;
     * it serves the standing ref. */
    if (client->dps_start_cancelled && client->dps_mqtt != NULL)
    {
      return r;
    }
    client->dps_standing_ref = false;
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_CONNECTION, "open: provision_only session did not start (%d)", (int)r);
    return r;
  }

  /* DPS will be used for registration: no hub is cached, or a previous outcome
   * demanded re-provisioning even though one is. */
  if (dps_configured(client) && (!client->opts.iothub_hostname || client->needs_reprovision))
  {
    /* Raised before the start, so a session that is already up is adopted
     * rather than replaced. */
    client->dps_registration_ref = true;

    az_iot_result r;
    if (az_iot_connection_client__dps_session_ready(client))
    {
      AZ_IOT_LOG_DEBUG(AZ_IOT_LOG_COMPONENT_DPS, "registering on the session already open");
      r = dps_register_on_ready_session(client);
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

    /* A feature client started a newer session from an announcement; the
     * registration ref rides on it. */
    if (r != AZ_IOT_OK && client->dps_start_cancelled)
    {
      return r;
    }
    if (r != AZ_IOT_OK)
    {
      client->dps_registration_ref = false;
      set_state_to(client, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, r);
      return r;
    }
    /* Consumed only once provisioning is really under way, so a failed start
     * still leaves the demand standing for the next open(). A start waiting
     * for its token is not under way yet. */
    if (client->sas_token_request[AZ_IOT_CONN_SCOPE_DPS].request_id == 0)
    {
      client->needs_reprovision = false;
    }
    return r;
  }

  if (!client->opts.iothub_hostname || !client->opts.device_id)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "open: iothub_hostname or device_id not set, and no DPS registration is due");
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
    /* A start still in SETTING_UP has no session yet: settling the scope is
     * what cancels it, since dps_start() re-checks the state. */
    if (c->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_SETTING_UP)
    {
      clear_sas_token_request(c, AZ_IOT_CONN_SCOPE_DPS);
      c->dps_phase = DPS_PHASE_NONE;
      /* The registration goes with it: a session started from the callback
       * registers only if a later open() asks. */
      c->dps_registration_ref = false;
      set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, AZ_IOT_OK);
    }
    return;
  }
  if (c->dps_mqtt->iface && c->dps_mqtt->iface->disconnect)
  {
    (void)c->dps_mqtt->iface->disconnect(c->dps_mqtt);
  }
  dps_teardown_mqtt(c);
  c->dps_registration_ref = false;
  c->dps_phase = DPS_PHASE_NONE;
  c->dps_cert_awaits_adoption = false;
  c->dps_cert_withheld = false;
  c->dps_pending_finalize = false;
  c->dps_pending_have_assignment = false;
  c->dps_pending_status = AZ_IOT_OK;
  set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_DISCONNECTING, AZ_IOT_OK);
  /* A session started from that callback is not this one to settle. */
  if (c->dps_mqtt != NULL)
  {
    return;
  }
  set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, AZ_IOT_OK);
}

az_iot_result az_iot_connection_client_close(az_iot_connection_client* client)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (client->callback_depth > 0)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "close: not callable from a callback; call after do_work()");
    return AZ_IOT_ERR_BUSY;
  }
  release_material(client, AZ_IOT_CONN_SCOPE_DPS);
  release_material(client, AZ_IOT_CONN_SCOPE_HUB);
  /* A stored chain whose assignment was never adopted: the cached hub no
   * longer matches the certificate, so the next open() registers again. */
  if (client->dps_cert_awaits_adoption)
  {
    client->needs_reprovision = true;
    client->dps_cert_awaits_adoption = false;
  }
  clear_sas_token_request(client, AZ_IOT_CONN_SCOPE_DPS);
  clear_sas_token_request(client, AZ_IOT_CONN_SCOPE_HUB);
  client->sas_token_ask_after_ms[AZ_IOT_CONN_SCOPE_DPS] = 0;
  client->sas_token_ask_after_ms[AZ_IOT_CONN_SCOPE_HUB] = 0;

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
  if (client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_RETRY_PENDING
      || client->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_RETRY_PENDING)
  {
    client->retry_attempt[AZ_IOT_CONN_SCOPE_DPS] = 0;
    client->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] = 0;
    client->reconnect_due_ms = 0;
    reset_identity_recovery(client);
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
   * Same shape as the RETRY_PENDING branch above: cancel the bookkeeping and
   * transition. The configuration is untouched, so a DPS client re-provisions
   * on the next open() and a client that had already been assigned a hub
   * reconnects to it. */
  if (client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_FAULTED
      || client->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_FAULTED)
  {
    client->retry_attempt[AZ_IOT_CONN_SCOPE_DPS] = 0;
    client->retry_attempt[AZ_IOT_CONN_SCOPE_HUB] = 0;
    client->reconnect_due_ms = 0;
    reset_identity_recovery(client);
    client->user_close = false;
    /* needs_reprovision survives on purpose: it says the cached assignment is
     * no good, which a close() does not change. Clearing it here would let the
     * following open() reconnect to the very hub that faulted. */
    /* Defensive: no fault path leaves one behind today, but close() must not
     * depend on that to reach IDLE. */
    end_session(client, AZ_IOT_CONNECTION_SESSION_END_PLANNED);
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
    reset_identity_recovery(client);
    /* needs_reprovision survives, as in the FAULTED branch above. */
    client->user_close = false;
    settle_all_scopes_to_idle(client);
    return AZ_IOT_OK;
  }

  client->user_close = true;
  set_state_to(client, AZ_IOT_CONN_SCOPE_HUB, AZ_IOT_CONN_STATE_DISCONNECTING, AZ_IOT_OK);
  /* A planned end already sent DISCONNECT; its completion or bound settles IDLE. */
  if (planned_hub_session_end_in_progress(client))
  {
    return AZ_IOT_OK;
  }
  az_iot_result r = client->active_client->iface->disconnect(client->active_client);
  if (r != AZ_IOT_OK && r != AZ_IOT_ERR_NOT_CONNECTED)
  {
    return r;
  }
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result
az_iot_connection_client_request_reprovision(az_iot_connection_client* client)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!dps_configured(client) || client->opts.dps.provision_only)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  /* A registration already in flight satisfies the request. */
  if (client->dps_registration_ref)
  {
    return AZ_IOT_OK;
  }
  client->needs_reprovision = true;
  /* A pending hub retry, possibly an hour away on the identity ladder, runs on
   * the next do_work(). A pending registration retry (DPS:RETRY_PENDING) keeps
   * its schedule: it carries the DPS backoff and any service retry-after. */
  if (client->reconnect_due_ms != 0
      && client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_RETRY_PENDING
      && client->state[AZ_IOT_CONN_SCOPE_DPS] != AZ_IOT_CONN_STATE_RETRY_PENDING)
  {
    uint64_t now = az_iot_time_mono_ms();
    client->reconnect_due_ms = now ? now : 1u;
  }
  return AZ_IOT_OK;
}

/**
 * @brief Settle a DPS:RETRY_PENDING whose retry fell back to the cached hub to
 * the provisioning session's real state: CONNECTED when ready, CONNECTING
 * while it comes up, IDLE when there is none.
 */
static void settle_spent_dps_retry(az_iot_connection_client* c)
{
  if (c->state[AZ_IOT_CONN_SCOPE_DPS] != AZ_IOT_CONN_STATE_RETRY_PENDING)
  {
    return;
  }
  az_iot_connection_state now = (c->dps_mqtt == NULL)  ? AZ_IOT_CONN_STATE_IDLE
      : az_iot_connection_client__dps_session_ready(c) ? AZ_IOT_CONN_STATE_CONNECTED
                                                       : AZ_IOT_CONN_STATE_CONNECTING;
  set_state_to(c, AZ_IOT_CONN_SCOPE_DPS, now, AZ_IOT_OK);
}

/** @brief One do_work() pass, without the tick observers. */
static az_iot_result do_work_pass(az_iot_connection_client* client, uint32_t timeout_ms)
{
  client->do_work_pass_started_ms = az_iot_time_mono_ms();
  process_sas_token_request(client);

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
    AZ_IOT_LOG_DEBUG(
        AZ_IOT_LOG_COMPONENT_DPS, "closing the provisioning session; nobody is holding it");
    if (client->dps_mqtt->iface && client->dps_mqtt->iface->disconnect)
    {
      (void)client->dps_mqtt->iface->disconnect(client->dps_mqtt);
    }
    dps_teardown_mqtt(client);
    client->dps_phase = DPS_PHASE_DONE;
    /* Nobody holds it: the next demand starts a new credential pass. */
    client->auth[AZ_IOT_CONN_SCOPE_DPS].first = AZ_IOT_AUTH_SOURCE_NONE;
    client->auth[AZ_IOT_CONN_SCOPE_DPS].pass_from = AZ_IOT_AUTH_SOURCE_NONE;
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
      AZ_IOT_LOG_ERRORF(
          AZ_IOT_LOG_COMPONENT_DPS,
          "could not re-establish the provisioning session (%d)",
          (int)sr);
    }
  }

  if (client->dps_mqtt != NULL)
  {
    /* A registration still running when identity recovery's duration is spent
     * is abandoned before HOLD or POLLING can publish again. The failure
     * path faults it through schedule_reconnect(). */
    if (client->dps_registration_ref && !client->dps_pending_finalize
        && identity_recovery_expired(client))
    {
      dps_finalize(client, AZ_IOT_ERR_TIMEOUT, false);
      client->dps_phase = DPS_PHASE_DONE;
    }

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
          AZ_IOT_LOG_ERROR(
              AZ_IOT_LOG_COMPONENT_DPS, "pre-registration hold timed out; registering anyway");
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
      /* A hub session beside it is pumped only after this wait. */
      wait_ms = limit_wait_to_connection_deadlines(client, wait_ms);
      r = client->dps_mqtt->iface->process_loop(client->dps_mqtt, wait_ms);
    }

    /* Deferred out of the SUBACK handler, so observers do not run inside the
     * adapter's process_loop.
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
    r = client->active_client->iface->process_loop(
        client->active_client, limit_wait_to_connection_deadlines(client, timeout_ms));
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
    puback_abandon(client, &client->csr_op);
    az_iot_csr_event evt;
    csr_event_init(&evt, AZ_IOT_CSR_FAILED);
    evt.status = AZ_IOT_ERR_TIMEOUT;
    report_csr_event(client, cb, &evt, uc);
  }

  /* Fail a stalled MQTTv5 presence (birth) handshake so a missing SUBACK or
   * birth-ack can't wedge the client in CONNECTING forever: end the session
   * with a DISCONNECT, then reconnect when a policy is configured, otherwise
   * fault. A missed birth-ack escalates the next birth's timeout. */
  if ((client->presence.phase == AZ_IOT_PRESENCE_PHASE_SUBSCRIBING
       || client->presence.phase == AZ_IOT_PRESENCE_PHASE_BIRTH)
      && !planned_hub_session_end_in_progress(client)
      && az_iot_deadline__has_passed(&client->presence.deadline, az_iot_time_mono_ms()))
  {
    if (client->presence.phase == AZ_IOT_PRESENCE_PHASE_BIRTH)
    {
      AZ_IOT_LOG_WARNF(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "no birth-ack within %lu ms; ending the session",
          (unsigned long)client->presence.birth_ack_timeout_ms);
    }
    /* Both paths tear down, which counts the missed birth-ack. close() has
     * already sent its DISCONNECT and decides how the session ends. */
    if (client->user_close)
    {
      settle_closed_hub_session(client);
    }
    else
    {
      start_planned_hub_session_end(client, AZ_IOT_ERR_TIMEOUT);
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

  /* A planned end whose disconnect was not reported in time ends anyway; at
   * IDLE once close() took it over. */
  if (planned_hub_session_end_in_progress(client)
      && az_iot_deadline__has_passed(
          &client->planned_hub_session_end.disconnect_deadline, az_iot_time_mono_ms()))
  {
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_CONNECTION, "planned session end: no disconnect event; ending it");
    if (client->user_close)
    {
      settle_closed_hub_session(client);
    }
    else
    {
      finish_planned_hub_session_end(client);
    }
  }

  process_sas_token_renewal(client);
  process_dps_sas_token_renewal(client);

  /* If we are waiting to reconnect and the deadline has passed, attempt it.
   *
   * `reconnect_due_ms` is the pending-retry token, and firing CONSUMES it.
   * Gating on the state alone is not enough now that the two scopes move
   * independently: a hub failure whose recovery is a re-registration leaves
   * HUB in RETRY_PENDING while the attempt runs on DPS, so a state-only test
   * would re-fire on every tick for as long as the hub stayed down. */
  if (client->reconnect_due_ms != 0
      && (client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_RETRY_PENDING
          || client->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_RETRY_PENDING)
      && client->active_client == NULL && az_iot_time_mono_ms() >= client->reconnect_due_ms)
  {
    az_iot_result cr;
    az_iot_connection_scope attempted;
    /* Checked again here: do_work() may run well after the due time. */
    if (identity_recovery_expired(client))
    {
      stop_identity_recovery(
          client,
          client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_RETRY_PENDING
              ? AZ_IOT_CONN_SCOPE_HUB
              : AZ_IOT_CONN_SCOPE_DPS);
      return r;
    }
    client->reconnect_due_ms = 0;
    if (client->needs_reprovision)
    {
      /* A re-provision was asked for (REPROVISION mode, the unreachable-hub
       * threshold, the application), or this device has not registered yet;
       * go to DPS rather than to the cached hub.
       *
       * The demand is cleared before the attempt ONLY when there is a cached
       * assignment to fall back to. That fallback exists so a failing
       * dps_start() cannot loop through provisioning forever -- but it is only
       * a fallback if the hub path has somewhere to go. With no assignment
       * (opts.iothub_hostname still NULL) an ordinary retry would hand the adapter a NULL
       * endpoint and the client would never reach DPS again, so the demand
       * survives and the next retry provisions. */
      client->needs_reprovision = (client->opts.iothub_hostname == NULL);
      /* session_role stays the cached hub's: the provisioning session carries
       * its own role, and the hub fallback must keep the assigned protocol. */
      attempted = AZ_IOT_CONN_SCOPE_DPS;
      /* Adopt a session that is already up rather than building a second one:
       * a session its users hold survives registration, and an unconditional
       * start here would overwrite dps_mqtt, orphan that socket and lose any
       * exchange in flight on it. Same rule as open(). */
      client->dps_registration_ref = true;
      if (az_iot_connection_client__dps_session_ready(client))
      {
        cr = dps_register_on_ready_session(client);
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
      /* A DPS retry that fell back to this hub attempt is spent. Left at
       * RETRY_PENDING, close() would take the no-session path and leave the
       * hub connected, and request_reprovision() would mistake it for a
       * pending registration. Settle it to what the session really is. */
      if (client->active_client != NULL
          || client->sas_token_request[AZ_IOT_CONN_SCOPE_HUB].request_id != 0)
      {
        settle_spent_dps_retry(client);
      }
    }
    /* A start a feature client replaced from an announcement is not a failure:
     * the registration ref rides on the newer session. */
    if (cr != AZ_IOT_OK && !(attempted == AZ_IOT_CONN_SCOPE_DPS && client->dps_start_cancelled))
    {
      /* The failure belongs to whichever attempt was just made. Which ladder
       * its retry climbs is decided inside schedule_reconnect(), from the
       * needs_reprovision left standing above.
       *
       * A failed registration releases its ref, as open() does: left set on a
       * session its users keep up, the DPS pump returns before this block and
       * the retry never fires. */
      if (attempted == AZ_IOT_CONN_SCOPE_DPS)
      {
        client->dps_registration_ref = false;
      }
      schedule_reconnect(client, attempted, cr);
      /* A failed hub fallback also spends the DPS retry it replaced; the next
       * retry is scheduled above. */
      if (attempted == AZ_IOT_CONN_SCOPE_HUB
          && client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_RETRY_PENDING)
      {
        settle_spent_dps_retry(client);
      }
    }
  }

  return r;
}

az_iot_result az_iot_connection_client_do_work(
    az_iot_connection_client* client,
    uint32_t timeout_ms)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_result r = do_work_pass(client, timeout_ms);
  /* After every timer, on every path. A callback may withdraw itself or another observer. */
  notify_feature_observers_of(client, AZ_IOT_CONNECTION_FEATURE_EVENT_TICK);
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

/* Internal helper used by both __set_iothub_hostname and __set_device_id. Copies `s` into
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

az_iot_result az_iot_connection_client__set_iothub_hostname(
    az_iot_connection_client* client,
    const char* iothub_hostname)
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
      &client->opts.iothub_hostname,
      iothub_hostname);
}

az_iot_result az_iot_connection_client__set_device_id(
    az_iot_connection_client* client,
    const char* device_id)
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
      &client->opts.device_id,
      device_id);
}

void az_iot_connection_client__seed_rng(az_iot_connection_client* client, uint64_t seed)
{
  if (!client)
  {
    return;
  }
  client->rng_state = seed ? seed : 1ull;
}

void az_iot_connection_client__set_defensive_timeout_random_source(
    az_iot_connection_client* client,
    uint64_t (*source)(void* context),
    void* context)
{
  if (!client)
  {
    return;
  }
  client->defensive_timeout_random_source = source;
  client->defensive_timeout_random_context = source ? context : NULL;
}

az_iot_result az_iot_connection_client__set_defensive_timeout_schedule(
    az_iot_connection_client* client,
    const uint32_t* base_ms_by_attempt,
    size_t attempt_count,
    uint32_t jitter_max_ms)
{
  if (!client || (base_ms_by_attempt == NULL) != (attempt_count == 0u))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  client->defensive_timeout_base_ms_by_attempt = base_ms_by_attempt;
  client->defensive_timeout_attempt_count = attempt_count;
  client->defensive_timeout_jitter_max_ms = base_ms_by_attempt ? jitter_max_ms : 0u;
  return AZ_IOT_OK;
}

uint32_t az_iot_connection_client__defensive_timeout_ms(
    az_iot_connection_client* client,
    uint32_t attempt)
{
  uint64_t random_value = 0;
  if (client && client->defensive_timeout_random_source)
  {
    random_value
        = client->defensive_timeout_random_source(client->defensive_timeout_random_context);
  }
  else if (client)
  {
    /* Same LCG as gen_uuid_v4(); the high half is the well-mixed part. */
    client->rng_state = client->rng_state * 6364136223846793005ull + 1442695040888963407ull;
    random_value = client->rng_state >> 32;
  }
  if (client && client->defensive_timeout_base_ms_by_attempt)
  {
    return az_iot_defensive_timeout__schedule_ms(
        client->defensive_timeout_base_ms_by_attempt,
        client->defensive_timeout_attempt_count,
        client->defensive_timeout_jitter_max_ms,
        attempt,
        random_value);
  }
  return az_iot_defensive_timeout__ms(attempt, random_value);
}

az_iot_result az_iot_connection_client__end_hub_session(
    az_iot_connection_client* client,
    az_iot_result reason)
{
  if (!client || reason == AZ_IOT_OK)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!client->active_client || client->user_close
      || client->state[AZ_IOT_CONN_SCOPE_HUB] != AZ_IOT_CONN_STATE_CONNECTED)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  if (!planned_hub_session_end_in_progress(client))
  {
    start_planned_hub_session_end(client, reason);
  }
  return AZ_IOT_OK;
}

void az_iot_connection_client__planned_hub_session_end_force_timeout(
    az_iot_connection_client* client)
{
  if (client && planned_hub_session_end_in_progress(client))
  {
    az_iot_deadline__expire_now(&client->planned_hub_session_end.disconnect_deadline);
  }
}

bool az_iot_connection_client__twin_push_expected(const az_iot_connection_client* client)
{
  return client && (client->opts.twin_push.push_desired || client->opts.twin_push.push_reported);
}

bool az_iot_connection_client__deadline_passed_before_pass(
    const az_iot_connection_client* client,
    const az_iot_deadline* deadline)
{
  return az_iot_deadline__has_passed(deadline, client->do_work_pass_started_ms);
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
    az_iot_deadline__expire_now(&client->presence.deadline);
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

az_iot_result az_iot_connection_client__birth_nonce(
    const az_iot_connection_client* client,
    uint8_t out[AZ_IOT_CORRELATION_UUID_LEN])
{
  if (!client || !out)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if ((client->presence.phase != AZ_IOT_PRESENCE_PHASE_BIRTH
       && client->presence.phase != AZ_IOT_PRESENCE_PHASE_DONE)
      || client->user_close
      || client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_DISCONNECTING)
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

bool az_iot_connection_client__twin_push_reported(const az_iot_connection_client* client)
{
  return client && client->opts.twin_push.push_reported;
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
  return client ? client->opts.device_id : NULL;
}

const char* az_iot_connection_client_get_iothub_address(const az_iot_connection_client* client)
{
  /* opts.iothub_hostname tracks the effective hub: the caller-supplied host for a direct
   * connection, or the DPS-assigned hub (copied into provisioned_iot_hub_hostname
   * and pointed-to here) after provisioning. NULL for a not-yet-provisioned
   * DPS-only client. */
  return client ? client->opts.iothub_hostname : NULL;
}

const char* az_iot_connection_client_get_device_id(const az_iot_connection_client* client)
{
  /* Gated on opts.iothub_hostname so the pair is never half-set: a DPS client may carry a
   * caller device_id that is not the device id DPS will assign. Both are
   * replaced together when an assignment is adopted. */
  return (client && client->opts.iothub_hostname) ? client->opts.device_id : NULL;
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
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "a feature client for the other hub generation is already "
        "attached to this connection");
    return AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH;
  }
  /* A stored issued chain is committed to the pending assignment's profile. */
  if (client->dps_cert_awaits_adoption && client->dps_assigned_profile != profile)
  {
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
  size_t free_slot = client->opts.limits.max_feature_clients;
  for (size_t i = 0; i < client->opts.limits.max_feature_clients; ++i)
  {
    if (client->feature_client_binds[i].owner == owner)
    {
      client->feature_client_binds[i].on_bind = on_bind;
      return AZ_IOT_OK;
    }
    if (!client->feature_client_binds[i].owner
        && free_slot == client->opts.limits.max_feature_clients)
    {
      free_slot = i;
    }
  }
  if (free_slot == client->opts.limits.max_feature_clients)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  client->feature_client_binds[free_slot].owner = owner;
  client->feature_client_binds[free_slot].on_bind = on_bind;
  /* Connected, or an mqttv5 session connecting, means the identity is settled
   * and this session's binds have run, so this one has to catch up or it would
   * sit idle until the next connect. Mirrors __add_subscription_on_connect
   * subscribing immediately. Not an mqttv3 session connecting: its binds subscribe,
   * and a client withdrawn before CONNECTED would not unsubscribe. */
  if (client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_CONNECTED
      || (client->state[AZ_IOT_CONN_SCOPE_HUB] == AZ_IOT_CONN_STATE_CONNECTING
          && client->connection_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5))
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
  for (size_t i = 0; i < client->opts.limits.max_feature_clients; ++i)
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
  for (size_t i = 0; i < c->opts.limits.max_feature_clients; ++i)
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
      AZ_IOT_LOG_ERROR(
          AZ_IOT_LOG_COMPONENT_CONNECTION,
          "a feature client could not bind its topics for this session");
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
     * backoff, a latched refusal, or a credential fallback pass, earned by
     * whoever came before it. A registration owns its own pass. */
    dps_user_retry_reset(client);
    if (!client->dps_registration_ref && !client->dps_standing_ref)
    {
      client->dps_demand_epoch++;
      /* A session still up keeps its pass: a holder may re-acquire it before
       * the pump closes it. The pump clears the pass when it does. */
      if (client->dps_mqtt == NULL)
      {
        client->auth[AZ_IOT_CONN_SCOPE_DPS].first = AZ_IOT_AUTH_SOURCE_NONE;
        client->auth[AZ_IOT_CONN_SCOPE_DPS].pass_from = AZ_IOT_AUTH_SOURCE_NONE;
      }
    }
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

  /* One exists but is not ready yet, or a start is inside its SETTING_UP
   * announcement (an observer asking from that callback). */
  if (client->dps_mqtt != NULL
      || client->state[AZ_IOT_CONN_SCOPE_DPS] == AZ_IOT_CONN_STATE_SETTING_UP)
  {
    return AZ_IOT_ERR_BUSY;
  }

  /* Never from a settled fault, and never while a retry is pending.
   *
   * FAULTED: starting a session would announce DPS:CONNECTING and drag that
   * lifecycle out of its terminal state, so the application would never see
   * the fault settle.
   *
   * RETRY_PENDING: a registration retry is already scheduled. Opening a session
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
    if (client->state[i] == AZ_IOT_CONN_STATE_RETRY_PENDING)
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
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_DPS, "could not open a provisioning session (%d)", (int)r);
    /* A SYNCHRONOUS failure never reaches dps_finalize(), so nothing else would
     * pace it: the caller gets the error and its next pump tick asks again
     * immediately. That is the same hot loop, on the path where the adapter
     * cannot even be created -- which is the one least likely to fix itself.
     *
     * But a start a feature client replaced from an announcement
     * (dps_start_cancelled) is not a failure: the newer session is up. */
    if (!client->dps_start_cancelled)
    {
      dps_user_retry_schedule(client, 0);
      /* A failure before the session existed leaves DPS:SETTING_UP; a held
       * session's failure settles at IDLE, as dps_apply_deferred() does. */
      set_state_to(client, AZ_IOT_CONN_SCOPE_DPS, AZ_IOT_CONN_STATE_IDLE, r);
      /* A feature client may have started a newer session from that callback;
       * report it as replaced so callers keep its refs. */
      if (client->dps_mqtt != NULL)
      {
        client->dps_start_cancelled = true;
      }
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
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_DPS, "a provisioning-session message observer is already registered");
    return;
  }
  client->dps_message_observer = observer;
  client->dps_message_observer_ctx = user_ctx;
}

#define PUBACK_POOL_SHARED UINT8_MAX

/** @brief puback_reservations[] index held by @p owner, or PUBACK_POOL_SHARED if none. */
static uint8_t puback_pool_of(const az_iot_connection_client* c, const void* owner)
{
  if (owner != NULL)
  {
    for (size_t i = 0; i < c->opts.limits.max_publish_reservations; ++i)
    {
      if (c->puback_reservations[i].owner == owner)
      {
        return (uint8_t)i;
      }
    }
  }
  return PUBACK_POOL_SHARED;
}

/** @brief Slots reserved by every owner except the one at @p skip. */
static size_t puback_reserved_except(const az_iot_connection_client* c, uint8_t skip)
{
  size_t reserved = 0;
  for (size_t i = 0; i < c->opts.limits.max_publish_reservations; ++i)
  {
    if (i != skip)
    {
      reserved += c->puback_reservations[i].count;
    }
  }
  return reserved;
}

/** @brief Slots @p pool may hold: its reservation, or what no reservation took. */
static size_t puback_pool_capacity(const az_iot_connection_client* c, uint8_t pool)
{
  return pool != PUBACK_POOL_SHARED
      ? c->puback_reservations[pool].count
      : c->opts.limits.max_pending_publishes - puback_reserved_except(c, PUBACK_POOL_SHARED);
}

/** @brief Slots in use that count against @p pool. */
static size_t puback_pool_used(const az_iot_connection_client* c, uint8_t pool)
{
  size_t used = 0;
  for (size_t i = 0; i < c->opts.limits.max_pending_publishes; ++i)
  {
    if (c->pending_pubacks[i].in_use && c->pending_pubacks[i].reservation == pool)
    {
      ++used;
    }
  }
  return used;
}

/**
 * @brief A free pending_pubacks[] index @p pool may take, or limits.max_pending_publishes if the
 *        pool is at capacity.
 *
 * Every pool's in-flight count stays within its capacity (reserve_pubacks() refuses a change that
 * would break that) and the capacities add up to the table size, so a pool below capacity always
 * finds a free slot.
 */
static size_t puback_free_slot(const az_iot_connection_client* c, uint8_t pool)
{
  if (puback_pool_used(c, pool) >= puback_pool_capacity(c, pool))
  {
    return c->opts.limits.max_pending_publishes;
  }
  for (size_t i = 0; i < c->opts.limits.max_pending_publishes; ++i)
  {
    if (!c->pending_pubacks[i].in_use)
    {
      return i;
    }
  }
  return c->opts.limits.max_pending_publishes;
}

/**
 * @brief Free @p owner's pending-PUBACK slots without calling their callbacks.
 *
 * For a request whose operation has ended. Its late PUBACK then matches no slot: the adapter
 * gives a new publish a different packet id while the old one is in flight.
 */
static void puback_abandon(az_iot_connection_client* c, const void* owner)
{
  uint8_t pool = puback_pool_of(c, owner);
  if (pool == PUBACK_POOL_SHARED)
  {
    return;
  }
  for (size_t i = 0; i < c->opts.limits.max_pending_publishes; ++i)
  {
    if (c->pending_pubacks[i].in_use && c->pending_pubacks[i].reservation == pool)
    {
      memset(&c->pending_pubacks[i], 0, sizeof(c->pending_pubacks[i]));
    }
  }
}

/**
 * @brief Publish on the hub session; completes through @p ack_cb or @p result_cb, whichever is
 * set (at most one).
 */
static az_iot_result publish_tracked(
    az_iot_connection_client* client,
    const void* owner,
    const az_iot_mqtt_message* msg,
    az_iot_publish_ack_callback ack_cb,
    az_iot_publish_result_callback result_cb,
    void* user_ctx,
    az_iot_publish_handle* out_handle)
{
  if (out_handle)
  {
    out_handle->slot = 0;
    out_handle->id = 0;
  }
  if (!client || !msg)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (!client->active_client || client->state[AZ_IOT_CONN_SCOPE_HUB] != AZ_IOT_CONN_STATE_CONNECTED)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }
  bool const has_cb = ack_cb != NULL || result_cb != NULL;

  /* QoS 1/2 with a callback: reserve the correlation slot BEFORE publishing,
   * so a full pool sends nothing and the caller can retry without
   * duplicating. The reserved slot has no callback, so neither the PUBACK match
   * nor teardown_active() acts on it. Adapters deliver events only from
   * process_loop() (az_iot_mqtt_iface), so no PUBACK can arrive before the
   * packet id is filled in below. */
  size_t slot = client->opts.limits.max_pending_publishes;
  if (msg->qos != AZ_IOT_MQTT_QOS_0 && has_cb)
  {
    uint8_t pool = puback_pool_of(client, owner);
    slot = puback_free_slot(client, pool);
    if (slot == client->opts.limits.max_pending_publishes)
    {
      return AZ_IOT_ERR_BUSY;
    }
    client->pending_pubacks[slot].packet_id = 0;
    client->pending_pubacks[slot].cb = NULL;
    client->pending_pubacks[slot].result_cb = NULL;
    client->pending_pubacks[slot].user_ctx = NULL;
    client->pending_pubacks[slot].reservation = pool;
    client->pending_pubacks[slot].publish_id = 0;
    client->pending_pubacks[slot].in_use = true;
  }

  uint16_t pid = 0;
  az_iot_result r = client->active_client->iface->publish(client->active_client, msg, &pid);
  if (slot < client->opts.limits.max_pending_publishes)
  {
    if (r != AZ_IOT_OK)
    {
      client->pending_pubacks[slot].in_use = false;
      return r;
    }
    client->pending_pubacks[slot].packet_id = pid;
    client->pending_pubacks[slot].cb = ack_cb;
    client->pending_pubacks[slot].result_cb = result_cb;
    client->pending_pubacks[slot].user_ctx = user_ctx;
    client->pending_pubacks[slot].publish_id = client->next_publish_id;
    client->next_publish_id
        = client->next_publish_id == UINT64_MAX ? 1u : client->next_publish_id + 1u;
    if (out_handle)
    {
      out_handle->slot = slot;
      out_handle->id = client->pending_pubacks[slot].publish_id;
    }
    return AZ_IOT_OK;
  }
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  /* QoS 0: there is no PUBACK on the wire. Complete synchronously. QoS 1/2
   * without a callback: the future PUBACK is silently absorbed. */
  if (msg->qos == AZ_IOT_MQTT_QOS_0)
  {
    complete_publish(client, ack_cb, result_cb, user_ctx, AZ_IOT_OK, NULL);
  }
  return AZ_IOT_OK;
}

az_iot_result az_iot_connection_client__publish(
    az_iot_connection_client* client,
    const void* owner,
    const az_iot_mqtt_message* msg,
    az_iot_publish_ack_callback ack_cb,
    void* ack_user_ctx)
{
  return publish_tracked(client, owner, msg, ack_cb, NULL, ack_user_ctx, NULL);
}

az_iot_result az_iot_connection_client__publish_with_result(
    az_iot_connection_client* client,
    const void* owner,
    const az_iot_mqtt_message* msg,
    az_iot_publish_result_callback result_cb,
    void* user_ctx,
    az_iot_publish_handle* out_handle)
{
  return publish_tracked(client, owner, msg, NULL, result_cb, user_ctx, out_handle);
}

void az_iot_connection_client__forget_publish(
    az_iot_connection_client* client,
    az_iot_publish_handle handle)
{
  if (client == NULL || handle.id == 0u || handle.slot >= client->opts.limits.max_pending_publishes)
  {
    return;
  }
  az_iot_connection_pending_publish* p = &client->pending_pubacks[handle.slot];
  if (p->in_use && p->publish_id == handle.id)
  {
    memset(p, 0, sizeof(*p));
  }
}

az_iot_result az_iot_connection_client__reserve_pubacks(
    az_iot_connection_client* client,
    const void* owner,
    size_t count)
{
  if (client == NULL || owner == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (count == 0)
  {
    az_iot_connection_client__release_pubacks(client, owner);
    return AZ_IOT_OK;
  }

  uint8_t entry = puback_pool_of(client, owner);
  if (entry == PUBACK_POOL_SHARED)
  {
    for (size_t i = 0; i < client->opts.limits.max_publish_reservations; ++i)
    {
      if (client->puback_reservations[i].owner == NULL)
      {
        entry = (uint8_t)i;
        break;
      }
    }
  }
  size_t others = puback_reserved_except(client, entry);
  if (entry == PUBACK_POOL_SHARED || count > client->opts.limits.max_pending_publishes - others)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "cannot reserve %lu pending-PUBACK slots: %lu of %u reserved by others, %u holders at most",
        (unsigned long)count,
        (unsigned long)others,
        (unsigned)client->opts.limits.max_pending_publishes,
        (unsigned)client->opts.limits.max_publish_reservations);
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  /* Keep every pool within its capacity, so a granted reservation is usable at once instead of
   * waiting for shared publishes already in flight to be acknowledged. */
  if (puback_pool_used(client, PUBACK_POOL_SHARED)
          > client->opts.limits.max_pending_publishes - others - count
      || (client->puback_reservations[entry].owner == owner
          && puback_pool_used(client, entry) > count))
  {
    return AZ_IOT_ERR_BUSY;
  }
  client->puback_reservations[entry].owner = owner;
  client->puback_reservations[entry].count = (uint16_t)count;
  return AZ_IOT_OK;
}

void az_iot_connection_client__release_pubacks(az_iot_connection_client* client, const void* owner)
{
  if (client == NULL)
  {
    return;
  }
  uint8_t entry = puback_pool_of(client, owner);
  if (entry == PUBACK_POOL_SHARED)
  {
    return;
  }
  /* In-flight publishes keep their callbacks and move to the shared pool, whose capacity grows by
   * at least as much, so the entry can be reused at once. */
  for (size_t i = 0; i < client->opts.limits.max_pending_publishes; ++i)
  {
    if (client->pending_pubacks[i].in_use && client->pending_pubacks[i].reservation == entry)
    {
      client->pending_pubacks[i].reservation = PUBACK_POOL_SHARED;
    }
  }
  client->puback_reservations[entry].owner = NULL;
  client->puback_reservations[entry].count = 0;
}

void az_iot_connection_client__abandon_pubacks(az_iot_connection_client* client, const void* owner)
{
  if (client == NULL || owner == NULL)
  {
    return;
  }
  puback_abandon(client, owner);
}

az_iot_result az_iot_connection_client__add_feature_observer(
    az_iot_connection_client* client,
    uint8_t events,
    az_iot_connection_feature_observer_callback cb,
    void* user_ctx)
{
  if (client == NULL || cb == NULL || events == 0u)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* A state observer added mid-dispatch would see the transition in flight. */
  if (client->dispatching_state && (events & (uint8_t)AZ_IOT_CONNECTION_FEATURE_EVENT_STATE))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION, "cannot add a state observer from inside one");
    return AZ_IOT_ERR_BUSY;
  }
  size_t const count = client->opts.limits.max_feature_observers;
  size_t free_slot = count;
  for (size_t i = 0; i < count; ++i)
  {
    az_iot_connection_feature_observer* observer = &client->feature_observers[i];
    if (observer->cb == cb && observer->user_ctx == user_ctx)
    {
      observer->events = events;
      return AZ_IOT_OK;
    }
    if (observer->cb == NULL && free_slot == count)
    {
      free_slot = i;
    }
  }
  if (free_slot == count)
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_CONNECTION, "no free feature-observer slot");
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  client->feature_observers[free_slot].cb = cb;
  client->feature_observers[free_slot].user_ctx = user_ctx;
  client->feature_observers[free_slot].events = events;
  ++client->feature_observer_registrations;
  return AZ_IOT_OK;
}

/** @brief Index of the observer with @p cb and @p user_ctx; limits.max_feature_observers if none.
 */
static size_t find_feature_observer(
    const az_iot_connection_client* client,
    az_iot_connection_feature_observer_callback cb,
    const void* user_ctx)
{
  size_t i = 0;
  while (i < client->opts.limits.max_feature_observers
         && !(
             client->feature_observers[i].cb == cb
             && client->feature_observers[i].user_ctx == user_ctx))
  {
    ++i;
  }
  return i;
}

void az_iot_connection_client__remove_feature_observer(
    az_iot_connection_client* client,
    az_iot_connection_feature_observer_callback cb,
    const void* user_ctx)
{
  if (client == NULL || cb == NULL)
  {
    return;
  }
  size_t const i = find_feature_observer(client, cb, user_ctx);
  if (i < client->opts.limits.max_feature_observers)
  {
    memset(&client->feature_observers[i], 0, sizeof(client->feature_observers[i]));
  }
}

bool az_iot_connection_client__has_feature_observer(
    const az_iot_connection_client* client,
    az_iot_connection_feature_observer_callback cb,
    const void* user_ctx)
{
  return client != NULL && cb != NULL
      && find_feature_observer(client, cb, user_ctx) < client->opts.limits.max_feature_observers;
}

bool az_iot_connection_client__can_track_publish(
    const az_iot_connection_client* client,
    const void* owner)
{
  return client != NULL
      && puback_free_slot(client, puback_pool_of(client, owner))
      < client->opts.limits.max_pending_publishes;
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
  size_t slot = client->opts.limits.max_persistent_subscriptions;
  for (size_t i = 0; i < client->opts.limits.max_persistent_subscriptions; ++i)
  {
    if (!client->persistent_subs[i].in_use)
    {
      slot = i;
      break;
    }
  }
  if (slot == client->opts.limits.max_persistent_subscriptions)
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
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "cannot register '%s': all %d persistent subscription slots are in use "
        "(raise limits.max_persistent_subscriptions)",
        topic_filter,
        (int)client->opts.limits.max_persistent_subscriptions);
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
      AZ_IOT_LOG_ERRORF(AZ_IOT_LOG_COMPONENT_CONNECTION, "could not subscribe '%s'", topic_filter);
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
  for (size_t i = 0; i < client->opts.limits.max_persistent_subscriptions; ++i)
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
  for (size_t i = 0; i < c->opts.limits.max_persistent_subscriptions; ++i)
  {
    if (!c->persistent_subs[i].in_use || c->persistent_subs[i].profile == c->connection_profile)
    {
      continue;
    }
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "dropping '%s' -- registered for a different hub generation than the one now "
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
#define CSR_JSON_TRACKING_ID "trackingId"
#define CSR_JSON_INFO "info"
#define CSR_JSON_CREDENTIAL_ERROR "credentialError"
#define CSR_JSON_CREDENTIAL_MESSAGE "credentialMessage"

/** @brief Fields of a hub renewal error body; strings are NUL-terminated, "" when absent. */
typedef struct csr_error_info
{
  int32_t code; /**< errorCode; 0 when absent. */
  int32_t retry_after_s; /**< retryAfter; 0 when absent. */
  char credential_error[64]; /**< info.credentialError. */
  char credential_message[256]; /**< info.credentialMessage. */
  char tracking_id[128]; /**< trackingId. */
} csr_error_info;

/** @brief Zero @p evt, set its kind and its string fields to "". */
static void csr_event_init(az_iot_csr_event* evt, az_iot_csr_event_kind kind)
{
  memset(evt, 0, sizeof(*evt));
  evt->kind = kind;
  evt->credential_error = "";
  evt->credential_message = "";
  evt->tracking_id = "";
}

/** @brief Hands @p evt to the CSR callback @p cb, if any, as a tracked callback. */
static void report_csr_event(
    az_iot_connection_client* c,
    az_iot_csr_callback cb,
    const az_iot_csr_event* evt,
    void* user_ctx)
{
  if (cb)
  {
    enter_callback(c);
    cb(evt, user_ctx);
    leave_callback(c);
  }
}

/**
 * @brief Copy the string at the reader's token into @p dst with JSON escapes decoded, except
 *        `\` `u` + 4 hex digits, kept as received. Left "" when the token is not a string or
 *        does not fit: never truncated.
 */
static void csr_json_copy_string(const az_json_reader* jr, char* dst, size_t dst_size)
{
  /* The reader has one segment, so the slice is the whole value, already validated. */
  const uint8_t* src = az_span_ptr(jr->token.slice);
  int32_t src_len = az_span_size(jr->token.slice);
  size_t out = 0;
  dst[0] = '\0';
  if (jr->token.kind != AZ_JSON_TOKEN_STRING)
  {
    return;
  }
  for (int32_t i = 0; i < src_len; ++i)
  {
    char ch = (char)src[i];
    if (ch == '\\' && i + 1 < src_len)
    {
      ch = (char)src[++i];
      switch (ch)
      {
        case 'b':
          ch = '\b';
          break;
        case 'f':
          ch = '\f';
          break;
        case 'n':
          ch = '\n';
          break;
        case 'r':
          ch = '\r';
          break;
        case 't':
          ch = '\t';
          break;
        case 'u':
          if (out + 2 >= dst_size)
          {
            dst[0] = '\0';
            return;
          }
          dst[out++] = '\\';
          break;
        default: /* '"', '\\', '/' */
          break;
      }
    }
    if (out + 1 >= dst_size)
    {
      dst[0] = '\0';
      return;
    }
    dst[out++] = ch;
  }
  dst[out] = '\0';
}

/**
 * @brief Read `info.credentialError` and `info.credentialMessage`; the reader is at the
 *        object's BEGIN_OBJECT and is left at its END_OBJECT.
 * @return false if the body ended or is malformed.
 */
static bool csr_parse_error_info(az_json_reader* jr, csr_error_info* out)
{
  while (az_result_succeeded(az_json_reader_next_token(jr)))
  {
    if (jr->token.kind == AZ_JSON_TOKEN_END_OBJECT)
    {
      return true;
    }
    if (jr->token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
    {
      continue;
    }
    bool is_error
        = az_json_token_is_text_equal(&jr->token, AZ_SPAN_FROM_STR(CSR_JSON_CREDENTIAL_ERROR));
    bool is_message
        = az_json_token_is_text_equal(&jr->token, AZ_SPAN_FROM_STR(CSR_JSON_CREDENTIAL_MESSAGE));
    if (az_result_failed(az_json_reader_next_token(jr)))
    {
      return false;
    }
    if (is_error)
    {
      csr_json_copy_string(jr, out->credential_error, sizeof(out->credential_error));
    }
    else if (is_message)
    {
      csr_json_copy_string(jr, out->credential_message, sizeof(out->credential_message));
    }
    if ((jr->token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT
         || jr->token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
        && az_result_failed(az_json_reader_skip_children(jr)))
    {
      return false;
    }
  }
  return false;
}

/** @brief Parse a hub renewal error body into @p out. Every field is optional. */
static void csr_parse_error(az_span payload, csr_error_info* out)
{
  memset(out, 0, sizeof(*out));
  az_json_reader jr;
  if (az_span_size(payload) <= 0 /* az_json_reader_init() would spin on it */
      || az_result_failed(az_json_reader_init(&jr, payload, NULL))
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
    bool is_tracking
        = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR(CSR_JSON_TRACKING_ID));
    bool is_info = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR(CSR_JSON_INFO));
    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      return;
    }
    if (is_code && jr.token.kind == AZ_JSON_TOKEN_NUMBER)
    {
      if (az_result_failed(az_json_token_get_int32(&jr.token, &out->code)))
      {
        out->code = 0;
      }
    }
    else if (is_retry && jr.token.kind == AZ_JSON_TOKEN_NUMBER)
    {
      if (az_result_failed(az_json_token_get_int32(&jr.token, &out->retry_after_s)))
      {
        out->retry_after_s = 0;
      }
    }
    else if (is_tracking && jr.token.kind == AZ_JSON_TOKEN_STRING)
    {
      csr_json_copy_string(&jr, out->tracking_id, sizeof(out->tracking_id));
    }
    else if (is_info && jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT)
    {
      if (!csr_parse_error_info(&jr, out))
      {
        return;
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

/**
 * @brief PUBACK for a renewal request. A rejection completes the operation now instead of at
 *        CSR_OP_TIMEOUT_MS. A session that ended first does not: the response may still arrive
 *        on the next one, as for a request acknowledged before the drop.
 *
 * Cancel and timeout abandon the request's slot, so an ack that still matches one is always for
 * the open operation.
 */
static void on_csr_puback(az_iot_result status, void* user_ctx)
{
  az_iot_connection_client* c = (az_iot_connection_client*)user_ctx;
  if (status == AZ_IOT_OK || status == AZ_IOT_ERR_NOT_CONNECTED || !c->csr_op.in_use)
  {
    return;
  }
  az_iot_csr_callback cb = c->csr_op.cb;
  void* uc = c->csr_op.user_ctx;
  c->csr_op.in_use = false;
  AZ_IOT_LOG_WARNF(
      AZ_IOT_LOG_COMPONENT_CONNECTION,
      "certificate renewal request %s rejected (%s)",
      c->csr_op.request_id,
      az_iot_result_to_string(status));
  az_iot_csr_event evt;
  csr_event_init(&evt, AZ_IOT_CSR_FAILED);
  evt.status = status;
  report_csr_event(c, cb, &evt, uc);
}

/**
 * @brief Store a hub-issued chain in the certificate provider. Never touches the
 * live session: on failure the previous credential stays in use for the next
 * connect.
 *
 * @return AZ_IOT_OK if stored, else the provider's error (AZ_IOT_ERR_NOT_SUPPORTED
 * if the provider can no longer store).
 */
static az_iot_result csr_store_issued(
    az_iot_connection_client* c,
    const az_iot_issued_certificate* issued)
{
  az_iot_certificate_provider* p = c->opts.certificate_provider;
  if (!provider_owns_csr(p))
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  az_iot_result rc = p->vtable->store_issued_certificate(p, issued);
  if (rc != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "storing the renewed certificate failed (%s); keeping the current one",
        az_iot_result_to_string(rc));
  }
  return rc;
}

/* Inbound handler for $iothub/credentials/res/{status}/?$rid={rid}. */
static void on_csr_response(void* user_ctx, const az_iot_mqtt_message* msg)
{
  az_iot_connection_client* c = (az_iot_connection_client*)user_ctx;
  if (!c || !msg || !msg->topic || !c->csr_op.in_use)
  {
    return;
  }

  /* Parse "<status>/?<properties>" after the prefix: a 3-digit status, then
   * $rid as one of the '&'-separated properties. Anything else is not a
   * response to this client's request. */
  const char* p = msg->topic + (sizeof(CSR_RES_PREFIX) - 1);
  int status = 0;
  int digits = 0;
  while (*p >= '0' && *p <= '9' && digits < 3)
  {
    status = status * 10 + (*p - '0');
    ++p;
    ++digits;
  }
  if (digits != 3 || p[0] != '/' || p[1] != '?')
  {
    return;
  }
  p += 2;
  const char* rid = NULL;
  for (const char* prop = p; prop != NULL;)
  {
    if (strncmp(prop, "$rid=", 5) == 0)
    {
      rid = prop + 5;
      break;
    }
    prop = strchr(prop, '&');
    if (prop != NULL)
    {
      ++prop; /* past the separator; empty properties are skipped in turn */
    }
  }
  if (!rid)
  {
    return;
  }
  size_t rid_len = strcspn(rid, "&");
  if (rid_len != strlen(c->csr_op.request_id) || strncmp(rid, c->csr_op.request_id, rid_len) != 0)
  {
    return; /* response for a different request */
  }

  az_iot_csr_callback cb = c->csr_op.cb;
  void* uc = c->csr_op.user_ctx;
  /* An empty message may carry a NULL payload: az_span_create() must not see it. */
  az_span payload = (msg->payload != NULL && msg->payload_len > 0)
      ? az_span_create((uint8_t*)(uintptr_t)msg->payload, (int32_t)msg->payload_len)
      : AZ_SPAN_EMPTY;
  az_iot_csr_event evt;
  csr_event_init(&evt, AZ_IOT_CSR_ACCEPTED);

  if (status == 202)
  {
    /* Accepted: signing in progress; keep the op open for the 200/error and
     * extend the deadline so a slow-but-alive signer is not timed out. */
    c->csr_op.deadline_ms = az_iot_time_mono_ms() + CSR_OP_TIMEOUT_MS;
    evt.kind = AZ_IOT_CSR_ACCEPTED;
    evt.status = AZ_IOT_OK;
    report_csr_event(c, cb, &evt, uc);
    return;
  }

  /* Terminal: the operation completes here regardless of outcome. Its PUBACK may still be due;
   * free the slot first so a renewal started from the callback is not refused. */
  c->csr_op.in_use = false;
  puback_abandon(c, &c->csr_op);

  if (status == 200)
  {
    az_span certs[CERT_CHAIN_MAX_CERTS];
    size_t count = 0;
    az_iot_result rc = AZ_IOT_ERR_PROTOCOL;
    az_json_reader jr;
    /* A truncated or malformed response is never treated as ISSUED. */
    if (az_iot_cert_util_json_is_complete(payload)
        && az_result_succeeded(az_json_reader_init(&jr, payload, NULL))
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
      evt.store_status = csr_store_issued(c, &issued);
      report_csr_event(c, cb, &evt, uc);
    }
    else
    {
      evt.kind = AZ_IOT_CSR_FAILED;
      evt.status = (rc == AZ_IOT_OK) ? AZ_IOT_ERR_PROTOCOL : rc;
      report_csr_event(c, cb, &evt, uc);
    }
    return;
  }

  /* Error response (4xx/5xx). */
  {
    csr_error_info err;
    csr_parse_error(payload, &err);
    evt.kind = AZ_IOT_CSR_FAILED;
    evt.status = AZ_IOT_ERR_MQTT;
    evt.service_code = err.code;
    evt.retry_after_s = (uint32_t)(err.retry_after_s > 0 ? err.retry_after_s : 0);
    evt.credential_error = err.credential_error;
    evt.credential_message = err.credential_message;
    evt.tracking_id = err.tracking_id;
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "certificate renewal failed: status=%d errorCode=%d credentialError=%s trackingId=%s",
        status,
        (int)err.code,
        err.credential_error[0] != '\0' ? err.credential_error : "-",
        err.tracking_id[0] != '\0' ? err.tracking_id : "-");
    report_csr_event(c, cb, &evt, uc);
  }
}

/**
 * @brief Validate @p csr_base64, then subscribe (once), build and publish the
 * renewal request, and open the in-flight slot.
 */
static az_iot_result csr_publish(
    az_iot_connection_client* client,
    const char* csr_base64,
    const char* device_id,
    const char* request_id,
    const char* replace)
{
  /* Validate the CSR: base64 and within the 8 KB service cap. */
  size_t csr_len = 0;
  if (!az_iot_cert_util_is_base64(csr_base64, &csr_len) || csr_len > CSR_MAX_BASE64)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "send_csr: the certificate provider's CSR is not base64 or exceeds 8 KB");
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
  char* body = (char*)az_span_ptr(client->opts.csr_payload_buffer);
  size_t body_len = 0;
  const char* body_parts[]
      = { CSR_RENEW_BODY_ID_PREFIX,     device_id, CSR_RENEW_BODY_CSR_INFIX, csr_base64,
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
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "send_csr: opts.csr_payload_buffer is too small for the request body");
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

  client->csr_op.in_use = true;
  client->csr_op.deadline_ms = az_iot_time_mono_ms() + CSR_OP_TIMEOUT_MS;

  az_iot_result r
      = az_iot_connection_client__publish(client, &client->csr_op, &msg, on_csr_puback, client);
  if (r != AZ_IOT_OK)
  {
    client->csr_op.in_use = false;
    return r;
  }
  return AZ_IOT_OK;
}

AZ_NODISCARD az_iot_result az_iot_connection_client_send_csr(
    az_iot_connection_client* client,
    const char* request_id,
    const char* replace,
    az_iot_csr_callback cb,
    void* user_ctx)
{
  if (!client || !cb)
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
  az_iot_certificate_provider* provider = client->opts.certificate_provider;
  if (!provider_owns_csr(provider))
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "send_csr: the certificate provider does not implement get_csr, release_csr and "
        "store_issued_certificate");
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }

  const char* device_id = az_iot_connection_client__device_id(client);
  if (!device_id)
  {
    device_id = "";
  }
  az_iot_certificate_signing_request csr = { 0 };
  az_iot_result r = provider->vtable->get_csr(provider, device_id, &csr);
  if (r != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_CONNECTION,
        "send_csr: certificate provider get_csr failed (%s)",
        az_iot_result_to_string(r));
    return r;
  }
  if (!csr.csr_base64)
  {
    AZ_IOT_LOG_ERROR(
        AZ_IOT_LOG_COMPONENT_CONNECTION, "send_csr: certificate provider get_csr returned no CSR");
    provider->vtable->release_csr(provider, &csr);
    return AZ_IOT_ERR_INTERNAL;
  }

  client->csr_op.cb = cb;
  client->csr_op.user_ctx = user_ctx;
  r = csr_publish(client, csr.csr_base64, device_id, request_id, replace);
  provider->vtable->release_csr(provider, &csr);
  return r;
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
  puback_abandon(client, &client->csr_op);
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
    case AZ_IOT_CONN_STATE_RETRY_PENDING:
      return "AZ_IOT_CONN_STATE_RETRY_PENDING";
    case AZ_IOT_CONN_STATE_DISCONNECTING:
      return "AZ_IOT_CONN_STATE_DISCONNECTING";
    case AZ_IOT_CONN_STATE_FAULTED:
      return "AZ_IOT_CONN_STATE_FAULTED";
    case AZ_IOT_CONN_STATE_SETTING_UP:
      return "AZ_IOT_CONN_STATE_SETTING_UP";
    default:
      return "UNKNOWN";
  }
}

const char* az_iot_connection_profile_to_string(az_iot_connection_profile profile)
{
  switch (profile)
  {
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V3:
      return "AZ_IOT_CONNECTION_PROFILE_MQTT_V3";
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V5:
      return "AZ_IOT_CONNECTION_PROFILE_MQTT_V5";
    case AZ_IOT_CONNECTION_PROFILE_UNKNOWN:
      return "AZ_IOT_CONNECTION_PROFILE_UNKNOWN";
    default:
      return "UNKNOWN";
  }
}

/** @brief Value of hex digit @p ch, or -1. */
static int hex_digit_value(char ch)
{
  if (ch >= '0' && ch <= '9')
  {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f')
  {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F')
  {
    return ch - 'A' + 10;
  }
  return -1;
}

/** @brief Whether URL-encoded @p enc decodes to the concatenation of the
 * NULL-terminated @p parts. */
static bool sas_uri_decodes_to(const char* enc, size_t enc_len, const char* const* parts)
{
  size_t part = 0;
  const char* expected = parts[0];
  size_t i = 0;
  while (i < enc_len)
  {
    char ch = enc[i];
    if (ch == '%')
    {
      int hi = i + 2 < enc_len ? hex_digit_value(enc[i + 1]) : -1;
      int lo = hi >= 0 ? hex_digit_value(enc[i + 2]) : -1;
      if (lo < 0)
      {
        return false;
      }
      ch = (char)((hi << 4) | lo);
      i += 3;
    }
    else
    {
      i++;
    }
    while (expected != NULL && *expected == '\0')
    {
      expected = parts[++part];
    }
    if (expected == NULL || *expected != ch)
    {
      return false;
    }
    expected++;
  }
  while (expected != NULL && *expected == '\0')
  {
    expected = parts[++part];
  }
  return expected == NULL;
}

/** @brief Counts @p key among the `&`-separated @p fields; the first one's
 * value goes to @p value. */
static size_t sas_token_field(
    const char* fields,
    size_t len,
    const char* key,
    const char** value,
    size_t* value_len)
{
  size_t key_len = strlen(key);
  size_t count = 0;
  size_t i = 0;
  while (i < len)
  {
    size_t end = i;
    while (end < len && fields[end] != '&')
    {
      end++;
    }
    if (end - i > key_len && memcmp(fields + i, key, key_len) == 0 && fields[i + key_len] == '=')
    {
      if (count++ == 0)
      {
        *value = fields + i + key_len + 1u;
        *value_len = end - i - key_len - 1u;
      }
    }
    i = end + 1u;
  }
  return count;
}

/**
 * @brief Checks that @p token is a SAS token for @p scope's current identity:
 * its `sr` decodes to the resource URI, and its `skn` is the key name.
 *
 * @return AZ_IOT_OK; AZ_IOT_ERR_NOT_FOUND when the identity is not known yet;
 * AZ_IOT_ERR_INVALID_ARG otherwise.
 */
static az_iot_result check_sas_token_identity(
    const az_iot_connection_client* c,
    az_iot_connection_scope scope,
    const char* token,
    size_t len)
{
  const char* parts[4] = { NULL, NULL, NULL, NULL };
  if (scope == AZ_IOT_CONN_SCOPE_DPS)
  {
    parts[0] = c->opts.dps.id_scope;
    parts[1] = "/registrations/";
    parts[2] = c->opts.dps.registration_id;
  }
  else
  {
    parts[0] = c->opts.iothub_hostname;
    parts[1] = "/devices/";
    parts[2] = c->opts.device_id;
  }
  if (!is_nonempty_cstr(parts[0]) || !is_nonempty_cstr(parts[2]))
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }
  static const char prefix[] = "SharedAccessSignature ";
  size_t prefix_len = sizeof(prefix) - 1u;
  if (len <= prefix_len || memcmp(token, prefix, prefix_len) != 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  const char* fields = token + prefix_len;
  size_t fields_len = len - prefix_len;
  const char* value = NULL;
  size_t value_len = 0;
  /* Each field once: a duplicate could be read differently by the service. */
  if (sas_token_field(fields, fields_len, "sr", &value, &value_len) != 1u
      || !sas_uri_decodes_to(value, value_len, parts)
      || sas_token_field(fields, fields_len, "sig", &value, &value_len) != 1u || value_len == 0
      || sas_token_field(fields, fields_len, "se", &value, &value_len) != 1u || value_len == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  size_t key_names = sas_token_field(fields, fields_len, "skn", &value, &value_len);
  bool key_name_ok = scope == AZ_IOT_CONN_SCOPE_DPS
      ? key_names == 1u && value_len == strlen(DPS_SAS_KEY_NAME)
          && memcmp(value, DPS_SAS_KEY_NAME, value_len) == 0
      : key_names == 0u;
  return key_name_ok ? AZ_IOT_OK : AZ_IOT_ERR_INVALID_ARG;
}

AZ_NODISCARD az_iot_result az_iot_connection_client_update_sas_token(
    az_iot_connection_client* client,
    az_iot_connection_scope scope,
    const char* token,
    size_t token_len,
    uint32_t lifetime_seconds)
{
  if (client == NULL || (scope != AZ_IOT_CONN_SCOPE_DPS && scope != AZ_IOT_CONN_SCOPE_HUB)
      || token == NULL || token_len == 0 || lifetime_seconds == 0
      || memchr(token, '\0', token_len) != NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (auth_of(client, scope)->sas.on_sas_token_required == NULL || client->sas_token == NULL)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  /* The token area is a CONNECT's until connect() has taken it, and the
   * asked scope's (with its resource URI) until its callback returns. */
  if (client->sas_token_in_use != 0
      || (client->sas_token_asking != 0 && client->sas_token_asking != (uint8_t)(scope + 1)))
  {
    return AZ_IOT_ERR_BUSY;
  }
  az_iot_result r = check_sas_token_identity(client, scope, token, token_len);
  if (r != AZ_IOT_OK)
  {
    return r;
  }
  if (token_len > client->sas_token_capacity)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  /* A session on a user-provided token is renewed with it. */
  bool renew = sas_session_renewable(client, scope)
      && client->auth[scope].source == AZ_IOT_AUTH_SOURCE_USER_PROVIDED;
  if (client->sas_token_request[scope].request_id == 0)
  {
    open_sas_token_request(client, scope, renew, 0);
    client->sas_token_request[scope].held = !renew;
  }
  else if (renew)
  {
    client->sas_token_request[scope].for_renewal = true;
    client->sas_token_request[scope].held = false;
  }
  client->sas_token_request[scope].asked = true;
  /* A token held for the other scope is replaced. Copied before the rest is
   * wiped: @p token may point into the area. */
  if (client->sas_token_holder != 0 && client->sas_token_holder != (uint8_t)(scope + 1))
  {
    az_iot_connection_scope other
        = client->sas_token_holder == 1u ? AZ_IOT_CONN_SCOPE_DPS : AZ_IOT_CONN_SCOPE_HUB;
    if (client->sas_token_request[other].held)
    {
      memset(&client->sas_token_request[other], 0, sizeof(client->sas_token_request[other]));
    }
    else
    {
      client->sas_token_request[other].asked = false;
      client->sas_token_request[other].ready = false;
    }
  }
  memmove(client->sas_token, token, token_len);
  client->sas_token[token_len] = '\0';
  if (client->sas_token_asking == 0)
  {
    memset(client->sas_token + token_len + 1u, 0, client->sas_token_size - token_len - 1u);
  }
  client->sas_token_holder = (uint8_t)(scope + 1);
  client->sas_token_request[scope].ready = true;
  client->sas_token_request[scope].token_len = token_len;
  client->sas_token_request[scope].valid_seconds = lifetime_seconds;
  client->sas_token_request[scope].delivered_ms = az_iot_time_mono_ms();
  client->sas_token_request[scope].delivered_unix_seconds = unix_now(client);
  return AZ_IOT_OK;
}
