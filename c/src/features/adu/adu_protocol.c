// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* The device-update wire protocol: topics, request bodies, response parsing and
 * error classification. Pure functions only -- see adu_protocol_internal.h for
 * why that boundary matters.
 */

#include <string.h>

#include <azure/core/az_json.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include "internal/adu_protocol_internal.h"
#include "internal/span_writer.h"

/* Local, so this file does not reach into azure-sdk-for-c internal headers. */
#define ADU_RETURN_IF_FAILED(exp) \
  do                              \
  {                               \
    az_result const _r = (exp);   \
    if (az_result_failed(_r))     \
    {                             \
      return _r;                  \
    }                             \
  } while (0)

/* ------------------------------------------------------------------------- */
/* topics                                                                    */
/* ------------------------------------------------------------------------- */

#define ADU_TOPIC_PREFIX "$dps/registrations/"
#define ADU_TOPIC_RID "/?$rid="

/* Operation names as they appear on the wire. The service lower-cases the
 * segment before matching, so these are emitted lower-case. */
static const char* operation_name(az_iot_adu_operation operation)
{
  switch (operation)
  {
    case AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE:
      return "iotdps-get-onboarding-deviceupdate";
    case AZ_IOT_ADU_OP_GET_UPDATE:
      return "iotdps-get-deviceupdate";
    case AZ_IOT_ADU_OP_REPORT_STATUS:
      return "iotdps-report-deviceupdatestatus";
    default:
      return NULL;
  }
}

az_iot_result az_iot_adu__build_topic(
    az_iot_adu_operation operation,
    const char* request_id,
    char* out,
    size_t out_size,
    size_t* out_len)
{
  const char* op = operation_name(operation);
  if (op == NULL || request_id == NULL || request_id[0] == '\0' || out == NULL || out_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, az_span_create((uint8_t*)out, (int32_t)out_size));
  az_iot_span_writer_append_str(&writer, ADU_TOPIC_PREFIX "POST/");
  az_iot_span_writer_append_str(&writer, op);
  az_iot_span_writer_append_str(&writer, ADU_TOPIC_RID);
  az_iot_span_writer_append_str(&writer, request_id);
  return az_iot_span_writer_end_str(&writer, out_len);
}

az_iot_result az_iot_adu__parse_response_topic(
    const char* topic,
    size_t topic_len,
    int32_t* out_status,
    char* out_request_id,
    size_t request_id_size)
{
  const char* k_res = ADU_TOPIC_PREFIX "res/";
  const size_t res_len = strlen(k_res);

  if (topic == NULL || out_status == NULL || out_request_id == NULL || request_id_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (topic_len <= res_len || memcmp(topic, k_res, res_len) != 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* status: digits up to the next '/'. */
  size_t i = res_len;
  int32_t status = 0;
  size_t digits = 0;
  while (i < topic_len && topic[i] >= '0' && topic[i] <= '9')
  {
    status = (status * 10) + (topic[i] - '0');
    ++digits;
    ++i;
    if (digits > 5)
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
  }
  if (digits == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  *out_status = status;

  /* request id: the value of $rid in the query string. */
  const char* k_rid = "$rid=";
  const size_t rid_key_len = strlen(k_rid);
  out_request_id[0] = '\0';

  while (i + rid_key_len <= topic_len)
  {
    if (memcmp(topic + i, k_rid, rid_key_len) == 0)
    {
      size_t v = i + rid_key_len;
      size_t n = 0;
      while (v < topic_len && topic[v] != '&')
      {
        if (n + 1 >= request_id_size)
        {
          return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
        }
        out_request_id[n++] = topic[v++];
      }
      out_request_id[n] = '\0';
      return (n > 0) ? AZ_IOT_OK : AZ_IOT_ERR_INVALID_ARG;
    }
    ++i;
  }

  return AZ_IOT_ERR_INVALID_ARG;
}

/* ------------------------------------------------------------------------- */
/* requests                                                                  */
/* ------------------------------------------------------------------------- */

static az_result write_string_property(az_json_writer* jw, const char* name, const char* value)
{
  ADU_RETURN_IF_FAILED(
      az_json_writer_append_property_name(jw, az_span_create_from_str((char*)(uintptr_t)name)));
  return az_json_writer_append_string(jw, az_span_create_from_str((char*)(uintptr_t)value));
}

/* Serialize an update-id triple. The service requires all three parts, so a
 * partially-populated triple is a caller error rather than something to paper
 * over with empty strings. */
static az_result write_update_id(
    az_json_writer* jw,
    const char* name,
    const az_iot_adu_report_update_id* id)
{
  ADU_RETURN_IF_FAILED(
      az_json_writer_append_property_name(jw, az_span_create_from_str((char*)(uintptr_t)name)));
  ADU_RETURN_IF_FAILED(az_json_writer_append_begin_object(jw));
  ADU_RETURN_IF_FAILED(write_string_property(jw, "provider", id->provider));
  ADU_RETURN_IF_FAILED(write_string_property(jw, "name", id->name));
  ADU_RETURN_IF_FAILED(write_string_property(jw, "version", id->version));
  return az_json_writer_append_end_object(jw);
}

az_iot_result az_iot_adu__build_fetch_request(
    const az_iot_adu_agent_info* agent_info,
    const az_iot_adu_report_update_id* installed_update_id,
    const char* agent_info_etag,
    const char* service_config_etag,
    uint8_t* out,
    size_t out_size,
    size_t* out_len)
{
  if (agent_info == NULL || agent_info->agent_sdk_version == NULL || out == NULL || out_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (installed_update_id != NULL
      && (installed_update_id->provider == NULL || installed_update_id->name == NULL
          || installed_update_id->version == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_json_writer jw;
  if (az_result_failed(az_json_writer_init(&jw, az_span_create(out, (int32_t)out_size), NULL)))
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  az_result r = az_json_writer_append_begin_object(&jw);

  if (az_result_succeeded(r))
  {
    r = az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("agentInfo"));
  }
  if (az_result_succeeded(r))
  {
    r = az_json_writer_append_begin_object(&jw);
  }
  if (az_result_succeeded(r))
  {
    r = write_string_property(&jw, "agentSdkVersion", agent_info->agent_sdk_version);
  }
  if (az_result_succeeded(r))
  {
    r = az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("agentProfile"));
  }
  if (az_result_succeeded(r))
  {
    /* An integer on the wire, not a string. */
    r = az_json_writer_append_int32(&jw, agent_info->agent_profile);
  }
  if (az_result_succeeded(r) && agent_info->compatibility_properties_count > 0)
  {
    if (agent_info->compatibility_properties == NULL)
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
    r = az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("compatibilityProperties"));
    if (az_result_succeeded(r))
    {
      r = az_json_writer_append_begin_object(&jw);
    }
    for (size_t i = 0; az_result_succeeded(r) && i < agent_info->compatibility_properties_count;
         ++i)
    {
      const az_iot_adu_custom_property* p = &agent_info->compatibility_properties[i];
      if (p->name == NULL || p->value == NULL)
      {
        return AZ_IOT_ERR_INVALID_ARG;
      }
      r = write_string_property(&jw, p->name, p->value);
    }
    if (az_result_succeeded(r))
    {
      r = az_json_writer_append_end_object(&jw);
    }
  }
  if (az_result_succeeded(r))
  {
    r = az_json_writer_append_end_object(&jw); /* agentInfo */
  }

  /* Omitted entirely on the onboarding route: a day-0 device has nothing
   * installed, and an empty triple is not the same statement. */
  if (az_result_succeeded(r) && installed_update_id != NULL)
  {
    r = write_update_id(&jw, "installedUpdateId", installed_update_id);
  }

  if (az_result_succeeded(r) && agent_info_etag != NULL)
  {
    r = write_string_property(&jw, "agentInfoEtag", agent_info_etag);
  }
  if (az_result_succeeded(r) && service_config_etag != NULL)
  {
    r = write_string_property(&jw, "serviceConfigEtag", service_config_etag);
  }

  if (az_result_succeeded(r))
  {
    r = az_json_writer_append_end_object(&jw);
  }
  if (az_result_failed(r))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  if (out_len != NULL)
  {
    *out_len = (size_t)az_span_size(az_json_writer_get_bytes_used_in_destination(&jw));
  }
  return AZ_IOT_OK;
}

static const char* outcome_name(az_iot_adu_outcome outcome)
{
  switch (outcome)
  {
    case AZ_IOT_ADU_OUTCOME_IN_PROGRESS:
      return "IN_PROGRESS";
    case AZ_IOT_ADU_OUTCOME_SUCCEEDED:
      return "SUCCEEDED";
    case AZ_IOT_ADU_OUTCOME_FAILED:
      return "FAILED";
    case AZ_IOT_ADU_OUTCOME_CANCELED:
      return "CANCELED";
    case AZ_IOT_ADU_OUTCOME_SKIPPED:
      return "SKIPPED";
    default:
      return NULL;
  }
}

static const char* failure_origin_name(az_iot_adu_failure_origin origin)
{
  switch (origin)
  {
    case AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE:
      return "NOT_APPLICABLE";
    case AZ_IOT_ADU_FAILURE_ORIGIN_ADU_CLOUD_SERVICE:
      return "ADU_CLOUD_SERVICE";
    case AZ_IOT_ADU_FAILURE_ORIGIN_ADU_MANAGED_RESOURCE:
      return "ADU_MANAGED_RESOURCE";
    case AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_CORE:
      return "AGENT_CORE";
    case AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_EXTENSION:
      return "AGENT_EXTENSION";
    case AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_DEPENDENCY:
      return "AGENT_DEPENDENCY";
    case AZ_IOT_ADU_FAILURE_ORIGIN_DEVICE:
      return "DEVICE";
    case AZ_IOT_ADU_FAILURE_ORIGIN_OTHER:
      return "OTHER";
    default:
      return NULL;
  }
}

az_iot_result az_iot_adu__build_report_request(
    const az_iot_adu_report* report,
    uint8_t* out,
    size_t out_size,
    size_t* out_len)
{
  if (report == NULL || report->workflow_id == NULL || report->workflow_id[0] == '\0' || out == NULL
      || out_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  const char* outcome = outcome_name(report->outcome);
  const char* origin = failure_origin_name(report->failure_origin);
  if (outcome == NULL || origin == NULL || report->extended_result_codes == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* A triple must be complete or absent. Passing a NULL member through to the
   * writer would trip an upstream precondition rather than returning an error
   * to the caller. */
  if (report->installed_update_id != NULL
      && (report->installed_update_id->provider == NULL || report->installed_update_id->name == NULL
          || report->installed_update_id->version == NULL))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* The contract ties these together: NOT_APPLICABLE unless the outcome is a
   * failure, and a real origin when it is. Catching it here keeps an invalid
   * pair off the wire rather than having the service reject it. */
  if ((report->outcome == AZ_IOT_ADU_OUTCOME_FAILED)
      == (report->failure_origin == AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_json_writer jw;
  if (az_result_failed(az_json_writer_init(&jw, az_span_create(out, (int32_t)out_size), NULL)))
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  az_result r = az_json_writer_append_begin_object(&jw);
  if (az_result_succeeded(r))
  {
    r = write_string_property(&jw, "workflowId", report->workflow_id);
  }

  /* Dropped rather than serialized as null when the device has nothing
   * installed. */
  if (az_result_succeeded(r) && report->installed_update_id != NULL)
  {
    r = write_update_id(&jw, "installedUpdateId", report->installed_update_id);
  }

  if (az_result_succeeded(r))
  {
    r = az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("installResult"));
  }
  if (az_result_succeeded(r))
  {
    r = az_json_writer_append_begin_object(&jw);
  }
  if (az_result_succeeded(r))
  {
    r = write_string_property(&jw, "outcome", outcome);
  }
  if (az_result_succeeded(r))
  {
    r = write_string_property(&jw, "failureOrigin", origin);
  }
  if (az_result_succeeded(r))
  {
    r = az_json_writer_append_property_name(&jw, AZ_SPAN_FROM_STR("resultCode"));
  }
  if (az_result_succeeded(r))
  {
    r = az_json_writer_append_int32(&jw, report->result_code);
  }
  if (az_result_succeeded(r))
  {
    r = write_string_property(&jw, "extendedResultCodes", report->extended_result_codes);
  }
  if (az_result_succeeded(r) && report->result_details != NULL)
  {
    r = write_string_property(&jw, "resultDetails", report->result_details);
  }
  if (az_result_succeeded(r))
  {
    r = az_json_writer_append_end_object(&jw); /* installResult */
  }
  if (az_result_succeeded(r))
  {
    r = az_json_writer_append_end_object(&jw);
  }
  if (az_result_failed(r))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  if (out_len != NULL)
  {
    *out_len = (size_t)az_span_size(az_json_writer_get_bytes_used_in_destination(&jw));
  }
  return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* responses                                                                 */
/* ------------------------------------------------------------------------- */

/* Capture the raw text of the value the reader is positioned on, including any
 * children, so the engine can verify the signed manifest over exactly the bytes
 * the service sent. */
static az_result capture_raw_value(az_json_reader* jr, az_span* out)
{
  uint8_t* start = az_span_ptr(jr->token.slice);
  ADU_RETURN_IF_FAILED(az_json_reader_skip_children(jr));
  uint8_t* end = az_span_ptr(jr->token.slice) + az_span_size(jr->token.slice);
  if (end <= start)
  {
    return AZ_ERROR_UNEXPECTED_END;
  }
  *out = az_span_create(start, (int32_t)(end - start));
  return AZ_OK;
}

az_iot_result az_iot_adu__parse_fetch_response(
    const uint8_t* payload,
    size_t payload_len,
    az_iot_adu_fetch_response* out_response)
{
  if (payload == NULL || payload_len == 0 || out_response == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  memset(out_response, 0, sizeof(*out_response));

  az_json_reader jr;
  az_span doc = az_span_create((uint8_t*)(uintptr_t)payload, (int32_t)payload_len);
  if (az_result_failed(az_json_reader_init(&jr, doc, NULL))
      || az_result_failed(az_json_reader_next_token(&jr))
      || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return AZ_IOT_ERR_PROTOCOL;
  }

  bool closed = false;
  while (az_result_succeeded(az_json_reader_next_token(&jr)))
  {
    if (jr.token.kind == AZ_JSON_TOKEN_END_OBJECT)
    {
      closed = true;
      break;
    }
    if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
    {
      return AZ_IOT_ERR_PROTOCOL;
    }

    bool is_update = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("updateMetadata"));
    bool is_agent_etag = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("agentInfoEtag"));
    bool is_cfg_etag
        = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("serviceConfigEtag"));
    bool is_cfg = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("serviceConfiguration"));

    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      return AZ_IOT_ERR_PROTOCOL;
    }

    if (is_update)
    {
      /* Absent OR null both mean "no update", which is a success. */
      if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT)
      {
        if (az_result_failed(capture_raw_value(&jr, &out_response->update_metadata)))
        {
          return AZ_IOT_ERR_PROTOCOL;
        }
        out_response->has_update = true;
      }
      continue;
    }
    if ((is_agent_etag || is_cfg_etag) && jr.token.kind == AZ_JSON_TOKEN_STRING)
    {
      az_span* dst
          = is_agent_etag ? &out_response->agent_info_etag : &out_response->service_config_etag;
      *dst = jr.token.slice;
      continue;
    }
    if (is_cfg && jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT)
    {
      /* Only the root-key URL is needed here; the rest is not the engine's
       * business. */
      while (az_result_succeeded(az_json_reader_next_token(&jr)))
      {
        if (jr.token.kind == AZ_JSON_TOKEN_END_OBJECT)
        {
          break;
        }
        if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
        {
          return AZ_IOT_ERR_PROTOCOL;
        }
        bool is_url
            = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("rootKeyDownloadUrl"));
        if (az_result_failed(az_json_reader_next_token(&jr)))
        {
          return AZ_IOT_ERR_PROTOCOL;
        }
        if (is_url && jr.token.kind == AZ_JSON_TOKEN_STRING)
        {
          out_response->root_key_download_url = jr.token.slice;
        }
        else if (az_result_failed(az_json_reader_skip_children(&jr)))
        {
          return AZ_IOT_ERR_PROTOCOL;
        }
      }
      continue;
    }

    if (az_result_failed(az_json_reader_skip_children(&jr)))
    {
      return AZ_IOT_ERR_PROTOCOL;
    }
  }

  /* The loop also ends when the reader itself fails -- truncated JSON, for
   * instance. Without this a partial document would be reported as a successful
   * parse carrying whatever was read before the failure. */
  if (!closed)
  {
    return AZ_IOT_ERR_PROTOCOL;
  }

  return AZ_IOT_OK;
}

/* ------------------------------------------------------------------------- */
/* errors                                                                    */
/* ------------------------------------------------------------------------- */

az_iot_result az_iot_adu__parse_error_code(
    const uint8_t* payload,
    size_t payload_len,
    char* out_code,
    size_t out_code_size,
    int32_t* out_numeric_code)
{
  if (out_code == NULL || out_code_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  out_code[0] = '\0';
  if (out_numeric_code != NULL)
  {
    *out_numeric_code = 0;
  }

  if (payload == NULL || payload_len == 0)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }

  az_json_reader jr;
  az_span doc = az_span_create((uint8_t*)(uintptr_t)payload, (int32_t)payload_len);
  if (az_result_failed(az_json_reader_init(&jr, doc, NULL))
      || az_result_failed(az_json_reader_next_token(&jr))
      || jr.token.kind != AZ_JSON_TOKEN_BEGIN_OBJECT)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }

  bool found_string_code = false;
  bool found_numeric_code = false;
  bool closed = false;

  while (az_result_succeeded(az_json_reader_next_token(&jr)))
  {
    if (jr.token.kind == AZ_JSON_TOKEN_END_OBJECT)
    {
      closed = true;
      break;
    }
    if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
    {
      return AZ_IOT_ERR_NOT_FOUND;
    }

    bool is_numeric = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("errorCode"));
    bool is_info = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("info"));

    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      break;
    }

    if (is_numeric && jr.token.kind == AZ_JSON_TOKEN_NUMBER)
    {
      /* Parse regardless of whether the caller wants the value: detection must
       * not depend on an optional output parameter. */
      int32_t v = 0;
      if (az_result_succeeded(az_json_token_get_int32(&jr.token, &v)))
      {
        found_numeric_code = true;
        if (out_numeric_code != NULL)
        {
          *out_numeric_code = v;
        }
      }
      continue;
    }

    /* The originating string code travels here. The service SHOULD surface it,
     * so its absence is normal and the numeric code carries the class. */
    if (is_info && jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT)
    {
      bool info_closed = false;
      while (az_result_succeeded(az_json_reader_next_token(&jr)))
      {
        if (jr.token.kind == AZ_JSON_TOKEN_END_OBJECT)
        {
          info_closed = true;
          break;
        }
        if (jr.token.kind != AZ_JSON_TOKEN_PROPERTY_NAME)
        {
          return AZ_IOT_ERR_NOT_FOUND;
        }
        bool is_adu_code = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("aduErrorCode"));
        if (az_result_failed(az_json_reader_next_token(&jr)))
        {
          break;
        }
        if (is_adu_code && jr.token.kind == AZ_JSON_TOKEN_STRING)
        {
          int32_t n = az_span_size(jr.token.slice);
          if (n < 0 || (size_t)n + 1 > out_code_size)
          {
            return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
          }
          memcpy(out_code, az_span_ptr(jr.token.slice), (size_t)n);
          out_code[n] = '\0';
          found_string_code = true;
          continue;
        }
        if (az_result_failed(az_json_reader_skip_children(&jr)))
        {
          return AZ_IOT_ERR_NOT_FOUND;
        }
      }
      if (!info_closed)
      {
        return AZ_IOT_ERR_NOT_FOUND;
      }
      continue;
    }

    if (az_result_failed(az_json_reader_skip_children(&jr)))
    {
      return AZ_IOT_ERR_NOT_FOUND;
    }
  }

  /* A body that never closed is truncated: acting on a partially read failure
   * could drive a resend or retry from incomplete JSON. */
  if (!closed)
  {
    return AZ_IOT_ERR_NOT_FOUND;
  }

  /* Either signal alone is enough to classify: the numeric code carries the
   * class even when the string code is absent. */
  if (found_string_code || found_numeric_code)
  {
    return AZ_IOT_OK;
  }
  return AZ_IOT_ERR_NOT_FOUND;
}

az_iot_adu_error_action az_iot_adu__classify_error(
    const char* error_code,
    int32_t numeric_code,
    az_iot_adu_operation operation)
{
  /* The originating string code is the most precise signal when present. */
  if (error_code != NULL && error_code[0] != '\0')
  {
    if (strcmp(error_code, "UPDATE_ACCOUNT_NOT_LINKED") == 0)
    {
      return AZ_IOT_ADU_ERROR_ACTION_PROCEED;
    }
    if (strcmp(error_code, "OUTDATED_AGENT_INFO") == 0
        || strcmp(error_code, "UNKNOWN_AGENT_INFO_VERSION") == 0)
    {
      return AZ_IOT_ADU_ERROR_ACTION_RESEND_AGENT_INFO;
    }
    if (strcmp(error_code, "OUTDATED_SERVICE_CONFIG") == 0)
    {
      return AZ_IOT_ADU_ERROR_ACTION_DROP_SERVICE_CONFIG_ETAG;
    }
    if (strcmp(error_code, "UPSTREAM_UNAVAILABLE") == 0
        || strcmp(error_code, "INTERNAL_SERVER_ERROR") == 0)
    {
      return AZ_IOT_ADU_ERROR_ACTION_RETRY;
    }
    if (strcmp(error_code, "REPORT_CONFLICT") == 0)
    {
      return AZ_IOT_ADU_ERROR_ACTION_ALREADY_REPORTED;
    }
    if (strcmp(error_code, "THROTTLED") == 0 || strcmp(error_code, "TOO_MANY_REQUESTS") == 0)
    {
      return AZ_IOT_ADU_ERROR_ACTION_RETRY_AFTER;
    }
    /* An unrecognized string code is NOT assumed retryable: repeating a request
     * the service has already rejected is the worse failure mode. */
    return AZ_IOT_ADU_ERROR_ACTION_FATAL;
  }

  /* No string code: the numeric code carries the class. This path matters --
   * surfacing the string code is a SHOULD, not a MUST. */
  switch (numeric_code)
  {
    case AZ_IOT_ADU_ERR_AGENT_INFO_RESEND_REQUIRED:
      /* The whole resend/re-sync family shares this code. Resending the full
       * agentInfo also drops the stale service-config ETag, so one action
       * covers every member. */
      return AZ_IOT_ADU_ERROR_ACTION_RESEND_AGENT_INFO;

    case AZ_IOT_ADU_ERR_GENERIC_CONFLICT:
      /* Shared by two conditions needing OPPOSITE handling: a fetch means the
       * account is not linked (proceed, do not retry); a report means a
       * terminal result is already recorded (treat as delivered). Without the
       * string code, the operation in flight is what disambiguates them. */
      return (operation == AZ_IOT_ADU_OP_REPORT_STATUS) ? AZ_IOT_ADU_ERROR_ACTION_ALREADY_REPORTED
                                                        : AZ_IOT_ADU_ERROR_ACTION_PROCEED;

    case AZ_IOT_ADU_ERR_THROTTLED:
    case AZ_IOT_ADU_ERR_QUOTA_EXCEEDED:
      return AZ_IOT_ADU_ERROR_ACTION_RETRY_AFTER;

    case AZ_IOT_ADU_ERR_SERVER_ERROR:
    case AZ_IOT_ADU_ERR_SERVICE_UNAVAILABLE:
      return AZ_IOT_ADU_ERROR_ACTION_RETRY;

    case 0:
      /* No code at all: nothing to classify. */
      return AZ_IOT_ADU_ERROR_ACTION_FATAL;

    default:
      /* Every other documented code is a request or credential fault: fix the
       * request, do not repeat it unchanged. */
      return AZ_IOT_ADU_ERROR_ACTION_FATAL;
  }
}
