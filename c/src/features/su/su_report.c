// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Software updates reporting: turns the engine state into the STRUCTURED result handed to
 * a channel, plus the standalone report builder used in library mode. The
 * upstream az_iot_su_client_device_properties type never escapes to the
 * application; it is built here, on demand, from the client-owned cache. */
#include <string.h>

#include <azure/core/az_json.h>
#include <azure/core/az_result.h>
#include <azure/core/az_span.h>

#include "azure/iot/az_iot_su.h"

#include "internal/su_device_properties_internal.h"
#include "internal/su_internal.h"
#include "internal/su_protocol_internal.h"

/* Reported-property payload buffer. v5 manifests with the upper-bounded step
 * count fit comfortably; bounded and stack-local, no heap. */
#ifndef AZ_IOT_SU_REPORT_BUFFER_SIZE
#define AZ_IOT_SU_REPORT_BUFFER_SIZE 1024
#endif

/* Bound on the free-form result detail carried in a structured report. */
#ifndef AZ_IOT_SU_RESULT_DETAILS_SIZE
#define AZ_IOT_SU_RESULT_DETAILS_SIZE 256
#endif

/* Agent result codes the contract defines. */
#define AZ_IOT_SU_RESULT_CODE_IN_PROGRESS 1
#define AZ_IOT_SU_RESULT_CODE_SUCCESS 700
#define AZ_IOT_SU_RESULT_CODE_FAILURE (-1)

az_iot_su_client_agent_state az_iot_su__agent_state(az_iot_su_state state)
{
  /* The service knows three agent states; the workflow has twelve. Every one is
   * listed rather than folded into default:, so that adding a workflow state
   * forces a decision about how the service should see it -- the previous
   * default: would silently have reported it as in-progress, including for a
   * state that was actually terminal. */
  switch (state)
  {
    case AZ_IOT_SU_STATE_IDLE:
      return AZ_IOT_SU_CLIENT_AGENT_STATE_IDLE;

    case AZ_IOT_SU_STATE_FAILED:
      return AZ_IOT_SU_CLIENT_AGENT_STATE_FAILED;

    case AZ_IOT_SU_STATE_MANIFEST_RECEIVED:
    case AZ_IOT_SU_STATE_VERIFYING_MANIFEST:
    case AZ_IOT_SU_STATE_DOWNLOAD_STARTED:
    case AZ_IOT_SU_STATE_DOWNLOAD_COMPLETE:
    case AZ_IOT_SU_STATE_BACKUP_STARTED:
    case AZ_IOT_SU_STATE_BACKUP_COMPLETE:
    case AZ_IOT_SU_STATE_INSTALL_STARTED:
    case AZ_IOT_SU_STATE_INSTALL_COMPLETE:
    case AZ_IOT_SU_STATE_APPLY_STARTED:
    case AZ_IOT_SU_STATE_RESTORE_STARTED:
      return AZ_IOT_SU_CLIENT_AGENT_STATE_DEPLOYMENT_IN_PROGRESS;

    default:
      /* A value from outside the enum: report the deployment as still running
       * rather than inventing a terminal outcome for it. */
      return AZ_IOT_SU_CLIENT_AGENT_STATE_DEPLOYMENT_IN_PROGRESS;
  }
}

/* Assemble the structured result and hand it to the channel. The engine emits
 * no wire format: how this becomes a request body is the channel's business
 * alone. Reporting is per-workflow and idempotent on the workflow id. */
az_iot_result az_iot_su__report_state(az_iot_su_client* client)
{
  if (client == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  if (SU_I(client).detached)
  {
    return AZ_IOT_ERR_DETACHED;
  }
  if (SU_I(client).channel.vtable == NULL || SU_I(client).channel.vtable->report == NULL)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  /* With no active workflow there is no correlation key, and therefore nothing
   * the service could attribute a report to. Not an error: a day-0 device that
   * has never been offered an update simply has nothing to say. */
  if (!SU_I(client).active_workflow_valid || SU_I(client).active_workflow_id_len == 0)
  {
    return AZ_IOT_OK;
  }

  char workflow_id[AZ_IOT_SU_WORKFLOW_ID_SIZE + 1];
  size_t wlen = SU_I(client).active_workflow_id_len;
  if (wlen > AZ_IOT_SU_WORKFLOW_ID_SIZE)
  {
    wlen = AZ_IOT_SU_WORKFLOW_ID_SIZE;
  }
  memcpy(workflow_id, SU_I(client).active_workflow_id, wlen);
  workflow_id[wlen] = '\0';

  az_iot_su_outcome outcome;
  if (SU_I(client).state == AZ_IOT_SU_STATE_FAILED)
  {
    outcome = AZ_IOT_SU_OUTCOME_FAILED;
  }
  else if (SU_I(client).state == AZ_IOT_SU_STATE_IDLE)
  {
    outcome = SU_I(client).pending_outcome;
  }
  else
  {
    outcome = AZ_IOT_SU_OUTCOME_IN_PROGRESS;
  }

  const az_iot_su_client_install_result* r = &SU_I(client).install_result;

  int32_t result_code;
  switch (outcome)
  {
    case AZ_IOT_SU_OUTCOME_IN_PROGRESS:
      result_code = AZ_IOT_SU_RESULT_CODE_IN_PROGRESS;
      break;
    case AZ_IOT_SU_OUTCOME_SUCCEEDED:
      result_code = AZ_IOT_SU_RESULT_CODE_SUCCESS;
      break;
    case AZ_IOT_SU_OUTCOME_FAILED:
    case AZ_IOT_SU_OUTCOME_CANCELED:
    case AZ_IOT_SU_OUTCOME_SKIPPED:
    default:
      /* Carry the engine's own code when it set one, so a specific failure is
       * not flattened into the generic one. */
      result_code = (r->result_code != 0) ? r->result_code : AZ_IOT_SU_RESULT_CODE_FAILURE;
      break;
  }

  /* Fixed-width hex, matching the contract's comma-separated hex form. A
   * single code is the only shape the engine produces today. */
  char extended[16];
  az_iot_su__format_extended_result_code(extended, sizeof(extended), r->extended_result_code);

  char details[AZ_IOT_SU_RESULT_DETAILS_SIZE];
  details[0] = '\0';
  int32_t dlen = az_span_size(r->result_details);
  if (dlen > 0)
  {
    if (dlen > (int32_t)sizeof(details) - 1)
    {
      dlen = (int32_t)sizeof(details) - 1;
    }
    memcpy(details, az_span_ptr(r->result_details), (size_t)dlen);
    details[dlen] = '\0';
  }

  /* installedUpdateId is what is installed on the device NOW. Once a workflow
   * has succeeded that is the update it applied; until then, and on any
   * non-success outcome, it is whatever was installed before. */
  az_iot_su_report_update_id installed;
  const az_iot_su_report_update_id* installed_ptr = NULL;
  if (outcome == AZ_IOT_SU_OUTCOME_SUCCEEDED && SU_I(client).applied_update_id_valid)
  {
    installed_ptr = &SU_I(client).applied_update_id;
  }
  else if (SU_I(client).device_properties_buffer != NULL)
  {
    const az_iot_su_device_properties* cached = &SU_I(client).device_properties;
    if (cached->installed_update_id.provider != NULL && cached->installed_update_id.name != NULL
        && cached->installed_update_id.version != NULL)
    {
      installed.provider = cached->installed_update_id.provider;
      installed.name = cached->installed_update_id.name;
      installed.version = cached->installed_update_id.version;
      installed_ptr = &installed;
    }
  }

  az_iot_su_report report;
  memset(&report, 0, sizeof(report));
  report.workflow_id = workflow_id;
  report.installed_update_id = installed_ptr;
  report.outcome = outcome;
  report.failure_origin = (outcome == AZ_IOT_SU_OUTCOME_FAILED)
      ? AZ_IOT_SU_FAILURE_ORIGIN_AGENT_CORE
      : AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE;
  report.result_code = result_code;
  report.extended_result_codes = extended;
  report.result_details = (details[0] != '\0') ? details : NULL;
  if (outcome != AZ_IOT_SU_OUTCOME_IN_PROGRESS && SU_I(client).step_results_count > 0)
  {
    report.step_results = SU_I(client).step_results;
    report.step_results_count = SU_I(client).step_results_count;
  }

  az_iot_result sent = SU_I(client).channel.vtable->report(SU_I(client).channel.ctx, &report);
  if (sent != AZ_IOT_OK)
  {
    /* Re-arm so do_work re-offers it. Most callers discard this result -- they
     * are state transitions, not report calls -- so without this a report the
     * channel could not take right now (no session yet, or a retry-after still
     * running) is lost, and a status report is the only record the service
     * gets of what this device did. The re-offer rebuilds the report from the
     * engine's state at that moment, so what eventually goes out is current
     * rather than a stale snapshot. */
    SU_I(client).device_properties_report_pending = true;
  }
  return sent;
}

/* ------------------------------------------------------------------------- */
/* agent core-library API: standalone report builder                         */
/* ------------------------------------------------------------------------- */

/**
 * @brief Wraps @p value in a span if the JSON writer accepts its length.
 *
 * @return false if longer than AZ_IOT_SU_MAX_JSON_STRING_SIZE, which would
 *   trip the writer's preconditions instead of failing.
 */
static bool bounded_span(const char* value, az_span* out)
{
  size_t length = strlen(value);
  if (length > AZ_IOT_SU_MAX_JSON_STRING_SIZE)
  {
    return false;
  }
  *out = az_span_create((uint8_t*)(uintptr_t)value, (int32_t)length);
  return true;
}

az_iot_result az_iot_su_build_report(
    const az_iot_su_device_properties* device_properties,
    const az_iot_su_client_install_result* result,
    const az_iot_su_client_update_request* request,
    az_iot_su_state state,
    uint8_t* out_json,
    size_t out_size,
    size_t* out_len)
{
  if (out_len != NULL)
  {
    *out_len = 0;
  }
  if (device_properties == NULL || out_json == NULL || out_size == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }

  if (out_size > INT32_MAX)
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }

  /* Stateless upstream formatter handle (no channel / state machine). */
  az_iot_adu_client az;
  if (az_result_failed(az_iot_adu_client_init(&az, NULL)))
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  /* Serialize the installed-update-id object the service expects. */
  char update_id_json[128];
  size_t update_id_json_len = 0;
  const char* prov = device_properties->installed_update_id.provider
      ? device_properties->installed_update_id.provider
      : "";
  const char* name = device_properties->installed_update_id.name
      ? device_properties->installed_update_id.name
      : "";
  const char* ver = device_properties->installed_update_id.version
      ? device_properties->installed_update_id.version
      : "";
  az_iot_su_report_update_id id = { prov, name, ver };
  az_json_writer id_writer;
  if (az_result_failed(az_json_writer_init(&id_writer, AZ_SPAN_FROM_BUFFER(update_id_json), NULL)))
  {
    return AZ_IOT_ERR_INTERNAL;
  }
  if (az_result_failed(az_iot_su__write_update_id(&id_writer, &id)))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  update_id_json_len
      = (size_t)az_span_size(az_json_writer_get_bytes_used_in_destination(&id_writer));

  az_iot_su_client_device_properties upstream_properties
      = az_iot_adu_client_device_properties_default();
  if (device_properties->manufacturer != NULL
      && !bounded_span(device_properties->manufacturer, &upstream_properties.manufacturer))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  if (device_properties->model != NULL
      && !bounded_span(device_properties->model, &upstream_properties.model))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
  }
  upstream_properties.update_id
      = az_span_create((uint8_t*)update_id_json, (int32_t)update_id_json_len);
  upstream_properties.adu_version = AZ_SPAN_FROM_STR(AZ_IOT_SU_CLIENT_AGENT_VERSION);

  /* Custom properties (az_span views over the caller's strings; read-only for
   * the duration of this call). Clamped to the upstream array capacity. */
  az_iot_su_device_custom_properties upstream_custom_properties;
  memset(&upstream_custom_properties, 0, sizeof(upstream_custom_properties));
  if (device_properties->custom_properties != NULL
      && device_properties->custom_properties_count > 0)
  {
    const size_t max_cp
        = sizeof(upstream_custom_properties.names) / sizeof(upstream_custom_properties.names[0]);
    size_t count = device_properties->custom_properties_count;
    if (count > max_cp)
    {
      return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    for (size_t i = 0; i < count; ++i)
    {
      if (device_properties->custom_properties[i].name == NULL
          || device_properties->custom_properties[i].value == NULL)
      {
        return AZ_IOT_ERR_INVALID_ARG;
      }
      if (!bounded_span(
              device_properties->custom_properties[i].name, &upstream_custom_properties.names[i])
          || !bounded_span(
              device_properties->custom_properties[i].value, &upstream_custom_properties.values[i]))
      {
        return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
      }
    }
    upstream_custom_properties.count = (int32_t)count;
    upstream_properties.custom_properties = &upstream_custom_properties;
  }

  /* Report the workflow id only when a deployment is in progress. */
  az_iot_su_client_workflow* workflow = NULL;
  if (request != NULL && az_span_size(request->workflow.id) > 0)
  {
    workflow = &((az_iot_su_client_update_request*)(uintptr_t)request)->workflow;
  }

  az_json_writer jw;
  if (az_result_failed(az_json_writer_init(&jw, az_span_create(out_json, (int32_t)out_size), NULL)))
  {
    return AZ_IOT_ERR_INTERNAL;
  }

  az_result ar = az_iot_adu_client_get_agent_state_payload(
      &az,
      &upstream_properties,
      az_iot_su__agent_state(state),
      workflow,
      (az_iot_su_client_install_result*)(uintptr_t)result,
      &jw);
  if (az_result_failed(ar))
  {
    return AZ_IOT_ERR_NOT_ENOUGH_SPACE; /* destination too small for the payload */
  }

  az_span payload = az_json_writer_get_bytes_used_in_destination(&jw);
  if (out_len != NULL)
  {
    *out_len = (size_t)az_span_size(payload);
  }
  return AZ_IOT_OK;
}
