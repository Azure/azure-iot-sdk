// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/**
 * @file adu_channel_internal.h
 * @brief INTERNAL. The transport seam between the ADU engine and whatever
 *        carries its update requests and status reports.
 *
 * NOT part of the public API. Applications do not implement this and do not see
 * it: they hand az_iot_adu_client_initialize() a connection client, and the SDK
 * builds the shipping channel itself. Putting the wire protocol behind an
 * application-supplied vtable would mean every application re-implemented the
 * device-update operations, their authentication and their error handling --
 * which is the SDK's job, not the application's.
 *
 * The seam exists for three internal reasons:
 *   1. the engine must name no transport, so a delivery mechanism can be
 *      replaced without touching the download/verify/install state machine;
 *   2. the bootstrap leg runs BEFORE the device is provisioned, so nothing here
 *      may assume a connected hub session underneath it;
 *   3. it lets the engine be exercised against a fake channel with no MQTT, no
 *      HTTP and no service.
 */

#ifndef AZ_IOT_ADU_CHANNEL_INTERNAL_H
#define AZ_IOT_ADU_CHANNEL_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_adu.h"

#include "adu_protocol_internal.h"
#include "adu_device_properties_internal.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* az_iot_adu_outcome / az_iot_adu_failure_origin / az_iot_adu_report_update_id
   * / az_iot_adu_report are declared in the public header: an application sees
   * the structured result, it just does not carry it. */

  /**
   * @brief Invoked by a channel when an update payload has been delivered.
   *
   * @param update_payload      The raw, channel-specific update document. The
   *                            engine parses and verifies it; the channel does
   *                            not interpret it. Valid only for the duration of
   *                            the call — the engine copies what it needs.
   * @param update_payload_len  Length of @p update_payload in bytes.
   * @param engine_ctx          The context the engine passed to `open()`.
   */
  typedef void (*az_iot_adu_channel_update_cb)(
      const uint8_t* update_payload,
      size_t update_payload_len,
      void* engine_ctx);

  /**
   * @brief Invoked by a channel when an operation reaches a verdict.
   *
   * `request_update()` and `report()` returning AZ_IOT_OK on an asynchronous
   * channel means "sent", not "accepted". Without this the engine would retire
   * a pending fetch or report on publish and never learn it failed, silently
   * losing the only record the service gets of what the device did.
   *
   * Called exactly once per accepted operation. A synchronous channel may call
   * it from inside request_update()/report().
   *
   * @param operation  Which operation this verdict is for.
   * @param result     AZ_IOT_OK when the service accepted it.
   * @param action     How to proceed when @p result is not AZ_IOT_OK;
   *                   AZ_IOT_ADU_ERROR_ACTION_NONE on success.
   * @param service_error What the service said, when it said anything. NULL
   *                      when the verdict did not come from a service
   *                      response. Valid only for the duration of the call.
   * @param engine_ctx The context the engine passed to `open()`.
   */
  /**
   * @brief A channel's verdict on an operation it accepted earlier.
   *
   * @p service_error is NEVER NULL, so neither the engine nor anything it
   * feeds has to check. When the service said nothing, it carries a zero code,
   * EMPTY (never NULL) strings and no delay -- "nothing to report" expressed as
   * a value rather than as an absent pointer.
   */
  typedef void (*az_iot_adu_channel_result_cb)(
      az_iot_adu_operation operation,
      az_iot_result result,
      az_iot_adu_error_action action,
      const az_iot_adu_service_error* service_error,
      void* engine_ctx);

  /**
   * @brief The delivery + reporting vtable.
   *
   * Every function takes the channel's own @p ctx. All are REQUIRED except
   * where noted; the engine validates them at initialization.
   */
  typedef struct az_iot_adu_channel_vtable
  {
    /**
     * @brief Bind the channel to the engine and begin delivery.
     *
     * For a push channel this is where a subscription is established. For a
     * pull channel it need only record the callback. Called once, from
     * az_iot_adu_client_initialize().
     */
    az_iot_result (*open)(
        void* ctx,
        az_iot_adu_channel_update_cb cb,
        az_iot_adu_channel_result_cb result_cb,
        void* engine_ctx);

    /**
     * @brief Unbind. Best-effort; the engine ignores the result during
     *        teardown. Called once, from az_iot_adu_client_destroy().
     */
    void (*close)(void* ctx);

    /**
     * @brief Ask the channel to check for an update now, on a named route.
     *
     * A pull channel performs its fetch. A push channel may treat this as a
     * proactive get. The channel reports the outcome by invoking the update
     * callback; "no update available" is a success and simply means the
     * callback is not invoked.
     *
     * @param operation Which fetch route to use: AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE
     *                  or AZ_IOT_ADU_OP_GET_UPDATE. The caller chooses; the
     *                  channel does not infer it, because the only state that
     *                  could distinguish the two lives in the application (it
     *                  persists its provisioning result across boots) and the
     *                  service cannot be probed for it -- a device with no
     *                  device record and a malformed request are both rejected
     *                  with the same error code.
     */
    az_iot_result (*request_update)(void* ctx, az_iot_adu_operation operation);

    /**
     * @brief Deliver a structured status report.
     *
     * The channel owns serialization and any durability guarantee. @p report
     * and everything it points at are valid only for the duration of the call.
     */
    az_iot_result (*report)(void* ctx, const az_iot_adu_report* report);

    /**
     * @brief OPTIONAL. The device properties changed; refresh anything the
     *        channel copied at initialization. May be NULL for a channel that
     *        holds no copy.
     *
     * Without this a channel that snapshots compatibility properties and the
     * installed update id at init keeps sending stale device identity after
     * az_iot_adu_client_update_device_properties().
     */
    az_iot_result (
        *set_device_properties)(void* ctx, const az_iot_adu_device_properties* properties);

    /**
     * @brief OPTIONAL. Driven from the engine's do_work() tick so a channel
     *        with its own asynchronous work has somewhere to run. May be NULL.
     */
    az_iot_result (*do_work)(void* ctx);

  } az_iot_adu_channel_vtable;

  /** @brief A channel instance: its vtable plus its own context. */
  typedef struct az_iot_adu_channel
  {
    const az_iot_adu_channel_vtable* vtable;
    void* ctx;
  } az_iot_adu_channel;

  /* --- The shipping channel ------------------------------------------------ */
  /*
   * State for the device-update channel the SDK builds from the application's
   * connection client. Lives inside az_iot_adu_client_t, so it is caller
   * allocated and needs no heap.
   */
  /* Bound on a request/response body. A compliant update-check response fits
   * every transport the service offers; this is sized for that. */
#ifndef AZ_IOT_ADU_CHANNEL_BODY_MAX_SIZE
#define AZ_IOT_ADU_CHANNEL_BODY_MAX_SIZE 2048
#endif

/** @brief Compatibility properties the channel sends; the service's limit. */
#define AZ_IOT_ADU_CHANNEL_MAX_COMPATIBILITY_PROPERTIES AZ_IOT_ADU_MAX_COMPATIBILITY_PROPERTIES

/* How many times an operation is RETRIED after losing the provisioning session
 * underneath it, before it is abandoned. N retries, so the operation is given
 * up on the (N+1)th consecutive loss.
 *
 * The retry reopens a SESSION, and what usually ended the last one was the
 * REGISTRATION failing -- which a new session cannot fix. Unbounded, that is a
 * reconnect loop for the life of the device; bounded, the application is told
 * the operation was abandoned.
 *
 * The BOUND only. Spacing the attempts out belongs to the connection client,
 * which already paces its own provisioning-session attempts under the
 * reconnection policy, with jitter. */
#ifndef AZ_IOT_ADU_CHANNEL_MAX_SESSION_RETRIES
#define AZ_IOT_ADU_CHANNEL_MAX_SESSION_RETRIES 4
#endif

/* The attempt counter is a uint8_t and saturates at 255, so the bound must be
 * strictly below that or the "> bound" test can never be true and the cap
 * silently disappears. Fail the build instead. */
#if AZ_IOT_ADU_CHANNEL_MAX_SESSION_RETRIES < 1 || AZ_IOT_ADU_CHANNEL_MAX_SESSION_RETRIES > 254
#error "AZ_IOT_ADU_CHANNEL_MAX_SESSION_RETRIES must be between 1 and 254"
#endif

  typedef struct az_iot_adu_channel_dps
  {
    az_iot_connection_client* connection;
    az_iot_adu_channel_update_cb update_cb;
    az_iot_adu_channel_result_cb result_cb;
    void* engine_ctx;

    /* Correlation for the one request that may be outstanding. The device
     * drives one operation at a time, so a single slot is enough and makes an
     * unsolicited or late response obvious rather than ambiguous. */
    char pending_rid[24];
    az_iot_adu_operation pending_operation;
    bool request_pending;
    uint32_t next_rid;

    /* Caller-allocated so the channel performs no allocation of its own. */
    uint8_t body[AZ_IOT_ADU_CHANNEL_BODY_MAX_SIZE];

    /* What the device reports about itself on a fetch. Copied at init: the
     * caller's device-properties struct may be freed once initialize returns. */
    char agent_sdk_version[32];
    int32_t agent_profile;

    /* Compatibility properties, and what is installed now. Both are required on
     * a fetch: they are how the service picks the right update. */
    az_iot_adu_custom_property compat[AZ_IOT_ADU_CHANNEL_MAX_COMPATIBILITY_PROPERTIES];
    size_t compat_count;
    az_iot_adu_device_properties_snapshot device_properties;

    az_iot_adu_report_update_id installed_update_id;
    bool has_installed_update_id;

    /* ETags from the last successful fetch, echoed on the next one. Empty
     * means "not held yet". */
    char agent_info_etag[128];
    char service_config_etag[128];

    /* True while this channel holds registration back for its pre-registration
     * exchange. Tracked so the release is idempotent and exactly matched. */
    bool holds_registration;

    /* Standing interest in holding registration, kept separate from the hold
     * actually taken. Binding to a session that is already registering cannot
     * take a hold; this is what makes the NEXT session (a reprovision) hold
     * instead of racing the check against registration again.
     *
     * Set for the lifetime of the binding, cleared only at close: a device that
     * reprovisions needs its check held on that session too. */
    bool wants_hold;

    /* Standing interest in the provisioning session itself, so one can be
     * opened on demand after the device has provisioned. Distinct from
     * wants_hold, which only delays a registration that is about to happen. */
    bool holds_user;

    /* An operation was refused because no session was up. The tick reads this
     * to know a session is actually wanted -- opening one speculatively would
     * just linger and close again. */
    bool wants_session;

    /* Set once a dps_session_ensure() failure has been logged, so a refusal
     * that persists does not emit one line per pump tick. Cleared as soon as
     * the call succeeds or reports BUSY, so a later failure is reported as a
     * new episode. */
    bool ensure_error_logged;

    /* When the retry-after the service put on a response topic expires: a
     * monotonic instant, not a duration, which is why it is named for the
     * deadline and not for the seconds it was derived from (see
     * dps_hold_deadline_ms for the same distinction). 0 means no delay is in
     * force. Honouring it is the difference between backing off on the
     * schedule the service asked for and hammering it on our own. */
    uint64_t retry_after_deadline_ms;

    /* Consecutive session losses that cost an operation its answer.
     *
     * Cleared by any answered operation -- an answer is proof the session works
     * -- and by a binding ending. Distinct from retry_after_deadline_ms, which
     * carries the delay the SERVICE asked for and must be honoured exactly as
     * given; this is only a count, because the spacing between attempts belongs
     * to the connection client's reconnection policy. */
    uint8_t session_loss_attempts;

    /* Whether the pre-registration exchange has already run on the CURRENT
     * session. Distinct from wants_hold: it stops the same session being held
     * twice, while leaving the standing interest intact for the next one. Reset
     * when the session goes away. */
    bool exchange_done;

    /** True while the channel holds a seat in the connection-state registry. */
    bool observes_state;

    /** Last state of each scope, seeded at open and kept by the observer. Tells
     * a settled fault apart from a session coming up. */
    az_iot_connection_state conn_state[AZ_IOT_CONN_SCOPE_COUNT];

    /** Bumped on every DPS:DISCONNECTING, i.e. every provisioning-session end. */
    uint32_t session_epoch;
    /** session_epoch when the outstanding request was published; a mismatch
     * means its session was replaced, even between two ticks. */
    uint32_t pending_epoch;
  } az_iot_adu_channel_dps;

  /* Bind the channel to a connection and an HTTPS transport and emit the vtable
   * pair the engine binds to. */
  az_iot_result az_iot_adu_channel_dps_init(
      az_iot_adu_channel_dps* channel_state,
      az_iot_connection_client* connection,
      const az_iot_adu_device_properties* device_properties,
      az_iot_adu_channel* out_channel);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADU_CHANNEL_INTERNAL_H */
