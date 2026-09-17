// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */

/* Unit tests for the device-update wire protocol.
 *
 * Every function under test is pure, so these run with no broker, no session and
 * no service. What they pin is the wire contract itself: topic shape, which
 * fields appear and which are omitted, and how a service error maps to a device
 * action. Those are the things a compiler cannot check and a service will
 * silently reject.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_adu.h"

#include "../../src/features/adu/internal/adu_protocol_internal.h"

/* ------------------------------------------------------------------------- */
/* topics                                                                    */
/* ------------------------------------------------------------------------- */

static void topics_match_the_operation_names(void** state)
{
  (void)state;
  char topic[AZ_IOT_ADU_TOPIC_MAX_SIZE];
  size_t len = 0;

  assert_int_equal(
      az_iot_adu__build_topic(AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE, "7", topic, sizeof(topic), &len),
      AZ_IOT_OK);
  assert_string_equal(topic, "$dps/registrations/POST/iotdps-get-onboarding-deviceupdate/?$rid=7");
  assert_int_equal(len, strlen(topic));

  assert_int_equal(
      az_iot_adu__build_topic(AZ_IOT_ADU_OP_GET_UPDATE, "7", topic, sizeof(topic), &len),
      AZ_IOT_OK);
  assert_string_equal(topic, "$dps/registrations/POST/iotdps-get-deviceupdate/?$rid=7");

  assert_int_equal(
      az_iot_adu__build_topic(AZ_IOT_ADU_OP_REPORT_STATUS, "7", topic, sizeof(topic), &len),
      AZ_IOT_OK);
  assert_string_equal(topic, "$dps/registrations/POST/iotdps-report-deviceupdatestatus/?$rid=7");
}

/* The documented maximum has to actually hold the longest topic, or the bound is
 * decorative. */
static void the_declared_topic_bound_is_sufficient(void** state)
{
  (void)state;
  char topic[AZ_IOT_ADU_TOPIC_MAX_SIZE];
  /* A GUID-shaped request id is the realistic worst case. */
  const char* rid = "0123456789abcdef-0123-4567-89ab-cdef01234567";
  assert_int_equal(
      az_iot_adu__build_topic(AZ_IOT_ADU_OP_REPORT_STATUS, rid, topic, sizeof(topic), NULL),
      AZ_IOT_OK);
}

static void a_short_topic_buffer_is_rejected_not_truncated(void** state)
{
  (void)state;
  char topic[16];
  assert_int_equal(
      az_iot_adu__build_topic(AZ_IOT_ADU_OP_GET_UPDATE, "7", topic, sizeof(topic), NULL),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

static void an_empty_request_id_is_rejected(void** state)
{
  (void)state;
  char topic[AZ_IOT_ADU_TOPIC_MAX_SIZE];
  assert_int_equal(
      az_iot_adu__build_topic(AZ_IOT_ADU_OP_GET_UPDATE, "", topic, sizeof(topic), NULL),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(
      az_iot_adu__build_topic(AZ_IOT_ADU_OP_GET_UPDATE, NULL, topic, sizeof(topic), NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

static void response_topics_yield_status_and_request_id(void** state)
{
  (void)state;
  int32_t status = 0;
  char rid[64];

  const char* t = "$dps/registrations/res/200/?$rid=42";
  assert_int_equal(
      az_iot_adu__parse_response_topic(t, strlen(t), &status, rid, sizeof(rid)), AZ_IOT_OK);
  assert_int_equal(status, 200);
  assert_string_equal(rid, "42");

  const char* e = "$dps/registrations/res/429/?$rid=abc&retry-after=3";
  assert_int_equal(
      az_iot_adu__parse_response_topic(e, strlen(e), &status, rid, sizeof(rid)), AZ_IOT_OK);
  assert_int_equal(status, 429);
  /* The value stops at the parameter separator, not at the end of the topic. */
  assert_string_equal(rid, "abc");
}

/* MQTT has no headers, so the service puts the delay in the topic's query
 * string. Measured against the live service, which answers a failed fetch with
 * "...&retry-after=3". */
static void a_retry_after_is_read_off_the_response_topic(void** state)
{
  (void)state;

  const char* t = "$dps/registrations/res/500/?$rid=adu1&retry-after=3";
  assert_int_equal(az_iot_adu__parse_retry_after_seconds(t, strlen(t)), 3u);

  /* Order does not matter, and a value at the end of the topic is fine. */
  const char* first = "$dps/registrations/res/429/?retry-after=12&$rid=adu2";
  assert_int_equal(az_iot_adu__parse_retry_after_seconds(first, strlen(first)), 12u);
}

/* 0 means "no delay asked for", which is also the safe reading of a value we
 * could not make sense of: the caller falls back on its own backoff rather
 * than stalling on a number it did not understand. */
static void an_absent_or_unusable_retry_after_reads_as_zero(void** state)
{
  (void)state;

  const char* none = "$dps/registrations/res/200/?$rid=adu1";
  assert_int_equal(az_iot_adu__parse_retry_after_seconds(none, strlen(none)), 0u);

  const char* empty = "$dps/registrations/res/500/?$rid=adu1&retry-after=";
  assert_int_equal(az_iot_adu__parse_retry_after_seconds(empty, strlen(empty)), 0u);

  /* Trailing junk is not a number we understood. */
  const char* units = "$dps/registrations/res/500/?$rid=adu1&retry-after=3s";
  assert_int_equal(az_iot_adu__parse_retry_after_seconds(units, strlen(units)), 0u);

  /* Beyond the bound, so we do not park the device for an implausible stretch
   * on the strength of one topic. */
  const char* huge = "$dps/registrations/res/500/?$rid=adu1&retry-after=999999999";
  assert_int_equal(az_iot_adu__parse_retry_after_seconds(huge, strlen(huge)), 0u);

  /* Must be a parameter in its own right, not the tail of another key. */
  const char* suffix = "$dps/registrations/res/500/?$rid=adu1&no-retry-after=9";
  assert_int_equal(az_iot_adu__parse_retry_after_seconds(suffix, strlen(suffix)), 0u);

  assert_int_equal(az_iot_adu__parse_retry_after_seconds(NULL, 0), 0u);
}

static void malformed_response_topics_are_rejected(void** state)
{
  (void)state;
  int32_t status = 0;
  char rid[64];

  const char* no_rid = "$dps/registrations/res/200/";
  assert_int_not_equal(
      az_iot_adu__parse_response_topic(no_rid, strlen(no_rid), &status, rid, sizeof(rid)),
      AZ_IOT_OK);

  const char* no_status = "$dps/registrations/res//?$rid=1";
  assert_int_not_equal(
      az_iot_adu__parse_response_topic(no_status, strlen(no_status), &status, rid, sizeof(rid)),
      AZ_IOT_OK);

  const char* wrong = "$dps/registrations/POST/iotdps-get-deviceupdate/?$rid=1";
  assert_int_not_equal(
      az_iot_adu__parse_response_topic(wrong, strlen(wrong), &status, rid, sizeof(rid)), AZ_IOT_OK);
}

/* ------------------------------------------------------------------------- */
/* fetch request                                                             */
/* ------------------------------------------------------------------------- */

static const az_iot_adu_custom_property k_compat[]
    = { { "manufacturer", "Contoso" }, { "model", "Tractor" } };

static az_iot_adu_agent_info make_agent_info(void)
{
  az_iot_adu_agent_info a = { 0 };
  a.agent_sdk_version = "1.0.0";
  a.agent_profile = 1;
  a.compatibility_properties = k_compat;
  a.compatibility_properties_count = 2;
  return a;
}

static void onboarding_request_omits_the_installed_update_id(void** state)
{
  (void)state;
  uint8_t buf[512];
  size_t len = 0;
  az_iot_adu_agent_info agent = make_agent_info();

  assert_int_equal(
      az_iot_adu__build_fetch_request(&agent, NULL, NULL, NULL, buf, sizeof(buf), &len), AZ_IOT_OK);
  buf[len] = '\0';
  const char* json = (const char*)buf;

  assert_non_null(strstr(json, "\"agentInfo\""));
  assert_non_null(strstr(json, "\"agentSdkVersion\":\"1.0.0\""));
  /* An integer, not a string: "agentProfile":1 and never "1". */
  assert_non_null(strstr(json, "\"agentProfile\":1"));
  assert_null(strstr(json, "\"agentProfile\":\"1\""));
  assert_non_null(strstr(json, "\"manufacturer\":\"Contoso\""));
  /* A day-0 device has nothing installed: the key is absent, not null. */
  assert_null(strstr(json, "installedUpdateId"));
  assert_null(strstr(json, "null"));
}

static void operational_request_carries_the_installed_update_id(void** state)
{
  (void)state;
  uint8_t buf[512];
  size_t len = 0;
  az_iot_adu_agent_info agent = make_agent_info();
  az_iot_adu_report_update_id installed = { "Contoso", "Tractor", "1.0" };

  assert_int_equal(
      az_iot_adu__build_fetch_request(&agent, &installed, NULL, NULL, buf, sizeof(buf), &len),
      AZ_IOT_OK);
  buf[len] = '\0';
  const char* json = (const char*)buf;

  assert_non_null(strstr(json, "\"installedUpdateId\""));
  assert_non_null(strstr(json, "\"provider\":\"Contoso\""));
  assert_non_null(strstr(json, "\"name\":\"Tractor\""));
  assert_non_null(strstr(json, "\"version\":\"1.0\""));
}

static void etags_are_echoed_only_when_held(void** state)
{
  (void)state;
  uint8_t buf[512];
  size_t len = 0;
  az_iot_adu_agent_info agent = make_agent_info();

  assert_int_equal(
      az_iot_adu__build_fetch_request(&agent, NULL, "aaa", "bbb", buf, sizeof(buf), &len),
      AZ_IOT_OK);
  buf[len] = '\0';
  assert_non_null(strstr((const char*)buf, "\"agentInfoEtag\":\"aaa\""));
  assert_non_null(strstr((const char*)buf, "\"serviceConfigEtag\":\"bbb\""));

  /* Without them the keys must not appear at all -- sending an empty ETag is a
   * different statement from sending none. */
  assert_int_equal(
      az_iot_adu__build_fetch_request(&agent, NULL, NULL, NULL, buf, sizeof(buf), &len), AZ_IOT_OK);
  buf[len] = '\0';
  assert_null(strstr((const char*)buf, "Etag"));
}

static void a_partial_installed_update_id_is_rejected(void** state)
{
  (void)state;
  uint8_t buf[512];
  az_iot_adu_agent_info agent = make_agent_info();
  az_iot_adu_report_update_id partial = { "Contoso", NULL, "1.0" };

  assert_int_equal(
      az_iot_adu__build_fetch_request(&agent, &partial, NULL, NULL, buf, sizeof(buf), NULL),
      AZ_IOT_ERR_INVALID_ARG);
}

static void a_short_request_buffer_is_rejected(void** state)
{
  (void)state;
  uint8_t buf[8];
  az_iot_adu_agent_info agent = make_agent_info();
  assert_int_equal(
      az_iot_adu__build_fetch_request(&agent, NULL, NULL, NULL, buf, sizeof(buf), NULL),
      AZ_IOT_ERR_NOT_ENOUGH_SPACE);
}

/* ------------------------------------------------------------------------- */
/* report request                                                            */
/* ------------------------------------------------------------------------- */

static void report_carries_workflow_id_and_install_result(void** state)
{
  (void)state;
  uint8_t buf[512];
  size_t len = 0;
  az_iot_adu_report_update_id installed = { "Contoso", "Tractor", "2.0" };
  az_iot_adu_report report = { 0 };
  report.workflow_id = "wf-1";
  report.installed_update_id = &installed;
  report.outcome = AZ_IOT_ADU_OUTCOME_SUCCEEDED;
  report.failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE;
  report.result_code = 700;
  report.extended_result_codes = "0";
  report.result_details = "done";

  assert_int_equal(az_iot_adu__build_report_request(&report, buf, sizeof(buf), &len), AZ_IOT_OK);
  buf[len] = '\0';
  const char* json = (const char*)buf;

  assert_non_null(strstr(json, "\"workflowId\":\"wf-1\""));
  /* The field is installResult, not lastInstallResult. */
  assert_non_null(strstr(json, "\"installResult\""));
  assert_null(strstr(json, "lastInstallResult"));
  assert_non_null(strstr(json, "\"outcome\":\"SUCCEEDED\""));
  assert_non_null(strstr(json, "\"failureOrigin\":\"NOT_APPLICABLE\""));
  assert_non_null(strstr(json, "\"resultCode\":700"));
  assert_non_null(strstr(json, "\"extendedResultCodes\":\"0\""));
  assert_non_null(strstr(json, "\"resultDetails\":\"done\""));
}

static void report_drops_installed_update_id_when_absent(void** state)
{
  (void)state;
  uint8_t buf[512];
  size_t len = 0;
  az_iot_adu_report report = { 0 };
  report.workflow_id = "wf-1";
  report.installed_update_id = NULL;
  report.outcome = AZ_IOT_ADU_OUTCOME_IN_PROGRESS;
  report.failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE;
  report.result_code = 1;
  report.extended_result_codes = "0";

  assert_int_equal(az_iot_adu__build_report_request(&report, buf, sizeof(buf), &len), AZ_IOT_OK);
  buf[len] = '\0';
  /* Dropped, not serialized as null. */
  assert_null(strstr((const char*)buf, "installedUpdateId"));
  assert_null(strstr((const char*)buf, "null"));
  assert_non_null(strstr((const char*)buf, "\"outcome\":\"IN_PROGRESS\""));
  /* resultDetails is optional and must be absent when unset. */
  assert_null(strstr((const char*)buf, "resultDetails"));
}

/* The contract ties outcome and failureOrigin together. Catching a mismatched
 * pair here keeps it off the wire instead of relying on the service to reject
 * it. */
static void outcome_and_failure_origin_must_agree(void** state)
{
  (void)state;
  uint8_t buf[512];
  az_iot_adu_report report = { 0 };
  report.workflow_id = "wf-1";
  report.result_code = -1;
  report.extended_result_codes = "80000001";

  /* FAILED with NOT_APPLICABLE is invalid. */
  report.outcome = AZ_IOT_ADU_OUTCOME_FAILED;
  report.failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE;
  assert_int_equal(
      az_iot_adu__build_report_request(&report, buf, sizeof(buf), NULL), AZ_IOT_ERR_INVALID_ARG);

  /* A non-failure outcome with a failure origin is equally invalid. */
  report.outcome = AZ_IOT_ADU_OUTCOME_SUCCEEDED;
  report.failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_CORE;
  assert_int_equal(
      az_iot_adu__build_report_request(&report, buf, sizeof(buf), NULL), AZ_IOT_ERR_INVALID_ARG);

  /* FAILED with a real origin is accepted. */
  report.outcome = AZ_IOT_ADU_OUTCOME_FAILED;
  report.failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_CORE;
  assert_int_equal(az_iot_adu__build_report_request(&report, buf, sizeof(buf), NULL), AZ_IOT_OK);
}

static void report_without_a_workflow_id_is_rejected(void** state)
{
  (void)state;
  uint8_t buf[512];
  az_iot_adu_report report = { 0 };
  report.workflow_id = "";
  report.outcome = AZ_IOT_ADU_OUTCOME_SUCCEEDED;
  report.failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE;
  report.extended_result_codes = "0";
  assert_int_equal(
      az_iot_adu__build_report_request(&report, buf, sizeof(buf), NULL), AZ_IOT_ERR_INVALID_ARG);
}

/* ------------------------------------------------------------------------- */
/* fetch response                                                            */
/* ------------------------------------------------------------------------- */

static void an_offered_update_is_captured_verbatim(void** state)
{
  (void)state;
  const char* body = "{\"agentInfoEtag\":\"a1\",\"serviceConfigEtag\":\"s1\","
                     "\"updateMetadata\":{\"workflowId\":\"wf-9\",\"updateManifest\":\"{}\"}}";
  az_iot_adu_fetch_response resp;

  assert_int_equal(
      az_iot_adu__parse_fetch_response((const uint8_t*)body, strlen(body), &resp), AZ_IOT_OK);
  assert_true(resp.has_update);
  assert_true(az_span_size(resp.update_metadata) > 0);

  /* Captured as raw bytes so the engine verifies the signature over exactly
   * what the service sent. */
  char raw[256];
  int32_t n = az_span_size(resp.update_metadata);
  memcpy(raw, az_span_ptr(resp.update_metadata), (size_t)n);
  raw[n] = '\0';
  assert_non_null(strstr(raw, "wf-9"));
  assert_true(raw[0] == '{');
  assert_true(raw[n - 1] == '}');

  assert_int_equal(az_span_size(resp.agent_info_etag), 2);
  assert_int_equal(az_span_size(resp.service_config_etag), 2);
}

/* "No update" is a SUCCESS, not an error. Getting this wrong would make an
 * ordinary poll look like a failure and drive pointless retries. */
static void no_update_is_success_whether_absent_or_null(void** state)
{
  (void)state;
  az_iot_adu_fetch_response resp;

  const char* absent = "{\"agentInfoEtag\":\"a1\",\"serviceConfigEtag\":\"s1\"}";
  assert_int_equal(
      az_iot_adu__parse_fetch_response((const uint8_t*)absent, strlen(absent), &resp), AZ_IOT_OK);
  assert_false(resp.has_update);

  const char* null_meta = "{\"updateMetadata\":null,\"agentInfoEtag\":\"a1\"}";
  assert_int_equal(
      az_iot_adu__parse_fetch_response((const uint8_t*)null_meta, strlen(null_meta), &resp),
      AZ_IOT_OK);
  assert_false(resp.has_update);
  assert_int_equal(az_span_size(resp.agent_info_etag), 2);
}

static void the_root_key_url_is_read_from_service_configuration(void** state)
{
  (void)state;
  const char* body = "{\"serviceConfiguration\":{\"pollIntervalSeconds\":300,"
                     "\"rootKeyDownloadUrl\":\"https://example/rk\"},\"updateMetadata\":null}";
  az_iot_adu_fetch_response resp;

  assert_int_equal(
      az_iot_adu__parse_fetch_response((const uint8_t*)body, strlen(body), &resp), AZ_IOT_OK);
  assert_int_equal(az_span_size(resp.root_key_download_url), 18);
  assert_false(resp.has_update);
}

static void a_malformed_response_body_is_rejected(void** state)
{
  (void)state;
  az_iot_adu_fetch_response resp;
  const char* junk = "not json";
  assert_int_equal(
      az_iot_adu__parse_fetch_response((const uint8_t*)junk, strlen(junk), &resp),
      AZ_IOT_ERR_PROTOCOL);
}

/* ------------------------------------------------------------------------- */
/* errors                                                                    */
/* ------------------------------------------------------------------------- */

/* A partial installed-update triple must be rejected rather than passed to the
 * writer, where a NULL member would trip an upstream precondition instead of
 * returning an error. */
static void a_report_with_a_partial_installed_update_id_is_rejected(void** state)
{
  (void)state;
  uint8_t buf[512];
  az_iot_adu_report_update_id partial = { "Contoso", NULL, "2.0" };
  az_iot_adu_report report = { 0 };
  report.workflow_id = "wf-1";
  report.installed_update_id = &partial;
  report.outcome = AZ_IOT_ADU_OUTCOME_SUCCEEDED;
  report.failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE;
  report.result_code = 700;
  report.extended_result_codes = "0";

  assert_int_equal(
      az_iot_adu__build_report_request(&report, buf, sizeof(buf), NULL), AZ_IOT_ERR_INVALID_ARG);
}

/* Truncated JSON must not read as a successful parse carrying partial data: the
 * caller would act on a half-read response as though the service had answered
 * it. */
static void a_truncated_response_body_is_rejected(void** state)
{
  (void)state;
  az_iot_adu_fetch_response resp;

  const char* cut_mid_object = "{\"agentInfoEtag\":\"a1\",\"updateMetadata\":{\"workflowId\":\"wf";
  assert_int_equal(
      az_iot_adu__parse_fetch_response(
          (const uint8_t*)cut_mid_object, strlen(cut_mid_object), &resp),
      AZ_IOT_ERR_PROTOCOL);

  const char* cut_after_value = "{\"agentInfoEtag\":\"a1\"";
  assert_int_equal(
      az_iot_adu__parse_fetch_response(
          (const uint8_t*)cut_after_value, strlen(cut_after_value), &resp),
      AZ_IOT_ERR_PROTOCOL);
}

/* The device-facing failure body is FLAT: a numeric errorCode plus, when the
 * service surfaces it, info.aduErrorCode. It is NOT the nested
 * {"error":{"code":...}} envelope -- that one is internal to the service chain
 * and never reaches a device. */

/* Per-step results are a MAP keyed step_0, step_1, ... -- NOT a JSON array. The
 * index is the only thing carrying step identity, so emitting an array would
 * lose it. */
static void step_results_serialize_as_an_indexed_map(void** state)
{
  (void)state;
  uint8_t buf[1024];
  size_t len = 0;

  az_iot_adu_client_step_result steps[2];
  memset(steps, 0, sizeof(steps));
  steps[0].result_code = 700;
  steps[0].extended_result_code = 0;
  steps[1].result_code = -1;
  steps[1].extended_result_code = (int32_t)0x80000001;
  steps[1].result_details = AZ_SPAN_FROM_STR("step two failed");

  az_iot_adu_report report = { 0 };
  report.workflow_id = "wf-1";
  report.outcome = AZ_IOT_ADU_OUTCOME_FAILED;
  report.failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_CORE;
  report.result_code = -1;
  report.extended_result_codes = "80000001";
  report.step_results = steps;
  report.step_results_count = 2;

  assert_int_equal(az_iot_adu__build_report_request(&report, buf, sizeof(buf), &len), AZ_IOT_OK);
  buf[len] = '\0';
  const char* json = (const char*)buf;

  assert_non_null(strstr(json, "\"stepResults\""));
  assert_non_null(strstr(json, "\"step_0\""));
  assert_non_null(strstr(json, "\"step_1\""));
  /* A map, not an array. */
  assert_null(strstr(json, "\"stepResults\":["));
  assert_non_null(strstr(json, "\"resultCode\":700"));
  assert_non_null(strstr(json, "\"step two failed\""));
  /* Per-step codes use the same bare-hex form as the aggregate. */
  assert_non_null(strstr(json, "\"extendedResultCodes\":\"80000001\""));
  assert_null(strstr(json, "0x80000001"));
}

/* Omitted entirely when there are none -- an empty map is a different statement
 * from having no per-step results. */
static void no_step_results_means_no_key(void** state)
{
  (void)state;
  uint8_t buf[512];
  size_t len = 0;
  az_iot_adu_report report = { 0 };
  report.workflow_id = "wf-1";
  report.outcome = AZ_IOT_ADU_OUTCOME_SUCCEEDED;
  report.failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE;
  report.result_code = 700;
  report.extended_result_codes = "0";

  assert_int_equal(az_iot_adu__build_report_request(&report, buf, sizeof(buf), &len), AZ_IOT_OK);
  buf[len] = '\0';
  assert_null(strstr((const char*)buf, "stepResults"));
}

static void both_error_signals_are_read_from_the_body(void** state)
{
  (void)state;
  char code[64];
  int32_t numeric = 0;

  const char* full
      = "{\"errorCode\":400004,\"trackingId\":\"abc-guid\","
        "\"message\":\"Agent info resend required.\","
        "\"info\":{\"aduErrorCode\":\"OUTDATED_AGENT_INFO\",\"aduErrorTarget\":\"agentInfo\"},"
        "\"timestampUtc\":\"2026-07-09T18:22:31Z\"}";
  assert_int_equal(
      az_iot_adu__parse_error_code(
          (const uint8_t*)full, strlen(full), code, sizeof(code), &numeric),
      AZ_IOT_OK);
  assert_string_equal(code, "OUTDATED_AGENT_INFO");
  assert_int_equal(numeric, 400004);
}

/* Surfacing info.aduErrorCode is a SHOULD, not a MUST, so a body without it is
 * a normal case and must still classify.
 *
 * "message" is picked up as a code candidate, but it is prose as often as it is
 * a code -- so what matters here is that prose does NOT decide the outcome: the
 * numeric code still does. */
static void a_numeric_only_body_is_still_usable(void** state)
{
  (void)state;
  char code[64];
  int32_t numeric = 0;

  const char* numeric_only
      = "{\"errorCode\":409000,\"trackingId\":\"g\",\"message\":\"Conflict.\"}";
  assert_int_equal(
      az_iot_adu__parse_error_code(
          (const uint8_t*)numeric_only, strlen(numeric_only), code, sizeof(code), &numeric),
      AZ_IOT_OK);
  assert_string_equal(code, "Conflict.");
  assert_int_equal(numeric, 409000);

  /* Unrecognized prose falls through to the numeric ladder, which is still what
   * splits 409000 by operation. */
  assert_int_equal(
      az_iot_adu__classify_error(code, numeric, AZ_IOT_ADU_OP_REPORT_STATUS),
      AZ_IOT_ADU_ERROR_ACTION_ALREADY_REPORTED);
  assert_int_equal(
      az_iot_adu__classify_error(code, numeric, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_PROCEED);

  /* No signal at all: not even a message. */
  const char* neither = "{\"trackingId\":\"g\"}";
  assert_int_equal(
      az_iot_adu__parse_error_code(
          (const uint8_t*)neither, strlen(neither), code, sizeof(code), &numeric),
      AZ_IOT_ERR_NOT_FOUND);
  assert_int_equal(numeric, 0);
}

/* The nested service-to-service envelope must NOT be mistaken for the
 * device-facing body. Parsing it as one is the bug this replaced. */
static void the_internal_envelope_is_not_the_device_body(void** state)
{
  (void)state;
  char code[64];
  int32_t numeric = 0;

  const char* internal = "{\"error\":{\"code\":\"UPDATE_ACCOUNT_NOT_LINKED\",\"message\":\"m\"}}";
  assert_int_equal(
      az_iot_adu__parse_error_code(
          (const uint8_t*)internal, strlen(internal), code, sizeof(code), &numeric),
      AZ_IOT_ERR_NOT_FOUND);
}

/* Detection must not depend on an optional output parameter: a caller that only
 * wants the string code still needs a numeric-only body reported as found. */
static void a_numeric_only_body_is_found_without_the_out_parameter(void** state)
{
  (void)state;
  char code[64];
  const char* numeric_only = "{\"errorCode\":400004,\"message\":\"resend\"}";

  assert_int_equal(
      az_iot_adu__parse_error_code(
          (const uint8_t*)numeric_only, strlen(numeric_only), code, sizeof(code), NULL),
      AZ_IOT_OK);
  /* The message is still captured; what this pins is that detection does not
   * depend on the caller wanting the numeric value. */
  assert_string_equal(code, "resend");
}

/* A truncated failure body must not classify: acting on a partially read error
 * could drive a resend or a retry from incomplete JSON. */
static void a_truncated_error_body_is_rejected(void** state)
{
  (void)state;
  char code[64];
  int32_t numeric = 0;

  /* Cut after a COMPLETE, delimited value, so the signal really was read and the
   * only defect is that the object never closed. (Truncating mid-number instead
   * would prove nothing: an unterminated number never yields a token, so the
   * body would be rejected for having no signal at all.) */
  const char* cut_after_value = "{\"errorCode\":400004,\"message\":\"x\"";
  assert_int_equal(
      az_iot_adu__parse_error_code(
          (const uint8_t*)cut_after_value, strlen(cut_after_value), code, sizeof(code), &numeric),
      AZ_IOT_ERR_NOT_FOUND);

  /* Cut inside the nested info object, after a complete string code. */
  const char* cut_in_info = "{\"errorCode\":409000,\"info\":{\"aduErrorCode\":\"REPORT_CONFLICT\"";
  assert_int_equal(
      az_iot_adu__parse_error_code(
          (const uint8_t*)cut_in_info, strlen(cut_in_info), code, sizeof(code), &numeric),
      AZ_IOT_ERR_NOT_FOUND);
}

static void string_codes_map_to_the_specified_actions(void** state)
{
  (void)state;
  assert_int_equal(
      az_iot_adu__classify_error("UPDATE_ACCOUNT_NOT_LINKED", 409000, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_PROCEED);
  assert_int_equal(
      az_iot_adu__classify_error("OUTDATED_AGENT_INFO", 400004, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_RESEND_AGENT_INFO);
  assert_int_equal(
      az_iot_adu__classify_error("UNKNOWN_AGENT_INFO_VERSION", 400004, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_RESEND_AGENT_INFO);
  assert_int_equal(
      az_iot_adu__classify_error("OUTDATED_SERVICE_CONFIG", 400004, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_DROP_SERVICE_CONFIG_ETAG);
  assert_int_equal(
      az_iot_adu__classify_error("REPORT_CONFLICT", 409000, AZ_IOT_ADU_OP_REPORT_STATUS),
      AZ_IOT_ADU_ERROR_ACTION_ALREADY_REPORTED);
}

/* The recoverable case has its own numeric code, so it survives the string code
 * being absent. Getting this wrong means giving up on a request the service
 * explicitly invited us to retry. */
static void the_resend_family_is_recoverable_from_the_numeric_code_alone(void** state)
{
  (void)state;
  assert_int_equal(
      az_iot_adu__classify_error(NULL, 400004, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_RESEND_AGENT_INFO);
  assert_int_equal(
      az_iot_adu__classify_error("", 400004, AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_RESEND_AGENT_INFO);

  /* Other 400s are NOT recoverable -- only 400004 is. */
  assert_int_equal(
      az_iot_adu__classify_error(NULL, 400002, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_FATAL);
  assert_int_equal(
      az_iot_adu__classify_error(NULL, 400000, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_FATAL);
}

/* 409000 covers two conditions needing OPPOSITE handling. Without a string code
 * the operation in flight is the only thing that separates them. */
static void the_shared_conflict_code_is_split_by_operation(void** state)
{
  (void)state;
  /* On a fetch: the account is not linked -- proceed, do not retry. */
  assert_int_equal(
      az_iot_adu__classify_error(NULL, 409000, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_PROCEED);
  assert_int_equal(
      az_iot_adu__classify_error(NULL, 409000, AZ_IOT_ADU_OP_GET_ONBOARDING_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_PROCEED);

  /* On a report: a terminal result is already recorded -- treat as delivered. */
  assert_int_equal(
      az_iot_adu__classify_error(NULL, 409000, AZ_IOT_ADU_OP_REPORT_STATUS),
      AZ_IOT_ADU_ERROR_ACTION_ALREADY_REPORTED);
}

static void transient_numeric_codes_are_retried(void** state)
{
  (void)state;
  assert_int_equal(
      az_iot_adu__classify_error(NULL, 503000, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_RETRY);
  assert_int_equal(
      az_iot_adu__classify_error(NULL, 500000, AZ_IOT_ADU_OP_REPORT_STATUS),
      AZ_IOT_ADU_ERROR_ACTION_RETRY);
  assert_int_equal(
      az_iot_adu__classify_error(NULL, 429000, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_RETRY_AFTER);
  assert_int_equal(
      az_iot_adu__classify_error(NULL, 429001, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_RETRY_AFTER);
}

/* An unrecognized signal is never assumed retryable: repeating a request the
 * service already rejected is the worse failure mode. */
static void unknown_and_absent_signals_are_fatal(void** state)
{
  (void)state;
  assert_int_equal(
      az_iot_adu__classify_error("SOMETHING_NEW", 400002, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_FATAL);
  assert_int_equal(
      az_iot_adu__classify_error(NULL, 401000, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_FATAL);
  assert_int_equal(
      az_iot_adu__classify_error(NULL, 403001, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_FATAL);
  /* No signal at all. */
  assert_int_equal(
      az_iot_adu__classify_error(NULL, 0, AZ_IOT_ADU_OP_GET_UPDATE), AZ_IOT_ADU_ERROR_ACTION_FATAL);
}

/* ------------------------------------------------------------------------- */
/* bodies captured from the live service                                     */
/* ------------------------------------------------------------------------- */
/*
 * Everything below is a verbatim response recorded from a real DPS/ADU
 * endpoint over MQTT, not a body derived from reading the spec. They are kept
 * byte-for-byte so a future change that breaks against the real service fails
 * here first.
 */

/* Both fetch routes answer this when there is nothing to install. Note the
 * ETag spelling: lowercase 't'. The service only ever emits "Etag", so a
 * case-sensitive parser looking for "ETag" silently loses both values. */
static void live_no_update_response_parses(void** state)
{
  (void)state;
  const char* live = "{\"serviceConfiguration\":{\"rootKeyDownloadUrl\":\"http://defaultv3--adu-"
                     "ewertons09092100.b.nlu.dl.adu.microsoft.com/westus2/rootkeypackages/"
                     "rootkeypackage-2.json\"},\"serviceConfigEtag\":\"0532baf1108f1cd8\","
                     "\"agentInfoEtag\":\"b8aef25be073c748\"}";

  az_iot_adu_fetch_response resp;
  memset(&resp, 0, sizeof(resp));
  assert_int_equal(
      az_iot_adu__parse_fetch_response((const uint8_t*)live, strlen(live), &resp), AZ_IOT_OK);

  /* No updateMetadata is SUCCESS, not a truncated payload. */
  assert_false(resp.has_update);
  assert_true(az_span_is_content_equal(resp.agent_info_etag, AZ_SPAN_FROM_STR("b8aef25be073c748")));
  assert_true(
      az_span_is_content_equal(resp.service_config_etag, AZ_SPAN_FROM_STR("0532baf1108f1cd8")));
}

/* Echoing matching ETags gets a much smaller reply with serviceConfiguration
 * omitted -- 75 bytes against 235. The omission means "nothing changed"; it
 * must not be read as a malformed response. */
static void live_steady_state_response_parses(void** state)
{
  (void)state;
  const char* live
      = "{\"serviceConfigEtag\":\"0532baf1108f1cd8\",\"agentInfoEtag\":\"b8aef25be073c748\"}";

  az_iot_adu_fetch_response resp;
  memset(&resp, 0, sizeof(resp));
  assert_int_equal(
      az_iot_adu__parse_fetch_response((const uint8_t*)live, strlen(live), &resp), AZ_IOT_OK);
  assert_false(resp.has_update);
  assert_true(az_span_is_content_equal(resp.agent_info_etag, AZ_SPAN_FROM_STR("b8aef25be073c748")));
  assert_true(
      az_span_is_content_equal(resp.service_config_etag, AZ_SPAN_FROM_STR("0532baf1108f1cd8")));
}

/* The real device-facing error body has no "info" object at all: the
 * originating code arrives in "message". 400000 is a shared bucket, so that
 * field is the only thing that tells these two apart. */
static void live_error_bodies_carry_the_code_in_message(void** state)
{
  (void)state;
  char code[64];
  int32_t numeric = 0;

  /* Operational fetch for a device the registry does not know. */
  const char* not_onboarded
      = "{\"errorCode\":400000,\"trackingId\":\"4465e486-8449-4f3d-ab38-f4053ab65f0b\","
        "\"message\":\"INVALID_REQUEST\",\"timestampUtc\":\"2026-09-12T19:44:21.9191084Z\"}";
  assert_int_equal(
      az_iot_adu__parse_error_code(
          (const uint8_t*)not_onboarded, strlen(not_onboarded), code, sizeof(code), &numeric),
      AZ_IOT_OK);
  assert_int_equal(numeric, 400000);
  assert_string_equal(code, "INVALID_REQUEST");
  assert_int_equal(
      az_iot_adu__classify_error(code, numeric, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_FATAL);

  /* Reporting against a workflow the service has no record of -- same numeric
   * code, different meaning. */
  const char* unknown_workflow
      = "{\"errorCode\":400000,\"trackingId\":\"887f0758-64cb-40ca-8b99-5dda46b9c6ac\","
        "\"message\":\"UNKNOWN_WORKFLOW_ID\",\"timestampUtc\":\"2026-09-12T19:44:34.8946783Z\"}";
  assert_int_equal(
      az_iot_adu__parse_error_code(
          (const uint8_t*)unknown_workflow, strlen(unknown_workflow), code, sizeof(code), &numeric),
      AZ_IOT_OK);
  assert_int_equal(numeric, 400000);
  assert_string_equal(code, "UNKNOWN_WORKFLOW_ID");
  /* Not ALREADY_REPORTED: nothing was delivered, and resending cannot help. */
  assert_int_equal(
      az_iot_adu__classify_error(code, numeric, AZ_IOT_ADU_OP_REPORT_STATUS),
      AZ_IOT_ADU_ERROR_ACTION_FATAL);
}

/* Sending extendedResultCodes as an array rather than a string is answered
 * 400012 -- and "message" here is prose, not a code. */
static void live_deserialization_error_parses(void** state)
{
  (void)state;
  char code[64];
  int32_t numeric = 0;

  const char* live
      = "{\"errorCode\":400012,\"trackingId\":\"a7908009-2e20-4adb-a35c-cd2100d9eefc\","
        "\"message\":\"Deserialization error.\",\"timestampUtc\":\"2026-09-12T19:44:35.5480356Z\"}";
  assert_int_equal(
      az_iot_adu__parse_error_code(
          (const uint8_t*)live, strlen(live), code, sizeof(code), &numeric),
      AZ_IOT_OK);
  assert_int_equal(numeric, AZ_IOT_ADU_ERR_DESERIALIZATION_FAILED);
  assert_string_equal(code, "Deserialization error.");
  assert_int_equal(
      az_iot_adu__classify_error(code, numeric, AZ_IOT_ADU_OP_REPORT_STATUS),
      AZ_IOT_ADU_ERROR_ACTION_FATAL);
}

/* The reason prose in "message" must not be classified on its own: a transient
 * server error carries prose too, and judging the string alone would turn a
 * retryable failure into a permanent one. */
static void prose_in_message_does_not_make_a_retryable_error_fatal(void** state)
{
  (void)state;
  char code[64];
  int32_t numeric = 0;

  const char* server_error
      = "{\"errorCode\":500000,\"trackingId\":\"g\",\"message\":\"Internal server error\"}";
  assert_int_equal(
      az_iot_adu__parse_error_code(
          (const uint8_t*)server_error, strlen(server_error), code, sizeof(code), &numeric),
      AZ_IOT_OK);
  assert_int_equal(
      az_iot_adu__classify_error(code, numeric, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_RETRY);
}

/* A message too long for the caller's buffer is dropped rather than truncated:
 * a truncated token could compare equal to nothing useful, and the numeric code
 * still classifies. */
static void an_oversized_message_is_dropped_not_truncated(void** state)
{
  (void)state;
  char code[8];
  int32_t numeric = 0;

  const char* live = "{\"errorCode\":500000,\"message\":\"a message far longer than the buffer\"}";
  assert_int_equal(
      az_iot_adu__parse_error_code(
          (const uint8_t*)live, strlen(live), code, sizeof(code), &numeric),
      AZ_IOT_OK);
  assert_string_equal(code, "");
  assert_int_equal(numeric, 500000);
  assert_int_equal(
      az_iot_adu__classify_error(code, numeric, AZ_IOT_ADU_OP_GET_UPDATE),
      AZ_IOT_ADU_ERROR_ACTION_RETRY);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(topics_match_the_operation_names),
    cmocka_unit_test(the_declared_topic_bound_is_sufficient),
    cmocka_unit_test(a_short_topic_buffer_is_rejected_not_truncated),
    cmocka_unit_test(an_empty_request_id_is_rejected),
    cmocka_unit_test(response_topics_yield_status_and_request_id),
    cmocka_unit_test(a_retry_after_is_read_off_the_response_topic),
    cmocka_unit_test(an_absent_or_unusable_retry_after_reads_as_zero),
    cmocka_unit_test(malformed_response_topics_are_rejected),
    cmocka_unit_test(onboarding_request_omits_the_installed_update_id),
    cmocka_unit_test(operational_request_carries_the_installed_update_id),
    cmocka_unit_test(etags_are_echoed_only_when_held),
    cmocka_unit_test(a_partial_installed_update_id_is_rejected),
    cmocka_unit_test(a_short_request_buffer_is_rejected),
    cmocka_unit_test(report_carries_workflow_id_and_install_result),
    cmocka_unit_test(report_drops_installed_update_id_when_absent),
    cmocka_unit_test(outcome_and_failure_origin_must_agree),
    cmocka_unit_test(report_without_a_workflow_id_is_rejected),
    cmocka_unit_test(an_offered_update_is_captured_verbatim),
    cmocka_unit_test(no_update_is_success_whether_absent_or_null),
    cmocka_unit_test(the_root_key_url_is_read_from_service_configuration),
    cmocka_unit_test(a_malformed_response_body_is_rejected),
    cmocka_unit_test(a_report_with_a_partial_installed_update_id_is_rejected),
    cmocka_unit_test(a_truncated_response_body_is_rejected),
    cmocka_unit_test(step_results_serialize_as_an_indexed_map),
    cmocka_unit_test(no_step_results_means_no_key),
    cmocka_unit_test(both_error_signals_are_read_from_the_body),
    cmocka_unit_test(a_numeric_only_body_is_still_usable),
    cmocka_unit_test(live_no_update_response_parses),
    cmocka_unit_test(live_steady_state_response_parses),
    cmocka_unit_test(live_error_bodies_carry_the_code_in_message),
    cmocka_unit_test(live_deserialization_error_parses),
    cmocka_unit_test(prose_in_message_does_not_make_a_retryable_error_fatal),
    cmocka_unit_test(an_oversized_message_is_dropped_not_truncated),
    cmocka_unit_test(the_internal_envelope_is_not_the_device_body),
    cmocka_unit_test(a_numeric_only_body_is_found_without_the_out_parameter),
    cmocka_unit_test(a_truncated_error_body_is_rejected),
    cmocka_unit_test(string_codes_map_to_the_specified_actions),
    cmocka_unit_test(the_resend_family_is_recoverable_from_the_numeric_code_alone),
    cmocka_unit_test(the_shared_conflict_code_is_split_by_operation),
    cmocka_unit_test(transient_numeric_codes_are_retried),
    cmocka_unit_test(unknown_and_absent_signals_are_fatal),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
