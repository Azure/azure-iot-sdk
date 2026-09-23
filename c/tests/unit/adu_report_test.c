// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include <stdarg.h>
#include <stddef.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include <azure/core/az_json.h>
#include <azure/core/az_span.h>

#include "azure/iot/az_iot_adu.h"
#include "internal/adu_channel_internal.h"
#include "internal/adu_internal.h"

static const char* const outcomes[]
    = { "IN_PROGRESS", "SUCCEEDED", "FAILED", "CANCELED", "SKIPPED" };
static const char* const origins[] = { "NOT_APPLICABLE",
                                       "ADU_CLOUD_SERVICE",
                                       "ADU_MANAGED_RESOURCE",
                                       "AGENT_CORE",
                                       "AGENT_EXTENSION",
                                       "AGENT_DEPENDENCY",
                                       "DEVICE",
                                       "OTHER" };

static az_span text_span(const char* text)
{
  return az_span_create_from_str((char*)(uintptr_t)text);
}

static void set_codes(az_iot_adu_step_result* result, az_span value)
{
  result->extended_result_codes_length = az_span_size(value);
  if (az_span_size(value) > 0 && az_span_size(value) <= AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH)
  {
    memcpy(result->extended_result_codes, az_span_ptr(value), (size_t)az_span_size(value));
  }
}

static void set_details(az_iot_adu_step_result* result, az_span value)
{
  result->result_details_length = az_span_size(value);
  if (az_span_size(value) > 0 && az_span_size(value) <= AZ_IOT_ADU_RESULT_DETAILS_MAX_SIZE)
  {
    memcpy(result->result_details, az_span_ptr(value), (size_t)az_span_size(value));
  }
}

static void set_install_codes(az_iot_adu_install_result* result, az_span value)
{
  result->extended_result_codes_length = az_span_size(value);
  if (az_span_size(value) > 0 && az_span_size(value) <= AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH)
  {
    memcpy(result->extended_result_codes, az_span_ptr(value), (size_t)az_span_size(value));
  }
}

static void set_install_details(az_iot_adu_install_result* result, az_span value)
{
  result->result_details_length = az_span_size(value);
  if (az_span_size(value) > 0 && az_span_size(value) <= AZ_IOT_ADU_RESULT_DETAILS_MAX_SIZE)
  {
    memcpy(result->result_details, az_span_ptr(value), (size_t)az_span_size(value));
  }
}

static az_iot_adu_step_result install_fields(const az_iot_adu_install_result* install)
{
  az_iot_adu_step_result step = { 0 };
  step.outcome = install->outcome;
  step.failure_origin = install->failure_origin;
  step.result_code = install->result_code;
  memcpy(
      step.extended_result_codes,
      install->extended_result_codes,
      sizeof(step.extended_result_codes));
  step.extended_result_codes_length = install->extended_result_codes_length;
  memcpy(step.result_details, install->result_details, sizeof(step.result_details));
  step.result_details_length = install->result_details_length;
  return step;
}

static void set_install_fields(
    az_iot_adu_install_result* install,
    const az_iot_adu_step_result* step)
{
  install->outcome = step->outcome;
  install->failure_origin = step->failure_origin;
  install->result_code = step->result_code;
  memcpy(
      install->extended_result_codes,
      step->extended_result_codes,
      sizeof(install->extended_result_codes));
  install->extended_result_codes_length = step->extended_result_codes_length;
  memcpy(install->result_details, step->result_details, sizeof(install->result_details));
  install->result_details_length = step->result_details_length;
}

static az_iot_adu_install_result valid_result(void)
{
  az_iot_adu_install_result result = AZ_IOT_ADU_INSTALL_RESULT_INIT;
  result.outcome = AZ_IOT_ADU_OUTCOME_IN_PROGRESS;
  result.failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE;
  set_install_codes(&result, AZ_SPAN_FROM_STR("0"));
  return result;
}

static az_iot_adu_report make_report(
    const char* workflow_id,
    const az_iot_adu_report_update_id* installed,
    const az_iot_adu_install_result* result)
{
  az_iot_adu_report report = AZ_IOT_ADU_REPORT_INIT;
  report.workflow_id = workflow_id;
  report.installed_update_id = installed;
  report.install_result = result;
  return report;
}

static void expect_token(az_json_reader* reader, az_json_token_kind kind)
{
  assert_int_equal(az_json_reader_next_token(reader), AZ_OK);
  assert_int_equal(reader->token.kind, kind);
}

static void expect_property(az_json_reader* reader, const char* name)
{
  expect_token(reader, AZ_JSON_TOKEN_PROPERTY_NAME);
  assert_true(az_json_token_is_text_equal(&reader->token, text_span(name)));
}

static void expect_json_string(az_json_reader* reader, az_span value)
{
  expect_token(reader, AZ_JSON_TOKEN_STRING);
  /* Upstream readers do not decode \uXXXX; check the writer's control escapes here. */
  const uint8_t* encoded = az_span_ptr(reader->token.slice);
  int32_t size = az_span_size(reader->token.slice);
  int32_t pos = 0;
  for (int32_t i = 0; i < az_span_size(value); ++i)
  {
    assert_true(pos < size);
    uint8_t byte = encoded[pos++];
    if (byte == '\\')
    {
      assert_true(pos < size);
      byte = encoded[pos++];
      switch (byte)
      {
        case 'b':
          byte = '\b';
          break;
        case 'f':
          byte = '\f';
          break;
        case 'n':
          byte = '\n';
          break;
        case 'r':
          byte = '\r';
          break;
        case 't':
          byte = '\t';
          break;
        case '"':
        case '\\':
        case '/':
          break;
        case 'u':
        {
          assert_true(size - pos >= 4);
          uint32_t code = 0;
          for (int j = 0; j < 4; ++j)
          {
            uint8_t c = encoded[pos++];
            uint32_t digit;
            if (c >= '0' && c <= '9')
            {
              digit = (uint32_t)(c - '0');
            }
            else if (c >= 'a' && c <= 'f')
            {
              digit = (uint32_t)(c - 'a') + 10;
            }
            else
            {
              assert_true(c >= 'A' && c <= 'F');
              digit = (uint32_t)(c - 'A') + 10;
            }
            code = code * 16 + digit;
          }
          assert_true(code < 0x20);
          byte = (uint8_t)code;
          break;
        }
        default:
          fail_msg("Unexpected JSON escape");
          break;
      }
    }
    assert_int_equal(byte, az_span_ptr(value)[i]);
  }
  assert_int_equal(pos, size);
}

static void expect_result_fields(
    az_json_reader* reader,
    az_iot_adu_outcome outcome,
    az_iot_adu_failure_origin origin,
    int64_t result_code,
    az_span extended,
    az_span details)
{
  expect_property(reader, "outcome");
  expect_json_string(reader, text_span(outcomes[outcome]));
  expect_property(reader, "failureOrigin");
  expect_json_string(reader, text_span(origins[origin]));
  expect_property(reader, "resultCode");
  expect_token(reader, AZ_JSON_TOKEN_NUMBER);
  /* Compare text: az_span_atoi64() overflows on INT64_MIN. */
  char code[21];
  (void)snprintf(code, sizeof(code), "%" PRId64, result_code);
  assert_true(az_span_is_content_equal(reader->token.slice, text_span(code)));
  expect_property(reader, "extendedResultCodes");
  expect_json_string(reader, extended);
  if (az_span_size(details) > 0)
  {
    expect_property(reader, "resultDetails");
    expect_json_string(reader, details);
  }
}

/* Consume the entire document, including every object boundary: this rejects
 * legacy envelopes, misplaced fields, arrays, updateId on steps and extra keys. */
static void expect_report(const az_iot_adu_report* report, uint8_t* json, size_t length)
{
  az_json_reader reader;
  assert_int_equal(
      az_json_reader_init(&reader, az_span_create(json, (int32_t)length), NULL), AZ_OK);
  expect_token(&reader, AZ_JSON_TOKEN_BEGIN_OBJECT);
  expect_property(&reader, "workflowId");
  expect_json_string(&reader, text_span(report->workflow_id));
  if (report->installed_update_id != NULL)
  {
    expect_property(&reader, "installedUpdateId");
    expect_token(&reader, AZ_JSON_TOKEN_BEGIN_OBJECT);
    expect_property(&reader, "provider");
    expect_json_string(&reader, text_span(report->installed_update_id->provider));
    expect_property(&reader, "name");
    expect_json_string(&reader, text_span(report->installed_update_id->name));
    expect_property(&reader, "version");
    expect_json_string(&reader, text_span(report->installed_update_id->version));
    expect_token(&reader, AZ_JSON_TOKEN_END_OBJECT);
  }
  expect_property(&reader, "installResult");
  expect_token(&reader, AZ_JSON_TOKEN_BEGIN_OBJECT);
  const az_iot_adu_install_result* result = report->install_result;
  expect_result_fields(
      &reader,
      result->outcome,
      result->failure_origin,
      result->result_code,
      az_span_create(
          (uint8_t*)(uintptr_t)result->extended_result_codes, result->extended_result_codes_length),
      az_span_create((uint8_t*)(uintptr_t)result->result_details, result->result_details_length));
  if (report->install_result->step_results_count > 0)
  {
    expect_property(&reader, "stepResults");
    expect_token(&reader, AZ_JSON_TOKEN_BEGIN_OBJECT);
    for (int32_t i = 0; i < report->install_result->step_results_count; ++i)
    {
      char name[32];
      assert_true(snprintf(name, sizeof(name), "step_%d", (int)i) > 0);
      expect_property(&reader, name);
      expect_token(&reader, AZ_JSON_TOKEN_BEGIN_OBJECT);
      const az_iot_adu_step_result* step = &report->install_result->step_results[i];
      expect_result_fields(
          &reader,
          step->outcome,
          step->failure_origin,
          step->result_code,
          az_span_create(
              (uint8_t*)(uintptr_t)step->extended_result_codes, step->extended_result_codes_length),
          az_span_create((uint8_t*)(uintptr_t)step->result_details, step->result_details_length));
      expect_token(&reader, AZ_JSON_TOKEN_END_OBJECT);
    }
    expect_token(&reader, AZ_JSON_TOKEN_END_OBJECT);
  }
  expect_token(&reader, AZ_JSON_TOKEN_END_OBJECT);
  expect_token(&reader, AZ_JSON_TOKEN_END_OBJECT);
  assert_int_equal(az_json_reader_next_token(&reader), AZ_ERROR_JSON_READER_DONE);
}

static void serialize_and_check(const az_iot_adu_report* report)
{
  uint8_t json[32768];
  memset(json, 0xa5, sizeof(json));
  size_t length = 999;
  assert_int_equal(az_iot_adu_build_report(report, json, sizeof(json), &length), AZ_IOT_OK);
  assert_true(length > 0 && length < sizeof(json));
  expect_report(report, json, length);
}

static void expect_invalid(const az_iot_adu_report* report)
{
  uint8_t json[256];
  memset(json, 0xa5, sizeof(json));
  size_t length = 999;
  assert_int_equal(
      az_iot_adu_build_report(report, json, sizeof(json), &length), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(length, 0);
  assert_int_equal(json[0], 0);
}

static void test_minimal_exact_shape(void** state)
{
  (void)state;
  az_iot_adu_install_result result = valid_result();
  az_iot_adu_report report = make_report("workflow", NULL, &result);
  uint8_t json[512];
  size_t length = 0;
  const char expected[] = "{\"workflowId\":\"workflow\",\"installResult\":{"
                          "\"outcome\":\"IN_PROGRESS\",\"failureOrigin\":\"NOT_APPLICABLE\","
                          "\"resultCode\":0,\"extendedResultCodes\":\"0\"}}";
  assert_int_equal(az_iot_adu_build_report(&report, json, sizeof(json), &length), AZ_IOT_OK);
  assert_int_equal(length, sizeof(expected) - 1);
  assert_memory_equal(json, expected, length);
  expect_report(&report, json, length);
  /* A zero-length detail is absent even when its owned storage contains text. */
  result.result_details[0] = 'x';
  result.result_details_length = 0;
  serialize_and_check(&report);
}

static void test_steps_and_int64_preserved(void** state)
{
  (void)state;
  const int64_t codes[] = { INT64_MIN,
                            INT64_MAX,
                            INT64_C(-2147483649),
                            INT64_C(2147483648),
                            INT64_C(9007199254740993),
                            -1,
                            0,
                            1,
                            700 };
  az_iot_adu_install_result result = valid_result();
  result.step_results_count = 2;
  result.step_results[0] = install_fields(&result);
  result.step_results[1] = install_fields(&result);
  result.step_results[0].outcome = AZ_IOT_ADU_OUTCOME_FAILED;
  result.step_results[0].failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_EXTENSION;
  set_codes(&result.step_results[0], AZ_SPAN_FROM_STR("aB,00000000FFFFFFFF"));
  set_details(&result.step_results[0], AZ_SPAN_FROM_STR("step failed"));
  result.step_results[1].outcome = AZ_IOT_ADU_OUTCOME_SUCCEEDED;
  const az_iot_adu_report_update_id installed = { "provider", "name", "1.0" };
  az_iot_adu_report report = make_report("workflow", &installed, &result);
  for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
  {
    result.result_code = codes[i];
    result.step_results[0].result_code = codes[i];
    result.step_results[1].result_code = codes[sizeof(codes) / sizeof(codes[0]) - 1 - i];
    serialize_and_check(&report);
  }
}

static void test_outcomes_and_failure_origins(void** state)
{
  (void)state;
  az_iot_adu_install_result result = valid_result();
  result.step_results_count = 1;
  az_iot_adu_report report = make_report("workflow", NULL, &result);
  for (size_t outcome = 0; outcome < sizeof(outcomes) / sizeof(outcomes[0]); ++outcome)
  {
    for (size_t origin = 0; origin < sizeof(origins) / sizeof(origins[0]); ++origin)
    {
      result.outcome = (az_iot_adu_outcome)outcome;
      result.failure_origin = (az_iot_adu_failure_origin)origin;
      result.step_results[0] = install_fields(&result);
      if ((outcome == AZ_IOT_ADU_OUTCOME_FAILED)
          != (origin == AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE))
      {
        assert_int_equal(az_iot_adu__validate_install_result(&result), AZ_IOT_OK);
        serialize_and_check(&report);
      }
      else
      {
        expect_invalid(&report);
      }
    }
  }
}

static void test_invalid_enums_and_counts(void** state)
{
  (void)state;
  az_iot_adu_install_result result = valid_result();
  az_iot_adu_report report = make_report("workflow", NULL, &result);
  assert_int_equal(az_iot_adu__validate_install_result(NULL), AZ_IOT_ERR_INVALID_ARG);
  result.step_results_count = -1;
  expect_invalid(&report);
  result.step_results_count = _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS + 1;
  expect_invalid(&report);
  result.step_results_count = 1;
  result.step_results[0] = install_fields(&result);
  const int invalid_enums[] = { -1, 99 };
  for (size_t i = 0; i < sizeof(invalid_enums) / sizeof(invalid_enums[0]); ++i)
  {
    result.outcome = (az_iot_adu_outcome)invalid_enums[i];
    expect_invalid(&report);
    set_install_fields(&result, &result.step_results[0]);
    result.failure_origin = (az_iot_adu_failure_origin)invalid_enums[i];
    expect_invalid(&report);
    set_install_fields(&result, &result.step_results[0]);
    result.step_results[0].outcome = (az_iot_adu_outcome)invalid_enums[i];
    expect_invalid(&report);
    result.step_results[0] = install_fields(&result);
    result.step_results[0].failure_origin = (az_iot_adu_failure_origin)invalid_enums[i];
    expect_invalid(&report);
    result.step_results[0] = install_fields(&result);
  }
  result.step_results[0].outcome = AZ_IOT_ADU_OUTCOME_FAILED;
  expect_invalid(&report);
  result.step_results[0] = install_fields(&result);
  result.step_results[0].failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_DEVICE;
  expect_invalid(&report);

  result.step_results_count = _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS;
  for (int32_t i = 0; i < result.step_results_count; ++i)
  {
    result.step_results[i] = install_fields(&result);
  }
  serialize_and_check(&report);
}

static void test_extended_result_validation(void** state)
{
  (void)state;
  const char* valid[] = { "0",
                          "f",
                          "FFFFFFFF",
                          "aBcDeF",
                          "0,1,abCD,ffffffff",
                          "0000000000000000000000000000000",
                          "000000000ffffffff" };
  const char* invalid[]
      = { "",          ",",         ",1",    "1,",  "1,,2", "100000000", "0000000100000000",
          "FFFFFFFFF", "0x1",       "0XFF",  "-1",  "+1",   "g",         " 1",
          "1 ",        "dead beef", "1,\t2", "1\n", "1.0",  "1;2" };
  az_iot_adu_install_result result = valid_result();
  az_iot_adu_report report = make_report("workflow", NULL, &result);
  for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i)
  {
    set_install_codes(&result, text_span(valid[i]));
    serialize_and_check(&report);
  }
  result.step_results_count = 1;
  result.step_results[0] = install_fields(&result);
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
  {
    set_install_codes(&result, text_span(invalid[i]));
    expect_invalid(&report);
    set_install_codes(&result, AZ_SPAN_FROM_STR("0"));
    set_codes(&result.step_results[0], text_span(invalid[i]));
    expect_invalid(&report);
    result.step_results[0] = install_fields(&result);
  }
  uint8_t long_code[AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH + 1];
  memset(long_code, '0', sizeof(long_code));
  set_install_codes(&result, az_span_create(long_code, AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH));
  serialize_and_check(&report);
  set_install_codes(&result, AZ_SPAN_FROM_BUFFER(long_code));
  expect_invalid(&report);
  set_install_codes(&result, AZ_SPAN_EMPTY);
  expect_invalid(&report);
  uint8_t embedded_nul[] = { '1', 0, '2' };
  set_install_codes(&result, AZ_SPAN_FROM_BUFFER(embedded_nul));
  expect_invalid(&report);
}

static void test_hex_formatter(void** state)
{
  (void)state;
  const uint32_t codes[] = { 0, 1, 0xa, 0xabcdef, 0x80000000, UINT32_MAX };
  const char* expected[] = { "0", "1", "a", "abcdef", "80000000", "ffffffff" };
  az_iot_adu_step_result result = { 0 };
  for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
  {
    memset(result.extended_result_codes, 0xa5, sizeof(result.extended_result_codes));
    az_iot_adu__set_extended_result(
        AZ_SPAN_FROM_BUFFER(result.extended_result_codes),
        &result.extended_result_codes_length,
        codes[i]);
    assert_int_equal(result.extended_result_codes_length, strlen(expected[i]));
    assert_memory_equal(result.extended_result_codes, expected[i], strlen(expected[i]));
    assert_int_equal(result.extended_result_codes[result.extended_result_codes_length], 0xa5);
  }
  az_iot_adu__set_extended_result(AZ_SPAN_EMPTY, NULL, 0);
  int32_t length = 123;
  az_iot_adu__set_extended_result(AZ_SPAN_EMPTY, &length, 0);
  assert_int_equal(length, 0);
}

static void test_escaping_and_owned_text(void** state)
{
  (void)state;
  az_iot_adu_install_result result = valid_result();
  uint8_t detail[] = { '"', '\\', '\b', '\f', '\n', '\r', '\t', 0, 1, 0xc3, 0xa9 };
  set_install_details(&result, AZ_SPAN_FROM_BUFFER(detail));
  result.step_results_count = 1;
  result.step_results[0] = install_fields(&result);
  const az_iot_adu_report_update_id installed = { "p\"\\\n", "n\t\xc3\xa9", "v\r\b" };
  az_iot_adu_report report = make_report("w\"\\\n\xc3\xa9", &installed, &result);
  serialize_and_check(&report);
  detail[0] = 'x';
  serialize_and_check(&report);
  assert_int_equal(result.result_details[0], '"');
  az_iot_adu_install_result copied = result;
  memset(&result, 0, sizeof(result));
  report.install_result = &copied;
  serialize_and_check(&report);
  assert_int_equal(copied.step_results[0].result_details[0], '"');
}

static void test_detail_character_limit(void** state)
{
  (void)state;
  const uint8_t scalars[][4]
      = { { 'a' }, { 0xc3, 0xa9 }, { 0xe2, 0x82, 0xac }, { 0xf0, 0x9f, 0x98, 0x80 } };
  uint8_t detail[4 * (AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH + 1)];
  az_iot_adu_install_result result = valid_result();
  az_iot_adu_report report = make_report("workflow", NULL, &result);
  for (int32_t width = 1; width <= 4; ++width)
  {
    for (int32_t i = 0; i <= AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH; ++i)
    {
      memcpy(&detail[i * width], scalars[width - 1], (size_t)width);
    }
    set_install_details(&result, az_span_create(detail, width * AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH));
    result.step_results_count = 1;
    result.step_results[0] = install_fields(&result);
    serialize_and_check(&report);
    set_install_details(
        &result, az_span_create(detail, width * (AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH + 1)));
    expect_invalid(&report);
    set_install_details(&result, AZ_SPAN_EMPTY);
    set_details(
        &result.step_results[0],
        az_span_create(detail, width * (AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH + 1)));
    expect_invalid(&report);
  }
  /* Embedded NUL is one valid scalar and expands to six JSON bytes. */
  memset(detail, 0, AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH);
  set_install_details(&result, az_span_create(detail, AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH));
  result.step_results_count = 0;
  serialize_and_check(&report);
}

static void test_malformed_utf8(void** state)
{
  (void)state;
  const struct
  {
    uint8_t bytes[4];
    int32_t length;
  } malformed[] = { { { 0x80 }, 1 },
                    { { 0xc0, 0xaf }, 2 },
                    { { 0xc1, 0xbf }, 2 },
                    { { 0xc2 }, 1 },
                    { { 0xc2, 'a' }, 2 },
                    { { 0xe0, 0x80, 0x80 }, 3 },
                    { { 0xe2, 0x82 }, 2 },
                    { { 0xed, 0xa0, 0x80 }, 3 },
                    { { 0xed, 0xbf, 0xbf }, 3 },
                    { { 0xf0, 0x80, 0x80, 0x80 }, 4 },
                    { { 0xf0, 0x9f, 0x98 }, 3 },
                    { { 0xf4, 0x90, 0x80, 0x80 }, 4 },
                    { { 0xf5, 0x80, 0x80, 0x80 }, 4 },
                    { { 0xff }, 1 } };
  az_iot_adu_install_result result = valid_result();
  result.step_results_count = 1;
  result.step_results[0] = install_fields(&result);
  az_iot_adu_report report = make_report("workflow", NULL, &result);
  for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i)
  {
    az_span bad = az_span_create((uint8_t*)(uintptr_t)malformed[i].bytes, malformed[i].length);
    set_install_details(&result, bad);
    expect_invalid(&report);
    set_install_details(&result, AZ_SPAN_EMPTY);
    set_details(&result.step_results[0], bad);
    expect_invalid(&report);
    set_details(&result.step_results[0], AZ_SPAN_EMPTY);
  }
  uint8_t boundary_scalars[]
      = { 0x7f, 0xc2, 0x80, 0xdf, 0xbf, 0xe0, 0xa0, 0x80, 0xed, 0x9f, 0xbf, 0xee, 0x80,
          0x80, 0xef, 0xbf, 0xbf, 0xf0, 0x90, 0x80, 0x80, 0xf4, 0x8f, 0xbf, 0xbf };
  set_install_details(&result, AZ_SPAN_FROM_BUFFER(boundary_scalars));
  serialize_and_check(&report);
  report.workflow_id = "\xc0\xaf";
  expect_invalid(&report);
  report.workflow_id = "workflow";
  const az_iot_adu_report_update_id bad_installed = { "\xff", "name", "1" };
  report.installed_update_id = &bad_installed;
  expect_invalid(&report);
}

static void test_invalid_owned_lengths(void** state)
{
  (void)state;
  az_iot_adu_install_result result = valid_result();
  az_iot_adu_report report = make_report("workflow", NULL, &result);
  result.step_results_count = 1;
  result.step_results[0] = install_fields(&result);
  const int32_t invalid_codes[] = { -1, 0, AZ_IOT_ADU_RESULT_TEXT_MAX_LENGTH + 1, INT32_MAX };
  const int32_t invalid_details[] = { -1, AZ_IOT_ADU_RESULT_DETAILS_MAX_SIZE + 1, INT32_MAX };
  for (size_t i = 0; i < sizeof(invalid_codes) / sizeof(invalid_codes[0]); ++i)
  {
    result.extended_result_codes_length = invalid_codes[i];
    expect_invalid(&report);
    result.extended_result_codes_length = 1;
    result.step_results[0].extended_result_codes_length = invalid_codes[i];
    expect_invalid(&report);
    result.step_results[0].extended_result_codes_length = 1;
  }
  for (size_t i = 0; i < sizeof(invalid_details) / sizeof(invalid_details[0]); ++i)
  {
    result.result_details_length = invalid_details[i];
    expect_invalid(&report);
    result.result_details_length = 0;
    result.step_results[0].result_details_length = invalid_details[i];
    expect_invalid(&report);
    result.step_results[0].result_details_length = 0;
  }
}

/* Both caller-allocated structs carry a size stamp (docs/struct_versioning.md).
 * Zero means the _INIT macro was skipped; any other size is a header from another
 * SDK version. Neither is serialized, and neither leaves partial output. */
static void test_size_stamps(void** state)
{
  (void)state;
  const az_iot_adu_report stamped_report = AZ_IOT_ADU_REPORT_INIT;
  const az_iot_adu_install_result stamped_result = AZ_IOT_ADU_INSTALL_RESULT_INIT;
  assert_int_equal(stamped_report._internal_size, sizeof(az_iot_adu_report));
  assert_int_equal(stamped_result._internal_size, sizeof(az_iot_adu_install_result));

  az_iot_adu_install_result result = valid_result();
  az_iot_adu_report report = make_report("workflow", NULL, &result);
  uint8_t json[512];
  size_t length = 0;
  assert_int_equal(az_iot_adu_build_report(&report, json, sizeof(json), &length), AZ_IOT_OK);

  const struct
  {
    uint32_t report_size;
    uint32_t result_size;
    az_iot_result expected;
  } cases[] = {
    { 0, sizeof(az_iot_adu_install_result), AZ_IOT_ERR_INVALID_ARG },
    { sizeof(az_iot_adu_report), 0, AZ_IOT_ERR_INVALID_ARG },
    /* An unsupported report stamp is refused before install_result is read. */
    { sizeof(az_iot_adu_report) - 1, 0, AZ_IOT_ERR_NOT_SUPPORTED },
    { sizeof(az_iot_adu_report) - 1, sizeof(az_iot_adu_install_result), AZ_IOT_ERR_NOT_SUPPORTED },
    { sizeof(az_iot_adu_report) + 8, sizeof(az_iot_adu_install_result), AZ_IOT_ERR_NOT_SUPPORTED },
    { sizeof(az_iot_adu_report), sizeof(az_iot_adu_install_result) - 1, AZ_IOT_ERR_NOT_SUPPORTED },
    { sizeof(az_iot_adu_report), sizeof(az_iot_adu_install_result) + 8, AZ_IOT_ERR_NOT_SUPPORTED },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    report._internal_size = cases[i].report_size;
    result._internal_size = cases[i].result_size;
    length = 999;
    json[0] = 'x';
    assert_int_equal(
        az_iot_adu_build_report(&report, json, sizeof(json), &length), cases[i].expected);
    assert_int_equal(length, 0);
    assert_int_equal(json[0], 0);
  }
}

static void test_invalid_arguments(void** state)
{
  (void)state;
  az_iot_adu_install_result result = valid_result();
  az_iot_adu_report report = make_report("workflow", NULL, &result);
  uint8_t json[512];
  size_t length = 999;
  expect_invalid(NULL);
  assert_int_equal(
      az_iot_adu_build_report(&report, NULL, sizeof(json), &length), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(length, 0);
  length = 999;
  assert_int_equal(az_iot_adu_build_report(&report, json, 0, &length), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(length, 0);
  length = 999;
  assert_int_equal(
      az_iot_adu_build_report(&report, json, (size_t)INT32_MAX + 1, &length),
      AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(length, 0);
  report.install_result = NULL;
  expect_invalid(&report);
  report.install_result = &result;
  report.workflow_id = NULL;
  expect_invalid(&report);
  report.workflow_id = "";
  expect_invalid(&report);
  report.workflow_id = "workflow";
  az_iot_adu_report_update_id installed = { NULL, "name", "1" };
  report.installed_update_id = &installed;
  expect_invalid(&report);
  installed.provider = "provider";
  installed.name = NULL;
  expect_invalid(&report);
  installed.name = "name";
  installed.version = NULL;
  expect_invalid(&report);
  installed.version = "1";
  assert_int_equal(az_iot_adu_build_report(&report, json, sizeof(json), NULL), AZ_IOT_OK);
  assert_int_equal(az_iot_adu_build_report(NULL, json, sizeof(json), NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(json[0], 0);
}

static void test_small_buffers_never_succeed_partially(void** state)
{
  (void)state;
  az_iot_adu_install_result result = valid_result();
  result.step_results_count = 2;
  result.step_results[0] = install_fields(&result);
  result.step_results[1] = install_fields(&result);
  result.step_results[0].result_code = INT64_MIN;
  result.step_results[1].result_code = INT64_MAX;
  set_install_details(&result, AZ_SPAN_FROM_STR("\"\\\n"));
  const az_iot_adu_report_update_id installed = { "provider", "name", "1.0" };
  az_iot_adu_report report = make_report("workflow", &installed, &result);
  uint8_t json[2048];
  size_t full_length = 0;
  assert_int_equal(az_iot_adu_build_report(&report, json, sizeof(json), &full_length), AZ_IOT_OK);
  bool succeeded = false;
  for (size_t capacity = 1; capacity <= full_length + 64; ++capacity)
  {
    memset(json, 0xa5, sizeof(json));
    size_t length = 999;
    az_iot_result status = az_iot_adu_build_report(&report, json + 1, capacity, &length);
    assert_int_equal(json[0], 0xa5);
    assert_int_equal(json[capacity + 1], 0xa5);
    if (status == AZ_IOT_OK)
    {
      assert_true(capacity >= full_length);
      assert_int_equal(length, full_length);
      expect_report(&report, json + 1, length);
      succeeded = true;
    }
    else
    {
      assert_int_equal(status, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
      assert_int_equal(length, 0);
      assert_int_equal(json[1], 0);
    }
  }
  assert_true(succeeded);
}

typedef struct report_capture
{
  const az_iot_adu_install_result* expected_result;
  const az_iot_adu_report_update_id* expected_installed;
  const char* expected_workflow;
  az_iot_result returned_status;
  int calls;
} report_capture;

static az_iot_result capture_report(void* ctx, const az_iot_adu_report* report)
{
  report_capture* capture = ctx;
  ++capture->calls;
  assert_ptr_equal(report->install_result, capture->expected_result);
  assert_string_equal(report->workflow_id, capture->expected_workflow);
  if (capture->expected_installed == NULL)
  {
    assert_null(report->installed_update_id);
  }
  else
  {
    assert_non_null(report->installed_update_id);
    assert_string_equal(
        report->installed_update_id->provider, capture->expected_installed->provider);
    assert_string_equal(report->installed_update_id->name, capture->expected_installed->name);
    assert_string_equal(report->installed_update_id->version, capture->expected_installed->version);
  }
  serialize_and_check(report);
  return capture->returned_status;
}

static const az_iot_adu_channel_vtable report_channel = {
  .open = NULL,
  .close = NULL,
  .request_update = NULL,
  .report = capture_report,
  .set_device_properties = NULL,
  .do_work = NULL,
};

static void test_report_state_preserves_canonical_result(void** state)
{
  (void)state;
  az_iot_adu_client_t client = { 0 };
  client._internal.install_result = valid_result();
  client._internal.install_result.result_code = INT64_MIN;
  set_install_codes(&client._internal.install_result, AZ_SPAN_FROM_STR("AB,000000ff"));
  set_install_details(&client._internal.install_result, AZ_SPAN_FROM_STR("owned diagnostics"));
  client._internal.install_result.step_results_count = 1;
  client._internal.install_result.step_results[0]
      = install_fields(&client._internal.install_result);
  client._internal.install_result.step_results[0].outcome = AZ_IOT_ADU_OUTCOME_FAILED;
  client._internal.install_result.step_results[0].failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_DEVICE;
  client._internal.install_result.step_results[0].result_code = INT64_MAX;
  client._internal.active_workflow_valid = true;
  memcpy(client._internal.active_workflow_id, "workflow", 8);
  client._internal.active_workflow_id_len = 8;
  az_iot_adu_device_properties cached = { 0 };
  cached.installed_update_id.provider = "old-provider";
  cached.installed_update_id.name = "old-name";
  cached.installed_update_id.version = "old-version";
  client._internal.device_props_buffer = (uint8_t*)&cached;
  const az_iot_adu_report_update_id old = { "old-provider", "old-name", "old-version" };
  client._internal.applied_update_id.provider = "new-provider";
  client._internal.applied_update_id.name = "new-name";
  client._internal.applied_update_id.version = "new-version";
  client._internal.applied_update_id_valid = true;
  report_capture capture = { &client._internal.install_result, &old, "workflow", AZ_IOT_OK, 0 };
  client._internal.channel.vtable = &report_channel;
  client._internal.channel.ctx = &capture;
  const az_iot_adu_state states[]
      = { AZ_IOT_ADU_STATE_IDLE, AZ_IOT_ADU_STATE_FAILED, AZ_IOT_ADU_STATE_DOWNLOAD_STARTED };
  for (size_t outcome = 0; outcome < sizeof(outcomes) / sizeof(outcomes[0]); ++outcome)
  {
    client._internal.install_result.outcome = (az_iot_adu_outcome)outcome;
    client._internal.install_result.failure_origin = outcome == AZ_IOT_ADU_OUTCOME_FAILED
        ? AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_DEPENDENCY
        : AZ_IOT_ADU_FAILURE_ORIGIN_NOT_APPLICABLE;
    capture.expected_installed
        = outcome == AZ_IOT_ADU_OUTCOME_SUCCEEDED ? &client._internal.applied_update_id : &old;
    az_iot_adu_install_result unchanged = client._internal.install_result;
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i)
    {
      client._internal.state = states[i];
      assert_int_equal(az_iot_adu__report_state(&client), AZ_IOT_OK);
      assert_memory_equal(&client._internal.install_result, &unchanged, sizeof(unchanged));
    }
  }
  assert_int_equal(capture.calls, 15);
  client._internal.install_result.outcome = AZ_IOT_ADU_OUTCOME_SUCCEEDED;
  client._internal.applied_update_id_valid = false;
  capture.expected_installed = &old;
  capture.returned_status = AZ_IOT_ERR_BUSY;
  assert_int_equal(az_iot_adu__report_state(&client), AZ_IOT_ERR_BUSY);
  client._internal.device_props_buffer = NULL;
  capture.expected_installed = NULL;
  assert_int_equal(az_iot_adu__report_state(&client), AZ_IOT_ERR_BUSY);
  client._internal.device_props_buffer = (uint8_t*)&cached;
  cached.installed_update_id.name = NULL;
  assert_int_equal(az_iot_adu__report_state(&client), AZ_IOT_ERR_BUSY);
}

static void test_report_state_no_workflow_and_invalid_inputs(void** state)
{
  (void)state;
  az_iot_adu_client_t client = { 0 };
  assert_int_equal(az_iot_adu__report_state(NULL), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(az_iot_adu__report_state(&client), AZ_IOT_OK);
  client._internal.active_workflow_valid = true;
  assert_int_equal(az_iot_adu__report_state(&client), AZ_IOT_OK);
  client._internal.active_workflow_id_len = 1;
  assert_int_equal(az_iot_adu__report_state(&client), AZ_IOT_ERR_INVALID_ARG);
  az_iot_adu_channel_vtable empty_channel = { 0 };
  client._internal.channel.vtable = &empty_channel;
  assert_int_equal(az_iot_adu__report_state(&client), AZ_IOT_ERR_INVALID_ARG);
  client._internal.channel.vtable = &report_channel;
  client._internal.active_workflow_id_len = AZ_IOT_ADU_WORKFLOW_ID_SIZE + 1;
  assert_int_equal(az_iot_adu__report_state(&client), AZ_IOT_ERR_INVALID_ARG);
  client._internal.detached = true;
  assert_int_equal(az_iot_adu__report_state(&client), AZ_IOT_ERR_DETACHED);
}

/* The largest report the engine produces fits the channel body: engine results
 * carry no resultDetails and one 8-hex extended code per result. */
static void test_largest_engine_report_fits_channel_body(void** state)
{
  (void)state;
  char workflow_id[AZ_IOT_ADU_MAX_WORKFLOW_ID_LEN];
  memset(workflow_id, 'w', sizeof(workflow_id) - 1);
  workflow_id[sizeof(workflow_id) - 1] = '\0';
  /* A 192-byte installed ID (applied-ID buffer size), every byte escaped. */
  char id[3][64];
  for (int i = 0; i < 3; ++i)
  {
    memset(id[i], 0x01, sizeof(id[i]) - 1);
    id[i][sizeof(id[i]) - 1] = '\0';
  }
  const az_iot_adu_report_update_id installed = { id[0], id[1], id[2] };

  az_iot_adu_install_result result = valid_result();
  result.outcome = AZ_IOT_ADU_OUTCOME_FAILED;
  result.failure_origin = AZ_IOT_ADU_FAILURE_ORIGIN_AGENT_CORE;
  result.result_code = INT64_MIN;
  set_install_codes(&result, AZ_SPAN_FROM_STR("ffffffff"));
  result.step_results_count = _az_IOT_ADU_CLIENT_MAX_INSTRUCTIONS_STEPS;
  for (int32_t i = 0; i < result.step_results_count; ++i)
  {
    result.step_results[i] = install_fields(&result);
  }
  az_iot_adu_report report = make_report(workflow_id, &installed, &result);

  uint8_t body[AZ_IOT_ADU_CHANNEL_BODY_MAX_SIZE];
  size_t length = 0;
  assert_int_equal(az_iot_adu_build_report(&report, body, sizeof(body), &length), AZ_IOT_OK);
  assert_true(length > 0 && length <= sizeof(body));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_minimal_exact_shape),
    cmocka_unit_test(test_steps_and_int64_preserved),
    cmocka_unit_test(test_outcomes_and_failure_origins),
    cmocka_unit_test(test_invalid_enums_and_counts),
    cmocka_unit_test(test_extended_result_validation),
    cmocka_unit_test(test_hex_formatter),
    cmocka_unit_test(test_escaping_and_owned_text),
    cmocka_unit_test(test_detail_character_limit),
    cmocka_unit_test(test_malformed_utf8),
    cmocka_unit_test(test_invalid_owned_lengths),
    cmocka_unit_test(test_size_stamps),
    cmocka_unit_test(test_largest_engine_report_fits_channel_body),
    cmocka_unit_test(test_invalid_arguments),
    cmocka_unit_test(test_small_buffers_never_succeed_partially),
    cmocka_unit_test(test_report_state_preserves_canonical_result),
    cmocka_unit_test(test_report_state_no_workflow_and_invalid_inputs),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
