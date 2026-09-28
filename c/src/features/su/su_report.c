// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Software updates reporting: turns the engine state into the STRUCTURED result handed to
 * a channel, plus the standalone report-body builder. */
#include <string.h>

#include <azure/core/az_span.h>

#include "azure/iot/az_iot_su.h"

#include "internal/su_channel_internal.h"
#include "internal/su_internal.h"
#include "internal/su_protocol_internal.h"

/* Agent result codes the contract defines. */
#define AZ_IOT_SU_RESULT_CODE_IN_PROGRESS 1
#define AZ_IOT_SU_RESULT_CODE_SUCCESS 700
#define AZ_IOT_SU_RESULT_CODE_FAILURE (-1)

/** @brief Storage behind the strings of a composed az_iot_su_report. */
typedef struct su_report_scratch
{
  char workflow_id[AZ_IOT_SU_WORKFLOW_ID_SIZE + 1]; /**< NUL-terminated workflow id. */
  char extended[16]; /**< Formatted extended result code. */
  char details[AZ_IOT_SU_RESULT_DETAILS_SIZE + 1]; /**< NUL-terminated result details. */
  az_iot_su_report_update_id installed; /**< Cached installed update id. */
} su_report_scratch;

az_iot_su_outcome az_iot_su__current_outcome(const az_iot_su_client* client)
{
  if (SU_I(client).state == AZ_IOT_SU_STATE_FAILED)
  {
    return AZ_IOT_SU_OUTCOME_FAILED;
  }
  if (SU_I(client).state == AZ_IOT_SU_STATE_IDLE)
  {
    return SU_I(client).pending_outcome;
  }
  return AZ_IOT_SU_OUTCOME_IN_PROGRESS;
}

/**
 * @brief Compose the report the engine state describes for @p outcome.
 *
 * @param[in] client The client; must have an active workflow.
 * @param[in] outcome Outcome to report.
 * @param[out] scratch Storage the report's strings point into.
 * @param[out] report The report.
 */
static void compose_report(
    const az_iot_su_client* client,
    az_iot_su_outcome outcome,
    su_report_scratch* scratch,
    az_iot_su_report* report)
{
  size_t wlen = SU_I(client).active_workflow_id_len;
  if (wlen > AZ_IOT_SU_WORKFLOW_ID_SIZE)
  {
    wlen = AZ_IOT_SU_WORKFLOW_ID_SIZE;
  }
  memcpy(scratch->workflow_id, SU_I(client).active_workflow_id, wlen);
  scratch->workflow_id[wlen] = '\0';

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
  az_iot_su__format_extended_result_code(
      scratch->extended, sizeof(scratch->extended), r->extended_result_code);

  scratch->details[0] = '\0';
  int32_t dlen = az_span_size(r->result_details);
  if (dlen > 0)
  {
    if (dlen > (int32_t)sizeof(scratch->details) - 1)
    {
      dlen = (int32_t)sizeof(scratch->details) - 1;
    }
    memcpy(scratch->details, az_span_ptr(r->result_details), (size_t)dlen);
    scratch->details[dlen] = '\0';
  }

  /* installedUpdateId is what is installed on the device NOW. Once a workflow
   * has succeeded that is the update it applied; until then, and on any
   * non-success outcome, it is whatever was installed before. */
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
      scratch->installed.provider = cached->installed_update_id.provider;
      scratch->installed.name = cached->installed_update_id.name;
      scratch->installed.version = cached->installed_update_id.version;
      installed_ptr = &scratch->installed;
    }
  }

  az_iot_su_failure_origin origin = AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE;
  if (outcome == AZ_IOT_SU_OUTCOME_FAILED)
  {
    origin = (SU_I(client).pending_failure_origin != AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE)
        ? SU_I(client).pending_failure_origin
        : AZ_IOT_SU_FAILURE_ORIGIN_AGENT_CORE;
  }

  memset(report, 0, sizeof(*report));
  report->workflow_id = scratch->workflow_id;
  report->installed_update_id = installed_ptr;
  report->outcome = outcome;
  report->failure_origin = origin;
  report->result_code = result_code;
  report->extended_result_codes = scratch->extended;
  report->result_details = (scratch->details[0] != '\0') ? scratch->details : NULL;
  if (outcome != AZ_IOT_SU_OUTCOME_IN_PROGRESS && SU_I(client).step_results_count > 0)
  {
    report->step_results = SU_I(client).step_results;
    report->step_results_count = SU_I(client).step_results_count;
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

  su_report_scratch scratch;
  az_iot_su_report report;
  compose_report(client, az_iot_su__current_outcome(client), &scratch, &report);

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

az_iot_result az_iot_su__check_report(const az_iot_su_client* client, az_iot_su_outcome outcome)
{
  if (client == NULL || !SU_I(client).active_workflow_valid
      || SU_I(client).active_workflow_id_len == 0)
  {
    return AZ_IOT_ERR_INVALID_ARG;
  }
  su_report_scratch scratch;
  az_iot_su_report report;
  compose_report(client, outcome, &scratch, &report);
  return az_iot_su__report_request_size(&report, AZ_IOT_SU_CHANNEL_BODY_MAX_SIZE, NULL);
}

/* ------------------------------------------------------------------------- */
/* transport-free primitive: standalone report builder                       */
/* ------------------------------------------------------------------------- */

az_iot_result az_iot_su_build_report(
    const az_iot_su_report* report,
    uint8_t* out_json,
    size_t out_size,
    size_t* out_len)
{
  return az_iot_su__build_report_request(report, out_json, out_size, out_len);
}
