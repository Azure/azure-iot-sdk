// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* The shipping device-update channel.
 *
 * This is the transport the SDK builds for the application when it calls
 * az_iot_su_client_init() with its connection client. The application
 * supplies nothing: URL construction, request bodies, response parsing, the
 * ETag round-trip and error classification all belong here, so that no
 * application has to re-implement the protocol.
 *
 * The operations ride the device's PROVISIONING session, not the hub session.
 * That is not an implementation convenience -- the bootstrap update check runs
 * BEFORE the device registers, so there is no hub session to use. Responses
 * arrive on the provisioning subscription the connection client already
 * establishes, which is why this channel adds no subscription of its own.
 *
 * STATUS: the bootstrap (pre-registration) path is wired. The operational poll
 * runs after the provisioning session has been torn down and needs session
 * lifetime work in the connection client that is not done yet; until then
 * request_update() reports AZ_IOT_ERR_NOT_CONNECTED rather than pretending.
 */

#include <string.h>

#include "azure/iot/az_iot_su.h"

#include "internal/su_channel_internal.h"
#include "internal/su_protocol_internal.h"
#include "internal/connection_client_internal.h"
#include "internal/reconnect.h" /* az_iot_time_mono_ms */
#include "internal/span_writer.h"
#include "internal/log_internal.h"

/* ------------------------------------------------------------------------- */
/* request correlation                                                       */
/* ------------------------------------------------------------------------- */

/* Requests are correlated by a per-request id echoed back on the response
 * topic. Monotonic and per-channel: it only has to distinguish this device's
 * own outstanding request from a stale or unsolicited one. */
#define SU_RID_PREFIX "su"

static az_iot_result next_request_id(az_iot_su_channel_dps* c, char* out, size_t out_size)
{
  c->next_rid++;

  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, az_span_create((uint8_t*)out, (int32_t)out_size));
  az_iot_span_writer_append_str(&writer, SU_RID_PREFIX);
  az_iot_span_writer_append_u32(&writer, c->next_rid);
  return az_iot_span_writer_end_str(&writer, NULL);
}

static bool rid_matches(const az_iot_su_channel_dps* c, const char* rid)
{
  return c->request_pending && rid != NULL && strcmp(c->pending_rid, rid) == 0;
}

/* Our request ids carry a prefix the provisioning flow never uses, so a response
 * can be attributed without depending on what is currently outstanding. */
static bool rid_is_ours(const char* rid)
{
  return rid != NULL && strncmp(rid, SU_RID_PREFIX, sizeof(SU_RID_PREFIX) - 1) == 0;
}

/* ------------------------------------------------------------------------- */
/* connection state                                                          */
/* ------------------------------------------------------------------------- */

/**
 * @brief Connection-state observer, run inside the connection client's
 * dispatch. Records state; the hold is taken here because it must precede the
 * new session's SUBACK, which can arrive before the channel's next tick.
 *
 * DPS:DISCONNECTING ends every provisioning session, so it marks the session a
 * request rode as gone; dps_session_ready() cannot, as a session replaced
 * between two ticks reads ready throughout. The hold is taken on
 * DPS:CONNECTING, because a teardown ending a registration is announced while
 * the phase still refuses a hold.
 *
 * @param event    The transition.
 * @param user_ctx The channel.
 */
static void on_connection_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  az_iot_su_channel_dps* c = (az_iot_su_channel_dps*)user_ctx;
  if (c == NULL || event == NULL || (size_t)event->scope >= AZ_IOT_CONN_SCOPE_COUNT)
  {
    return;
  }
  c->conn_state[event->scope] = event->state;
  if (event->scope != AZ_IOT_CONN_SCOPE_DPS)
  {
    return;
  }

  if (event->state == AZ_IOT_CONN_STATE_DISCONNECTING)
  {
    c->session_epoch++;
    c->exchange_done = false;
  }
  else if (
      event->state == AZ_IOT_CONN_STATE_CONNECTING && c->wants_hold && !c->holds_registration
      && !c->exchange_done
      && az_iot_connection_client__dps_hold_acquire(c->connection) == AZ_IOT_OK)
  {
    c->holds_registration = true;
  }
}

/**
 * @brief Take a seat in the feature-client state-observer pool.
 *
 * State is seeded from the scoped getter first: a past transition is not
 * announced again.
 *
 * @param c The channel.
 * @return AZ_IOT_OK if observing. AZ_IOT_ERR_BUSY inside a dispatch (e.g. the
 * Software updates client initialized from a state callback); retried by
 * channel_observe_deferred(). Otherwise the registry's error.
 */
static az_iot_result channel_observe(az_iot_su_channel_dps* c)
{
  if (c->observes_state)
  {
    return AZ_IOT_OK;
  }
  for (size_t i = 0; i < AZ_IOT_CONN_SCOPE_COUNT; ++i)
  {
    c->conn_state[i]
        = az_iot_connection_client_get_state(c->connection, (az_iot_connection_scope)i);
  }
  az_iot_result r
      = az_iot_connection_client__add_state_observer(c->connection, on_connection_state, c);
  if (r == AZ_IOT_OK)
  {
    c->observes_state = true;
  }
  return r;
}

/**
 * @brief Retry for a seat a bind inside a dispatch could not take.
 *
 * Without a seat a replaced session is invisible, so no operation goes out
 * until the seat is taken.
 *
 * @param c The channel.
 * @return AZ_IOT_OK if unbound or observing. AZ_IOT_ERR_BUSY while still
 * inside a dispatch. Otherwise the registry's error, e.g.
 * AZ_IOT_ERR_NOT_ENOUGH_SPACE when the pool is full, until a seat frees up.
 */
static az_iot_result channel_observe_deferred(az_iot_su_channel_dps* c)
{
  return c->holds_user ? channel_observe(c) : AZ_IOT_OK;
}

/**
 * @brief Whether either scope has settled in FAULTED.
 *
 * Consulted only when no provisioning session is up: the connection client
 * will not open one from there until close() (same rule as
 * dps_session_ensure()). A live session stays usable whatever the hub state.
 *
 * @param c The channel.
 * @return true if a new provisioning session cannot be had until close().
 */
static bool connection_settled_in_fault(const az_iot_su_channel_dps* c)
{
  for (size_t i = 0; i < AZ_IOT_CONN_SCOPE_COUNT; ++i)
  {
    if (c->conn_state[i] == AZ_IOT_CONN_STATE_FAULTED)
    {
      return true;
    }
  }
  return false;
}

/**
 * @brief Refuse an operation because no provisioning session is up.
 *
 * Records the demand so the next tick opens a session, unless the connection
 * has settled in a fault; the first refusal after close() raises it again.
 *
 * @param c The channel.
 * @return AZ_IOT_ERR_NOT_CONNECTED.
 */
static az_iot_result refuse_for_no_session(az_iot_su_channel_dps* c)
{
  c->wants_session = !connection_settled_in_fault(c);
  return AZ_IOT_ERR_NOT_CONNECTED;
}

/* ------------------------------------------------------------------------- */
/* outbound                                                                  */
/* ------------------------------------------------------------------------- */

/* True while the service's retry-after is still running. Clears itself once it
 * expires, so the caller never has to.
 *
 * Checked in two places, and both matter. publish_operation() is the gate every
 * operation funnels through, so nothing can forget it. But the request entry
 * points have to refuse BEFORE they ask for a provisioning session: otherwise a
 * request during a long delay would open a provisioning session only to have
 * the publish refused, over and over for the whole backoff. */
static bool retry_after_in_force(az_iot_su_channel_dps* c)
{
  if (c->retry_after_deadline_ms == 0)
  {
    return false;
  }
  if (az_iot_time_mono_ms() < c->retry_after_deadline_ms)
  {
    return true;
  }
  c->retry_after_deadline_ms = 0;
  return false;
}

static az_iot_result publish_operation(
    az_iot_su_channel_dps* c,
    az_iot_su_operation operation,
    const uint8_t* body,
    size_t body_len)
{
  /* Until the pre-registration exchange is done, publishing is only safe while
   * registration is actually being held. Three ways it is not, all of
   * which leave the session still reporting ready because the registration
   * response has not arrived yet:
   *
   *   - the channel never got a hold, because it bound to a session that was
   *     already registering;
   *   - the channel held one, but the deadline expired and the connection
   *     registered anyway. The hold is advisory, so this is normal;
   *   - the exchange finished and the channel released the hold. Registration
   *     goes out on the next pump, so anything published in between -- a status
   *     report on the engine's next tick, say -- rides a session about to be
   *     torn down.
   *
   * In every case the request would be accepted and its reply lost. Refusing
   * leaves the operation pending in the engine, which retries it, and avoids
   * the worse outcome: a report the service acts on while the device never
   * learns it was delivered.
   *
   * Checked here rather than in each caller so a new operation cannot forget
   * it. */
  /* The hold only governs the PRE-REGISTRATION exchange -- it exists to stop
   * the device registering before this client has had its turn. Once there is
   * no registration pending on the session there is nothing to hold back, so
   * requiring a hold then would reject every operational publish on a session
   * that is perfectly usable. */
  if (c->wants_hold && az_iot_connection_client__dps_registration_pending(c->connection)
      && (!c->holds_registration || !az_iot_connection_client__dps_hold_is_active(c->connection)))
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  if (retry_after_in_force(c))
  {
    return AZ_IOT_ERR_BUSY;
  }

  char topic[AZ_IOT_SU_TOPIC_MAX_SIZE];
  char rid[sizeof(c->pending_rid)] = { 0 };

  az_iot_result r = next_request_id(c, rid, sizeof(rid));
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  r = az_iot_su__build_topic(operation, rid, topic, sizeof(topic), NULL);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  az_iot_mqtt_message msg = { 0 };
  msg.topic = topic;
  msg.qos = AZ_IOT_MQTT_QOS_1;
  msg.payload = body;
  msg.payload_len = body_len;

  r = az_iot_connection_client__dps_publish(c->connection, &msg);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  /* Recorded only after the publish is accepted, so a failed send does not
   * leave the channel waiting for a response that will never come. */
  (void)memcpy(c->pending_rid, rid, sizeof(rid));
  c->pending_operation = operation;
  c->pending_epoch = c->session_epoch;
  c->request_pending = true;
  return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* inbound                                                                   */
/* ------------------------------------------------------------------------- */

/* Apply a failure to the channel's state. Returns the action so the caller can
 * decide whether the operation may be retried. */
static az_iot_su_error_action handle_failure(
    az_iot_su_channel_dps* c,
    az_iot_su_operation operation,
    const uint8_t* payload,
    size_t payload_len,
    char* code,
    size_t code_size,
    char* tracking_id,
    size_t tracking_id_size,
    int32_t* out_numeric)
{
  int32_t numeric = 0;
  (void)az_iot_su__parse_error_code(payload, payload_len, code, code_size, &numeric);
  /* Diagnostics only, and best-effort: an empty tracking id is not a parse
   * failure, it is a body that carried none. */
  (void)az_iot_su__parse_tracking_id(payload, payload_len, tracking_id, tracking_id_size);
  *out_numeric = numeric;

  az_iot_su_error_action action = az_iot_su__classify_error(code, numeric, operation);

  switch (action)
  {
    case AZ_IOT_SU_ERROR_ACTION_RESEND_AGENT_INFO:
      /* Our cached view is behind the service. Dropping both ETags makes the
       * next request a full one, which is what the service is asking for. */
      c->agent_info_etag[0] = '\0';
      c->service_config_etag[0] = '\0';
      break;

    case AZ_IOT_SU_ERROR_ACTION_DROP_SERVICE_CONFIG_ETAG:
      c->service_config_etag[0] = '\0';
      break;

    case AZ_IOT_SU_ERROR_ACTION_NONE:
    case AZ_IOT_SU_ERROR_ACTION_PROCEED:
    case AZ_IOT_SU_ERROR_ACTION_RETRY_AFTER:
    case AZ_IOT_SU_ERROR_ACTION_RETRY:
    case AZ_IOT_SU_ERROR_ACTION_ALREADY_REPORTED:
    case AZ_IOT_SU_ERROR_ACTION_FATAL:
    default:
      /* Nothing cached to invalidate; the caller decides whether to retry. */
      break;
  }

  return action;
}

static void store_etag(char* dst, size_t dst_size, az_span value)
{
  int32_t n = az_span_size(value);
  if (n <= 0 || (size_t)n + 1 > dst_size)
  {
    return;
  }
  (void)memcpy(dst, az_span_ptr(value), (size_t)n);
  dst[n] = '\0';
}

/* One verdict per accepted operation. Without this the engine retires a pending
 * fetch or report when it is published and never learns the service rejected
 * it. */
static void channel_release_hold(az_iot_su_channel_dps* c);

static void emit_result(
    az_iot_su_channel_dps* c,
    az_iot_su_operation operation,
    az_iot_result result,
    az_iot_su_error_action action,
    const az_iot_su_service_error* service_error)
{
  /* The pre-registration exchange is over once an update check reaches a
   * verdict it will not immediately repeat: either it succeeded, or it failed
   * in a way retrying cannot fix. A retryable failure keeps the hold, and the
   * connection client's deadline is what bounds that. */
  if (operation != AZ_IOT_SU_OP_REPORT_STATUS
      && (result == AZ_IOT_OK || action == AZ_IOT_SU_ERROR_ACTION_FATAL
          || action == AZ_IOT_SU_ERROR_ACTION_PROCEED))
  {
    channel_release_hold(c);
  }

  /* Reported to the engine and nowhere else. Whether a verdict ends the
   * client's re-arming -- and so whether the application hears about it -- is
   * the engine's decision, and it already makes exactly that decision in
   * on_channel_result(). Deciding it here too would be the same rule in two
   * files, free to drift apart. */
  if (c->result_cb != NULL)
  {
    /* NEVER NULL to the engine. A NULL here would have to be checked at every
     * point the diagnosis is read, and one missed check is a crash in the
     * application's own callback. "The service said nothing" is a value, so it
     * is passed as one: zero code, empty (not NULL) strings, no delay. */
    static const az_iot_su_service_error k_no_service_error
        = { .code = 0, .message = "", .tracking_id = "", .retry_after_ms = 0 };
    c->result_cb(
        operation,
        result,
        action,
        service_error != NULL ? service_error : &k_no_service_error,
        c->engine_ctx);
  }
}

/* Provisioning-session messages the provisioning flow did not claim are offered
 * here. Returning true means this channel consumed the message. */
static bool on_dps_message(
    const char* topic,
    const uint8_t* payload,
    size_t payload_len,
    void* user_ctx)
{
  az_iot_su_channel_dps* c = (az_iot_su_channel_dps*)user_ctx;
  if (c == NULL || topic == NULL)
  {
    return false;
  }

  int32_t status = 0;
  char rid[sizeof(c->pending_rid)];
  if (az_iot_su__parse_response_topic(topic, strlen(topic), &status, rid, sizeof(rid)) != AZ_IOT_OK)
  {
    /* Not a response topic shape we recognize -- leave it for the provisioning
     * flow. */
    return false;
  }

  /* The request id tells us whether this is the response we are waiting for.
   * Either way the message is OURS: it carries a device-update request id, and
   * handing it back would send it to the provisioning parser, which would judge
   * it a malformed registration response and fault the whole provisioning
   * attempt. So a late or unmatched response is consumed and dropped. */
  if (!rid_is_ours(rid))
  {
    return false;
  }
  if (!rid_matches(c, rid))
  {
    AZ_IOT_LOG_DEBUG("su: dropping a response we are no longer waiting for");
    return true;
  }

  az_iot_su_operation operation = c->pending_operation;
  c->request_pending = false;
  /* An answer arrived, so the session works: the run of losses that armed the
   * bounded retry is over. */
  c->session_loss_attempts = 0;

  if (status < 200 || status >= 300)
  {
    /* Sized for prose: "message" is often a sentence, and dropping it leaves
     * the operator with only a numeric bucket. */
    char code[256];
    char tracking_id[64];
    int32_t numeric = 0;
    az_iot_su_error_action action = handle_failure(
        c,
        operation,
        payload,
        payload_len,
        code,
        sizeof(code),
        tracking_id,
        sizeof(tracking_id),
        &numeric);
    /* MQTT has no headers, so the delay rides the response topic. Taken from
     * any failure that carries one, not only a throttle: the service attaches
     * it to 5xx as well, and the point is to wait as long as it asked.
     *
     * Reported to the engine as well as gating this channel: the engine bounds
     * the OPERATION against the caller's own deadline, so it has to see a delay
     * that will not fit rather than discover it one refusal at a time. */
    uint32_t retry_after_s = az_iot_su__parse_retry_after_seconds(topic, strlen(topic));
    uint32_t retry_after_ms = (uint32_t)((uint64_t)retry_after_s * 1000ull);
    if (retry_after_s > 0)
    {
      c->retry_after_deadline_ms = az_iot_time_mono_ms() + (uint64_t)retry_after_ms;
      AZ_IOT_LOG_DEBUGF("su: service asked for a %u second delay", (unsigned)retry_after_s);
    }
    az_iot_su_service_error service_error = {
      .code = numeric, .message = code, .tracking_id = tracking_id, .retry_after_ms = retry_after_ms
    };
    AZ_IOT_LOG_ERRORF("su: operation failed with status %d", (int)status);
    AZ_IOT_LOG_ERRORF(
        "su: service error %d (%s) trackingId=%s",
        (int)numeric,
        code[0] != '\0' ? code : "-",
        tracking_id[0] != '\0' ? tracking_id : "-");
    emit_result(c, operation, AZ_IOT_ERR_DPS, action, &service_error);
    return true;
  }

  if (operation == AZ_IOT_SU_OP_REPORT_STATUS)
  {
    /* Nothing to parse: the report was accepted. */
    emit_result(c, operation, AZ_IOT_OK, AZ_IOT_SU_ERROR_ACTION_NONE, NULL);
    return true;
  }

  az_iot_su_fetch_response resp;
  if (az_iot_su__parse_fetch_response(payload, payload_len, &resp) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("su: could not parse the update-check response");
    emit_result(c, operation, AZ_IOT_ERR_PROTOCOL, AZ_IOT_SU_ERROR_ACTION_FATAL, NULL);
    return true;
  }

  store_etag(c->agent_info_etag, sizeof(c->agent_info_etag), resp.agent_info_etag);
  store_etag(c->service_config_etag, sizeof(c->service_config_etag), resp.service_config_etag);

  /* No update available is a SUCCESS, not an error: the device asked, and the
   * service answered that there is nothing to do. */
  if (!resp.has_update)
  {
    AZ_IOT_LOG_DEBUG("su: no update available");
    emit_result(c, operation, AZ_IOT_OK, AZ_IOT_SU_ERROR_ACTION_NONE, NULL);
    return true;
  }

  if (c->update_cb != NULL)
  {
    c->update_cb(
        az_span_ptr(resp.update_metadata),
        (size_t)az_span_size(resp.update_metadata),
        c->engine_ctx);
  }
  emit_result(c, operation, AZ_IOT_OK, AZ_IOT_SU_ERROR_ACTION_NONE, NULL);
  return true;
}

/* A request can only be answered on the session it was sent on. Once that
 * session is gone the reply can never arrive, so the slot is released and the
 * engine is free to ask again. */
/* Returns true when the operation was ABANDONED rather than re-armed, so the
 * caller can tell that the demand it saw a moment ago is gone. */
static bool channel_forget_pending_if_session_gone(az_iot_su_channel_dps* c)
{
  if (c->request_pending
      && (c->pending_epoch != c->session_epoch
          || !az_iot_connection_client__dps_session_ready(c->connection)))
  {
    AZ_IOT_LOG_DEBUG("su: provisioning session ended with a request outstanding");
    az_iot_su_operation operation = c->pending_operation;
    c->request_pending = false;

    /* Bounded. The session normally ended because REGISTRATION failed, and
     * opening another session cannot fix that -- so an unbounded retry is a
     * reconnect loop that never succeeds. Past the bound the operation is
     * abandoned, which is reported to the application instead of being retried
     * silently for ever.
     *
     * The bound only. Spacing the retries out is the connection client's job:
     * it already paces its own provisioning-session attempts under the
     * reconnection policy, with jitter. A second ladder here would pace the
     * same reconnect twice, with a fixed delay and no jitter -- which is a
     * synchronised retry storm across a fleet that loses DPS together. */
    if (c->session_loss_attempts < UINT8_MAX)
    {
      c->session_loss_attempts++;
    }
    if (c->session_loss_attempts > AZ_IOT_SU_CHANNEL_MAX_SESSION_RETRIES)
    {
      AZ_IOT_LOG_ERRORF(
          "su: giving up on the operation after %u consecutive provisioning-session losses",
          (unsigned)c->session_loss_attempts);
      c->session_loss_attempts = 0;
      c->wants_session = false;
      emit_result(c, operation, AZ_IOT_ERR_NOT_CONNECTED, AZ_IOT_SU_ERROR_ACTION_FATAL, NULL);
      return true;
    }

    emit_result(c, operation, AZ_IOT_ERR_NOT_CONNECTED, AZ_IOT_SU_ERROR_ACTION_RETRY, NULL);
  }
  return false;
}

/* ------------------------------------------------------------------------- */
/* vtable                                                                    */
/* ------------------------------------------------------------------------- */

static az_iot_result channel_open(
    void* ctx,
    az_iot_su_channel_update_cb cb,
    az_iot_su_channel_result_cb result_cb,
    void* engine_ctx)
{
  az_iot_su_channel_dps* c = (az_iot_su_channel_dps*)ctx;
  if (c == NULL || cb == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  c->update_cb = cb;
  c->result_cb = result_cb;
  c->engine_ctx = engine_ctx;

  /* Responses arrive on the subscription the connection client already
   * establishes for provisioning, so this only has to ask to see them. */
  az_iot_connection_client__set_dps_message_observer(c->connection, on_dps_message, c);

  /* Hold registration so the first update check actually has a session to run
   * on. Without this the connection client registers straight from the SUBACK
   * and tears the session down on the response, and the check never happens.
   *
   * NOT_SUPPORTED means the connection is already registering or past it; that
   * is not an error here -- the channel simply missed this session and its
   * operations wait for the next one. */
  c->wants_hold = true;
  c->exchange_done = false;

  /* Standing interest in the provisioning session, so one can be opened on
   * demand after the device has provisioned. Without it every operation after
   * registration has nothing to publish on. Released at close. */
  az_iot_result ur = az_iot_connection_client__dps_user_acquire(c->connection);
  if (ur != AZ_IOT_OK)
  {
    /* Reported rather than swallowed: without the interest every operation
     * after registration would fail with no indication why. */
    AZ_IOT_LOG_ERROR("su: could not register interest in the provisioning session");
    az_iot_connection_client__set_dps_message_observer(c->connection, NULL, NULL);
    return ur;
  }
  c->holds_user = true;

  /* How the channel learns that its session was replaced, or that the
   * connection settled in a fault. BUSY is retried later
   * (channel_observe_deferred()); anything else refuses the bind. */
  az_iot_result sr = channel_observe(c);
  if (sr != AZ_IOT_OK && sr != AZ_IOT_ERR_BUSY)
  {
    AZ_IOT_LOG_ERROR("su: could not observe the connection state");
    c->wants_hold = false;
    c->holds_user = false;
    az_iot_connection_client__dps_user_release(c->connection);
    az_iot_connection_client__set_dps_message_observer(c->connection, NULL, NULL);
    return sr;
  }

  az_iot_result hr = az_iot_connection_client__dps_hold_acquire(c->connection);
  if (hr == AZ_IOT_OK)
  {
    c->holds_registration = true;
  }
  else if (hr != AZ_IOT_ERR_NOT_SUPPORTED)
  {
    if (c->observes_state)
    {
      c->observes_state = false;
      (void)az_iot_connection_client__remove_state_observer(c->connection, on_connection_state, c);
    }
    c->wants_hold = false;
    c->holds_user = false;
    az_iot_connection_client__dps_user_release(c->connection);
    az_iot_connection_client__set_dps_message_observer(c->connection, NULL, NULL);
    return hr;
  }
  /* NOT_SUPPORTED leaves wants_hold set on purpose: this session is already
   * registering, so the interest carries to the next one. */
  return AZ_IOT_OK;
}

/* Let registration proceed. Idempotent: the hold is released exactly once, on
 * whichever comes first -- the pre-registration exchange finishing, or close.
 * Never held past that, because a device must still provision if device update
 * is unavailable. */
static void channel_release_hold(az_iot_su_channel_dps* c)
{
  /* Marks the exchange done for THIS session only. wants_hold deliberately
   * survives: it is the standing interest, and a reprovision opens a new
   * session that must be held for its own check. */
  c->exchange_done = true;
  if (c->holds_registration)
  {
    c->holds_registration = false;
    az_iot_connection_client__dps_hold_release(c->connection);
  }
}

static void channel_close(void* ctx)
{
  az_iot_su_channel_dps* c = (az_iot_su_channel_dps*)ctx;
  if (c == NULL)
  {
    return;
  }
  az_iot_connection_client__set_dps_message_observer(c->connection, NULL, NULL);
  /* Before anything else: the entry points at this channel. Removal is legal
   * from inside a dispatch, which is where an application deinitializing the software updates
   * client from its own state observer calls this. */
  if (c->observes_state)
  {
    c->observes_state = false;
    (void)az_iot_connection_client__remove_state_observer(c->connection, on_connection_state, c);
  }
  if (c->holds_user)
  {
    c->holds_user = false;
    az_iot_connection_client__dps_user_release(c->connection);
  }
  /* The standing interest ends with the binding, so a closed channel cannot
   * hold a later session hostage. */
  c->wants_hold = false;
  channel_release_hold(c);
  c->update_cb = NULL;
  c->result_cb = NULL;
  c->engine_ctx = NULL;
  c->request_pending = false;
  /* The demand for a session belongs to the binding that raised it. Leaving it
   * set would make the next binding open a session for an operation nobody
   * asked for. */
  c->wants_session = false;
  /* The delay belongs to the binding that earned it. A fresh bind is a fresh
   * start, not a continuation of someone else's backoff. */
  c->retry_after_deadline_ms = 0;
  c->session_loss_attempts = 0;
}

static az_iot_result channel_request_update(void* ctx, az_iot_su_operation operation)
{
  az_iot_su_channel_dps* c = (az_iot_su_channel_dps*)ctx;
  if (c == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (operation != AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE && operation != AZ_IOT_SU_OP_GET_UPDATE)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_result sr = channel_observe_deferred(c);
  if (sr != AZ_IOT_OK)
  {
    return sr;
  }
  /* Before the session check, not after: asking for a session we may not
   * publish on is what turns one delay into a reconnect loop. */
  if (retry_after_in_force(c))
  {
    return AZ_IOT_ERR_BUSY;
  }
  /* A request outstanding on a session that no longer exists can never be
   * answered. Clearing it here is what stops a lost response -- or one lost to
   * the session being torn down at registration -- from wedging the channel in
   * BUSY for the life of the client. */
  (void)channel_forget_pending_if_session_gone(c);

  if (c->request_pending)
  {
    /* One operation at a time. The caller retries on the next tick. */
    return AZ_IOT_ERR_BUSY;
  }
  if (!az_iot_connection_client__dps_session_ready(c->connection))
  {
    return refuse_for_no_session(c);
  }

  az_iot_su_agent_info agent = { 0 };
  agent.agent_sdk_version
      = (c->agent_sdk_version[0] != '\0') ? c->agent_sdk_version : AZ_IOT_SU_CLIENT_AGENT_VERSION;
  agent.agent_profile = c->agent_profile;
  agent.compatibility_properties = (c->compat_count > 0) ? c->compat : NULL;
  agent.compatibility_properties_count = c->compat_count;

  /* The route is the caller's to choose: only the application knows whether it
   * has a device record yet, and the service cannot be asked -- "no device
   * record" and "malformed request" share one error code, so a probe-and-fall-
   * back would fire on genuinely bad requests too. */

  /* The onboarding route omits installedUpdateId by contract: a day-0 device
   * has nothing installed. The operational route sends it, which is how the
   * service knows what to offer next. */
  const az_iot_su_report_update_id* installed
      = (operation == AZ_IOT_SU_OP_GET_ONBOARDING_UPDATE || !c->has_installed_update_id)
      ? NULL
      : &c->installed_update_id;

  size_t body_len = 0;
  az_iot_result r = az_iot_su__build_fetch_request(
      &agent,
      installed,
      (c->agent_info_etag[0] != '\0') ? c->agent_info_etag : NULL,
      (c->service_config_etag[0] != '\0') ? c->service_config_etag : NULL,
      c->body,
      sizeof(c->body),
      &body_len);
  if (r == AZ_IOT_ERR_NOT_ENOUGH_SPACE
      && (c->agent_info_etag[0] != '\0' || c->service_config_etag[0] != '\0'))
  {
    /* ETags are optional; property validation sized the request without them.
     * Drop them rather than fail every later request. */
    AZ_IOT_LOG_ERROR("su: cached ETags do not fit the request; sending without them");
    c->agent_info_etag[0] = '\0';
    c->service_config_etag[0] = '\0';
    r = az_iot_su__build_fetch_request(
        &agent, installed, NULL, NULL, c->body, sizeof(c->body), &body_len);
  }
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  return publish_operation(c, operation, c->body, body_len);
}

static az_iot_result channel_report(void* ctx, const az_iot_su_report* report)
{
  az_iot_su_channel_dps* c = (az_iot_su_channel_dps*)ctx;
  if (c == NULL || report == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_result sr = channel_observe_deferred(c);
  if (sr != AZ_IOT_OK)
  {
    return sr;
  }
  if (retry_after_in_force(c))
  {
    return AZ_IOT_ERR_BUSY;
  }
  if (c->request_pending)
  {
    return AZ_IOT_ERR_BUSY;
  }
  if (!az_iot_connection_client__dps_session_ready(c->connection))
  {
    return refuse_for_no_session(c);
  }

  size_t body_len = 0;
  az_iot_result r = az_iot_su__build_report_request(report, c->body, sizeof(c->body), &body_len);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  return publish_operation(c, AZ_IOT_SU_OP_REPORT_STATUS, c->body, body_len);
}

static az_iot_result channel_set_device_properties(
    void* ctx,
    const az_iot_su_device_properties* properties)
{
  az_iot_su_channel_dps* c = (az_iot_su_channel_dps*)ctx;
  if (c == NULL || properties == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  az_iot_su_device_properties_snapshot snapshot;
  az_iot_result r = az_iot_su__prepare_device_properties(properties, &snapshot);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  az_iot_su_custom_property compatibility[AZ_IOT_SU_MAX_COMPATIBILITY_PROPERTIES];
  az_iot_su_agent_info agent = { 0 };
  agent.agent_sdk_version
      = c->agent_sdk_version[0] != '\0' ? c->agent_sdk_version : AZ_IOT_SU_CLIENT_AGENT_VERSION;
  agent.agent_profile = c->agent_profile;
  agent.compatibility_properties = compatibility;
  agent.compatibility_properties_count
      = az_iot_su__compatibility_properties(&snapshot.properties, compatibility);
  az_iot_su_report_update_id installed = { snapshot.properties.installed_update_id.provider,
                                           snapshot.properties.installed_update_id.name,
                                           snapshot.properties.installed_update_id.version };
  /* Size only: c->body may hold an outstanding operation. Check the
   * operational shape too, even when this session uses onboarding. ETags are
   * left out: they are optional, and a request that cannot fit them drops them. */
  r = az_iot_su__fetch_request_size(
      &agent, installed.provider != NULL ? &installed : NULL, NULL, NULL, sizeof(c->body), NULL);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  az_iot_su__commit_device_properties(
      &snapshot,
      &c->device_properties.properties,
      c->device_properties.custom_properties,
      c->device_properties.strings);
  c->device_properties.strings_size = snapshot.strings_size;
  c->compat_count
      = az_iot_su__compatibility_properties(&c->device_properties.properties, c->compat);
  const az_iot_su_update_id_info* id = &c->device_properties.properties.installed_update_id;
  c->installed_update_id = (az_iot_su_report_update_id){ id->provider, id->name, id->version };
  c->has_installed_update_id = id->provider != NULL;
  return AZ_IOT_OK;
}

/* Driven from the engine's tick.
 *
 * Two things can only be noticed here. A request outstanding when the session
 * went away can never be answered, and nothing else would ever retire it: the
 * engine cleared its pending flag when the request was accepted, so it will not
 * call request_update() again on its own. Reporting the loss re-arms it.
 *
 * And a hold that could not be taken at bind time (or was dropped with the
 * session) is taken now, so the next provisioning session stops for the check
 * instead of racing it. */
static az_iot_result channel_do_work(void* ctx)
{
  az_iot_su_channel_dps* c = (az_iot_su_channel_dps*)ctx;
  if (c == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Returned at the end, not here: the rest of the tick still runs. */
  az_iot_result sr = channel_observe_deferred(c);

  /* Recorded BEFORE the pending request is retired: that call clears
   * request_pending precisely when the session is gone, so testing it
   * afterwards could never be true and the session would never be reopened. */
  bool had_work = c->request_pending || c->wants_session;

  /* An operation ABANDONED here takes its demand with it: wants_session was
   * just cleared, and had_work was captured before that. Acting on the stale
   * value would open a session for an operation that has already been given up
   * on -- the exact loop the bound exists to end. */
  if (channel_forget_pending_if_session_gone(c))
  {
    had_work = false;
  }

  /* Ask for a session when there is work and none is up. This is what makes an
   * operation possible after the device has provisioned: the ordinary flow tore
   * its session down at registration, and nothing else would open another.
   *
   * Only when there is work -- a session opened speculatively would linger and
   * close again on every tick, for nothing.
   *
   * The demand is satisfied by the session being READY, which is the only thing
   * the refused caller was waiting for. It is deliberately not cleared on the
   * result of dps_session_ensure(): that call answers AZ_IOT_OK only when a
   * session is already usable, which this branch has just excluded, so clearing
   * on it would never happen. The flag would then latch on for the life of the
   * client and every linger expiry would reopen a session nobody wants. */
  bool ensure_failed = false;
  if (az_iot_connection_client__dps_session_ready(c->connection))
  {
    c->wants_session = false;
  }
  else if (connection_settled_in_fault(c))
  {
    /* Nothing to ask for until close(); the demand is raised again after it. */
    c->wants_session = false;
  }
  else if (c->holds_user && had_work && !retry_after_in_force(c))
  {
    /* Not fatal, so the tick continues either way -- but not silent. BUSY is
     * the ordinary answer (a session is coming up, ask again next tick);
     * anything else means no session will appear, and without a line here an
     * operation that never goes out has no explanation in the log.
     *
     * Latched, because this runs at the application's pump frequency and the
     * demand is not cleared until a session is ready: a persistent refusal
     * would otherwise emit one line per tick, for ever. One line per failure
     * episode is what has diagnostic value; the repeats carry nothing. */
    az_iot_result er = az_iot_connection_client__dps_session_ensure(c->connection);
    if (er != AZ_IOT_OK && er != AZ_IOT_ERR_BUSY)
    {
      ensure_failed = true;
      if (!c->ensure_error_logged)
      {
        c->ensure_error_logged = true;
        AZ_IOT_LOG_ERRORF("su: could not obtain a provisioning session (%d)", (int)er);
      }
    }
  }
  /* Cleared by ANY tick that did not fail -- including one that did not ask,
   * because there was no work or a session was already up. Clearing it only on
   * a successful ask would let a quiet spell swallow the next episode: the
   * latch would still be set from the previous one, so the new failure would go
   * unreported. An episode is a run of CONSECUTIVE failures; anything else ends
   * it. */
  if (!ensure_failed)
  {
    c->ensure_error_logged = false;
  }

  /* A session that is gone takes its exchange with it: the next one is a fresh
   * provisioning attempt and needs its own check, held again. */
  if (!az_iot_connection_client__dps_session_ready(c->connection))
  {
    c->exchange_done = false;
  }

  if (c->wants_hold && !c->holds_registration && !c->exchange_done)
  {
    if (az_iot_connection_client__dps_hold_acquire(c->connection) == AZ_IOT_OK)
    {
      c->holds_registration = true;
    }
  }
  return sr;
}

/* Persisted layout: u8 agent_len, agent ETag, u8 config_len, config ETag. */
typedef char az_iot_su_channel_dps_state_fits
    [(2u + (sizeof(((az_iot_su_channel_dps*)0)->agent_info_etag) - 1u)
          + (sizeof(((az_iot_su_channel_dps*)0)->service_config_etag) - 1u)
      <= AZ_IOT_SU_CHANNEL_STATE_MAX_SIZE)
         ? 1
         : -1];

/* Each length is one byte. */
typedef char az_iot_su_channel_dps_etag_len_fits
    [(sizeof(((az_iot_su_channel_dps*)0)->agent_info_etag) - 1u <= UINT8_MAX
      && sizeof(((az_iot_su_channel_dps*)0)->service_config_etag) - 1u <= UINT8_MAX)
         ? 1
         : -1];

/** @brief Write one length-prefixed ETag at @p p. @return Bytes written. */
static size_t put_etag(uint8_t* p, const char* etag)
{
  size_t n = strlen(etag);
  p[0] = (uint8_t)n;
  /* NOLINTNEXTLINE(bugprone-not-null-terminated-result): length-prefixed, not NUL-terminated. */
  memcpy(p + 1, etag, n);
  return n + 1u;
}

/** @brief Serialize the cached ETags so a reboot does not force a full agentInfo resend. */
static az_iot_result channel_save_state(void* ctx, uint8_t* buf, size_t cap, size_t* out_len)
{
  az_iot_su_channel_dps* c = (az_iot_su_channel_dps*)ctx;
  if (c == NULL || buf == NULL || out_len == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  *out_len = 0;
  if (c->agent_info_etag[0] == '\0' && c->service_config_etag[0] == '\0')
  {
    return AZ_IOT_OK;
  }
  if (2u + strlen(c->agent_info_etag) + strlen(c->service_config_etag) > cap)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  size_t n = put_etag(buf, c->agent_info_etag);
  n += put_etag(buf + n, c->service_config_etag);
  *out_len = n;
  return AZ_IOT_OK;
}

/** @brief Restore ETags written by channel_save_state(); rejects any other shape. */
static az_iot_result channel_restore_state(void* ctx, const uint8_t* buf, size_t len)
{
  az_iot_su_channel_dps* c = (az_iot_su_channel_dps*)ctx;
  if (c == NULL || buf == NULL || len < 2u)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  size_t a = buf[0];
  if (a >= sizeof(c->agent_info_etag) || 1u + a + 1u > len)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  size_t s = buf[1u + a];
  /* save_state() writes nothing when both are empty, so {0, 0} is malformed. */
  if (s >= sizeof(c->service_config_etag) || 2u + a + s != len || a + s == 0u)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (memchr(buf + 1, '\0', a) != NULL || memchr(buf + 2u + a, '\0', s) != NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  memcpy(c->agent_info_etag, buf + 1, a);
  c->agent_info_etag[a] = '\0';
  memcpy(c->service_config_etag, buf + 2u + a, s);
  c->service_config_etag[s] = '\0';
  return AZ_IOT_OK;
}

/* When the SERVICE asked us to wait until. Distinct from an operation already
 * being outstanding, which also answers BUSY but IS a request going unserved
 * and must stay bounded.
 *
 * Reported raw, without expiring it: retry_after_in_force() clears the deadline
 * as a side effect of reading it, and the engine needs the instant itself. */
/**
 * @brief Drop the outstanding request if it is @p operation (a fetch).
 *
 * Its response is then ignored (no rid match), the slot is free for the next
 * request, and the pre-registration hold is released, as on any other final
 * verdict for a fetch.
 *
 * @param ctx       The channel.
 * @param operation The fetch the engine abandoned.
 */
static void channel_cancel_update(void* ctx, az_iot_su_operation operation)
{
  az_iot_su_channel_dps* c = (az_iot_su_channel_dps*)ctx;
  if (c == NULL || operation == AZ_IOT_SU_OP_REPORT_STATUS || !c->request_pending
      || c->pending_operation != operation)
  {
    return;
  }
  AZ_IOT_LOG_DEBUG("su: no longer waiting for the answer to an abandoned update check");
  c->request_pending = false;
  channel_release_hold(c);
}

static const az_iot_su_channel_vtable k_channel_vtable = {
  .open = channel_open,
  .close = channel_close,
  .request_update = channel_request_update,
  .report = channel_report,
  .set_device_properties = channel_set_device_properties,
  .do_work = channel_do_work,
  .cancel_update = channel_cancel_update,
  .save_state = channel_save_state,
  .restore_state = channel_restore_state,
};

az_iot_result az_iot_su_channel_dps_init(
    az_iot_su_channel_dps* channel_state,
    az_iot_connection_client* connection,
    const az_iot_su_device_properties* device_properties,
    az_iot_su_channel* out_channel)
{
  if (channel_state == NULL || connection == NULL || out_channel == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  memset(channel_state, 0, sizeof(*channel_state));
  channel_state->connection = connection;
  /* The profile the device reports for compatibility matching. */
  channel_state->agent_profile = 1;

  az_iot_result r = channel_set_device_properties(channel_state, device_properties);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  out_channel->vtable = &k_channel_vtable;
  out_channel->ctx = channel_state;
  return AZ_IOT_OK;
}
