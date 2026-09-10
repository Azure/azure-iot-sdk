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

static void the_error_code_is_read_from_the_body(void** state)
{
  (void)state;
  char code[64];
  const char* body = "{\"error\":{\"code\":\"OUTDATED_AGENT_INFO\",\"message\":\"stale\"}}";
  assert_int_equal(
      az_iot_adu__parse_error_code((const uint8_t*)body, strlen(body), code, sizeof(code)),
      AZ_IOT_OK);
  assert_string_equal(code, "OUTDATED_AGENT_INFO");

  const char* none = "{\"message\":\"no code here\"}";
  assert_int_equal(
      az_iot_adu__parse_error_code((const uint8_t*)none, strlen(none), code, sizeof(code)),
      AZ_IOT_ERR_NOT_FOUND);
}

/* The code drives behaviour, not the status: 400 covers both "resend your agent
 * info and retry" and "your request is wrong, do not retry", which need opposite
 * handling. */
static void error_codes_map_to_the_specified_actions(void** state)
{
  (void)state;
  assert_int_equal(
      az_iot_adu__classify_error("UPDATE_ACCOUNT_NOT_LINKED", 409),
      AZ_IOT_ADU_ERROR_ACTION_PROCEED);
  assert_int_equal(
      az_iot_adu__classify_error("OUTDATED_AGENT_INFO", 400),
      AZ_IOT_ADU_ERROR_ACTION_RESEND_AGENT_INFO);
  assert_int_equal(
      az_iot_adu__classify_error("UNKNOWN_AGENT_INFO_VERSION", 400),
      AZ_IOT_ADU_ERROR_ACTION_RESEND_AGENT_INFO);
  assert_int_equal(
      az_iot_adu__classify_error("OUTDATED_SERVICE_CONFIG", 400),
      AZ_IOT_ADU_ERROR_ACTION_DROP_SERVICE_CONFIG_ETAG);
  assert_int_equal(
      az_iot_adu__classify_error("UPSTREAM_UNAVAILABLE", 503), AZ_IOT_ADU_ERROR_ACTION_RETRY);
  assert_int_equal(
      az_iot_adu__classify_error("REPORT_CONFLICT", 409), AZ_IOT_ADU_ERROR_ACTION_ALREADY_REPORTED);

  /* Same status, opposite handling -- which is the whole reason the code wins. */
  assert_int_not_equal(
      az_iot_adu__classify_error("OUTDATED_AGENT_INFO", 400),
      az_iot_adu__classify_error("BAD_REQUEST", 400));
}

/* An unrecognized code must not be assumed retryable: repeating a request the
 * service has already rejected is the worse failure. */
static void an_unknown_error_code_is_not_retried(void** state)
{
  (void)state;
  assert_int_equal(az_iot_adu__classify_error("SOMETHING_NEW", 400), AZ_IOT_ADU_ERROR_ACTION_FATAL);
}

static void a_missing_error_code_falls_back_to_the_status(void** state)
{
  (void)state;
  assert_int_equal(az_iot_adu__classify_error(NULL, 200), AZ_IOT_ADU_ERROR_ACTION_NONE);
  assert_int_equal(az_iot_adu__classify_error(NULL, 429), AZ_IOT_ADU_ERROR_ACTION_RETRY_AFTER);
  assert_int_equal(az_iot_adu__classify_error(NULL, 409), AZ_IOT_ADU_ERROR_ACTION_ALREADY_REPORTED);
  assert_int_equal(az_iot_adu__classify_error(NULL, 503), AZ_IOT_ADU_ERROR_ACTION_RETRY);
  assert_int_equal(az_iot_adu__classify_error(NULL, 401), AZ_IOT_ADU_ERROR_ACTION_FATAL);
  assert_int_equal(az_iot_adu__classify_error("", 403), AZ_IOT_ADU_ERROR_ACTION_FATAL);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(topics_match_the_operation_names),
    cmocka_unit_test(the_declared_topic_bound_is_sufficient),
    cmocka_unit_test(a_short_topic_buffer_is_rejected_not_truncated),
    cmocka_unit_test(an_empty_request_id_is_rejected),
    cmocka_unit_test(response_topics_yield_status_and_request_id),
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
    cmocka_unit_test(the_error_code_is_read_from_the_body),
    cmocka_unit_test(error_codes_map_to_the_specified_actions),
    cmocka_unit_test(an_unknown_error_code_is_not_retried),
    cmocka_unit_test(a_missing_error_code_falls_back_to_the_status),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
