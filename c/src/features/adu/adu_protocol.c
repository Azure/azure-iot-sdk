// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* The device-update wire protocol: topics, request bodies, response parsing and
 * error classification. Pure functions only -- see adu_protocol_internal.h for
 * why that boundary matters.
 */

#include <stdint.h>
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

/* MQTT carries no headers, so a retry-after rides the response topic's query
 * string: "$dps/registrations/res/500/?$rid=adu1&retry-after=3". */
#define ADU_TOPIC_RETRY_AFTER "retry-after="

/* Upper bound on a retry-after we will honour. The service asks for seconds,
 * not hours; a value past this is treated as no value at all rather than
 * parking the device for an implausible stretch on one malformed topic. */
#define ADU_RETRY_AFTER_MAX_SECONDS 86400u

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

uint32_t az_iot_adu__parse_retry_after_seconds(const char* topic, size_t topic_len)
{
  if (topic == NULL || topic_len == 0 || topic_len > (size_t)INT32_MAX)
  {
    return 0;
  }

  az_span full = az_span_create((uint8_t*)(uintptr_t)topic, (int32_t)topic_len);
  az_span key = AZ_SPAN_FROM_STR(ADU_TOPIC_RETRY_AFTER);
  int32_t from = 0;

  while (from < az_span_size(full))
  {
    int32_t at = az_span_find(az_span_slice(full, from, az_span_size(full)), key);
    if (at < 0)
    {
      return 0;
    }
    int32_t start = from + at;

    /* Only as a query parameter in its own right: without this, a key such as
     * "no-retry-after=" would match on its tail. */
    if (start > 0 && az_span_ptr(full)[start - 1] != '?' && az_span_ptr(full)[start - 1] != '&')
    {
      from = start + 1;
      continue;
    }

    az_span value = az_span_slice(full, start + az_span_size(key), az_span_size(full));
    int32_t sep = az_span_find(value, AZ_SPAN_FROM_STR("&"));
    if (sep >= 0)
    {
      value = az_span_slice(value, 0, sep);
    }

    /* az_span_atou32 rejects a non-digit and rejects overflow, so both are
     * covered without a hand-rolled digit loop. Two things it does NOT do:
     * an empty span trips a precondition rather than returning an error, and
     * it accepts a leading '+'. The contract here is plain digits, so guard
     * both before handing the span over. */
    if (az_span_size(value) == 0 || az_span_ptr(value)[0] < '0' || az_span_ptr(value)[0] > '9')
    {
      return 0;
    }

    uint32_t seconds = 0;
    if (az_result_failed(az_span_atou32(value, &seconds)) || seconds > ADU_RETRY_AFTER_MAX_SECONDS)
    {
      return 0;
    }
    return seconds;
  }

  return 0;
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

az_iot_result az_iot_adu__build_report_request(
    const az_iot_adu_report* report,
    uint8_t* out,
    size_t out_size,
    size_t* out_len)
{
  return az_iot_adu_build_report(report, out, out_size, out_len);
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
  bool found_message = false;
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
    bool is_message = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("message"));

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

    /* Measured against the live service: the device-facing error body carries
     * the originating code in "message" and has no "info" object at all. So
     * this -- not info.aduErrorCode -- is the field that actually discriminates
     * the shared numeric buckets (400000 is INVALID_REQUEST *and*
     * UNKNOWN_WORKFLOW_ID).
     *
     * "message" is not always a code: it is sometimes free text
     * ("Deserialization error."). That is safe because an unrecognized string
     * falls through to the numeric ladder rather than being judged on its own.
     * Text too long for the buffer is dropped for the same reason -- a truncated
     * token must not be compared, and the numeric code still classifies. */
    if (is_message && jr.token.kind == AZ_JSON_TOKEN_STRING && !found_string_code)
    {
      int32_t n = az_span_size(jr.token.slice);
      if (n >= 0 && (size_t)n + 1 <= out_code_size)
      {
        memcpy(out_code, az_span_ptr(jr.token.slice), (size_t)n);
        out_code[n] = '\0';
        found_message = true;
      }
      continue;
    }

    /* The originating string code travels here when the service does surface
     * the documented envelope. Preferred over "message" because it is always a
     * code, never prose. */
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
  if (found_string_code || found_message || found_numeric_code)
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
    /* Both sides of the 400000 bucket. Neither can be fixed by resending the
     * same request: the device is not onboarded, or the workflow no longer
     * exists. Not reported as ALREADY_REPORTED -- that would claim a delivery
     * that never happened. */
    if (strcmp(error_code, "INVALID_REQUEST") == 0
        || strcmp(error_code, "UNKNOWN_WORKFLOW_ID") == 0)
    {
      return AZ_IOT_ADU_ERROR_ACTION_FATAL;
    }
    /* Unrecognized: fall through to the numeric ladder rather than judging the
     * string alone. The field this usually arrives in ("message") is free text
     * as often as it is a code, and treating prose as an unknown code would
     * turn a retryable 5xx into a fatal one. The numeric ladder still defaults
     * 4xx to FATAL, so nothing becomes more optimistic than before. */
  }

  /* No string code: the numeric code carries the class. This path matters --
   * surfacing the string code is a SHOULD, not a MUST.
   *
   * Classified by status, not by exact code. The codes are the HTTP status
   * times 1000 plus a sub-code (400012, 429001, 503000), so the status is
   * readable even when the exact sub-code is new to us -- and the status is
   * the part the service cannot redefine.
   *
   * Matching only known values would call a server-side fault permanent the
   * first time the service added a sub-code. Measured: a real deployment
   * answers with 500001, which no list here contains, and treating that as
   * "never retry" makes a device abandon its update check over a transient
   * fault.
   *
   * A code below the scale is already a bare status: dividing it would give 0
   * and send a transient 503 to FATAL -- the exact failure this classification
   * exists to prevent. The contract says the code is always status-prefixed
   * and every value measured has been, so this is not a shape we expect; it is
   * here because the wire value is taken as-is and the two outcomes are not
   * symmetric. A needless retry costs one request; a wrong FATAL makes the
   * device abandon updates for good. */
  int32_t status = (numeric_code >= ADU_ERROR_CODE_STATUS_SCALE)
      ? (numeric_code / ADU_ERROR_CODE_STATUS_SCALE)
      : numeric_code;

  switch (status)
  {
    case ADU_ERROR_STATUS_BAD_REQUEST:
      /* One 400 IS recoverable, which its status does not say. The whole
       * resend/re-sync family shares that code, and resending the full
       * agentInfo also drops the stale service-config ETag, so one action
       * covers every member. Every other 400 is a malformed request. */
      return (numeric_code == AZ_IOT_ADU_ERR_AGENT_INFO_RESEND_REQUIRED)
          ? AZ_IOT_ADU_ERROR_ACTION_RESEND_AGENT_INFO
          : AZ_IOT_ADU_ERROR_ACTION_FATAL;

    case ADU_ERROR_STATUS_CONFLICT:
      /* The one documented 409 is shared by two conditions needing OPPOSITE
       * handling: a fetch means the account is not linked (proceed, do not
       * retry); a report means a terminal result is already recorded (treat as
       * delivered). Without the string code, the operation in flight is what
       * disambiguates them. An undocumented 409 is not assumed to mean either. */
      if (numeric_code == AZ_IOT_ADU_ERR_GENERIC_CONFLICT)
      {
        return (operation == AZ_IOT_ADU_OP_REPORT_STATUS) ? AZ_IOT_ADU_ERROR_ACTION_ALREADY_REPORTED
                                                          : AZ_IOT_ADU_ERROR_ACTION_PROCEED;
      }
      return AZ_IOT_ADU_ERROR_ACTION_FATAL;

    case ADU_ERROR_STATUS_TOO_MANY_REQUESTS:
      /* Load shedding, whatever the sub-code. The retry-after on the response
       * topic supplies the delay. */
      return AZ_IOT_ADU_ERROR_ACTION_RETRY_AFTER;

    case ADU_ERROR_STATUS_INTERNAL_SERVER_ERROR:
    case ADU_ERROR_STATUS_BAD_GATEWAY:
    case ADU_ERROR_STATUS_SERVICE_UNAVAILABLE:
    case ADU_ERROR_STATUS_GATEWAY_TIMEOUT:
      /* The request was not rejected on its merits; the service could not
       * answer it. Repeating it unchanged is exactly right. */
      return AZ_IOT_ADU_ERROR_ACTION_RETRY;

    default:
      /* Other 4xx, no code at all (0), and anything unrecognized: a request or
       * credential fault. Fix the request, do not repeat it unchanged. */
      return AZ_IOT_ADU_ERROR_ACTION_FATAL;
  }
}

az_iot_result az_iot_adu__parse_tracking_id(
    const uint8_t* payload,
    size_t payload_len,
    char* out_tracking_id,
    size_t out_tracking_id_size)
{
  if (out_tracking_id == NULL || out_tracking_id_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  out_tracking_id[0] = '\0';

  /* az_span carries an int32_t length, so an oversized payload would wrap
   * before az_span_create() saw it and trip az_core's precondition handler
   * rather than returning. Refused here instead. */
  if (payload == NULL || payload_len == 0 || payload_len > (size_t)INT32_MAX)
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

  /* Top level only, and skipping whole child values: trackingId is documented
   * at the root, and a nested one belongs to something else.
   *
   * The value is held as a candidate until the root object is seen to CLOSE,
   * matching the other response parsers here: a truncated body must be
   * rejected, not reported as a successful parse of whatever arrived before
   * the cut. */
  bool found = false;
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

    bool is_tracking = az_json_token_is_text_equal(&jr.token, AZ_SPAN_FROM_STR("trackingId"));

    if (az_result_failed(az_json_reader_next_token(&jr)))
    {
      break;
    }

    if (is_tracking && jr.token.kind == AZ_JSON_TOKEN_STRING)
    {
      int32_t n = az_span_size(jr.token.slice);
      /* Dropped rather than truncated when it does not fit: half a correlation
       * id is worse than none, because it would be quoted in a support request
       * and match nothing. */
      if (n >= 0 && (size_t)n + 1 <= out_tracking_id_size && out_tracking_id_size <= INT32_MAX)
      {
        /* az_core's own copy: it NUL-terminates, and it asserts the span fits
         * rather than silently truncating. The guard above keeps that
         * precondition satisfied instead of relying on it to catch us. */
        az_span_to_str(out_tracking_id, (int32_t)out_tracking_id_size, jr.token.slice);
        found = true;
        continue;
      }
      /* Too long to fit is dropped, not truncated: half a correlation id would
       * be quoted in a support request and match nothing. The buffer is cleared
       * because a duplicate key may already have put a good value there, and
       * the caller ignores the status -- an empty string is the contract. */
      out_tracking_id[0] = 0;
      return AZ_IOT_ERR_NOT_FOUND;
    }

    if (jr.token.kind == AZ_JSON_TOKEN_BEGIN_OBJECT || jr.token.kind == AZ_JSON_TOKEN_BEGIN_ARRAY)
    {
      if (az_result_failed(az_json_reader_skip_children(&jr)))
      {
        break;
      }
    }
  }

  if (!closed || !found)
  {
    out_tracking_id[0] = 0;
    return AZ_IOT_ERR_NOT_FOUND;
  }
  return AZ_IOT_OK;
}
