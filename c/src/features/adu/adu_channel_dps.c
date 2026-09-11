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
#include "internal/span_writer.h"
#include "internal/log_internal.h"

/* ------------------------------------------------------------------------- */
/* request correlation                                                       */
/* ------------------------------------------------------------------------- */

/* Requests are correlated by a per-request id echoed back on the response
 * topic. Monotonic and per-channel: it only has to distinguish this device's
 * own outstanding request from a stale or unsolicited one. */
static az_iot_result next_request_id(az_iot_adu_channel_dps* c, char* out, size_t out_size)
{
  c->next_rid++;

  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, az_span_create((uint8_t*)out, (int32_t)out_size));
  az_iot_span_writer_append_str(&writer, "adu");
  az_iot_span_writer_append_u32(&writer, c->next_rid);
  return az_iot_span_writer_end_str(&writer, NULL);
}

static bool rid_matches(const az_iot_adu_channel_dps* c, const char* rid)
{
  return c->request_pending && rid != NULL && strcmp(c->pending_rid, rid) == 0;
}

/* ------------------------------------------------------------------------- */
/* outbound                                                                  */
/* ------------------------------------------------------------------------- */

static az_iot_result publish_operation(
    az_iot_adu_channel_dps* c,
    az_iot_adu_operation operation,
    const uint8_t* body,
    size_t body_len)
{
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

  /* A response whose id we are not waiting for is not ours: it belongs to the
   * provisioning flow, or it is a late reply to a request we already gave up
   * on. Claiming it either way would be wrong. */
  if (!rid_matches(c, rid))
  {
    return false;
  }

  az_iot_adu_operation operation = c->pending_operation;
  c->request_pending = false;
  c->last_action = AZ_IOT_ADU_ERROR_ACTION_NONE;

  if (status < 200 || status >= 300)
  {
    c->last_action = handle_failure(c, operation, payload, payload_len);
    AZ_IOT_LOG_ERRORF("adu: operation failed with status %d", (int)status);
    return true;
  }

  if (operation == AZ_IOT_ADU_OP_REPORT_STATUS)
  {
    /* Nothing to parse: the report was accepted. */
    return true;
  }

  az_iot_adu_fetch_response resp;
  if (az_iot_adu__parse_fetch_response(payload, payload_len, &resp) != AZ_IOT_OK)
  {
    AZ_IOT_LOG_ERROR("adu: could not parse the update-check response");
    c->last_action = AZ_IOT_ADU_ERROR_ACTION_FATAL;
    return true;
  }

  store_etag(c->agent_info_etag, sizeof(c->agent_info_etag), resp.agent_info_etag);
  store_etag(c->service_config_etag, sizeof(c->service_config_etag), resp.service_config_etag);

  /* No update available is a SUCCESS, not an error: the device asked, and the
   * service answered that there is nothing to do. */
  if (!resp.has_update)
  {
    AZ_IOT_LOG_DEBUG("adu: no update available");
    return true;
  }

  if (c->update_cb != NULL)
  {
    c->update_cb(
        az_span_ptr(resp.update_metadata),
        (size_t)az_span_size(resp.update_metadata),
        c->engine_ctx);
  }
  return true;
}

/* ------------------------------------------------------------------------- */
/* vtable                                                                    */
/* ------------------------------------------------------------------------- */

static az_iot_result channel_open(void* ctx, az_iot_adu_channel_update_cb cb, void* engine_ctx)
{
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)ctx;
  if (c == NULL || cb == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  c->update_cb = cb;
  c->engine_ctx = engine_ctx;

  /* Responses arrive on the subscription the connection client already
   * establishes for provisioning, so this only has to ask to see them. */
  az_iot_connection_client__set_dps_message_observer(c->connection, on_dps_message, c);
  return AZ_IOT_OK;
}

static void channel_close(void* ctx)
{
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)ctx;
  if (c == NULL)
  {
    return;
  }
  az_iot_connection_client__set_dps_message_observer(c->connection, NULL, NULL);
  c->update_cb = NULL;
  c->engine_ctx = NULL;
  c->request_pending = false;
}

static az_iot_result channel_request_update(void* ctx)
{
  az_iot_adu_channel_dps* c = (az_iot_adu_channel_dps*)ctx;
  if (c == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (c->request_pending)
  {
    /* One operation at a time. The caller retries on the next tick. */
    return AZ_IOT_ERR_BUSY;
  }
  if (!az_iot_connection_client__dps_session_ready(c->connection))
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  az_iot_adu_agent_info agent = { 0 };
  agent.agent_sdk_version
      = (c->agent_sdk_version[0] != '\0') ? c->agent_sdk_version : AZ_IOT_ADU_CLIENT_AGENT_VERSION;
  agent.agent_profile = c->agent_profile;

  /* The device picks the route from its own provisioning state rather than
   * probing: the service reports "not onboarded yet" with a code that also
   * covers ordinary bad requests, so a probe-and-fall-back would fire on
   * malformed requests too. */
  az_iot_adu_operation operation = AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE;

  size_t body_len = 0;
  az_iot_result r = az_iot_adu__build_fetch_request(
      &agent,
      NULL,
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
  if (c->request_pending)
  {
    return AZ_IOT_ERR_BUSY;
  }
  if (!az_iot_connection_client__dps_session_ready(c->connection))
  {
    return AZ_IOT_ERR_NOT_CONNECTED;
  }

  size_t body_len = 0;
  az_iot_result r = az_iot_adu__build_report_request(report, c->body, sizeof(c->body), &body_len);
  if (r != AZ_IOT_OK)
  {
    return r;
  }

  return publish_operation(c, AZ_IOT_ADU_OP_REPORT_STATUS, c->body, body_len);
}

static const az_iot_adu_channel_vtable k_channel_vtable = {
  .open = channel_open,
  .close = channel_close,
  .request_update = channel_request_update,
  .report = channel_report,
  .do_work = NULL,
};

az_iot_result az_iot_adu_channel_dps_init(
    az_iot_adu_channel_dps* channel_state,
    az_iot_connection_client* connection,
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

  out_channel->vtable = &k_channel_vtable;
  out_channel->ctx = channel_state;
  return AZ_IOT_OK;
}
