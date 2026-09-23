// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <string.h>

#include <azure/core/az_json.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include "azure/iot/az_iot_adu.h"
#include "internal/adu_internal.h"
#include "internal/log_internal.h"
#include "internal/span_writer.h"

static az_span outcome_name(az_iot_adu_outcome outcome)
{
  switch (outcome)
  {
    case AZ_IOT_ADU_OUTCOME_IN_PROGRESS:
      return AZ_SPAN_FROM_STR("IN_PROGRESS");
    case AZ_IOT_ADU_OUTCOME_SUCCEEDED:
      return AZ_SPAN_FROM_STR("SUCCEEDED");
    case AZ_IOT_ADU_OUTCOME_FAILED:
      return AZ_SPAN_FROM_STR("FAILED");
    case AZ_IOT_ADU_OUTCOME_CANCELED:
      return AZ_SPAN_FROM_STR("CANCELED");
    case AZ_IOT_ADU_OUTCOME_SKIPPED:
      return AZ_SPAN_FROM_STR("SKIPPED");
    default:
      return AZ_SPAN_EMPTY;
  }
}

static az_span failure_origin_name(az_iot_adu_failure_origin origin)
{
  switch (origin)
  {
    case AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE:
      return AZ_SPAN_FROM_STR("NOT_APPLICABLE");
    case AZ_IOT_ADU_FAILURE_ORIGIN_ADU_CLOUD_SERVICE:
      return AZ_SPAN_FROM_STR("ADU_CLOUD_SERVICE");
    case AZ_IOT_ADU_FAILURE_ORIGIN_ADU_MANAGED_RESOURCE:
      return AZ_SPAN_FROM_STR("ADU_MANAGED_RESOURCE");
    case AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_CORE:
      return AZ_SPAN_FROM_STR("AGENT_CORE");
    case AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_EXTENSION:
      return AZ_SPAN_FROM_STR("AGENT_EXTENSION");
    case AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_DEPENDENCY:
      return AZ_SPAN_FROM_STR("AGENT_DEPENDENCY");
    case AZ_IOT_ADU_FAILURE_ORIGIN_DEVICE:
      return AZ_SPAN_FROM_STR("DEVICE");
    case AZ_IOT_ADU_FAILURE_ORIGIN_OTHER:
      return AZ_SPAN_FROM_STR("OTHER");
    default:
      return AZ_SPAN_EMPTY;
  }
}

/* az_json_writer escapes bytes but does not validate UTF-8. Count Unicode scalar
 * values here, not bytes, so the contract's character limit also accepts non-ASCII. */
static bool valid_utf8(az_span text, int32_t max_characters)
{
  int32_t size = az_span_size(text);
  const uint8_t* bytes = az_span_ptr(text);
  if (size < 0 || (size > 0 && bytes == NULL))
  {
    return false;
  }

  int32_t characters = 0;
  for (int32_t i = 0; i < size;)
  {
    if (characters == max_characters)
    {
      return false;
    }
    ++characters;
    uint32_t codepoint = bytes[i++];
    int32_t continuation;
    uint32_t minimum;
    if (codepoint < 0x80)
    {
      continue;
    }
    if (codepoint >= 0xc2 && codepoint <= 0xdf)
    {
      continuation = 1;
      minimum = 0x80;
      codepoint &= 0x1f;
    }
    else if (codepoint >= 0xe0 && codepoint <= 0xef)
    {
      continuation = 2;
      minimum = 0x800;
      codepoint &= 0x0f;
    }
    else if (codepoint >= 0xf0 && codepoint <= 0xf4)
    {
      continuation = 3;
      minimum = 0x10000;
      codepoint &= 0x07;
    }
    else
    {
      return false;
    }
    if (continuation > size - i)
    {
      return false;
    }
    while (continuation-- > 0)
    {
      uint8_t next = bytes[i++];
      if ((next & 0xc0) != 0x80)
      {
        return false;
      }
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
    {
      return false;
    }
  }
  return true;
}

static bool valid_extended_results(az_span codes)
{
  int32_t size = az_span_size(codes);
  const uint8_t* bytes = az_span_ptr(codes);
  if (size <= 0 || size > AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH || bytes == NULL)
  {
    return false;
  }

  uint32_t value = 0;
  bool has_digit = false;
  for (int32_t i = 0; i < size; ++i)
  {
    uint8_t c = bytes[i];
    if (c == ',')
    {
      if (!has_digit)
      {
        return false;
      }
      value = 0;
      has_digit = false;
      continue;
    }
    uint32_t digit;
    if (c >= '0' && c <= '9')
    {
      digit = (uint32_t)(c - '0');
    }
    else if (c >= 'a' && c <= 'f')
    {
      digit = (uint32_t)(c - 'a') + 10;
    }
    else if (c >= 'A' && c <= 'F')
    {
      digit = (uint32_t)(c - 'A') + 10;
    }
    else
    {
      return false;
    }
    if (value > (UINT32_MAX - digit) / 16)
    {
      return false;
    }
    value = value * 16 + digit;
    has_digit = true;
  }
  return has_digit;
}

bool az_iot_adu__valid_result_fields(
    az_iot_adu_outcome outcome,
    az_iot_adu_failure_origin origin,
    const uint8_t* extended,
    int32_t extended_length,
    const uint8_t* details,
    int32_t details_length)
{
  if (extended == NULL || (details == NULL && details_length > 0) || extended_length <= 0
      || extended_length > AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH || details_length < 0
      || details_length > AZ_IOT_ADU_RESULT_DETAILS_MAX_SIZE
      || az_span_size(outcome_name(outcome)) == 0 || az_span_size(failure_origin_name(origin)) == 0)
  {
    return false;
  }
  bool failed = outcome == AZ_IOT_ADU_OUTCOME_FAILED;
  bool not_applicable = origin == AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE;
  return failed != not_applicable
      && valid_extended_results(az_span_create((uint8_t*)(uintptr_t)extended, extended_length))
      && valid_utf8(
             az_span_create((uint8_t*)(uintptr_t)details, details_length),
             AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH);
}

az_iot_result az_iot_adu__validate_install_result(const az_iot_adu_install_result* result)
{
  if (result == NULL || result->step_results_count < 0
      || result->step_results_count > _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS
      || !az_iot_adu__valid_result_fields(
          result->outcome,
          result->failure_origin,
          result->extended_result_codes,
          result->extended_result_codes_length,
          result->result_details,
          result->result_details_length))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  for (int32_t i = 0; i < result->step_results_count; ++i)
  {
    const az_iot_adu_step_result* step = &result->step_results[i];
    if (!az_iot_adu__valid_result_fields(
            step->outcome,
            step->failure_origin,
            step->extended_result_codes,
            step->extended_result_codes_length,
            step->result_details,
            step->result_details_length))
    {
      return AZ_IOT_ERR_INVALID_ARG;
    }
  }
  return AZ_IOT_OK;
}

void az_iot_adu__set_extended_result(az_span destination, int32_t* length, uint32_t code)
{
  if (length == NULL)
  {
    AZ_IOT_LOG_ERROR("adu: cannot format diagnostics without an output length");
    return;
  }
  *length = 0;
  az_iot_span_writer writer;
  az_iot_span_writer_init(&writer, destination);
  az_iot_span_writer_append_hex32(&writer, code, 1);
  az_span written;
  if (az_iot_span_writer_end(&writer, &written) == AZ_IOT_OK)
  {
    *length = az_span_size(written);
  }
  else
  {
    AZ_IOT_LOG_ERROR("adu: extended diagnostic formatting failed");
  }
}

az_iot_result az_iot_adu__report_state(az_iot_adu_client_t* client)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (ADU_I(client).detached)
  {
    return AZ_IOT_ERR_DETACHED;
  }
  if (!ADU_I(client).active_workflow_valid || ADU_I(client).active_workflow_id_len == 0)
  {
    return AZ_IOT_OK;
  }
  if (ADU_I(client).channel.vtable == NULL || ADU_I(client).channel.vtable->report == NULL
      || ADU_I(client).active_workflow_id_len > AZ_IOT_ADU_WORKFLOW_ID_SIZE)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  char workflow_id[AZ_IOT_ADU_WORKFLOW_ID_SIZE + 1];
  size_t wlen = ADU_I(client).active_workflow_id_len;
  memcpy(workflow_id, ADU_I(client).active_workflow_id, wlen);
  workflow_id[wlen] = '\0';

  az_iot_adu_report_update_id installed;
  const az_iot_adu_report_update_id* installed_ptr = NULL;
  if (ADU_I(client).install_result.outcome == AZ_IOT_ADU_OUTCOME_SUCCEEDED
      && ADU_I(client).applied_update_id_valid)
  {
    installed_ptr = &ADU_I(client).applied_update_id;
  }
  else if (ADU_I(client).device_props_buffer != NULL)
  {
    const az_iot_adu_device_properties* cached = &ADU_I(client).device_props;
    if (cached->installed_update_id.provider != NULL && cached->installed_update_id.name != NULL
        && cached->installed_update_id.version != NULL)
    {
      installed.provider = cached->installed_update_id.provider;
      installed.name = cached->installed_update_id.name;
      installed.version = cached->installed_update_id.version;
      installed_ptr = &installed;
    }
  }

  az_iot_adu_report report = AZ_IOT_ADU_REPORT_INIT;
  report.workflow_id = workflow_id;
  report.installed_update_id = installed_ptr;
  report.install_result = &ADU_I(client).install_result;

  az_iot_result sent = ADU_I(client).channel.vtable->report(ADU_I(client).channel.ctx, &report);
  if (sent != AZ_IOT_OK)
  {
    /* Re-arm so do_work re-offers it. Most callers discard this result -- they
     * are state transitions, not report calls -- so without this a report the
     * channel could not take right now (no session yet, or a retry-after still
     * running) is lost, and a status report is the only record the service
     * gets of what this device did. The re-offer rebuilds the report from the
     * engine's state at that moment, so what eventually goes out is current
     * rather than a stale snapshot. */
    ADU_I(client).device_props_report_pending = true;
  }
  return sent;
}

#define ADU_JSON_TRY(expression) \
  do                             \
  {                              \
    az_result r = (expression);  \
    if (az_result_failed(r))     \
    {                            \
      return r;                  \
    }                            \
  } while (0)

static az_result append_string_property(az_json_writer* writer, az_span name, az_span value)
{
  ADU_JSON_TRY(az_json_writer_append_property_name(writer, name));
  return az_json_writer_append_string(writer, value);
}

static az_result append_result_fields(
    az_json_writer* writer,
    az_iot_adu_outcome outcome,
    az_iot_adu_failure_origin origin,
    int64_t result_code,
    az_span extended,
    az_span details)
{
  ADU_JSON_TRY(append_string_property(writer, AZ_SPAN_FROM_STR("outcome"), outcome_name(outcome)));
  ADU_JSON_TRY(append_string_property(
      writer, AZ_SPAN_FROM_STR("failureOrigin"), failure_origin_name(origin)));
  ADU_JSON_TRY(az_json_writer_append_property_name(writer, AZ_SPAN_FROM_STR("resultCode")));
  /* The upstream writer's integer overload is int32, and az_span_i64toa()
   * negates INT64_MIN as a signed value (undefined). Negate in uint64_t. */
  uint8_t number[21];
  az_span digits = AZ_SPAN_FROM_BUFFER(number);
  uint64_t magnitude = (uint64_t)result_code;
  if (result_code < 0)
  {
    number[0] = (uint8_t)'-';
    digits = az_span_slice_to_end(digits, 1);
    magnitude = 0u - magnitude;
  }
  az_span remainder;
  ADU_JSON_TRY(az_span_u64toa(digits, magnitude, &remainder));
  ADU_JSON_TRY(az_json_writer_append_json_text(
      writer, az_span_create(number, (int32_t)sizeof(number) - az_span_size(remainder))));
  ADU_JSON_TRY(append_string_property(writer, AZ_SPAN_FROM_STR("extendedResultCodes"), extended));
  if (az_span_size(details) > 0)
  {
    ADU_JSON_TRY(append_string_property(writer, AZ_SPAN_FROM_STR("resultDetails"), details));
  }
  return AZ_OK;
}

static az_result append_install_result(
    az_json_writer* writer,
    const az_iot_adu_install_result* result)
{
  ADU_JSON_TRY(az_json_writer_append_begin_object(writer));
  ADU_JSON_TRY(append_result_fields(
      writer,
      result->outcome,
      result->failure_origin,
      result->result_code,
      az_span_create(
          (uint8_t*)(uintptr_t)result->extended_result_codes, result->extended_result_codes_length),
      az_span_create((uint8_t*)(uintptr_t)result->result_details, result->result_details_length)));
  if (result->step_results_count > 0)
  {
    ADU_JSON_TRY(az_json_writer_append_property_name(writer, AZ_SPAN_FROM_STR("stepResults")));
    ADU_JSON_TRY(az_json_writer_append_begin_object(writer));
    for (int32_t i = 0; i < result->step_results_count; ++i)
    {
      uint8_t name[16];
      az_iot_span_writer name_writer;
      az_iot_span_writer_init(&name_writer, AZ_SPAN_FROM_BUFFER(name));
      az_iot_span_writer_append_str(&name_writer, "step_");
      az_iot_span_writer_append_i32(&name_writer, i);
      az_span name_span;
      if (az_iot_span_writer_end(&name_writer, &name_span) != AZ_IOT_OK)
      {
        return AZ_ERROR_NOT_ENOUGH_SPACE;
      }
      ADU_JSON_TRY(az_json_writer_append_property_name(writer, name_span));
      ADU_JSON_TRY(az_json_writer_append_begin_object(writer));
      const az_iot_adu_step_result* step = &result->step_results[i];
      ADU_JSON_TRY(append_result_fields(
          writer,
          step->outcome,
          step->failure_origin,
          step->result_code,
          az_span_create(
              (uint8_t*)(uintptr_t)step->extended_result_codes, step->extended_result_codes_length),
          az_span_create((uint8_t*)(uintptr_t)step->result_details, step->result_details_length)));
      ADU_JSON_TRY(az_json_writer_append_end_object(writer));
    }
    ADU_JSON_TRY(az_json_writer_append_end_object(writer));
  }
  return az_json_writer_append_end_object(writer);
}

static bool validated_string_span(const char* text, az_span* span)
{
  if (text == NULL)
  {
    return false;
  }
  size_t length = strlen(text);
  /* Match az_json_writer's maximum unescaped string size before its precondition. */
  if (length > 1000000000 / 6)
  {
    return false;
  }
  *span = az_span_create((uint8_t*)(uintptr_t)text, (int32_t)length);
  return valid_utf8(*span, INT32_MAX);
}

static az_result append_report(
    az_json_writer* writer,
    const az_iot_adu_report* report,
    az_span workflow,
    const az_span installed[3])
{
  ADU_JSON_TRY(az_json_writer_append_begin_object(writer));
  ADU_JSON_TRY(append_string_property(writer, AZ_SPAN_FROM_STR("workflowId"), workflow));
  if (report->installed_update_id != NULL)
  {
    ADU_JSON_TRY(
        az_json_writer_append_property_name(writer, AZ_SPAN_FROM_STR("installedUpdateId")));
    ADU_JSON_TRY(az_json_writer_append_begin_object(writer));
    ADU_JSON_TRY(append_string_property(writer, AZ_SPAN_FROM_STR("provider"), installed[0]));
    ADU_JSON_TRY(append_string_property(writer, AZ_SPAN_FROM_STR("name"), installed[1]));
    ADU_JSON_TRY(append_string_property(writer, AZ_SPAN_FROM_STR("version"), installed[2]));
    ADU_JSON_TRY(az_json_writer_append_end_object(writer));
  }
  ADU_JSON_TRY(az_json_writer_append_property_name(writer, AZ_SPAN_FROM_STR("installResult")));
  ADU_JSON_TRY(append_install_result(writer, report->install_result));
  return az_json_writer_append_end_object(writer);
}

az_iot_result az_iot_adu_build_report(
    const az_iot_adu_report* report,
    uint8_t* out_json,
    size_t out_size,
    size_t* out_len)
{
  if (out_len != NULL)
  {
    *out_len = 0;
  }
  if (out_json != NULL && out_size > 0)
  {
    out_json[0] = '\0';
  }
  if (report == NULL || out_json == NULL || out_size == 0 || out_size > INT32_MAX)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  /* Size stamps (docs/struct_versioning.md). Zero means the caller skipped the
   * _INIT macro. Any other mismatch is a header from another SDK version, and is
   * refused outright: step_results is an inline array, so a different step size
   * shifts every element after the first and nothing can be safely read.
   * The report stamp is checked before any later field is read. */
  if (report->_internal_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (report->_internal_size != sizeof(az_iot_adu_report))
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  const az_iot_adu_install_result* install = report->install_result;
  if (install != NULL && install->_internal_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (install != NULL && install->_internal_size != sizeof(az_iot_adu_install_result))
  {
    return AZ_IOT_ERR_NOT_SUPPORTED;
  }
  if (az_iot_adu__validate_install_result(install) != AZ_IOT_OK)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_span workflow;
  az_span installed[3] = { 0 };
  if (!validated_string_span(report->workflow_id, &workflow) || az_span_size(workflow) == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (report->installed_update_id != NULL
      && (!validated_string_span(report->installed_update_id->provider, &installed[0])
          || !validated_string_span(report->installed_update_id->name, &installed[1])
          || !validated_string_span(report->installed_update_id->version, &installed[2])))
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  az_json_writer writer;
  az_result result
      = az_json_writer_init(&writer, az_span_create(out_json, (int32_t)out_size), NULL);
  if (az_result_succeeded(result))
  {
    result = append_report(&writer, report, workflow, installed);
  }
  if (az_result_failed(result))
  {
    out_json[0] = '\0';
    return result == AZ_ERROR_NOT_ENOUGH_SPACE ? AZ_IOT_ERR_NOT_ENOUGH_SPACE : AZ_IOT_ERR_INTERNAL;
  }
  if (out_len != NULL)
  {
    *out_len = (size_t)az_span_size(az_json_writer_get_bytes_used_in_destination(&writer));
  }
  return AZ_IOT_OK;
}
