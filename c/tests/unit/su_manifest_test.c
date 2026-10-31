// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Unit tests for az_iot_su__parse_update_manifest(): every field, unknown members skipped at
 * every level, delta file members, wrong types rejected, and the AZ_IOT_SU_MAX_* limits. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "azure/iot/az_iot_log.h"
#include "internal/su_manifest_internal.h"

/** @brief Output of the last parse(). */
static az_iot_su_client_update_manifest g_manifest;

/** @brief Parse @p text into g_manifest. */
static az_iot_result parse(const char* text)
{
  return az_iot_su__parse_update_manifest(
      az_span_create((uint8_t*)(uintptr_t)text, (int32_t)strlen(text)), &g_manifest);
}

/** @brief Assert @p actual holds @p expected. */
static void assert_span_equal(az_span actual, const char* expected)
{
  assert_true(az_span_is_content_equal(
      actual, az_span_create((uint8_t*)(uintptr_t)expected, (int32_t)strlen(expected))));
}

static void every_field_is_parsed(void** state)
{
  (void)state;
  assert_int_equal(
      parse("{\"manifestVersion\":\"5\","
            "\"updateId\":{\"provider\":\"Contoso\",\"name\":\"Toaster\",\"version\":\"1.2\"},"
            "\"compatibility\":[{\"deviceManufacturer\":\"Contoso\"}],"
            "\"instructions\":{\"steps\":["
            "{\"handler\":\"microsoft/swupdate:2\",\"files\":[\"f1\",\"f2\"],"
            "\"handlerProperties\":{\"installedCriteria\":\"1.2\"}},"
            "{\"handler\":\"microsoft/script:1\",\"files\":[\"f2\"]}]},"
            "\"files\":{"
            "\"f1\":{\"fileName\":\"a.swu\",\"sizeInBytes\":4096,"
            "\"hashes\":{\"sha256\":\"AAA=\",\"sha384\":\"BBB=\"}},"
            "\"f2\":{\"fileName\":\"b.sh\",\"sizeInBytes\":7,\"mimeType\":\"text/x-sh\","
            "\"hashes\":{\"sha256\":\"CCC=\"}}},"
            "\"createdDateTime\":\"2022-07-07T03:02:48.8449038Z\"}"),
      AZ_IOT_OK);

  const az_iot_su_client_update_manifest* m = &g_manifest;
  assert_span_equal(m->manifest_version, "5");
  assert_span_equal(m->update_id.provider, "Contoso");
  assert_span_equal(m->update_id.name, "Toaster");
  assert_span_equal(m->update_id.version, "1.2");
  assert_int_equal(m->instructions.steps_count, 2);
  assert_span_equal(m->instructions.steps[0].handler, "microsoft/swupdate:2");
  assert_int_equal(m->instructions.steps[0].files_count, 2);
  assert_span_equal(m->instructions.steps[0].files[0], "f1");
  assert_span_equal(m->instructions.steps[0].files[1], "f2");
  assert_span_equal(m->instructions.steps[0].handler_properties.installed_criteria, "1.2");
  assert_span_equal(m->instructions.steps[1].handler, "microsoft/script:1");
  assert_int_equal(m->instructions.steps[1].files_count, 1);
  assert_int_equal(az_span_size(m->instructions.steps[1].handler_properties.installed_criteria), 0);
  assert_int_equal(m->files_count, 2);
  assert_span_equal(m->files[0].id, "f1");
  assert_span_equal(m->files[0].file_name, "a.swu");
  assert_int_equal(m->files[0].size_in_bytes, 4096);
  assert_int_equal(m->files[0].hashes_count, 2);
  assert_span_equal(m->files[0].hashes[0].hash_type, "sha256");
  assert_span_equal(m->files[0].hashes[0].hash_value, "AAA=");
  assert_span_equal(m->files[0].hashes[1].hash_type, "sha384");
  assert_span_equal(m->files[0].hashes[1].hash_value, "BBB=");
  assert_span_equal(m->files[1].id, "f2");
  assert_int_equal(m->files[1].size_in_bytes, 7);
  assert_int_equal(m->files[1].hashes_count, 1);
  assert_span_equal(m->create_date_time, "2022-07-07T03:02:48.8449038Z");
}

/* Members the parser does not know are skipped, whatever their type, at every level; whitespace
 * after the root object is allowed. */
static void unknown_members_are_skipped_at_every_level(void** state)
{
  (void)state;
  assert_int_equal(
      parse("{\"future\":{\"a\":[1,{\"b\":null}]},\"manifestVersion\":\"5\","
            "\"updateId\":{\"provider\":\"P\",\"extra\":[true],\"name\":\"N\",\"version\":\"1\"},"
            "\"instructions\":{\"extra\":{\"x\":1},\"steps\":["
            "{\"type\":\"inline\",\"handler\":\"h\",\"description\":\"d\",\"files\":[\"f\"],"
            "\"handlerProperties\":{\"scriptFileName\":\"s.sh\",\"arguments\":\"--x\","
            "\"installedCriteria\":\"1\",\"nested\":{\"y\":[]}}}]},"
            "\"files\":{\"f\":{\"properties\":{\"k\":\"v\"},\"fileName\":\"f.bin\","
            "\"sizeInBytes\":1,\"hashes\":{\"sha256\":\"h\"},\"future\":[1,2]}},"
            "\"createdDateTime\":\"t\",\"trailing\":[]} \r\n\t"),
      AZ_IOT_OK);
  assert_span_equal(g_manifest.update_id.name, "N");
  assert_int_equal(g_manifest.instructions.steps_count, 1);
  assert_span_equal(g_manifest.instructions.steps[0].handler_properties.installed_criteria, "1");
  assert_int_equal(g_manifest.files_count, 1);
  assert_span_equal(g_manifest.files[0].file_name, "f.bin");
  assert_int_equal(g_manifest.files[0].hashes_count, 1);
  assert_span_equal(g_manifest.create_date_time, "t");
}

/* A delta update's file carries relatedFiles, downloadHandler and properties: the manifest
 * parses, and the related files' hashes stay out of the file's hashes. */
static void delta_file_members_are_accepted(void** state)
{
  (void)state;
  assert_int_equal(
      parse("{\"manifestVersion\":\"5\",\"updateId\":{\"provider\":\"P\",\"name\":\"N\","
            "\"version\":\"2\"},\"instructions\":{\"steps\":[{\"handler\":\"microsoft/swupdate:2\","
            "\"files\":[\"full\"]}]},\"files\":{\"full\":{\"fileName\":\"full.swu\","
            "\"sizeInBytes\":100,\"hashes\":{\"sha256\":\"FULL\"},"
            "\"relatedFiles\":{\"delta\":{\"fileName\":\"d.delta\",\"sizeInBytes\":10,"
            "\"hashes\":{\"sha256\":\"D1\",\"sha384\":\"D2\",\"sha512\":\"D3\"},"
            "\"properties\":{\"microsoft.sourceFileHashAlgorithm\":\"sha256\","
            "\"microsoft.sourceFileHash\":\"SRC\"}}},"
            "\"downloadHandler\":{\"id\":\"microsoft/delta:1\"},"
            "\"properties\":{\"k\":\"v\"}}},\"createdDateTime\":\"t\"}"),
      AZ_IOT_OK);
  assert_int_equal(g_manifest.files_count, 1);
  assert_span_equal(g_manifest.files[0].file_name, "full.swu");
  assert_int_equal(g_manifest.files[0].size_in_bytes, 100);
  assert_int_equal(g_manifest.files[0].hashes_count, 1);
  assert_span_equal(g_manifest.files[0].hashes[0].hash_value, "FULL");
}

/* A known member of the wrong type, malformed JSON or data after the root object is refused. */
static void malformed_manifests_are_rejected(void** state)
{
  (void)state;
  static const char* const manifests[] = {
    "",
    "[]",
    "{",
    "{\"manifestVersion\":5}",
    "{\"updateId\":[]}",
    "{\"updateId\":{\"provider\":1}}",
    "{\"updateId\":{\"name\":null}}",
    "{\"updateId\":{\"version\":{}}}",
    "{\"instructions\":[]}",
    "{\"instructions\":{\"steps\":{}}}",
    "{\"instructions\":{\"steps\":[1]}}",
    "{\"instructions\":{\"steps\":[{\"handler\":1}]}}",
    "{\"instructions\":{\"steps\":[{\"files\":\"f\"}]}}",
    "{\"instructions\":{\"steps\":[{\"files\":[1]}]}}",
    "{\"instructions\":{\"steps\":[{\"handlerProperties\":\"x\"}]}}",
    "{\"instructions\":{\"steps\":[{\"handlerProperties\":{\"installedCriteria\":1}}]}}",
    "{\"files\":[]}",
    "{\"files\":{\"f\":\"x\"}}",
    "{\"files\":{\"f\":{\"fileName\":1}}}",
    "{\"files\":{\"f\":{\"sizeInBytes\":\"1\"}}}",
    "{\"files\":{\"f\":{\"sizeInBytes\":1.5}}}",
    "{\"files\":{\"f\":{\"hashes\":[]}}}",
    "{\"files\":{\"f\":{\"hashes\":{\"sha256\":1}}}}",
    "{\"files\":{\"f\":{\"mimeType\":{\"hashes\":{\"sha256\":\"a\"}}}}}",
    "{\"createdDateTime\":1}",
    "{\"files\":{\"f\":{\"fileName\":\"x\"}}",
    "{}x",
    "{}}",
    "{}{}",
    "{} 1",
    "{\"manifestVersion\":\"5\"},",
  };
  for (size_t i = 0; i < sizeof(manifests) / sizeof(manifests[0]); ++i)
  {
    if (parse(manifests[i]) != AZ_IOT_ERR_INVALID_ARG)
    {
      fail_msg("manifest %u not rejected: %s", (unsigned)i, manifests[i]);
    }
  }
}

/**
 * @brief Build a manifest: step 0 lists @p first_step_file_count files, the others one; each file
 * has @p hash_count hashes; @p extra_step, when not NULL, is appended as one more step.
 *
 * @return A static buffer, valid until the next call.
 */
static const char* build_manifest(
    int step_count,
    int file_count,
    int first_step_file_count,
    int hash_count,
    const char* extra_step)
{
  static char text[16384];
  int used = snprintf(text, sizeof(text), "{\"instructions\":{\"steps\":[");
  for (int step = 0; step < step_count; ++step)
  {
    used += snprintf(
        text + used,
        sizeof(text) - (size_t)used,
        "%s{\"handler\":\"h\",\"files\":[",
        step == 0 ? "" : ",");
    int listed = step == 0 ? first_step_file_count : 1;
    for (int i = 0; i < listed; ++i)
    {
      used += snprintf(text + used, sizeof(text) - (size_t)used, "%s\"f%d\"", i == 0 ? "" : ",", i);
    }
    used += snprintf(text + used, sizeof(text) - (size_t)used, "]}");
  }
  if (extra_step != NULL)
  {
    used += snprintf(text + used, sizeof(text) - (size_t)used, ",%s", extra_step);
  }
  used += snprintf(text + used, sizeof(text) - (size_t)used, "]},\"files\":{");
  for (int file = 0; file < file_count; ++file)
  {
    used += snprintf(
        text + used,
        sizeof(text) - (size_t)used,
        "%s\"f%d\":{\"fileName\":\"n\",\"sizeInBytes\":1,\"hashes\":{",
        file == 0 ? "" : ",",
        file);
    for (int hash = 0; hash < hash_count; ++hash)
    {
      used += snprintf(
          text + used, sizeof(text) - (size_t)used, "%s\"h%d\":\"v\"", hash == 0 ? "" : ",", hash);
    }
    used += snprintf(text + used, sizeof(text) - (size_t)used, "}}");
  }
  used += snprintf(text + used, sizeof(text) - (size_t)used, "},\"createdDateTime\":\"end\"}");
  assert_true(used > 0 && (size_t)used < sizeof(text));
  return text;
}

static void a_manifest_at_the_limits_is_parsed(void** state)
{
  (void)state;
  assert_int_equal(
      parse(build_manifest(
          AZ_IOT_SU_MAX_STEP_COUNT,
          AZ_IOT_SU_MAX_FILE_COUNT,
          AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP,
          AZ_IOT_SU_MAX_FILE_HASH_COUNT,
          NULL)),
      AZ_IOT_OK);
  assert_int_equal(g_manifest.instructions.steps_count, AZ_IOT_SU_MAX_STEP_COUNT);
  assert_int_equal(g_manifest.instructions.steps[0].files_count, AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP);
  assert_int_equal(g_manifest.files_count, AZ_IOT_SU_MAX_FILE_COUNT);
  assert_int_equal(
      g_manifest.files[AZ_IOT_SU_MAX_FILE_COUNT - 1].hashes_count, AZ_IOT_SU_MAX_FILE_HASH_COUNT);
  assert_span_equal(g_manifest.create_date_time, "end");
}

/** @brief Counts log messages containing a needle. */
typedef struct
{
  const char* needle;
  int count;
} log_capture;

/** @brief Log sink counting messages that contain the capture's needle. */
static void capture_log(
    void* user_ctx,
    az_iot_log_level level,
    const char* component,
    const char* file,
    int line,
    const char* message)
{
  (void)level;
  (void)component;
  (void)file;
  (void)line;
  log_capture* capture = (log_capture*)user_ctx;
  capture->count += strstr(message, capture->needle) != NULL ? 1 : 0;
}

/* One over any limit is refused with AZ_IOT_ERR_NOT_ENOUGH_SPACE and logged; the stored
 * counts stay within the arrays. */
static void a_manifest_over_a_limit_is_refused(void** state)
{
  (void)state;
  const struct
  {
    int steps;
    int files;
    int first_step_files;
    int hashes;
    const char* logged;
  } cases[] = {
    { AZ_IOT_SU_MAX_STEP_COUNT + 1, 1, 1, 1, "AZ_IOT_SU_MAX_STEP_COUNT" },
    { 1, AZ_IOT_SU_MAX_FILE_COUNT + 1, 1, 1, "AZ_IOT_SU_MAX_FILE_COUNT is" },
    { 1, 1, AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP + 1, 1, "AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP" },
    { 1, 1, 1, AZ_IOT_SU_MAX_FILE_HASH_COUNT + 1, "AZ_IOT_SU_MAX_FILE_HASH_COUNT" },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    log_capture capture = { .needle = cases[i].logged, .count = 0 };
    az_iot_log_sink sink
        = { .sink = capture_log, .user_ctx = &capture, .min_level = AZ_IOT_LOG_LEVEL_ERROR };
    az_iot_log_set_global_sink(&sink);
    az_iot_result result = parse(build_manifest(
        cases[i].steps, cases[i].files, cases[i].first_step_files, cases[i].hashes, NULL));
    az_iot_log_set_global_sink(NULL);

    assert_int_equal(result, AZ_IOT_ERR_NOT_ENOUGH_SPACE);
    assert_int_equal(capture.count, 1);
    assert_true(g_manifest.instructions.steps_count <= AZ_IOT_SU_MAX_STEP_COUNT);
    assert_true(g_manifest.instructions.steps[0].files_count <= AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP);
    assert_true(g_manifest.files_count <= AZ_IOT_SU_MAX_FILE_COUNT);
    assert_true(g_manifest.files[0].hashes_count <= AZ_IOT_SU_MAX_FILE_HASH_COUNT);
  }
}

/* Entries past a limit are still checked: a malformed one makes the manifest malformed. */
static void entries_past_a_limit_are_still_checked(void** state)
{
  (void)state;
  assert_int_equal(
      parse(build_manifest(AZ_IOT_SU_MAX_STEP_COUNT, 1, 1, 1, "{\"handler\":\"h\",\"files\":[1]}")),
      AZ_IOT_ERR_INVALID_ARG);
}

/** @brief az_iot_su__check_step_file_references() with @p url_ids as the request's file ids. */
static az_iot_result check_references(
    const char* manifest_text,
    uint32_t first_step,
    const char* const* url_ids,
    uint32_t url_count)
{
  assert_int_equal(parse(manifest_text), AZ_IOT_OK);
  az_iot_su_client_file_url urls[4];
  assert_true(url_count <= sizeof(urls) / sizeof(urls[0]));
  for (uint32_t i = 0; i < url_count; ++i)
  {
    urls[i].id = az_span_create((uint8_t*)(uintptr_t)url_ids[i], (int32_t)strlen(url_ids[i]));
    urls[i].url = AZ_SPAN_FROM_STR("http://example.com/file");
  }
  return az_iot_su__check_step_file_references(&g_manifest, first_step, urls, url_count);
}

/* Every step file must be in the manifest `files` and in the request URLs; ids match exactly. */
static void step_files_must_be_described_and_have_urls(void** state)
{
  (void)state;
  const char* const both[] = { "f1", "f2" };
  const char* two_steps
      = "{\"instructions\":{\"steps\":[{\"files\":[\"f1\"]},{\"files\":[\"f2\",\"f1\"]}]},"
        "\"files\":{\"f1\":{},\"f2\":{}}}";
  const char* one_file
      = "{\"instructions\":{\"steps\":[{\"files\":[\"f1\"]}]},\"files\":{\"f1\":{}}}";

  assert_int_equal(check_references(two_steps, 0, both, 2), AZ_IOT_OK);

  /* A file used by several steps needs one URL. */
  const char* const only_f1[] = { "f1" };
  assert_int_equal(
      check_references(
          "{\"instructions\":{\"steps\":[{\"files\":[\"f1\"]},{\"files\":[\"f1\"]}]},"
          "\"files\":{\"f1\":{}}}",
          0,
          only_f1,
          1),
      AZ_IOT_OK);

  /* A step file missing from the URLs. */
  assert_int_equal(check_references(two_steps, 0, only_f1, 1), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(check_references(two_steps, 0, NULL, 0), AZ_IOT_ERR_INVALID_ARG);

  /* A step file missing from `files`, though it has a URL. */
  assert_int_equal(
      check_references(
          "{\"instructions\":{\"steps\":[{\"files\":[\"f1\"]},{\"files\":[\"f2\"]}]},"
          "\"files\":{\"f1\":{}}}",
          0,
          both,
          2),
      AZ_IOT_ERR_INVALID_ARG);

  /* Ids are compared whole: "f1" is neither "f10" nor "F1". */
  const char* const longer[] = { "f10" };
  const char* const other_case[] = { "F1" };
  assert_int_equal(check_references(one_file, 0, longer, 1), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(check_references(one_file, 0, other_case, 1), AZ_IOT_ERR_INVALID_ARG);

  /* Ids are compared decoded: `f\u0031` is "f1" in a step, a `files` key, or both. */
  assert_int_equal(
      check_references(
          "{\"instructions\":{\"steps\":[{\"files\":[\"f\\u0031\"]}]},\"files\":{\"f1\":{}}}",
          0,
          only_f1,
          1),
      AZ_IOT_OK);
  assert_int_equal(
      check_references(
          "{\"instructions\":{\"steps\":[{\"files\":[\"f1\"]}]},\"files\":{\"f\\u0031\":{}}}",
          0,
          only_f1,
          1),
      AZ_IOT_OK);
  assert_int_equal(
      check_references(
          "{\"instructions\":{\"steps\":[{\"files\":[\"\\u0066\\u0031\"]}]},"
          "\"files\":{\"f\\u0031\":{}}}",
          0,
          only_f1,
          1),
      AZ_IOT_OK);
  assert_int_equal(
      check_references(
          "{\"instructions\":{\"steps\":[{\"files\":[\"f\\u0032\"]}]},\"files\":{\"f1\":{}}}",
          0,
          both,
          2),
      AZ_IOT_ERR_INVALID_ARG);
}

/* Steps before first_step are not checked; steps without files, or none at all, always pass. */
static void only_steps_from_first_step_are_checked(void** state)
{
  (void)state;
  const char* const only_f2[] = { "f2" };
  const char* unresolved_first
      = "{\"instructions\":{\"steps\":[{\"files\":[\"f1\"]},{\"files\":[\"f2\"]}]},"
        "\"files\":{\"f2\":{}}}";

  assert_int_equal(check_references(unresolved_first, 0, only_f2, 1), AZ_IOT_ERR_INVALID_ARG);
  assert_int_equal(check_references(unresolved_first, 1, only_f2, 1), AZ_IOT_OK);
  assert_int_equal(check_references(unresolved_first, 2, NULL, 0), AZ_IOT_OK);
  assert_int_equal(check_references("{\"instructions\":{\"steps\":[{}]}}", 0, NULL, 0), AZ_IOT_OK);
  assert_int_equal(check_references("{}", 0, NULL, 0), AZ_IOT_OK);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(every_field_is_parsed),
    cmocka_unit_test(unknown_members_are_skipped_at_every_level),
    cmocka_unit_test(delta_file_members_are_accepted),
    cmocka_unit_test(malformed_manifests_are_rejected),
    cmocka_unit_test(a_manifest_at_the_limits_is_parsed),
    cmocka_unit_test(a_manifest_over_a_limit_is_refused),
    cmocka_unit_test(entries_past_a_limit_are_still_checked),
    cmocka_unit_test(step_files_must_be_described_and_have_urls),
    cmocka_unit_test(only_steps_from_first_step_are_checked),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
