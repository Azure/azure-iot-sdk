// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* The shipping device-update channel.
 *
 * This is the transport the SDK builds for the application when it calls
 * az_iot_adu_client_initialize() with its connection client. The application
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

#include "azure/iot/az_iot_adu.h"

#include "internal/adu_channel_internal.h"
#include "internal/adu_protocol_internal.h"
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
#define ADU_RID_PREFIX "adu"

static az_iot_result next_request_id(az_iot_adu_channel_dps* c, char* out, size_t out_size)
{
  c->next_rid++;

  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, az_span_create((uint8_t*)out, (int32_t)out_size));
  az_iot_span_writer_append_str(&writer, ADU_RID_PREFIX);
  az_iot_span_writer_append_u32(&writer, c->next_rid);
  return az_iot_span_writer_end_str(&writer, NULL);
}

static bool rid_matches(const az_iot_adu_channel_dps* c, const char* rid)
{
  return c->request_pending && rid != NULL && strcmp(c->pending_rid, rid) == 0;
}

/* Our request ids carry a prefix the provisioning flow never uses, so a response
 * can be attributed without depending on what is currently outstanding. */
static bool rid_is_ours(const char* rid)
{
  return rid != NULL && strncmp(rid, ADU_RID_PREFIX, sizeof(ADU_RID_PREFIX) - 1) == 0;
}

/* ------------------------------------------------------------------------- */
/* connection state                                                          */
/* ------------------------------------------------------------------------- */

/* Told by the connection client on every state transition.
 *
 * The channel's own signal, dps_session_ready(), is false both while a session
 * is coming up and after the connection has given up. Treating the second as
 * the first is what turns a terminal fault into a loop: the channel reports the
 * lost request as retryable, the engine re-arms it, the next tick asks for a
 * provisioning session, and the connection leaves AZ_IOT_CONN_STATE_FAULTED for
 * AZ_IOT_CONN_STATE_CONNECTING again -- so the application never observes a
 * settled fault it could act on.
 *
 * Only the fault is recorded. Everything else the channel needs it already has,
 * and a feature client that mirrors the whole state machine acquires a second
 * copy to keep correct.
 *
 * NOTE: the event carries no scope yet, so this is the connection's single
 * state -- a HUB fault sets it too. That matches what a single state machine
 * can express: FAULTED is terminal for the whole client, and only close()
 * leaves it. When scoped states land, this narrows to the DPS scope. */
static void on_connection_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)user_ctx;
  if (c == NULL || event == NULL)
  {
    return;
  }
  c->connection_faulted = (event->state == AZ_IOT_CONN_STATE_FAULTED);
}

/* True once the connection has settled into a fault. */
static bool connection_is_faulted(const az_iot_adu_channel_dps* c) { return c->connection_faulted; }

/* ------------------------------------------------------------------------- */
/* outbound                                                                  */
/* ------------------------------------------------------------------------- */

/* True while the service's retry-after is still running. Clears itself once it
 * expires, so the caller never has to.
 *
 * Checked in two places, and both matter. publish_operation() is the gate every
 * operation funnels through, so nothing can forget it. But the request entry
 * points have to refuse BEFORE they ask for a provisioning session: otherwise a
 * request during a long delay sets the standing interest, the tick opens an
 * auxiliary session, the publish is refused, the session lingers idle and
 * closes, and the cycle repeats for the whole backoff -- reconnecting over and
 * over to say nothing. */
static bool retry_after_in_force(az_iot_adu_channel_dps* c)
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

/* Refuse an operation because no provisioning session is up.
 *
 * The demand is recorded so the next tick opens one -- unless the connection
 * has faulted. Asking for a session there drags the connection back to
 * CONNECTING, so the application never sees the fault settle, and it cannot
 * succeed anyway: what failed was the registration, not the session. */
static az_iot_result refuse_for_no_session(az_iot_adu_channel_dps* c)
{
  c->wants_session = !connection_is_faulted(c);
  return AZ_IOT_ERR_NOT_CONNECTED;
}

static az_iot_result publish_operation(
    az_iot_adu_channel_dps* c,
    az_iot_adu_operation operation,
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
  /* The hold only governs the PRE-REGISTRATION exchange. An auxiliary session
   * is opened after registration and deliberately has no hold -- there is no
   * registration left to hold back -- so requiring one here would reject every
   * operational publish on a session that is perfectly usable. */
  if (c->wants_hold && !az_iot_connection_client__dps_session_is_auxiliary(c->connection)
      && (!c->holds_registration || !az_iot_connection_client__dps_hold_is_active(c->connection)))
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  if (retry_after_in_force(c))
  {
    return AZ_IOT_ERR_BUSY;
  }

  char topic[AZ_IOT_ADU_TOPIC_MAX_SIZE];
  char rid[sizeof(c->pending_rid)];

  az_iot_result r = next_request_id(c, rid, sizeof(rid));
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  r = az_iot_adu__build_topic(operation, rid, topic, sizeof(topic), NULL);
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
  c->request_pending = true;
  return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* inbound                                                                   */
/* ------------------------------------------------------------------------- */

/* Apply a failure to the channel's state. Returns the action so the caller can
 * decide whether the operation may be retried. */
static az_iot_adu_error_action handle_failure(
    az_iot_adu_channel_dps* c,
    az_iot_adu_operation operation,
    const uint8_t* payload,
    size_t payload_len)
{
  char code[64];
  int32_t numeric = 0;
  (void)az_iot_adu__parse_error_code(payload, payload_len, code, sizeof(code), &numeric);

  az_iot_adu_error_action action = az_iot_adu__classify_error(code, numeric, operation);

  switch (action)
  {
    case AZ_IOT_ADU_ERROR_ACTION_RESEND_AGENT_INFO:
      /* Our cached view is behind the service. Dropping both ETags makes the
       * next request a full one, which is what the service is asking for. */
      c->agent_info_etag[0] = '\0';
      c->service_config_etag[0] = '\0';
      break;

    case AZ_IOT_ADU_ERROR_ACTION_DROP_SERVICE_CONFIG_ETAG:
      c->service_config_etag[0] = '\0';
      break;

    case AZ_IOT_ADU_ERROR_ACTION_NONE:
    case AZ_IOT_ADU_ERROR_ACTION_PROCEED:
    case AZ_IOT_ADU_ERROR_ACTION_RETRY_AFTER:
    case AZ_IOT_ADU_ERROR_ACTION_RETRY:
    case AZ_IOT_ADU_ERROR_ACTION_ALREADY_REPORTED:
    case AZ_IOT_ADU_ERROR_ACTION_FATAL:
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
static void channel_release_hold(az_iot_adu_channel_dps* c);

static void emit_result(
    az_iot_adu_channel_dps* c,
    az_iot_adu_operation operation,
    az_iot_result result,
    az_iot_adu_error_action action)
{
  /* The pre-registration exchange is over once an update check reaches a
   * verdict it will not immediately repeat: either it succeeded, or it failed
   * in a way retrying cannot fix. A retryable failure keeps the hold, and the
   * connection client's deadline is what bounds that. */
  if (operation != AZ_IOT_ADU_OP_REPORT_STATUS
      && (result == AZ_IOT_OK || action == AZ_IOT_ADU_ERROR_ACTION_FATAL
          || action == AZ_IOT_ADU_ERROR_ACTION_PROCEED))
  {
    channel_release_hold(c);
  }

  if (c->result_cb != NULL)
  {
    c->result_cb(operation, result, action, c->engine_ctx);
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
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)user_ctx;
  if (c == NULL || topic == NULL)
  {
    return false;
  }

  int32_t status = 0;
  char rid[sizeof(c->pending_rid)];
  if (az_iot_adu__parse_response_topic(topic, strlen(topic), &status, rid, sizeof(rid))
      != AZ_IOT_OK)
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
    AZ_IOT_LOG_DEBUG("adu: dropping a response we are no longer waiting for");
    return true;
  }

  az_iot_adu_operation operation = c->pending_operation;
  c->request_pending = false;

  if (status < 200 || status >= 300)
  {
    az_iot_adu_error_action action = handle_failure(c, operation, payload, payload_len);
    /* MQTT has no headers, so the delay rides the response topic. Taken from
     * any failure that carries one, not only a throttle: the service attaches
     * it to 5xx as well, and the point is to wait as long as it asked. */
    uint32_t retry_after_s = az_iot_adu__parse_retry_after_seconds(topic, strlen(topic));
    if (retry_after_s > 0)
    {
      c->retry_after_deadline_ms = az_iot_time_mono_ms() + ((uint64_t)retry_after_s * 1000ull);
      AZ_IOT_LOG_DEBUGF("adu: service asked for a %u second delay", (unsigned)retry_after_s);
    }
    AZ_IOT_LOG_ERRORF("adu: operation failed with status %d", (int)status);
    emit_result(c, operation, AZ_IOT_ERR_DPS, action);
    return true;
  }

  if (operation == AZ_IOT_ADU_OP_REPORT_STATUS)
  {
    /* Nothing to parse: the report was accepted. */
    emit_result(c, operation, AZ_IOT_OK, AZ_IOT_ADU_ERROR_ACTION_NONE);
    return true;
  }

  az_iot_adu_fetch_response resp;
  if (az_iot_adu__parse_fetch_response(payload, payload_len, &resp) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("adu: could not parse the update-check response");
    emit_result(c, operation, AZ_IOT_ERR_PROTOCOL, AZ_IOT_ADU_ERROR_ACTION_FATAL);
    return true;
  }

  store_etag(c->agent_info_etag, sizeof(c->agent_info_etag), resp.agent_info_etag);
  store_etag(c->service_config_etag, sizeof(c->service_config_etag), resp.service_config_etag);

  /* No update available is a SUCCESS, not an error: the device asked, and the
   * service answered that there is nothing to do. */
  if (!resp.has_update)
  {
    AZ_IOT_LOG_DEBUG("adu: no update available");
    emit_result(c, operation, AZ_IOT_OK, AZ_IOT_ADU_ERROR_ACTION_NONE);
    return true;
  }

  if (c->update_cb != NULL)
  {
    c->update_cb(
        az_span_ptr(resp.update_metadata),
        (size_t)az_span_size(resp.update_metadata),
        c->engine_ctx);
  }
  emit_result(c, operation, AZ_IOT_OK, AZ_IOT_ADU_ERROR_ACTION_NONE);
  return true;
}

/* A request can only be answered on the session it was sent on. Once that
 * session is gone the reply can never arrive, so the slot is released and the
 * engine is free to ask again -- unless the connection has faulted, in which
 * case asking again cannot help and saying so is what lets the fault settle. */
static void channel_forget_pending_if_session_gone(az_iot_adu_channel_dps* c)
{
  if (c->request_pending && !az_iot_connection_client__dps_session_ready(c->connection))
  {
    AZ_IOT_LOG_DEBUG("adu: provisioning session ended with a request outstanding");
    az_iot_adu_operation operation = c->pending_operation;
    c->request_pending = false;
    if (connection_is_faulted(c))
    {
      AZ_IOT_LOG_ERROR("adu: the connection has faulted; the operation cannot be retried");
      c->wants_session = false;
      emit_result(c, operation, AZ_IOT_ERR_NOT_CONNECTED, AZ_IOT_ADU_ERROR_ACTION_FATAL);
      return;
    }
    emit_result(c, operation, AZ_IOT_ERR_NOT_CONNECTED, AZ_IOT_ADU_ERROR_ACTION_RETRY);
  }
}

/* ------------------------------------------------------------------------- */
/* vtable                                                                    */
/* ------------------------------------------------------------------------- */

static az_iot_result channel_open(
    void* ctx,
    az_iot_adu_channel_update_cb cb,
    az_iot_adu_channel_result_cb result_cb,
    void* engine_ctx)
{
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)ctx;
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
    AZ_IOT_LOG_ERROR("adu: could not register interest in the provisioning session");
    az_iot_connection_client__set_dps_message_observer(c->connection, NULL, NULL);
    return ur;
  }
  c->holds_user = true;

  /* Be told when the connection faults, instead of polling its state. The seat
   * is in the feature-client pool, so an application that fills its own cannot
   * leave the channel unable to attach.
   *
   * Withdrawn in close(): the entry holds a pointer to this channel, so one
   * left behind is a call into freed storage on the next transition. */
  c->connection_faulted = false;
  az_iot_result sr
      = az_iot_connection_client__add_state_observer(c->connection, on_connection_state, c);
  if (sr != AZ_IOT_OK)
  {
    /* Reported rather than swallowed: without it the channel cannot tell a
     * session that is coming up from a connection that has given up, which is
     * what makes a terminal fault look retryable. */
    AZ_IOT_LOG_ERROR("adu: could not observe the connection state");
    c->holds_user = false;
    az_iot_connection_client__dps_user_release(c->connection);
    az_iot_connection_client__set_dps_message_observer(c->connection, NULL, NULL);
    return sr;
  }
  c->observes_state = true;

  az_iot_result hr = az_iot_connection_client__dps_hold_acquire(c->connection);
  if (hr == AZ_IOT_OK)
  {
    c->holds_registration = true;
  }
  else if (hr != AZ_IOT_ERR_NOT_SUPPORTED)
  {
    c->wants_hold = false;
    c->holds_user = false;
    c->observes_state = false;
    (void)az_iot_connection_client__remove_state_observer(c->connection, on_connection_state, c);
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
static void channel_release_hold(az_iot_adu_channel_dps* c)
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
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)ctx;
  if (c == NULL)
  {
    return;
  }
  az_iot_connection_client__set_dps_message_observer(c->connection, NULL, NULL);
  if (c->observes_state)
  {
    /* Withdrawn before anything else is torn down: the registry holds a pointer
     * to this channel, and an entry left behind is a call into freed storage on
     * the next transition.
     *
     * The seat is only marked released if it actually was. Removal is legal
     * from inside a dispatch, which is the case that matters here -- an
     * application may destroy the ADU client from its own state observer -- so
     * this does not fail in practice; keeping the flag set if it ever did is
     * what stops a second close() from reporting success it did not achieve. */
    if (az_iot_connection_client__remove_state_observer(c->connection, on_connection_state, c)
        == AZ_IOT_OK)
    {
      c->observes_state = false;
    }
    else
    {
      AZ_IOT_LOG_ERROR("adu: could not withdraw the connection-state observer");
    }
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
  /* The fault belonged to the connection this binding watched. */
  c->connection_faulted = false;
}

static az_iot_result channel_request_update(void* ctx, az_iot_adu_operation operation)
{
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)ctx;
  if (c == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (operation != AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE && operation != AZ_IOT_ADU_OP_GET_UPDATE)
  {
    return AZ_IOT_ERR_INVALID_ARG;
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
  channel_forget_pending_if_session_gone(c);

  if (c->request_pending)
  {
    /* One operation at a time. The caller retries on the next tick. */
    return AZ_IOT_ERR_BUSY;
  }
  if (!az_iot_connection_client__dps_session_ready(c->connection))
  {
    return refuse_for_no_session(c);
  }

  az_iot_adu_agent_info agent = { 0 };
  agent.agent_sdk_version
      = (c->agent_sdk_version[0] != '\0') ? c->agent_sdk_version : AZ_IOT_ADU_CLIENT_AGENT_VERSION;
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
  const az_iot_adu_report_update_id* installed
      = (operation == AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE || !c->has_installed_update_id)
      ? NULL
      : &c->installed_update_id;

  size_t body_len = 0;
  az_iot_result r = az_iot_adu__build_fetch_request(
      &agent,
      installed,
      (c->agent_info_etag[0] != '\0') ? c->agent_info_etag : NULL,
      (c->service_config_etag[0] != '\0') ? c->service_config_etag : NULL,
      c->body,
      sizeof(c->body),
      &body_len);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  return publish_operation(c, operation, c->body, body_len);
}

static az_iot_result channel_report(void* ctx, const az_iot_adu_report* report)
{
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)ctx;
  if (c == NULL || report == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
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
  az_iot_result r = az_iot_adu__build_report_request(report, c->body, sizeof(c->body), &body_len);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  return publish_operation(c, AZ_IOT_ADU_OP_REPORT_STATUS, c->body, body_len);
}

static const char* pack_str(char* storage, size_t storage_size, size_t* used, const char* value);

/* Copy what the device reports about itself. Shared by init and the refresh
 * hook so a later az_iot_adu_client_update_device_properties() does not leave
 * the channel sending the identity and installed version it saw at startup. */
static void channel_copy_device_properties(
    az_iot_adu_channel_dps* c,
    const az_iot_adu_device_properties* device_props)
{
  c->compat_count = 0;
  c->has_installed_update_id = false;
  memset(c->compat_storage, 0, sizeof(c->compat_storage));
  memset(c->installed_storage, 0, sizeof(c->installed_storage));
  memset(&c->installed_update_id, 0, sizeof(c->installed_update_id));
  if (device_props == NULL)
  {
    return;
  }
  /* Manufacturer and model are the compatibility properties the service
   * matches on; without them it cannot pick the right update. */
  size_t used = 0;
  const char* manufacturer
      = pack_str(c->compat_storage, sizeof(c->compat_storage), &used, device_props->manufacturer);
  const char* model
      = pack_str(c->compat_storage, sizeof(c->compat_storage), &used, device_props->model);

  if (manufacturer != NULL)
  {
    c->compat[c->compat_count].name = "manufacturer";
    c->compat[c->compat_count].value = manufacturer;
    c->compat_count++;
  }
  if (model != NULL)
  {
    c->compat[c->compat_count].name = "model";
    c->compat[c->compat_count].value = model;
    c->compat_count++;
  }

  for (size_t i = 0;
       i < device_props->custom_properties_count && c->compat_count < AZ_IOT_ADU_CHANNEL_MAX_COMPAT;
       ++i)
  {
    const char* name = pack_str(
        c->compat_storage,
        sizeof(c->compat_storage),
        &used,
        device_props->custom_properties[i].name);
    const char* value = pack_str(
        c->compat_storage,
        sizeof(c->compat_storage),
        &used,
        device_props->custom_properties[i].value);
    if (name == NULL || value == NULL)
    {
      break;
    }
    c->compat[c->compat_count].name = name;
    c->compat[c->compat_count].value = value;
    c->compat_count++;
  }

  /* What is installed now. A complete triple or nothing: a partial one would
   * be rejected when the request is built. */
  size_t iused = 0;
  const char* provider = pack_str(
      c->installed_storage,
      sizeof(c->installed_storage),
      &iused,
      device_props->installed_update_id.provider);
  const char* name = pack_str(
      c->installed_storage,
      sizeof(c->installed_storage),
      &iused,
      device_props->installed_update_id.name);
  const char* version = pack_str(
      c->installed_storage,
      sizeof(c->installed_storage),
      &iused,
      device_props->installed_update_id.version);
  if (provider != NULL && name != NULL && version != NULL)
  {
    c->installed_update_id.provider = provider;
    c->installed_update_id.name = name;
    c->installed_update_id.version = version;
    c->has_installed_update_id = true;
  }
}

static az_iot_result channel_set_device_properties(
    void* ctx,
    const az_iot_adu_device_properties* props)
{
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)ctx;
  if (c == NULL || props == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  channel_copy_device_properties(c, props);
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
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)ctx;
  if (c == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* Recorded BEFORE the pending request is retired: that call clears
   * request_pending precisely when the session is gone, so testing it
   * afterwards could never be true and the session would never be reopened. */
  bool had_work = c->request_pending || c->wants_session;

  channel_forget_pending_if_session_gone(c);

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
  if (az_iot_connection_client__dps_session_ready(c->connection))
  {
    c->wants_session = false;
  }
  else if (connection_is_faulted(c))
  {
    /* Nothing to ask for. Opening a session here is what pulls the connection
     * out of AZ_IOT_CONN_STATE_FAULTED and back into CONNECTING, so the
     * application's state callback never settles and it cannot tell that
     * provisioning has actually failed. */
    c->wants_session = false;
  }
  else if (c->holds_user && had_work)
  {
    (void)az_iot_connection_client__dps_session_ensure(c->connection);
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
  return AZ_IOT_OK;
}

static const az_iot_adu_channel_vtable k_channel_vtable = {
  .open = channel_open,
  .close = channel_close,
  .request_update = channel_request_update,
  .report = channel_report,
  .set_device_properties = channel_set_device_properties,
  .do_work = channel_do_work,
};

/* Pack a NUL-terminated copy into storage and return it, or NULL when it does
 * not fit. Copied because the caller's struct may be freed once initialize
 * returns, and the channel outlives that call. */
static const char* pack_str(char* storage, size_t storage_size, size_t* used, const char* value)
{
  if (value == NULL)
  {
    return NULL;
  }
  size_t n = strlen(value);
  if (n + 1 > storage_size - *used)
  {
    return NULL;
  }
  char* dst = storage + *used;
  (void)memcpy(dst, value, n + 1);
  *used += n + 1;
  return dst;
}

az_iot_result az_iot_adu_channel_dps_init(
    az_iot_adu_channel_dps* channel_state,
    az_iot_connection_client* connection,
    const az_iot_adu_device_properties* device_props,
    az_iot_adu_channel* out_channel)
{
  if (channel_state == NULL || connection == NULL || out_channel == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  memset(channel_state, 0, sizeof(*channel_state));
  channel_state->connection = connection;
  /* The profile the device reports for compatibility matching. */
  channel_state->agent_profile = 1;

  channel_copy_device_properties(channel_state, device_props);

  out_channel->vtable = &k_channel_vtable;
  out_channel->ctx = channel_state;
  return AZ_IOT_OK;
}
