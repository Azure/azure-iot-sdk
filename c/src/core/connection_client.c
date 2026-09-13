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
#include <stddef.h> /* offsetof, for the bounded write in get_hub_profile */
#include <stdlib.h>
#include <string.h>

#include "azure/iot/az_iot_connection_client.h"
#include "azure/iot/az_iot_mqtt_iface.h"
#include "azure/iot/az_iot_result.h"
#include "azure/iot/az_iot_version.h"

#include "internal/cert_util.h"
#include "internal/connection_client_internal.h"
#include "internal/dispatch.h"
#include "internal/log_internal.h"
#include "internal/reconnect.h"
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

/* DPS registration body carrying the operational-cert CSR (base64 DER). */
#define DPS_REGISTER_CSR_BODY_PREFIX "{\"csr\":\""
#define DPS_REGISTER_CSR_BODY_SUFFIX "\"}"

/* CSR-based operational-certificate issuance (Azure Device Registration / ADR)
 * requires a newer DPS API version than the azure-sdk-for-c default GA version
 * ("2019-03-31"), which does not support it. When enrolling for an operational
 * certificate the DPS MQTT username is rebuilt with this version. */
#define DPS_CSR_API_VERSION "2025-07-01-preview"
#define DPS_USERNAME_CSR_INFIX "/registrations/"
#define DPS_USERNAME_CSR_SUFFIX "/api-version=" DPS_CSR_API_VERSION

/* DPS ASSIGNED result fields that carry the issued operational chain. */
#define DPS_JSON_REGISTRATION_STATE "registrationState"
#define DPS_JSON_ISSUED_CERT_CHAIN "issuedCertificateChain"
/* The hub generation the device was assigned to. A string, and an extensible
 * union: absent or null means "classic". New in api-version 2026-11-02-preview,
 * which this repo patches azure-sdk-for-c up to -- see
 * patches/azure-sdk-for-c/0001-dps-api-version.patch. */
#define DPS_JSON_CONNECTION_PROFILE "connectionProfile"
#define CONNECTION_PROFILE_CLASSIC_STR "classic"
#define CONNECTION_PROFILE_MQTT_V5_STR "mqttV5"
#define DPS_CONNECTION_PROFILE_OVERRIDE_ENV "AZ_IOT_DPS_CONNECTION_PROFILE_OVERRIDE"

/* Max certs in an issued chain (leaf + a few intermediates). The chain is
 * delivered as zero-copy spans into the payload. */
#define CERT_CHAIN_MAX_CERTS 6u

/* ------------------------------------------------------------------------- */
/* AEG/Hub-Next presence (birth) handshake wire constants. Mirrors the .NET    */
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
 * subscription that AEG's topic-space authorization is guaranteed to grant and
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
 * echoed unchanged on the birth-ack. 16 bytes matches the .NET GUID nonce. */
#define PRESENCE_NONCE_LEN 16u

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

const char* az_iot_mqtt_role_to_string(az_iot_mqtt_role r)
{
  switch (r)
  {
    case AZ_IOT_MQTT_ROLE_DPS:
      return "DPS";
    case AZ_IOT_MQTT_ROLE_HUB_CLASSIC:
      return "HUB_CLASSIC";
    case AZ_IOT_MQTT_ROLE_HUB_NEXT:
      return "HUB_NEXT";
    default:
      return "ROLE?";
  }
}

static bool reconnect_enabled(const az_iot_connection_client* c)
{
  return c->opts.reconnection_policy.initial_delay_ms > 0;
}

static void transition(
    az_iot_connection_client* c,
    az_iot_connection_state next,
    az_iot_result reason)
{
  if (c->state == next)
  {
    return;
  }
  c->state = next;
  if (c->state_cb)
  {
    az_iot_hub_profile profile = AZ_IOT_HUB_PROFILE_INIT;
    az_iot_connection_state_event event = {
      ._internal_size = sizeof(az_iot_connection_state_event),
      .state = next,
      .reason = reason,
      .profile = NULL,
    };
    if (next == AZ_IOT_CONN_STATE_CONNECTED || reason == AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH
        || reason == AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED)
    {
      profile.connection_profile = c->connection_profile;
      profile.connection_profile_raw = c->connection_profile_raw;
      profile.connection_profile_raw_truncated = c->connection_profile_raw_truncated;
      event.profile = &profile;
    }
    if (next == AZ_IOT_CONN_STATE_CONNECTED)
    {
      c->consecutive_hub_connect_failures = 0;
    }
    c->state_cb(&event, c->state_cb_ctx);
  }
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
  return transport == AZ_IOT_MQTT_TRANSPORT_WEBSOCKET ? (uint16_t)443 : (uint16_t)8883;
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

static void teardown_active(az_iot_connection_client* c)
{
  if (c->active_client && c->active_client->iface && c->active_client->iface->destroy)
  {
    c->active_client->iface->destroy(c->active_client);
  }
  c->active_client = NULL;
  /* Abandon any in-flight AEG presence (birth) handshake: it belonged to the
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

/* Forward decl — used in dps_apply_deferred(). */
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

static void schedule_reconnect(az_iot_connection_client* c, az_iot_result reason)
{
  teardown_active(c);
  c->reconnect_attempt++;

  /* A hub vacated service-side may stop answering rather than rejecting the
   * identity, in which case nothing else would ever send us back to DPS. Only
   * hub attempts count -- a failing dps_start() must fall back to an ordinary
   * retry rather than re-arming this and pinning every attempt to DPS. */
  if (c->session_role != AZ_IOT_MQTT_ROLE_DPS)
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
  }

  if (c->opts.reconnection_policy.max_attempts > 0
      && c->reconnect_attempt > c->opts.reconnection_policy.max_attempts)
  {
    transition(c, AZ_IOT_CONN_STATE_FAULTED, reason);
    return;
  }

  uint32_t delay = az_iot_reconnect_delay_ms(
      &c->opts.reconnection_policy, c->reconnect_attempt, &c->rng_state);
  c->reconnect_due_ms = az_iot_time_mono_ms() + delay;
  transition(c, AZ_IOT_CONN_STATE_RECONNECTING, reason);
}

/* ------------------------------------------------------------------------- */
/* DPS provisioning (internal, driven from open/do_work)                     */
/* ------------------------------------------------------------------------- */

static bool dps_configured(const az_iot_connection_client* c)
{
  return is_nonempty_cstr(c->opts.dps.id_scope);
}

static void dps_teardown_mqtt(az_iot_connection_client* c)
{
  c->dps_subscription_confirmed = false;
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

  /* CSR-based enrollment (D2): request an operational cert by sending the
   * provider's CSR as the registration body {"csr":"<base64 DER>"}, built into
   * the CALLER-PROVIDED payload buffer (opts.csr_payload_buffer) - the SDK
   * declares no payload buffer of its own. The registration id travels in the
   * DPS username/topic, not the body. */
  if (c->dps_enrolling)
  {
    az_iot_certificate_provider* provider = c->opts.certificate_provider;
    if (provider == NULL || provider->vtable->get_csr == NULL)
    {
      AZ_IOT_LOG_ERROR("dps register: request_operational_certificate is set but the certificate "
                       "provider does not implement get_csr");
      return AZ_IOT_ERR_NOT_SUPPORTED;
    }

    char* body = (char*)az_span_ptr(c->opts.csr_payload_buffer);
    size_t body_cap = (size_t)az_span_size(c->opts.csr_payload_buffer);
    if (body == NULL || body_cap == 0)
    {
      AZ_IOT_LOG_ERROR("dps register: opts.csr_payload_buffer is required for CSR enrollment");
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }

    az_iot_certificate_signing_request csr = { 0 };
    az_iot_result csr_result
        = provider->vtable->get_csr(provider, c->opts.dps.registration_id, &csr);
    if (csr_result != AZ_IOT_OK || csr.csr_base64 == NULL)
    {
      AZ_IOT_LOG_ERROR("dps register: certificate provider get_csr failed");
      return (csr_result != AZ_IOT_OK) ? csr_result : AZ_IOT_ERR_INTERNAL;
    }

    size_t body_len = 0;
    const char* body_parts[]
        = { DPS_REGISTER_CSR_BODY_PREFIX, csr.csr_base64, DPS_REGISTER_CSR_BODY_SUFFIX };
    az_iot_result body_result
        = az_iot_span_writer_build_str(c->opts.csr_payload_buffer, &body_len, body_parts, 3);

    if (provider->vtable->release_csr != NULL)
    {
      provider->vtable->release_csr(provider, &csr);
    }
    if (body_result != AZ_IOT_OK)
    {
      AZ_IOT_LOG_ERROR("dps register: opts.csr_payload_buffer is too small for the CSR body");
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }

    msg.payload = (const uint8_t*)body;
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
  else if (strcmp(c->connection_profile_raw, CONNECTION_PROFILE_CLASSIC_STR) == 0)
  {
    c->connection_profile = AZ_IOT_CONNECTION_PROFILE_CLASSIC;
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
  char value[AZ_IOT_CONNECTION_PROFILE_RAW_BUF] = { 0 };

#ifdef _WIN32
  size_t needed = 0;
  errno_t env_result = getenv_s(&needed, NULL, 0, DPS_CONNECTION_PROFILE_OVERRIDE_ENV);
  if (env_result != 0)
  {
    AZ_IOT_LOG_ERROR("dps: could not read " DPS_CONNECTION_PROFILE_OVERRIDE_ENV);
    return AZ_IOT_ERR_INTERNAL;
  }
  if (needed == 0)
  {
    return AZ_IOT_OK;
  }
  if (needed > sizeof(value))
  {
    AZ_IOT_LOG_ERROR("dps: " DPS_CONNECTION_PROFILE_OVERRIDE_ENV " is too long");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  env_result = getenv_s(&needed, value, sizeof(value), DPS_CONNECTION_PROFILE_OVERRIDE_ENV);
  if (env_result != 0)
  {
    AZ_IOT_LOG_ERROR("dps: could not read " DPS_CONNECTION_PROFILE_OVERRIDE_ENV);
    return AZ_IOT_ERR_INTERNAL;
  }
  if (value[0] == '\0')
  {
    return AZ_IOT_OK;
  }
#else
  const char* configured = getenv(DPS_CONNECTION_PROFILE_OVERRIDE_ENV);
  if (!is_nonempty_cstr(configured))
  {
    return AZ_IOT_OK;
  }
  size_t needed = strlen(configured) + 1u;
  if (needed > sizeof(value))
  {
    AZ_IOT_LOG_ERROR("dps: " DPS_CONNECTION_PROFILE_OVERRIDE_ENV " is too long");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memcpy(value, configured, needed);
#endif

  if (strcmp(value, CONNECTION_PROFILE_CLASSIC_STR) != 0
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
 * meaning "classic" -- so the caller is left with the classic default it was
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
      /* null (or any non-string) resolves to the classic default unless the
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
          AZ_IOT_LOG_ERRORF(
              "dps register: provisioning failed/disabled; DPS response: %.*s",
              (int)az_span_size(payload_span),
              (const char*)az_span_ptr(payload_span));
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

  /* Build the DPS MQTT username. CSR-based operational-certificate issuance
   * (Azure Device Registration) requires a newer DPS API version than the
   * azure-sdk-for-c default (2019-03-31); build the username with it when
   * enrolling, otherwise use the SDK helper for the default version. */
  char dps_username[AZ_IOT_MQTT_USERNAME_BUF];
  if (c->opts.dps.request_operational_certificate)
  {
    const char* username_parts[] = { c->opts.dps.id_scope,
                                     DPS_USERNAME_CSR_INFIX,
                                     c->opts.dps.registration_id,
                                     DPS_USERNAME_CSR_SUFFIX };
    if (az_iot_span_writer_build_str(AZ_SPAN_FROM_BUFFER(dps_username), NULL, username_parts, 4)
        != AZ_IOT_OK)
    {
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
  }
  else
  {
    size_t dps_username_len = 0;
    ar = az_iot_provisioning_client_get_user_name(
        &c->dps_prov, dps_username, sizeof(dps_username), &dps_username_len);
    if (az_result_failed(ar))
    {
      return AZ_IOT_ERR_INTERNAL;
    }
  }
  copts.username = dps_username;
  AZ_IOT_LOG_DEBUGF("dps: connecting with username %s", dps_username);

  /* Populate TLS from certificate_provider if available. DPS uses the bootstrap
   * identity; the operational cert (if any) is issued during this exchange. */
  if (c->opts.certificate_provider)
  {
    az_iot_certificate_material mat = { 0 };
    if (c->opts.certificate_provider->vtable->load(
            c->opts.certificate_provider, AZ_IOT_CRED_BOOTSTRAP, &mat)
        == AZ_IOT_OK)
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
    }
    else
    {
      AZ_IOT_LOG_ERROR("dps: certificate provider load() failed for the bootstrap identity");
    }
  }
  else
  {
    AZ_IOT_LOG_DEBUG("dps: no certificate provider configured; connecting without client TLS");
  }

  c->dps_mqtt = mc;
  c->dps_phase = DPS_PHASE_CONNECTING;
  c->dps_pending_finalize = false;
  c->dps_pending_have_assignment = false;
  c->dps_pending_status = AZ_IOT_OK;
  c->dps_enrolling = c->opts.dps.request_operational_certificate;

  transition(c, AZ_IOT_CONN_STATE_CONNECTING, AZ_IOT_OK);

  az_iot_result r = mc->iface->connect(mc, &copts);
  if (r != AZ_IOT_OK)
  {
    dps_teardown_mqtt(c);
    c->dps_phase = DPS_PHASE_NONE;
  }
  return r;
}

/* Process deferred DPS finalization. Called from _do_work() after process_loop.
 * On success, tears down DPS MQTT, sets host/client_id and starts hub connect.
 * On failure, transitions to FAULTED. */
static void dps_apply_deferred(az_iot_connection_client* c)
{
  if (!c->dps_pending_finalize)
  {
    return;
  }
  bool have_assignment = c->dps_pending_have_assignment;
  az_iot_result status = c->dps_pending_status;
  c->dps_pending_finalize = false;
  c->dps_pending_have_assignment = false;
  c->dps_pending_status = AZ_IOT_OK;

  /* Disconnect and destroy the DPS MQTT session. */
  if (c->dps_mqtt && c->dps_mqtt->iface && c->dps_mqtt->iface->disconnect)
  {
    (void)c->dps_mqtt->iface->disconnect(c->dps_mqtt);
  }
  dps_teardown_mqtt(c);
  c->dps_phase = DPS_PHASE_DONE;

  if (status != AZ_IOT_OK || !have_assignment)
  {
    transition(c, AZ_IOT_CONN_STATE_FAULTED, status);
    return;
  }

  /* Apply the assigned hub + device_id and connect to the hub. */
  az_iot_result r;
  r = replace_owned_string(
      c->provisioned_iot_hub_hostname,
      sizeof(c->provisioned_iot_hub_hostname),
      &c->opts.host,
      c->dps_assigned_hub);
  if (r != AZ_IOT_OK)
  {
    transition(c, AZ_IOT_CONN_STATE_FAULTED, r);
    return;
  }
  r = replace_owned_string(
      c->provisioned_device_id,
      sizeof(c->provisioned_device_id),
      &c->opts.client_id,
      c->dps_assigned_device_id);
  if (r != AZ_IOT_OK)
  {
    transition(c, AZ_IOT_CONN_STATE_FAULTED, r);
    return;
  }
  /* The assigned profile picks the wire protocol for the hub session. An
   * unrecognised value fails the connection instead of guessing an MQTT version
   * -- a device that appears to connect and then misbehaves is far worse to
   * diagnose than one clear error here. The profile stays readable through
   * az_iot_connection_client_get_hub_profile() so the offending value can be
   * logged or reported. */
  switch (c->connection_profile)
  {
    case AZ_IOT_CONNECTION_PROFILE_CLASSIC:
      c->session_role = AZ_IOT_MQTT_ROLE_HUB_CLASSIC;
      break;
    case AZ_IOT_CONNECTION_PROFILE_MQTT_V5:
      c->session_role = AZ_IOT_MQTT_ROLE_HUB_NEXT;
      break;
    case AZ_IOT_CONNECTION_PROFILE_UNKNOWN:
    default:
      AZ_IOT_LOG_ERRORF(
          "dps: assigned an unsupported connectionProfile \"%s\"; this SDK does not know which "
          "protocol to speak",
          c->connection_profile_raw);
      transition(c, AZ_IOT_CONN_STATE_FAULTED, AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED);
      return;
  }
  /* The assignment may have moved the device to a different generation than the
   * one its registered filters were built for. */
  c->connection_profile_resolved = true;
  if (c->required_profile_refs > 0 && c->required_profile != c->connection_profile)
  {
    /* Terminal on purpose: re-provisioning would return this same profile, so a
     * retry cannot succeed. The application owns the recovery -- destroy the
     * feature clients and rebuild them for the profile this event carries. */
    AZ_IOT_LOG_ERRORF(
        "dps: assigned connectionProfile \"%s\", but the attached feature clients require the "
        "other hub generation; destroy them and rebuild for the assigned profile",
        c->connection_profile_raw);
    transition(c, AZ_IOT_CONN_STATE_FAULTED, AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH);
    return;
  }
  drop_subscriptions_from_other_generations(c);
  c->dps_phase = DPS_PHASE_NONE;

  r = start_connect_attempt(c);
  if (r != AZ_IOT_OK)
  {
    transition(c, AZ_IOT_CONN_STATE_FAULTED, r);
  }
}

/* ------------------------------------------------------------------------- */
/* AEG/Hub-Next presence (birth) handshake                                    */
/*                                                                           */
/* On a HUB_NEXT (MQTT v5) session the connection is not "up" at CONNACK: the  */
/* device must announce presence by publishing a birth message and waiting for */
/* a birth-ack before the SDK reports CONNECTED. Classic/DPS sessions skip all */
/* of this. Sequenced as a small sub-state machine driven from on_mqtt_event:  */
/*   CONNACK  -> SUBSCRIBE ih/{id}/dev/#                  (phase SUBSCRIBING)   */
/*   SUBACK   -> PUBLISH   ih/{id}/srv/presence (birth)   (phase BIRTH)         */
/*   birth-ack MESSAGE on ih/{id}/dev/presence, matching nonce                  */
/*                                     -> announce CONNECTED (phase DONE)       */
/* A stalled handshake is timed out from do_work().                            */
/* ------------------------------------------------------------------------- */

/* Fill `out` with a per-connection nonce. Uniqueness (not cryptographic
 * strength) is what matters: it is echoed on the birth-ack so the SDK can
 * discard acks from a prior attempt. Uses the same LCG as the CSR request-id
 * generator, advanced through the client's rng_state and salted by the attempt
 * count so successive attempts never collide. */
static void presence_gen_nonce(az_iot_connection_client* c, uint8_t out[PRESENCE_NONCE_LEN])
{
  for (size_t i = 0; i < PRESENCE_NONCE_LEN; i += 8)
  {
    uint64_t x = az_iot_time_mono_ms()
        ^ (c->rng_state * 6364136223846793005ull + 1442695040888963407ull)
        ^ ((uint64_t)(c->reconnect_attempt + 1u) << 40);
    c->rng_state = x;
    for (size_t b = 0; b < 8u; ++b)
    {
      out[i + b] = (uint8_t)(x >> (b * 8u));
    }
  }
}

/* Build the Hub-Next (AEG) CONNECT username. The IoT Hub auth webhook denies a
 * connect with an empty username (WebhookAuthUserNameMissing), so the SDK sends
 * "correlationId=<hex nonce>&clientVersion=c%2F<version>", mirroring the .NET
 * SDK. correlationId is the uppercase hex of the 16-byte connection nonce; the
 * SAME nonce bytes ride the birth message as raw Correlation Data so the
 * service can correlate the CONNECT with the birth.
 *
 * Returns false if `cap` (AZ_IOT_MQTT_USERNAME_BUF) cannot hold the whole
 * username; `buf` is left unusable and the caller must fail the attempt. */
static bool presence_build_username(const az_iot_connection_client* c, char* buf, size_t cap)
{
  static const char hexdigits[] = "0123456789ABCDEF";
  char hex[PRESENCE_NONCE_LEN * 2u + 1u];
  for (size_t i = 0; i < PRESENCE_NONCE_LEN; ++i)
  {
    hex[i * 2u] = hexdigits[(c->presence.nonce[i] >> 4) & 0x0Fu];
    hex[i * 2u + 1u] = hexdigits[c->presence.nonce[i] & 0x0Fu];
  }
  hex[PRESENCE_NONCE_LEN * 2u] = '\0';

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
 * side. We emit push_desired/push_reported (both true) and, when set,
 * session_present; reported_version/desired_version stay 0 (the device does not
 * persist twin state yet) and are omitted. Returns the encoded length. */
static size_t presence_encode_birth(uint8_t* out, size_t cap, bool session_present)
{
  size_t n = 0;
  if (session_present && n + 2u <= cap)
  {
    out[n++] = 0x08;
    out[n++] = 0x01;
  } /* f1  session_present */
  if (n + 2u <= cap)
  {
    out[n++] = 0x60;
    out[n++] = 0x01;
  } /* f12 push_desired = true */
  if (n + 2u <= cap)
  {
    out[n++] = 0x68;
    out[n++] = 0x01;
  } /* f13 push_reported = true */
  return n;
}

/* Announce CONNECTED once the session's persistent subscriptions are live.
 *
 * The ordering matters and the obvious explanation for why is the wrong one.
 * MQTT does preserve ordering: a broker processes one connection's control
 * packets in the order it receives them, so a PUBLISH cannot overtake a
 * SUBSCRIBE already written to that connection. The problem was that ours had
 * not been written yet -- transition() invokes the application callback
 * SYNCHRONOUSLY, so a request published from inside that callback reached the
 * wire ahead of its own SUBSCRIBE, and ordering worked against us.
 *
 * Waiting for the SUBACK rather than merely re-ordering the loop also covers
 * the case where the broker REFUSES a filter, which no amount of local ordering
 * would catch. See AB#39366084. */
static void announce_connected(az_iot_connection_client* c)
{
  transition(c, AZ_IOT_CONN_STATE_CONNECTED, AZ_IOT_OK);
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
  c->deferred
      = (reason != AZ_IOT_ERR_SUBSCRIPTION_REFUSED && reconnect_enabled(c) && !c->user_close)
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

/* Begin the presence handshake after a successful HUB_NEXT CONNACK: subscribe
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
  size_t body_len = presence_encode_birth(body, sizeof(body), c->presence.session_present);

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
        if (c->user_close || c->state == AZ_IOT_CONN_STATE_DISCONNECTING)
        {
          AZ_IOT_LOG_DEBUG("connack ignored: close already requested");
          break;
        }

        /* Successful CONNACK: clear the burst counter. */
        c->reconnect_attempt = 0;
        c->reconnect_due_ms = 0;

        /* AEG/Hub-Next (MQTT v5): the connection is not usable until
         * presence is established. Kick off the birth handshake and
         * defer the CONNECTED announcement until the birth-ack arrives.
         * Classic (and DPS-assigned Classic) sessions announce now. */
        if (c->session_role == AZ_IOT_MQTT_ROLE_HUB_NEXT)
        {
          az_iot_result pr = presence_start(c, evt->session_present);
          if (pr != AZ_IOT_OK)
          {
            c->presence.phase = PRESENCE_PHASE_NONE;
            c->deferred = (reconnect_enabled(c) && !c->user_close) ? DEFER_RECONNECT : DEFER_FAULT;
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
         * adapter contract maps these CONNACK codes to a distinct result. The
         * retry is still scheduled through the reconnection policy, so backoff
         * and max_attempts continue to bound it (a device whose enrollment has
         * been deleted must not hammer DPS either). */
        if (evt->status == AZ_IOT_ERR_IDENTITY_REJECTED && dps_configured(c) && !c->user_close
            && reconnect_enabled(c))
        {
          AZ_IOT_LOG_WARN("connack: identity rejected; re-provisioning through DPS");
          c->needs_reprovision = true;
        }
        c->deferred = (reconnect_enabled(c) && !c->user_close) ? DEFER_RECONNECT : DEFER_FAULT;
        c->deferred_reason = evt->status;
      }
      break;

    case AZ_IOT_MQTT_EVT_DISCONNECTED:
      if (c->user_close || !reconnect_enabled(c))
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
      c->deferred = (reconnect_enabled(c) && !c->user_close) ? DEFER_RECONNECT : DEFER_FAULT;
      c->deferred_reason = r;
      break;
    }

    /* Phase 2.3: route inbound application messages through the
     * dispatch table; unmatched messages are dropped silently (the same
     * behaviour MQTT brokers rely on for unsubscribed wildcards). */
    case AZ_IOT_MQTT_EVT_MESSAGE:
      if (evt->message)
      {
        /* During the AEG birth handshake, intercept the birth-ack and
         * complete the connection; everything else routes normally. */
        if (c->presence.phase == PRESENCE_PHASE_BIRTH && presence_is_birth_ack(c, evt->message))
        {
          /* Same reasoning as the CONNACK case above. On the Hub-Next path it
           * is the birth-ack, not the CONNACK, that completes the connection,
           * so suppressing only the CONNACK would leave this route able to
           * announce CONNECTED for an attempt the application has already
           * abandoned. A birth-ack the broker sent before close() reached it
           * arrives in a later process_loop batch, when user_close is set and
           * the state is DISCONNECTING. */
          if (c->user_close || c->state == AZ_IOT_CONN_STATE_DISCONNECTING)
          {
            AZ_IOT_LOG_DEBUG("birth-ack ignored: close already requested");
            break;
          }
          c->presence.phase = PRESENCE_PHASE_DONE;
          begin_feature_subscriptions(c);
          break;
        }
        (void)az_iot_dispatch_route(&c->dispatch, evt->message);
      }
      break;

    /* Phase 3.1: route PUBACK to the publishing feature client via the
     * correlation table. Unmatched packet_ids are dropped (could be a
     * publish issued by a future feature without an ack callback). */
    case AZ_IOT_MQTT_EVT_PUBLISH_ACK:
      for (size_t i = 0; i < AZ_IOT_MAX_PENDING_PUBACKS; ++i)
      {
        if (c->pending_pubacks[i].in_use && c->pending_pubacks[i].packet_id == evt->packet_id)
        {
          az_iot_publish_ack_callback cb = c->pending_pubacks[i].cb;
          void* ctx = c->pending_pubacks[i].user_ctx;
          c->pending_pubacks[i].in_use = false;
          c->pending_pubacks[i].cb = NULL;
          c->pending_pubacks[i].user_ctx = NULL;
          if (cb)
          {
            cb(evt->status, ctx);
          }
          break;
        }
      }
      break;

    /* The dev/presence SUBACK advances the AEG birth handshake: publish the
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
          c->deferred = (reconnect_enabled(c) && !c->user_close) ? DEFER_RECONNECT : DEFER_FAULT;
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

  /* Build hub MQTT username via azure-sdk-for-c (Classic only).
   * Hub-Next does not use the Classic username format. */
  if (c->session_role != AZ_IOT_MQTT_ROLE_HUB_NEXT && c->opts.host && c->opts.client_id)
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
  else if (c->session_role == AZ_IOT_MQTT_ROLE_HUB_NEXT && c->opts.host && c->opts.client_id)
  {
    /* Hub-Next (AEG): generate the per-attempt connection nonce now so it
     * rides the CONNECT username (correlationId) and is reused as the birth
     * Correlation Data. The auth webhook denies an empty username. */
    presence_gen_nonce(c, c->presence.nonce);
    if (!presence_build_username(c, c->hub_username, sizeof(c->hub_username)))
    {
      AZ_IOT_LOG_ERROR(
          "connection: AZ_IOT_MQTT_USERNAME_BUF is too small for the hub-next CONNECT username");
      mc->iface->destroy(mc);
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    copts.username = c->hub_username;
  }

  /* Populate TLS from certificate_provider if available. Prefer the issued
   * OPERATIONAL identity (from this DPS session, or persisted by the provider
   * on a prior run, or supplied for a direct hub connection); fall back to the
   * BOOTSTRAP identity when the provider has no operational cert yet. */
  if (c->opts.certificate_provider)
  {
    az_iot_certificate_provider* prov = c->opts.certificate_provider;
    az_iot_certificate_material mat = { 0 };
    az_iot_result lr = prov->vtable->load(prov, AZ_IOT_CRED_OPERATIONAL, &mat);
    if (lr == AZ_IOT_ERR_NOT_FOUND || lr == AZ_IOT_ERR_NOT_INITIALIZED)
    {
      lr = prov->vtable->load(prov, AZ_IOT_CRED_BOOTSTRAP, &mat);
    }
    if (lr == AZ_IOT_OK)
    {
      az_iot_result cr = apply_certificate_material(&copts, &mat, prov);
      prov->vtable->release(prov, &mat);
      if (cr != AZ_IOT_OK)
      {
        mc->iface->destroy(mc);
        return cr;
      }
    }
  }

  transition(c, AZ_IOT_CONN_STATE_CONNECTING, AZ_IOT_OK);
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
      transition(c, AZ_IOT_CONN_STATE_FAULTED, reason);
      break;
    case DEFER_RECONNECT:
      schedule_reconnect(c, reason);
      break;
    case DEFER_IDLE:
      teardown_active(c);
      c->user_close = false;
      c->reconnect_attempt = 0;
      c->reconnect_due_ms = 0;
      transition(c, AZ_IOT_CONN_STATE_IDLE, reason);
      break;
    default:
      break;
  }
}

/* ------------------------------------------------------------------------- */
/* Hub-Next mock bypass (env-var-driven, for local dev/test only)            */
/* ------------------------------------------------------------------------- */

/* When AZ_IOT_HUB_NEXT_MOCK_ENDPOINT is set (e.g. "localhost:8883"), skip DPS
 * entirely and connect to the mock Hub-Next using MQTT v5. The device identity
 * comes from AZ_IOT_DEVICE_ID (must match the cert CN in the mock). This
 * avoids the need for a real DPS service during local development.
 *
 * ALLOCATION NOTE: the Windows branch uses _dupenv_s (getenv is deprecated
 * under MSVC), which allocates; the buffer is freed in the same function, so
 * nothing is retained. This is the only allocation in the core state machine
 * and it is dev/test-only -- it runs solely when the mock env vars are set and
 * never on a production connect path. The non-Windows branch uses getenv and
 * does not allocate.
 *
 * az-iot-allow: free -- releases the _dupenv_s buffer in the same function */
static bool mock_next_configured(void)
{
#ifdef _WIN32
  char* buf = NULL;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, "AZ_IOT_HUB_NEXT_MOCK_ENDPOINT") != 0 || !is_nonempty_cstr(buf))
  {
    free(buf);
    return false;
  }
  free(buf);
  return true;
#else
  const char* val = getenv("AZ_IOT_HUB_NEXT_MOCK_ENDPOINT");
  return is_nonempty_cstr(val);
#endif
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

static az_iot_result apply_mock_next_bypass(az_iot_connection_client* c)
{
  const char* endpoint;
  const char* device_id;

#ifdef _WIN32
  char* ep_buf = NULL;
  char* id_buf = NULL;
  size_t ep_len = 0, id_len = 0;
  if (_dupenv_s(&ep_buf, &ep_len, "AZ_IOT_HUB_NEXT_MOCK_ENDPOINT") != 0 || !ep_buf)
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  if (_dupenv_s(&id_buf, &id_len, "AZ_IOT_DEVICE_ID") != 0 || !is_nonempty_cstr(id_buf))
  {
    /* Fall back to DPS registration_id if AZ_IOT_DEVICE_ID not set. */
    free(id_buf);
    id_buf = NULL;
  }
  endpoint = ep_buf;
  device_id = id_buf ? id_buf : c->opts.dps.registration_id;
#else
  endpoint = getenv("AZ_IOT_HUB_NEXT_MOCK_ENDPOINT");
  device_id = getenv("AZ_IOT_DEVICE_ID");
  if (!is_nonempty_cstr(device_id))
  {
    device_id = c->opts.dps.registration_id;
  }
#endif

  if (!is_nonempty_cstr(endpoint) || !is_nonempty_cstr(device_id))
  {
#ifdef _WIN32
    free(ep_buf);
    free(id_buf);
#endif
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
    c->session_role = AZ_IOT_MQTT_ROLE_HUB_NEXT;
    c->dps_phase = DPS_PHASE_DONE;

    /* Warn, not debug: provisioning was skipped entirely, so anyone reading
     * the log needs to know this session never talked to DPS. */
    AZ_IOT_LOG_WARNF(
        "dps: mock-next bypass active; host=%s port=%u device=%s",
        host,
        (unsigned)(port ? port : default_port_for_transport(c->opts.transport)),
        device_id);
  }

#ifdef _WIN32
  free(ep_buf);
  free(id_buf);
#endif
  return r;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */
/* ------------------------------------------------------------------------- */

az_iot_connection_client_options az_iot_connection_client_options_default(void)
{
  az_iot_connection_client_options opts = { 0 };
  /* 0, not 8883: the port is derived from the transport at connect time, so a
   * caller that selects WebSockets does not also have to remember to change a
   * port that was defaulted for TCP. */
  opts.port = 0;
  opts.dps.max_hub_connect_attempts_before_reprovision
      = AZ_IOT_DEFAULT_MAX_HUB_CONNECT_ATTEMPTS_BEFORE_REPROVISION;
  return opts;
}

az_iot_result az_iot_connection_client_init(
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
  if (opts->connection_profile != AZ_IOT_CONNECTION_PROFILE_CLASSIC
      && opts->connection_profile != AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    AZ_IOT_LOG_ERROR("connection_client_init: connection_profile is not a profile this SDK speaks");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memset(client, 0, sizeof(*client));
  client->opts = *opts;
  client->state = AZ_IOT_CONN_STATE_IDLE;
  /* Determine session role early so feature clients can query the profile
   * during their init (which happens before open()). When mock-next is
   * configured, also resolve the device_id so subscriptions can be built. */
  if (mock_next_configured())
  {
    client->session_role = AZ_IOT_MQTT_ROLE_HUB_NEXT;
    /* Resolve device_id from AZ_IOT_DEVICE_ID or DPS registration_id */
    const char* dev_id = NULL;
#ifdef _WIN32
    char* id_buf = NULL;
    size_t id_len = 0;
    if (_dupenv_s(&id_buf, &id_len, "AZ_IOT_DEVICE_ID") == 0 && is_nonempty_cstr(id_buf))
    {
      dev_id = id_buf;
    }
#else
    dev_id = getenv("AZ_IOT_DEVICE_ID");
#endif
    if (!is_nonempty_cstr(dev_id))
    {
      dev_id = client->opts.dps.registration_id;
    }
    if (is_nonempty_cstr(dev_id))
    {
      (void)replace_owned_string(
          client->provisioned_device_id,
          sizeof(client->provisioned_device_id),
          &client->opts.client_id,
          dev_id);
    }
#ifdef _WIN32
    free(id_buf);
#endif
  }
  else if (
      client->opts.host && client->opts.connection_profile == AZ_IOT_CONNECTION_PROFILE_MQTT_V5)
  {
    /* Direct connect to an IoT Hub Next / AEG endpoint (MQTT v5). For DPS
     * (host == NULL) the profile is learned during provisioning, so
     * opts.connection_profile is honored only when a direct host is supplied. */
    client->session_role = AZ_IOT_MQTT_ROLE_HUB_NEXT;
  }
  else
  {
    client->session_role = AZ_IOT_MQTT_ROLE_HUB_CLASSIC;
  }
  /* Seed the reported profile from the role settled above, so a direct connect
   * -- where there is no service to ask -- is answerable from init onward. The
   * DPS path overwrites this when the ASSIGNED payload arrives. */
  connection_profile_set(
      client,
      client->session_role == AZ_IOT_MQTT_ROLE_HUB_NEXT
          ? AZ_SPAN_FROM_STR(CONNECTION_PROFILE_MQTT_V5_STR)
          : AZ_SPAN_FROM_STR(CONNECTION_PROFILE_CLASSIC_STR));
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

void az_iot_connection_client_destroy(az_iot_connection_client* client)
{
  if (!client)
  {
    return;
  }
  /* Abandon pending QoS-1 acknowledgements WITHOUT completing them. On a
   * dropped session the callback is useful -- it tells the caller the publish
   * needs resending. On destroy() it is not: the application is tearing the
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
  /* Same reasoning for the session-end handlers: on destroy() the feature
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

az_iot_result az_iot_connection_client_register_mqtt_factory(
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
   * but destroy() walks the whole registry and calls every entry's destroy
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

az_iot_result az_iot_connection_client_set_state_callback(
    az_iot_connection_client* client,
    az_iot_connection_state_callback cb,
    void* user_ctx)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  client->state_cb = cb;
  client->state_cb_ctx = user_ctx;
  return AZ_IOT_OK;
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

az_iot_result az_iot_connection_client_open(az_iot_connection_client* client)
{
  if (!client)
  {
    AZ_IOT_LOG_ERROR("connection_client_open: NULL client");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (client->state != AZ_IOT_CONN_STATE_IDLE)
  {
    AZ_IOT_LOG_ERROR("connection_client_open: client not in IDLE state");
    return AZ_IOT_ERR_ALREADY_INITIALIZED;
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
  client->reconnect_attempt = 0;
  client->reconnect_due_ms = 0;
  client->needs_reprovision = false;

  /* --- Mock-Next bypass: when AZ_IOT_HUB_NEXT_MOCK_ENDPOINT is set,
   * skip DPS and connect directly to the mock Hub-Next (MQTT v5). --- */
  if (mock_next_configured())
  {
    az_iot_result r = apply_mock_next_bypass(client);
    if (r != AZ_IOT_OK)
    {
      transition(client, AZ_IOT_CONN_STATE_IDLE, r);
      return r;
    }
    /* host + client_id are set, session_role = HUB_NEXT → fall through
     * to start_connect_attempt which will resolve the v5 factory. */
    r = start_connect_attempt(client);
    if (r != AZ_IOT_OK)
    {
      transition(client, AZ_IOT_CONN_STATE_IDLE, r);
    }
    return r;
  }

  /* When host is NULL but DPS is configured, provision first. */
  if (!client->opts.host && dps_configured(client))
  {
    az_iot_result r = dps_start(client);
    if (r != AZ_IOT_OK)
    {
      transition(client, AZ_IOT_CONN_STATE_IDLE, r);
    }
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
    transition(client, AZ_IOT_CONN_STATE_IDLE, r);
  }
  return r;
}

az_iot_result az_iot_connection_client_close(az_iot_connection_client* client)
{
  if (!client)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (client->state == AZ_IOT_CONN_STATE_IDLE)
  {
    return AZ_IOT_OK; /* idempotent */
  }

  /* Closing while waiting to reconnect: cancel the schedule and go straight
   * to IDLE. There is no live adapter to disconnect at this point. */
  if (client->state == AZ_IOT_CONN_STATE_RECONNECTING)
  {
    client->reconnect_attempt = 0;
    client->reconnect_due_ms = 0;
    client->user_close = false;
    transition(client, AZ_IOT_CONN_STATE_IDLE, AZ_IOT_OK);
    return AZ_IOT_OK;
  }

  if (!client->active_client)
  {
    return AZ_IOT_ERR_NOT_INITIALIZED;
  }

  client->user_close = true;
  transition(client, AZ_IOT_CONN_STATE_DISCONNECTING, AZ_IOT_OK);
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
  if (client->dps_phase != DPS_PHASE_NONE && client->dps_phase != DPS_PHASE_DONE)
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

    dps_apply_deferred(client);
    return r;
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

  /* Fail a stalled AEG presence (birth) handshake so a missing SUBACK or
   * birth-ack can't wedge the client in CONNECTING forever. Reconnect when a
   * policy is configured (mirrors the .NET SDK, which disconnects and
   * retries), otherwise fault. */
  if ((client->presence.phase == AZ_IOT_PRESENCE_PHASE_SUBSCRIBING
       || client->presence.phase == AZ_IOT_PRESENCE_PHASE_BIRTH)
      && az_iot_time_mono_ms() >= client->presence.deadline_ms)
  {
    client->presence.phase = AZ_IOT_PRESENCE_PHASE_NONE;
    if (reconnect_enabled(client) && !client->user_close)
    {
      schedule_reconnect(client, AZ_IOT_ERR_TIMEOUT);
    }
    else
    {
      teardown_active(client);
      transition(client, AZ_IOT_CONN_STATE_FAULTED, AZ_IOT_ERR_TIMEOUT);
    }
  }

  /* A gated filter withdrawn while its ack was still outstanding stops holding
   * the transition -- but the ack that would have announced CONNECTED is never
   * coming, so the release has to be noticed here. */
  if (client->subscription_gate.active && client->subscription_gate.gated_outstanding == 0
      && client->state == AZ_IOT_CONN_STATE_CONNECTING)
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
      && (client->state == AZ_IOT_CONN_STATE_CONNECTING
          || client->state == AZ_IOT_CONN_STATE_CONNECTED)
      && az_iot_time_mono_ms() >= client->subscription_gate.deadline_ms)
  {
    memset(&client->subscription_gate, 0, sizeof(client->subscription_gate));
    if (reconnect_enabled(client) && !client->user_close)
    {
      schedule_reconnect(client, AZ_IOT_ERR_TIMEOUT);
    }
    else
    {
      teardown_active(client);
      transition(client, AZ_IOT_CONN_STATE_FAULTED, AZ_IOT_ERR_TIMEOUT);
    }
  }

  /* If we're waiting to reconnect and the deadline has passed, attempt it. */
  if (client->state == AZ_IOT_CONN_STATE_RECONNECTING && client->active_client == NULL
      && az_iot_time_mono_ms() >= client->reconnect_due_ms)
  {
    az_iot_result cr;
    if (client->needs_reprovision)
    {
      /* The hub refused this identity; go back to DPS for a new assignment
       * rather than reconnecting to the same rejected credential. Cleared
       * before the attempt so a failure here falls back to a normal retry
       * instead of looping through provisioning forever. */
      client->needs_reprovision = false;
      client->dps_phase = DPS_PHASE_NONE;
      client->session_role = AZ_IOT_MQTT_ROLE_DPS;
      cr = dps_start(client);
    }
    else
    {
      cr = start_connect_attempt(client);
    }
    if (cr != AZ_IOT_OK)
    {
      schedule_reconnect(client, cr);
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
  if (client->state != AZ_IOT_CONN_STATE_IDLE)
  {
    return AZ_IOT_ERR_ALREADY_INITIALIZED;
  }
  client->session_role = role;
  return AZ_IOT_OK;
}

/* Internal helper used by both __set_host and __set_client_id. Copies `s` into
 * the in-struct fixed buffer `owned_buf` (bounded by `buf_cap`) and points
 * `*opts_slot` (the live pointer the rest of the code reads) at it. No heap. */
static az_iot_result replace_owned_string(
    char* owned_buf,
    size_t buf_cap,
    const char** opts_slot,
    const char* s)
{
  if (!is_nonempty_cstr(s))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  size_t n = strlen(s);
  if (n + 1 > buf_cap)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
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
  if (client->state != AZ_IOT_CONN_STATE_IDLE)
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
  if (client->state != AZ_IOT_CONN_STATE_IDLE)
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

bool az_iot_connection_client__is_connected(const az_iot_connection_client* client)
{
  return client && client->state == AZ_IOT_CONN_STATE_CONNECTED;
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

az_iot_result az_iot_connection_client_get_hub_profile(
    const az_iot_connection_client* client,
    az_iot_hub_profile* out_profile)
{
  if (!client || !out_profile)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* The size stamp is what makes this struct safe to grow. A zero stamp means
   * the caller used `= {0}` instead of AZ_IOT_HUB_PROFILE_INIT, so the library
   * cannot tell which fields it may write -- reject rather than guess. */
  if (out_profile->_internal_size == 0)
  {
    AZ_IOT_LOG_ERROR("get_hub_profile: out_profile was not initialized with "
                     "AZ_IOT_HUB_PROFILE_INIT");
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* A caller built against a newer header than the library is the one case the
   * size stamp cannot rescue: it would expect fields this build never writes. */
  if (out_profile->_internal_size > sizeof(az_iot_hub_profile))
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  /* Readable once connected, and also after a profile-driven failure -- that is
   * the case where an application most needs to see what the service said. */
  if (client->state != AZ_IOT_CONN_STATE_CONNECTED
      && client->connection_profile != AZ_IOT_CONNECTION_PROFILE_UNKNOWN)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  /* Written field by field, bounded by the caller's stamp, so a caller compiled
   * against an older (smaller) header is never written past. */
  if (out_profile->_internal_size
      >= offsetof(az_iot_hub_profile, connection_profile) + sizeof(out_profile->connection_profile))
  {
    out_profile->connection_profile = client->connection_profile;
  }
  if (out_profile->_internal_size >= offsetof(az_iot_hub_profile, connection_profile_raw)
          + sizeof(out_profile->connection_profile_raw))
  {
    out_profile->connection_profile_raw = client->connection_profile_raw;
  }
  if (out_profile->_internal_size >= offsetof(az_iot_hub_profile, connection_profile_raw_truncated)
          + sizeof(out_profile->connection_profile_raw_truncated))
  {
    out_profile->connection_profile_raw_truncated = client->connection_profile_raw_truncated;
  }
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
  if (client->state == AZ_IOT_CONN_STATE_CONNECTED)
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
  if (!client->active_client || client->state != AZ_IOT_CONN_STATE_CONNECTED)
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  uint16_t pid = 0;
  az_iot_result r = client->active_client->iface->publish(client->active_client, msg, &pid);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  /* QoS 0: there is no PUBACK on the wire. Fire the cb synchronously. */
  if (msg->qos == AZ_IOT_MQTT_QOS_0)
  {
    if (ack_cb)
    {
      ack_cb(AZ_IOT_OK, ack_user_ctx);
    }
    return AZ_IOT_OK;
  }

  /* QoS 1/2: register correlation entry. If no callback was requested, we
   * still succeeded; the future PUBACK will be silently absorbed. */
  if (ack_cb)
  {
    for (size_t i = 0; i < AZ_IOT_MAX_PENDING_PUBACKS; ++i)
    {
      if (!client->pending_pubacks[i].in_use)
      {
        client->pending_pubacks[i].packet_id = pid;
        client->pending_pubacks[i].cb = ack_cb;
        client->pending_pubacks[i].user_ctx = ack_user_ctx;
        client->pending_pubacks[i].in_use = true;
        return AZ_IOT_OK;
      }
    }
    /* Table full: the publish itself succeeded but we cannot deliver the
     * ack. Surface it so the caller can apply backpressure. */
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  return AZ_IOT_OK;
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
  if (!client->active_client || client->state != AZ_IOT_CONN_STATE_CONNECTED)
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
      && (client->state == AZ_IOT_CONN_STATE_CONNECTED || client->subscription_gate.active))
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
   * touch AEG's device-wide ih/{device_id}/dev/# subscription: the presence
   * handshake issues that one directly, not through this registry, so it has no
   * owner and never appears in the loop below. On AEG, entries in this registry
   * are application custom topics; feature delivery uses the wildcard instead.
   * Withdrawing a custom filter leaves the wildcard -- and therefore every
   * feature's delivery -- untouched. */
  const bool unsubscribe_on_the_wire = client->active_client && client->active_client->iface
      && client->active_client->iface->unsubscribe && client->state == AZ_IOT_CONN_STATE_CONNECTED;

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
 * the one now resolved. Without this, a device reassigned from Classic to AEG
 * would re-issue its $iothub/... filters at the new hub, which does not grant
 * them -- and once CONNECTED is gated on those SUBACKs, the session could never
 * come up and the application would never get the callback that would have
 * removed them. See docs/eng/client-separation.md section 9. */
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

az_iot_result az_iot_connection_client_send_csr(
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
  if (client->session_role != AZ_IOT_MQTT_ROLE_HUB_CLASSIC)
  {
    return AZ_IOT_ERR_NOT_SUPPORTED; /* Hub-Next (AEG) path not defined yet */
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

az_iot_result az_iot_connection_client_cancel_csr(az_iot_connection_client* client)
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
