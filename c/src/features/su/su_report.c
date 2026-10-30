// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Software updates reporting: turns the engine state into the STRUCTURED result handed to
 * a channel, plus the standalone report builder. */
#include <string.h>

#include <azure/core/az_span.h>

#include "azure/iot/az_iot_su.h"

#include "internal/log_internal.h"
#include "internal/retry_policy.h"
#include "internal/span_writer.h"
#include "internal/su_internal.h"
#include "internal/su_protocol_internal.h"

/* Agent result codes the contract defines. */
#define AZ_IOT_SU_RESULT_CODE_IN_PROGRESS 1
#define AZ_IOT_SU_RESULT_CODE_SUCCESS 700
#define AZ_IOT_SU_RESULT_CODE_FAILURE (-1)

bool az_iot_su__build_report_view(const az_iot_su_client* client, az_iot_su__report_view* v)
{
  /* With no active workflow there is no correlation key, and therefore nothing
   * the service could attribute a report to. */
  if (!SU_I(client).active_workflow_valid || SU_I(client).active_workflow_id_len == 0)
  {
    return false;
  }

  size_t wlen = SU_I(client).active_workflow_id_len;
  if (wlen > AZ_IOT_SU_WORKFLOW_ID_SIZE)
  {
    wlen = AZ_IOT_SU_WORKFLOW_ID_SIZE;
  }
  memcpy(v->workflow_id, SU_I(client).active_workflow_id, wlen);
  v->workflow_id[wlen] = '\0';

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
  az_iot_su__format_extended_result_code(v->extended, sizeof(v->extended), r->extended_result_code);

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
      v->installed.provider = cached->installed_update_id.provider;
      v->installed.name = cached->installed_update_id.name;
      v->installed.version = cached->installed_update_id.version;
      installed_ptr = &v->installed;
    }
  }

  az_iot_su_report* report = &v->report;
  memset(report, 0, sizeof(*report));
  report->workflow_id = v->workflow_id;
  report->installed_update_id = installed_ptr;
  report->outcome = outcome;
  if (outcome == AZ_IOT_SU_OUTCOME_FAILED)
  {
    report->failure_origin
        = (SU_I(client).pending_failure_origin != AZ_IOT_SU_FAILURE_ORIGIN_NOT_APPLICABLE)
        ? SU_I(client).pending_failure_origin
        : AZ_IOT_SU_FAILURE_ORIGIN_AGENT_CORE;
  }
  report->result_code = result_code;
  report->extended_result_codes = v->extended;
  /* Only report_status() sets overall details, in a NUL-terminated buffer. */
  report->result_details
      = (az_span_size(r->result_details) > 0) ? (const char*)az_span_ptr(r->result_details) : NULL;
  if (outcome != AZ_IOT_SU_OUTCOME_IN_PROGRESS && SU_I(client).step_results_count > 0)
  {
    report->step_results = SU_I(client).step_results;
    report->step_results_count = SU_I(client).step_results_count;
  }

  return true;
}

/**
 * @brief Copy @p src, NUL-terminated, into @p dst.
 *
 * @return false, leaving @p dst empty, when it does not fit.
 */
static bool copy_cstr(char* dst, size_t cap, const char* src)
{
  const char* parts[1] = { src != NULL ? src : "" };
  return az_iot_span_writer_build_str(az_span_create((uint8_t*)dst, (int32_t)cap), NULL, parts, 1)
      == AZ_IOT_OK;
}

/**
 * @brief Keep @p report in sent_terminal slot @p slot, copying what it points at.
 *
 * The engine sets no overall result details, so none are kept. An installed
 * update id longer than AZ_IOT_SU_APPLIED_UPDATE_ID_SIZE is not kept.
 */
static void keep_sent_terminal(
    az_iot_su_client* client,
    uint8_t slot,
    const az_iot_su_report* report)
{
  az_iot_su_sent_report_internal* t = &SU_I(client).sent_terminal[slot];
  t->valid = true;
  t->outcome = report->outcome;
  t->failure_origin = report->failure_origin;
  t->result_code = report->result_code;
  (void)copy_cstr(t->workflow_id, sizeof(t->workflow_id), report->workflow_id);
  (void)copy_cstr(
      t->extended_result_codes, sizeof(t->extended_result_codes), report->extended_result_codes);
  t->has_installed = false;
  const az_iot_su_report_update_id* id = report->installed_update_id;
  if (id != NULL && id->provider != NULL && id->name != NULL && id->version != NULL)
  {
    /* provider, name, version, each NUL-terminated; not kept unless all fit. */
    az_iot_span_writer writer;
    az_iot_span_writer_init(
        &writer, az_span_create((uint8_t*)t->installed, (int32_t)sizeof(t->installed)));
    az_iot_span_writer_append_str(&writer, id->provider);
    az_iot_span_writer_append_u8(&writer, 0);
    az_iot_span_writer_append_str(&writer, id->name);
    az_iot_span_writer_append_u8(&writer, 0);
    az_iot_span_writer_append_str(&writer, id->version);
    az_iot_span_writer_append_u8(&writer, 0);
    t->has_installed = az_iot_span_writer_end(&writer, NULL) == AZ_IOT_OK;
  }
  size_t d = SU_I(client).step_details_size;
  uint8_t* region = SU_I(client).sent_details
      + (size_t)slot * (size_t)_az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS * d;
  int32_t count = report->step_results_count;
  if (count < 0 || count > _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS)
  {
    count = 0;
  }
  t->step_results_count = count;
  for (int32_t i = 0; i < count; ++i)
  {
    t->step_results[i] = report->step_results[i];
    int32_t len = az_span_size(report->step_results[i].result_details);
    if (len > 0 && (size_t)len <= d)
    {
      memcpy(
          region + (size_t)i * d, az_span_ptr(report->step_results[i].result_details), (size_t)len);
      t->step_results[i].result_details = az_span_create(region + (size_t)i * d, len);
    }
    else
    {
      t->step_results[i].result_details = AZ_SPAN_EMPTY;
    }
  }
}

bool az_iot_su__sent_terminal_report(
    const az_iot_su_client* client,
    az_iot_su_report* out,
    az_iot_su_report_update_id* installed)
{
  const az_iot_su_sent_report_internal* t
      = &SU_I(client).sent_terminal[SU_I(client).sent_terminal_slot];
  if (!t->valid)
  {
    return false;
  }
  memset(out, 0, sizeof(*out));
  out->workflow_id = t->workflow_id;
  out->outcome = t->outcome;
  out->failure_origin = t->failure_origin;
  out->result_code = t->result_code;
  out->extended_result_codes = t->extended_result_codes;
  if (t->has_installed)
  {
    installed->provider = t->installed;
    installed->name = installed->provider + strlen(installed->provider) + 1;
    installed->version = installed->name + strlen(installed->name) + 1;
    out->installed_update_id = installed;
  }
  if (t->step_results_count > 0)
  {
    out->step_results = t->step_results;
    out->step_results_count = t->step_results_count;
  }
  return true;
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

  /* With no active workflow there is nothing to report. Not an error: a day-0
   * device that has never been offered an update simply has nothing to say. */
  az_iot_su__report_view v;
  if (!az_iot_su__build_report_view(client, &v))
  {
    return AZ_IOT_OK;
  }
  az_iot_su_outcome outcome = v.report.outcome;

  /* A retryable refusal armed the backoff: do_work() sends the then-current
   * report once it is due, so callers cannot outpace the service. */
  if (SU_I(client).report_paced && !az_iot_retry_state__due(&SU_I(client).retry))
  {
    SU_I(client).device_properties_report_pending = true;
    return AZ_IOT_ERR_BUSY;
  }

  /* Consumed by on_channel_result(), which may run inside report(). A refused
   * call restores the previous value: it may belong to an earlier accepted
   * report still awaiting its verdict. */
  bool was_sent_terminal = SU_I(client).report_sent_terminal;
  bool was_in_flight = SU_I(client).terminal_report_in_flight;
  bool terminal = (outcome != AZ_IOT_SU_OUTCOME_IN_PROGRESS);
  SU_I(client).terminal_report_in_flight = terminal;
  SU_I(client).report_sent_terminal = terminal;
  /* Its verdict may arrive inside report(): keep it first, in the other slot,
   * so a refusal leaves an earlier report still awaiting its verdict intact. */
  uint8_t was_slot = SU_I(client).sent_terminal_slot;
  if (terminal)
  {
    SU_I(client).sent_terminal_slot = (uint8_t)(was_slot ^ 1u);
    keep_sent_terminal(client, SU_I(client).sent_terminal_slot, &v.report);
  }
  az_iot_result sent = SU_I(client).channel.vtable->report(SU_I(client).channel.ctx, &v.report);
  if (sent == AZ_IOT_ERR_NOT_ENOUGH_SPACE
      && (v.report.step_results_count > 0 || v.report.result_details != NULL))
  {
    /* The details do not fit az_iot_su_limits::max_message_size: send the
     * result without them rather than not at all. */
    az_iot_su_step_result plain[_az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS];
    for (int32_t i = 0; i < v.report.step_results_count; ++i)
    {
      plain[i] = v.report.step_results[i];
      plain[i].result_details = AZ_SPAN_EMPTY;
    }
    AZ_IOT_LOG_WARN(
        AZ_IOT_LOG_COMPONENT_SU,
        "report does not fit az_iot_su_limits::max_message_size; sent without result details");
    v.report.step_results = (v.report.step_results_count > 0) ? plain : NULL;
    v.report.result_details = NULL;
    if (terminal)
    {
      keep_sent_terminal(client, SU_I(client).sent_terminal_slot, &v.report);
    }
    sent = SU_I(client).channel.vtable->report(SU_I(client).channel.ctx, &v.report);
  }
  if (sent != AZ_IOT_OK)
  {
    SU_I(client).terminal_report_in_flight = was_in_flight;
    SU_I(client).report_sent_terminal = was_sent_terminal;
    SU_I(client).sent_terminal_slot = was_slot;
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
/* transport-free primitive: standalone report builder                       */
/* ------------------------------------------------------------------------- */

AZ_NODISCARD az_iot_result az_iot_su_build_report(
    const az_iot_su_report* report,
    uint8_t* out_json,
    size_t out_size,
    size_t* out_len)
{
  return az_iot_su__build_report_request(report, out_json, out_size, out_len);
}
