// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file su_sim_test.c
 * @brief The PC Software Update samples' simulated hooks
 * (samples/software_update/pc/common/su_sim.c) with a mini manifest's detached manifest file.
 * Files are created in the working directory and removed by each test.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include "su_sim.h"

#define DETACHED_MANIFEST_FILE "su_sim_test_detached_manifest.tmp"
#define MISSING_FILE "su_sim_test_missing.tmp"
/** A file_directory that does not exist: writing a synthesized file there fails. */
#define MISSING_DIRECTORY "su_sim_test_missing_directory.tmp"

static const char k_detached_manifest[]
    = "{\"updateManifest\":\"{\\\"manifestVersion\\\":\\\"5\\\"}\"}";

/* su_simulation_control holds mutable strings. */
static char g_detached_manifest_path[] = DETACHED_MANIFEST_FILE;
static char g_missing_path[] = MISSING_FILE;
static char g_missing_directory[] = MISSING_DIRECTORY;

static int setup(void** state)
{
  (void)state;
  (void)remove(DETACHED_MANIFEST_FILE);
  FILE* file = fopen(DETACHED_MANIFEST_FILE, "wb");
  if (file == NULL)
  {
    return -1;
  }
  size_t length = strlen(k_detached_manifest);
  int written = fwrite(k_detached_manifest, 1, length, file) == length;
  return (fclose(file) == 0 && written) ? 0 : -1;
}

static int teardown(void** state)
{
  (void)state;
  (void)remove(DETACHED_MANIFEST_FILE);
  return 0;
}

/** @brief A manifest file entry of @p size bytes. */
static az_iot_su_client_update_manifest_file make_file(int64_t size)
{
  az_iot_su_client_update_manifest_file file;
  memset(&file, 0, sizeof(file));
  file.id = AZ_SPAN_FROM_STR("f1");
  file.file_name = AZ_SPAN_FROM_STR("detached.json");
  file.size_in_bytes = size;
  return file;
}

/** @brief Deliver a WORKFLOW_STATE_CHANGED event for @p state to su_sim_track_workflow_state(). */
static void enter_state(su_simulation_control* control, az_iot_su_state state)
{
  az_iot_su_event event;
  memset(&event, 0, sizeof(event));
  event.kind = AZ_IOT_SU_EVENT_WORKFLOW_STATE_CHANGED;
  event.state = state;
  su_sim_track_workflow_state(&event, control);
}

/**
 * @brief Read @p file as the client reads a detached manifest file: its size from offset 0, then
 * one more byte, which must not exist.
 */
static void assert_read_as_by_the_client(
    su_simulation_control* control,
    const az_iot_su_client_update_manifest_file* file,
    const char* expected)
{
  uint8_t content[sizeof(k_detached_manifest)] = { 0 };
  size_t size = (size_t)file->size_in_bytes;
  size_t total = 0;
  while (total < size)
  {
    size_t got = 0;
    assert_int_equal(
        read_back_simulated_update_file(
            file, 0, total, content + total, size - total, &got, control),
        AZ_IOT_SU_RESULT_SUCCESS);
    assert_true(got > 0 && got <= size - total);
    total += got;
  }
  assert_memory_equal(content, expected, size);
  uint8_t extra = 0;
  size_t extra_read = 1;
  assert_int_equal(
      read_back_simulated_update_file(file, 0, size, &extra, 1, &extra_read, control),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(extra_read, 0);
}

static void detached_manifest_file_is_served_as_written(void** state)
{
  (void)state;
  su_simulation_control control;
  memset(&control, 0, sizeof(control));
  control.detached_manifest_file = g_detached_manifest_path;
  /* Neither applies to the detached manifest file. */
  control.file_directory = g_missing_directory;
  control.hash_mismatch = 1;
  az_iot_su_client_update_manifest_file file = make_file((int64_t)strlen(k_detached_manifest));

  enter_state(&control, AZ_IOT_SU_STATE_MANIFEST_RECEIVED);
  enter_state(&control, AZ_IOT_SU_STATE_VERIFYING_MANIFEST);

  assert_int_equal(
      simulate_update_file_download(
          &file, AZ_SPAN_FROM_STR("http://example.com/detached.json"), 0, 1, &control),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_read_as_by_the_client(&control, &file, k_detached_manifest);
}

static void detached_manifest_file_download_fails_without_the_variable(void** state)
{
  (void)state;
  su_simulation_control control;
  memset(&control, 0, sizeof(control));
  az_iot_su_client_update_manifest_file file = make_file((int64_t)strlen(k_detached_manifest));
  enter_state(&control, AZ_IOT_SU_STATE_VERIFYING_MANIFEST);

  assert_int_equal(
      simulate_update_file_download(
          &file, AZ_SPAN_FROM_STR("http://example.com/detached.json"), 0, 1, &control),
      AZ_IOT_SU_RESULT_FAILURE);
  uint8_t byte = 0;
  size_t read = 1;
  assert_int_equal(
      read_back_simulated_update_file(&file, 0, 0, &byte, 1, &read, &control),
      AZ_IOT_SU_RESULT_FAILURE);
  assert_int_equal(read, 0);
}

static void detached_manifest_file_download_fails_when_the_copy_is_missing(void** state)
{
  (void)state;
  su_simulation_control control;
  memset(&control, 0, sizeof(control));
  control.detached_manifest_file = g_missing_path;
  az_iot_su_client_update_manifest_file file = make_file((int64_t)strlen(k_detached_manifest));
  enter_state(&control, AZ_IOT_SU_STATE_VERIFYING_MANIFEST);

  assert_int_equal(
      simulate_update_file_download(
          &file, AZ_SPAN_FROM_STR("http://example.com/detached.json"), 0, 1, &control),
      AZ_IOT_SU_RESULT_FAILURE);
}

static void step_files_stay_zero_filled_after_verification(void** state)
{
  (void)state;
  su_simulation_control control;
  memset(&control, 0, sizeof(control));
  control.detached_manifest_file = g_detached_manifest_path;
  az_iot_su_client_update_manifest_file file = make_file(16);
  enter_state(&control, AZ_IOT_SU_STATE_VERIFYING_MANIFEST);
  enter_state(&control, AZ_IOT_SU_STATE_DOWNLOAD_STARTED);

  assert_int_equal(
      simulate_update_file_download(
          &file, AZ_SPAN_FROM_STR("http://example.com/payload.bin"), 0, 1, &control),
      AZ_IOT_SU_RESULT_SUCCESS);
  uint8_t content[16];
  memset(content, 0xA5, sizeof(content));
  size_t read = 0;
  assert_int_equal(
      read_back_simulated_update_file(&file, 0, 0, content, sizeof(content), &read, &control),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(read, sizeof(content));
  static const uint8_t k_zeros[16] = { 0 };
  assert_memory_equal(content, k_zeros, sizeof(content));
}

static void detached_manifest_file_reads_from_an_offset(void** state)
{
  (void)state;
  su_simulation_control control;
  memset(&control, 0, sizeof(control));
  control.detached_manifest_file = g_detached_manifest_path;
  size_t length = strlen(k_detached_manifest);

  char content[sizeof(k_detached_manifest)] = { 0 };
  size_t read = 0;
  assert_int_equal(
      su_sim_read_detached_manifest_file(&control, 2, (uint8_t*)content, sizeof(content), &read),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(read, length - 2);
  assert_memory_equal(content, k_detached_manifest + 2, length - 2);
  assert_int_equal(
      su_sim_read_detached_manifest_file(
          &control, length + 1, (uint8_t*)content, sizeof(content), &read),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(read, 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(detached_manifest_file_is_served_as_written, setup, teardown),
    cmocka_unit_test_setup_teardown(
        detached_manifest_file_download_fails_without_the_variable, setup, teardown),
    cmocka_unit_test_setup_teardown(
        detached_manifest_file_download_fails_when_the_copy_is_missing, setup, teardown),
    cmocka_unit_test_setup_teardown(
        step_files_stay_zero_filled_after_verification, setup, teardown),
    cmocka_unit_test_setup_teardown(detached_manifest_file_reads_from_an_offset, setup, teardown),
  };
  return cmocka_run_group_tests_name("az_iot_su_sim", tests, NULL, NULL);
}
