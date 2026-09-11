// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* INTERNAL. The device-update wire protocol, as pure functions.
 *
 * Everything here builds or parses bytes. Nothing here opens a connection,
 * publishes, subscribes, retries or waits, so the whole protocol is testable
 * without a broker, a session or a service -- and the transport binding above it
 * can change without touching any of it.
 *
 * NOT part of the public API: applications hand the ADU client a connection and
 * the SDK owns everything below that.
 */

#ifndef AZ_IOT_ADU_PROTOCOL_INTERNAL_H
#define AZ_IOT_ADU_PROTOCOL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_adu.h"
#include "azure/iot/az_iot_result.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* --- Operations ---------------------------------------------------------- */

  typedef enum az_iot_adu_operation
  {
    /* Day-0 fetch: the device has no registry entry yet. Sends agentInfo and
     * omits installedUpdateId. */
    AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE = 0,
    /* Operational fetch: the device is already registered. */
    AZ_IOT_ADU_OP_GET_UPDATE,
    /* Status report for a workflow. */
    AZ_IOT_ADU_OP_REPORT_STATUS,
  } az_iot_adu_operation;

/* --- Topics -------------------------------------------------------------- */

/* Longest topic this module emits, including the NUL:
 *   "$dps/registrations/POST/" + longest operation + "/?$rid=" + rid
 * The operation names are compile-time constants, so this is a fixed bound
 * plus room for the request id. */
#define AZ_IOT_ADU_TOPIC_MAX_SIZE 128

/* Filter the channel subscribes to in order to receive responses. */
#define AZ_IOT_ADU_RESPONSE_TOPIC_FILTER "$dps/registrations/res/#"

  /**
   * Build the publish topic for an operation.
   *
   * Shape: $dps/registrations/POST/{operation}/?$rid={request_id}
   *
   * The operation segment is matched case-insensitively by the service but is
   * emitted lower-case, which is the only form observed on the wire.
   *
   * @param out         Destination buffer.
   * @param out_size    Capacity of @p out; AZ_IOT_ADU_TOPIC_MAX_SIZE is always
   *                    sufficient.
   * @param out_len     Receives the length written, excluding the NUL. Optional.
   * @return AZ_IOT_ERR_NOT_ENOUGH_SPACE if the buffer is too small,
   *         AZ_IOT_ERR_INVALID_ARG on a NULL/empty argument or unknown operation.
   */
  az_iot_result az_iot_adu__build_topic(
      az_iot_adu_operation operation,
      const char* request_id,
      char* out,
      size_t out_size,
      size_t* out_len);

  /**
   * Extract the request id and status code from a response topic.
   *
   * Shape: $dps/registrations/res/{status}/?$rid={request_id}
   *
   * The status is the transport-level code; the machine-readable error code in
   * the body is what drives behaviour (see az_iot_adu__classify_error).
   */
  az_iot_result az_iot_adu__parse_response_topic(
      const char* topic,
      size_t topic_len,
      int32_t* out_status,
      char* out_request_id,
      size_t request_id_size);

  /* --- Requests ------------------------------------------------------------ */

  /* What the device says about itself on a fetch. Spans are borrowed. */
  typedef struct az_iot_adu_agent_info
  {
    const char* agent_sdk_version;
    /* Sent as an integer, not a string. */
    int32_t agent_profile;
    /* Opaque compatibility properties; the agent assigns them no meaning. */
    const az_iot_adu_custom_property* compatibility_properties;
    size_t compatibility_properties_count;
  } az_iot_adu_agent_info;

  /**
   * Build a fetch request body.
   *
   * agentInfo is always emitted. installed_update_id is omitted entirely when
   * NULL, which is the day-0 onboarding case -- a device with nothing installed
   * must not send a null or an empty triple. The ETags are omitted when NULL, and
   * are echoed back so the service can tell us our view is stale.
   */
  az_iot_result az_iot_adu__build_fetch_request(
      const az_iot_adu_agent_info* agent_info,
      const az_iot_adu_report_update_id* installed_update_id,
      const char* agent_info_etag,
      const char* service_config_etag,
      uint8_t* out,
      size_t out_size,
      size_t* out_len);

  /**
   * Build a status report body.
   *
   * workflowId is the sole correlation key. installedUpdateId is dropped from the
   * body when NULL rather than serialized as null.
   */
  az_iot_result az_iot_adu__build_report_request(
      const az_iot_adu_report* report,
      uint8_t* out,
      size_t out_size,
      size_t* out_len);

  /* --- Responses ----------------------------------------------------------- */

  /* Parsed fetch response. The spans point INTO the caller's payload buffer, so
   * they are valid only as long as it is. */
  typedef struct az_iot_adu_fetch_response
  {
    /* The raw updateMetadata object, for the engine to verify and parse. Empty
     * when the service offered no update. */
    az_span update_metadata;
    bool has_update;

    az_span agent_info_etag;
    az_span service_config_etag;
    az_span root_key_download_url;
  } az_iot_adu_fetch_response;

  /**
   * Parse a fetch response body.
   *
   * An absent or null updateMetadata is NOT an error: it means "no update
   * available", which is a successful outcome. has_update reports which it was.
   */
  az_iot_result az_iot_adu__parse_fetch_response(
      const uint8_t* payload,
      size_t payload_len,
      az_iot_adu_fetch_response* out_response);

  /* --- Errors -------------------------------------------------------------- */

  /* What the device should do about a failed operation. Driven by the
   * machine-readable error code in the body, never by the transport status:
   * the same status carries conditions needing opposite handling. */
  typedef enum az_iot_adu_error_action
  {
    /* Not an error. */
    AZ_IOT_ADU_ERROR_ACTION_NONE = 0,
    /* No update service is configured for this device. Carry on; do not retry. */
    AZ_IOT_ADU_ERROR_ACTION_PROCEED,
    /* Our agentInfo or service config is stale: resend it in full and retry. */
    AZ_IOT_ADU_ERROR_ACTION_RESEND_AGENT_INFO,
    /* Our service-config ETag is stale: re-request without it. */
    AZ_IOT_ADU_ERROR_ACTION_DROP_SERVICE_CONFIG_ETAG,
    /* Throttled: wait for the advertised delay, then retry. */
    AZ_IOT_ADU_ERROR_ACTION_RETRY_AFTER,
    /* Transient upstream failure: retry. A fetch may proceed meanwhile; a report
     * must not be dropped. */
    AZ_IOT_ADU_ERROR_ACTION_RETRY,
    /* A terminal report already exists for this workflow. Treat as delivered:
     * reporting is idempotent on workflowId. */
    AZ_IOT_ADU_ERROR_ACTION_ALREADY_REPORTED,
    /* The request or our credentials are wrong. Do not retry unchanged. */
    AZ_IOT_ADU_ERROR_ACTION_FATAL,
  } az_iot_adu_error_action;

  /* --- Service numeric error codes ----------------------------------------- */
  /*
   * The device-facing error body carries a numeric code that conveys the error
   * CLASS, and -- when the service chooses to surface it -- the originating
   * string code under info.aduErrorCode. Surfacing the string is a SHOULD, so
   * the numeric code is the signal that is always present.
   */
#define AZ_IOT_ADU_ERR_GENERIC_BAD_REQUEST 400000
#define AZ_IOT_ADU_ERR_INVALID_API_VERSION 400001
#define AZ_IOT_ADU_ERR_ARGUMENT_INVALID 400002
#define AZ_IOT_ADU_ERR_DESERIALIZATION 400003
/* The whole resend / re-sync family -- the only recoverable 400. */
#define AZ_IOT_ADU_ERR_AGENT_INFO_RESEND_REQUIRED 400004
#define AZ_IOT_ADU_ERR_UNAUTHORIZED 401000
#define AZ_IOT_ADU_ERR_FORBIDDEN 403000
#define AZ_IOT_ADU_ERR_FORBIDDEN_DEVICE_DISABLED 403001
/* Shared by "account not linked" (fetch) and "report conflict" (report). */
#define AZ_IOT_ADU_ERR_GENERIC_CONFLICT 409000
#define AZ_IOT_ADU_ERR_ENTITY_TOO_LARGE 413000
#define AZ_IOT_ADU_ERR_THROTTLED 429000
#define AZ_IOT_ADU_ERR_QUOTA_EXCEEDED 429001
#define AZ_IOT_ADU_ERR_SERVER_ERROR 500000
#define AZ_IOT_ADU_ERR_SERVICE_UNAVAILABLE 503000

  /**
   * Map a service error to the action the device should take.
   *
   * @param error_code    The originating string code from info.aduErrorCode, or
   *                      NULL/empty when the service did not surface one. Most
   *                      precise when present.
   * @param numeric_code  The numeric errorCode from the body. Used when no
   *                      string code is available; 0 means none was found.
   * @param operation     The operation that failed. Required because one
   *                      numeric code covers two conditions needing opposite
   *                      handling, and only the operation separates them when
   *                      the string code is absent.
   */
  az_iot_adu_error_action az_iot_adu__classify_error(
      const char* error_code,
      int32_t numeric_code,
      az_iot_adu_operation operation);

  /**
   * Extract the error signals from a device-facing failure body.
   *
   * The body is flat: a numeric `errorCode`, and optionally `info.aduErrorCode`
   * carrying the originating string code. (This is NOT the nested
   * {"error":{"code":...}} envelope, which is internal to the service chain and
   * never reaches a device.)
   *
   * @param out_code          Receives info.aduErrorCode, or "" when absent.
   * @param out_numeric_code  Receives errorCode, or 0 when absent. May be NULL.
   * @return AZ_IOT_OK when EITHER signal was found; AZ_IOT_ERR_NOT_FOUND when
   *         the body carried neither.
   */
  az_iot_result az_iot_adu__parse_error_code(
      const uint8_t* payload,
      size_t payload_len,
      char* out_code,
      size_t out_code_size,
      int32_t* out_numeric_code);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_ADU_PROTOCOL_INTERNAL_H */
