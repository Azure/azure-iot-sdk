// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Unit tests for the ESP32 OTA software updates hooks (az_iot_su_esp32_ota.c) over desktop
 * ESP-IDF fakes (fake_esp_ota.h). Each test replays what the software updates client does:
 * WORKFLOW_STATE_CHANGED events to az_iot_su_esp32_ota_handle_event(), then download_fn until it
 * stops returning IN_PROGRESS, read_file_fn for the hash check, install_fn and apply_fn. The client
 * side (download only in VERIFYING_MANIFEST and DOWNLOAD_STARTED, the state re-announced on resume,
 * a detached manifest file longer than sizeInBytes) is covered by su_client_test.c. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "az_iot_su_esp32_ota.h"
#include "fake_esp_ota.h"

/** @brief Capacity of the detached manifest buffer the tests give the hooks. */
#define DETACHED_MANIFEST_BUFFER_SIZE 32768
/** @brief Bytes of a test app image: more than one download_fn call stores. */
#define IMAGE_SIZE 40000

static const az_span image_url = AZ_SPAN_LITERAL_FROM_STR("https://example.com/firmware.bin");
static const az_span other_image_url
    = AZ_SPAN_LITERAL_FROM_STR("https://example.com/other-firmware.bin");
static const az_span detached_manifest_url
    = AZ_SPAN_LITERAL_FROM_STR("https://example.com/detached-manifest.json");
static const az_span other_detached_manifest_url
    = AZ_SPAN_LITERAL_FROM_STR("https://example.com/other-detached-manifest.json");

static uint8_t detached_manifest_buffer[DETACHED_MANIFEST_BUFFER_SIZE];
static az_iot_su_esp32_ota_context ota_context;
static az_iot_su_platform_hooks hooks;
static az_iot_su_client_update_manifest update_manifest;
static uint8_t image[IMAGE_SIZE];
static uint8_t other_image[IMAGE_SIZE];
static uint8_t detached_manifest_file[DETACHED_MANIFEST_BUFFER_SIZE + 16];
static uint8_t read_back[FAKE_ESP_OTA_PARTITION_SIZE];

/** @brief Fill @p buffer with bytes derived from @p seed; an app image starts with 0xE9. */
static void fill_content(uint8_t* buffer, size_t size, uint8_t seed, bool app_image)
{
  for (size_t i = 0; i < size; ++i)
  {
    buffer[i] = (uint8_t)((i * 31u + seed) & 0xFFu);
  }
  if (app_image && size > 0)
  {
    buffer[0] = 0xE9;
  }
}

/** @brief A manifest file entry of @p size_in_bytes bytes. */
static az_iot_su_client_update_manifest_file manifest_file(int64_t size_in_bytes)
{
  az_iot_su_client_update_manifest_file file;
  memset(&file, 0, sizeof(file));
  file.size_in_bytes = size_in_bytes;
  return file;
}

/** @brief Deliver the WORKFLOW_STATE_CHANGED event the client raises on entering @p state. */
static void announce_workflow_state(az_iot_su_state state)
{
  az_iot_su_event event;
  memset(&event, 0, sizeof(event));
  event.kind = AZ_IOT_SU_EVENT_WORKFLOW_STATE_CHANGED;
  event.previous_state = ota_context.workflow_state;
  event.state = state;
  az_iot_su_esp32_ota_handle_event(&event, &ota_context);
}

/** @brief Call download_fn while it returns IN_PROGRESS; set @p out_calls to the call count. */
static int32_t download_until_done(
    const az_iot_su_client_update_manifest_file* file,
    az_span url,
    int* out_calls)
{
  int32_t result;
  int calls = 0;
  do
  {
    result = hooks.download_fn(file, url, 0, 1, hooks.user_ctx);
    calls++;
  } while (result == AZ_IOT_SU_RESULT_IN_PROGRESS && calls < 100);
  if (out_calls != NULL)
  {
    *out_calls = calls;
  }
  return result;
}

/** @brief Read the file back with read_file_fn, 256 bytes at a time, into #read_back. */
static size_t read_all(const az_iot_su_client_update_manifest_file* file)
{
  size_t total = 0;
  size_t read = 0;
  do
  {
    assert_int_equal(
        hooks.read_file_fn(file, 0, total, read_back + total, 256, &read, hooks.user_ctx),
        AZ_IOT_SU_RESULT_SUCCESS);
    total += read;
  } while (read > 0 && total + 256 <= sizeof(read_back));
  return total;
}

/** @brief Download the image of @p size bytes in DOWNLOAD_STARTED; expect SUCCESS. */
static void download_image(const uint8_t* content, size_t size, az_span url)
{
  az_iot_su_client_update_manifest_file const file = manifest_file((int64_t)size);
  fake_esp_ota_serve(content, size);
  assert_int_equal(download_until_done(&file, url, NULL), AZ_IOT_SU_RESULT_SUCCESS);
}

static int setup(void** state)
{
  (void)state;
  fake_esp_ota_reset();
  memset(&ota_context, 0, sizeof(ota_context));
  memset(detached_manifest_buffer, 0, sizeof(detached_manifest_buffer));
  ota_context.installed_version = "1.0";
  ota_context.detached_manifest_buffer
      = az_span_create(detached_manifest_buffer, (int32_t)sizeof(detached_manifest_buffer));
  hooks = az_iot_su_esp32_ota_get_platform_hooks(&ota_context);
  fill_content(image, sizeof(image), 1, true);
  fill_content(other_image, sizeof(other_image), 2, true);
  fill_content(detached_manifest_file, sizeof(detached_manifest_file), 3, false);
  return 0;
}

static int teardown(void** state)
{
  (void)state;
  az_iot_su_esp32_ota_release_download(&ota_context);
  assert_int_equal(fake_esp_ota_open_http_clients(), 0);
  assert_int_equal(fake_esp_ota_open_write_sessions(), 0);
  return 0;
}

/* (1) A detached manifest file fetched in VERIFYING_MANIFEST goes to the buffer, never to the
 * OTA partition, and read_file_fn serves exactly its bytes. */
static void detached_manifest_file_is_stored_in_buffer_not_partition(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const file = manifest_file(5000);
  announce_workflow_state(AZ_IOT_SU_STATE_MANIFEST_RECEIVED);
  announce_workflow_state(AZ_IOT_SU_STATE_VERIFYING_MANIFEST);
  fake_esp_ota_serve(detached_manifest_file, 5000);

  assert_int_equal(
      download_until_done(&file, detached_manifest_url, NULL), AZ_IOT_SU_RESULT_SUCCESS);

  assert_int_equal(ota_context.detached_manifest_size, 5000);
  assert_string_equal(g_fake_esp_ota.http_last_url, "https://example.com/detached-manifest.json");
  assert_int_equal(fake_esp_ota_open_http_clients(), 0);
  assert_int_equal(g_fake_esp_ota.update_partition_lookups, 0);
  assert_int_equal(g_fake_esp_ota.ota_begins, 0);
  assert_int_equal(g_fake_esp_ota.ota_writes, 0);
  assert_int_equal(read_all(&file), 5000);
  assert_memory_equal(read_back, detached_manifest_file, 5000);
}

/* (2) After the detached manifest file, the step image goes to the OTA partition with exactly
 * one esp_ota_begin(), is read back, and installs. */
static void image_after_detached_manifest_file_goes_to_partition(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const detached_file = manifest_file(5000);
  az_iot_su_client_update_manifest_file const image_file = manifest_file(IMAGE_SIZE);
  announce_workflow_state(AZ_IOT_SU_STATE_MANIFEST_RECEIVED);
  announce_workflow_state(AZ_IOT_SU_STATE_VERIFYING_MANIFEST);
  fake_esp_ota_serve(detached_manifest_file, 5000);
  assert_int_equal(
      download_until_done(&detached_file, detached_manifest_url, NULL), AZ_IOT_SU_RESULT_SUCCESS);

  announce_workflow_state(AZ_IOT_SU_STATE_DOWNLOAD_STARTED);
  fake_esp_ota_serve(image, IMAGE_SIZE);
  int calls = 0;
  assert_int_equal(download_until_done(&image_file, image_url, &calls), AZ_IOT_SU_RESULT_SUCCESS);

  assert_true(calls > 2);
  assert_int_equal(g_fake_esp_ota.ota_begins, 1);
  assert_int_equal(g_fake_esp_ota.ota_bytes_written, IMAGE_SIZE);
  assert_memory_equal(g_fake_esp_ota.update_partition_data, image, IMAGE_SIZE);
  assert_int_equal(ota_context.detached_manifest_size, 5000);
  assert_memory_equal(detached_manifest_buffer, detached_manifest_file, 5000);

  announce_workflow_state(AZ_IOT_SU_STATE_DOWNLOAD_COMPLETE);
  assert_int_equal(read_all(&image_file), IMAGE_SIZE);
  assert_memory_equal(read_back, image, IMAGE_SIZE);

  announce_workflow_state(AZ_IOT_SU_STATE_INSTALL_STARTED);
  assert_int_equal(
      hooks.install_fn(&update_manifest, 0, hooks.user_ctx), AZ_IOT_SU_RESULT_REBOOT_REQUIRED);
  assert_int_equal(g_fake_esp_ota.ota_ends, 1);
  assert_int_equal(g_fake_esp_ota.ota_aborts, 0);
  assert_ptr_equal(g_fake_esp_ota.boot_partition, fake_esp_ota_update_partition());
  assert_true(ota_context.reboot_pending);
}

/* apply_fn waits for the reboot install_fn asked for: the old firmware still runs until then.
 * After the reboot, in the new image, apply succeeds. */
static void apply_waits_for_reboot_after_install(void** state)
{
  (void)state;
  announce_workflow_state(AZ_IOT_SU_STATE_DOWNLOAD_STARTED);
  download_image(image, IMAGE_SIZE, image_url);
  announce_workflow_state(AZ_IOT_SU_STATE_INSTALL_STARTED);
  assert_int_equal(
      hooks.install_fn(&update_manifest, 0, hooks.user_ctx), AZ_IOT_SU_RESULT_REBOOT_REQUIRED);

  announce_workflow_state(AZ_IOT_SU_STATE_APPLY_STARTED);
  assert_int_equal(
      hooks.apply_fn(&update_manifest, 0, hooks.user_ctx), AZ_IOT_SU_RESULT_IN_PROGRESS);
  assert_int_equal(
      hooks.apply_fn(&update_manifest, 0, hooks.user_ctx), AZ_IOT_SU_RESULT_IN_PROGRESS);
  assert_true(ota_context.reboot_pending);

  /* Rebooted into the new image, resumed at apply. */
  memset(&ota_context, 0, sizeof(ota_context));
  announce_workflow_state(AZ_IOT_SU_STATE_APPLY_STARTED);
  assert_int_equal(hooks.apply_fn(&update_manifest, 0, hooks.user_ctx), AZ_IOT_SU_RESULT_SUCCESS);
}

/* (3) A detached manifest file declared larger than the buffer fails before any request. */
static void detached_manifest_file_larger_than_buffer_fails(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const file
      = manifest_file(DETACHED_MANIFEST_BUFFER_SIZE + 1);
  announce_workflow_state(AZ_IOT_SU_STATE_VERIFYING_MANIFEST);
  fake_esp_ota_serve(detached_manifest_file, DETACHED_MANIFEST_BUFFER_SIZE + 1);

  assert_int_equal(
      hooks.download_fn(&file, detached_manifest_url, 0, 1, hooks.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);

  assert_int_equal(g_fake_esp_ota.http_inits, 0);
  assert_int_equal(g_fake_esp_ota.ota_begins, 0);
  assert_int_equal(ota_context.detached_manifest_size, 0);
}

/* (4a) A detached manifest file body longer than sizeInBytes, and than the buffer, fails. */
static void detached_manifest_body_longer_than_buffer_fails(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const file = manifest_file(4000);
  ota_context.detached_manifest_buffer = az_span_create(detached_manifest_buffer, 4000);
  announce_workflow_state(AZ_IOT_SU_STATE_VERIFYING_MANIFEST);
  fake_esp_ota_serve(detached_manifest_file, 4001);

  assert_int_equal(
      download_until_done(&file, detached_manifest_url, NULL), AZ_IOT_SU_RESULT_FAILURE);

  assert_int_equal(fake_esp_ota_open_http_clients(), 0);
  assert_int_equal(g_fake_esp_ota.ota_begins, 0);
}

/* (4b) A detached manifest file body longer than sizeInBytes but within the buffer is served
 * whole, so the client's read of sizeInBytes + 1 bytes rejects it. */
static void detached_manifest_body_longer_than_size_is_served_whole(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const file = manifest_file(4000);
  announce_workflow_state(AZ_IOT_SU_STATE_VERIFYING_MANIFEST);
  fake_esp_ota_serve(detached_manifest_file, 4001);

  assert_int_equal(
      download_until_done(&file, detached_manifest_url, NULL), AZ_IOT_SU_RESULT_SUCCESS);

  size_t read = 0;
  assert_int_equal(
      hooks.read_file_fn(&file, 0, 0, read_back, 4001, &read, hooks.user_ctx),
      AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(read, 4001);
  assert_int_equal(g_fake_esp_ota.ota_begins, 0);
}

/* (4c) An image body longer than sizeInBytes is written and read back whole, so the client's
 * hash check, which reads to the end, rejects it. */
static void image_body_longer_than_size_is_read_back_whole(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const file = manifest_file(IMAGE_SIZE - 100);
  announce_workflow_state(AZ_IOT_SU_STATE_DOWNLOAD_STARTED);
  fake_esp_ota_serve(image, IMAGE_SIZE);

  assert_int_equal(download_until_done(&file, image_url, NULL), AZ_IOT_SU_RESULT_SUCCESS);

  assert_int_equal(read_all(&file), IMAGE_SIZE);
}

/* (5a) A workflow replacing one mid-way through its detached manifest file: the new file is all
 * the buffer holds. */
static void replacement_workflow_mid_detached_manifest_download(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const first_file = manifest_file(20000);
  az_iot_su_client_update_manifest_file const second_file = manifest_file(3000);
  announce_workflow_state(AZ_IOT_SU_STATE_VERIFYING_MANIFEST);
  fake_esp_ota_serve(detached_manifest_file, 20000);
  assert_int_equal(
      hooks.download_fn(&first_file, detached_manifest_url, 0, 1, hooks.user_ctx),
      AZ_IOT_SU_RESULT_IN_PROGRESS);
  assert_int_equal(
      hooks.download_fn(&first_file, detached_manifest_url, 0, 1, hooks.user_ctx),
      AZ_IOT_SU_RESULT_IN_PROGRESS);
  assert_true(ota_context.detached_manifest_size > 0);

  announce_workflow_state(AZ_IOT_SU_STATE_MANIFEST_RECEIVED);
  assert_int_equal(fake_esp_ota_open_http_clients(), 0);
  announce_workflow_state(AZ_IOT_SU_STATE_VERIFYING_MANIFEST);
  fake_esp_ota_serve(detached_manifest_file + 100, 3000);
  assert_int_equal(
      download_until_done(&second_file, other_detached_manifest_url, NULL),
      AZ_IOT_SU_RESULT_SUCCESS);

  assert_int_equal(read_all(&second_file), 3000);
  assert_memory_equal(read_back, detached_manifest_file + 100, 3000);
  assert_int_equal(g_fake_esp_ota.ota_begins, 0);
}

/* (5b) A workflow replacing one mid-way through its image: the old OTA write is aborted and the
 * new image is the only one written. */
static void replacement_workflow_mid_image_download(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const file = manifest_file(IMAGE_SIZE);
  announce_workflow_state(AZ_IOT_SU_STATE_DOWNLOAD_STARTED);
  fake_esp_ota_serve(image, IMAGE_SIZE);
  assert_int_equal(
      hooks.download_fn(&file, image_url, 0, 1, hooks.user_ctx), AZ_IOT_SU_RESULT_IN_PROGRESS);
  assert_int_equal(
      hooks.download_fn(&file, image_url, 0, 1, hooks.user_ctx), AZ_IOT_SU_RESULT_IN_PROGRESS);
  assert_int_equal(fake_esp_ota_open_write_sessions(), 1);

  announce_workflow_state(AZ_IOT_SU_STATE_MANIFEST_RECEIVED);
  assert_int_equal(g_fake_esp_ota.ota_aborts, 1);
  assert_int_equal(fake_esp_ota_open_write_sessions(), 0);
  assert_int_equal(fake_esp_ota_open_http_clients(), 0);

  announce_workflow_state(AZ_IOT_SU_STATE_DOWNLOAD_STARTED);
  download_image(other_image, IMAGE_SIZE, other_image_url);
  assert_int_equal(g_fake_esp_ota.ota_begins, 2);
  assert_int_equal(fake_esp_ota_open_write_sessions(), 1);
  assert_int_equal(read_all(&file), IMAGE_SIZE);
  assert_memory_equal(read_back, other_image, IMAGE_SIZE);
}

/* (5c) A workflow ending in FAILED mid-download releases the download and the OTA write. */
static void failed_workflow_releases_download(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const file = manifest_file(IMAGE_SIZE);
  announce_workflow_state(AZ_IOT_SU_STATE_DOWNLOAD_STARTED);
  fake_esp_ota_serve(image, IMAGE_SIZE);
  assert_int_equal(
      hooks.download_fn(&file, image_url, 0, 1, hooks.user_ctx), AZ_IOT_SU_RESULT_IN_PROGRESS);

  announce_workflow_state(AZ_IOT_SU_STATE_FAILED);

  assert_int_equal(g_fake_esp_ota.ota_aborts, 1);
  assert_int_equal(fake_esp_ota_open_write_sessions(), 0);
  assert_int_equal(fake_esp_ota_open_http_clients(), 0);
}

/* (6a) A download of another URL while one is in progress aborts the stale OTA write. */
static void other_url_mid_download_aborts_stale_write(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const file = manifest_file(IMAGE_SIZE);
  announce_workflow_state(AZ_IOT_SU_STATE_DOWNLOAD_STARTED);
  fake_esp_ota_serve(image, IMAGE_SIZE);
  assert_int_equal(
      hooks.download_fn(&file, image_url, 0, 1, hooks.user_ctx), AZ_IOT_SU_RESULT_IN_PROGRESS);
  assert_int_equal(
      hooks.download_fn(&file, image_url, 0, 1, hooks.user_ctx), AZ_IOT_SU_RESULT_IN_PROGRESS);

  download_image(other_image, IMAGE_SIZE, other_image_url);

  assert_int_equal(g_fake_esp_ota.ota_aborts, 1);
  assert_int_equal(g_fake_esp_ota.ota_begins, 2);
  assert_int_equal(fake_esp_ota_open_write_sessions(), 1);
  assert_int_equal(read_all(&file), IMAGE_SIZE);
  assert_memory_equal(read_back, other_image, IMAGE_SIZE);
}

/* (6b) A complete image never installed is aborted when the next download opens, so only one
 * OTA write session is ever open. */
static void complete_uninstalled_image_is_aborted_on_next_download(void** state)
{
  (void)state;
  announce_workflow_state(AZ_IOT_SU_STATE_DOWNLOAD_STARTED);
  download_image(image, IMAGE_SIZE, image_url);
  assert_int_equal(fake_esp_ota_open_write_sessions(), 1);

  download_image(other_image, IMAGE_SIZE, other_image_url);

  assert_int_equal(g_fake_esp_ota.ota_aborts, 1);
  assert_int_equal(fake_esp_ota_open_write_sessions(), 1);
  assert_int_equal(
      hooks.install_fn(&update_manifest, 0, hooks.user_ctx), AZ_IOT_SU_RESULT_REBOOT_REQUIRED);
  assert_int_equal(fake_esp_ota_open_write_sessions(), 0);
  assert_memory_equal(g_fake_esp_ota.update_partition_data, other_image, IMAGE_SIZE);
}

/* (7) After a reboot, the state the client re-announces on resume routes the downloads as
 * before: the detached manifest file to the buffer, the image to the partition. */
static void resume_after_reboot_routes_downloads_by_announced_state(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const detached_file = manifest_file(5000);
  az_iot_su_client_update_manifest_file const image_file = manifest_file(IMAGE_SIZE);
  /* Resumed in VERIFYING_MANIFEST: the event's previous_state is IDLE. */
  announce_workflow_state(AZ_IOT_SU_STATE_VERIFYING_MANIFEST);
  fake_esp_ota_serve(detached_manifest_file, 5000);
  assert_int_equal(
      download_until_done(&detached_file, detached_manifest_url, NULL), AZ_IOT_SU_RESULT_SUCCESS);
  assert_int_equal(g_fake_esp_ota.update_partition_lookups, 0);
  assert_int_equal(read_all(&detached_file), 5000);
  assert_memory_equal(read_back, detached_manifest_file, 5000);

  /* Rebooted again, resumed in DOWNLOAD_STARTED. */
  memset(&ota_context, 0, sizeof(ota_context));
  ota_context.detached_manifest_buffer
      = az_span_create(detached_manifest_buffer, (int32_t)sizeof(detached_manifest_buffer));
  announce_workflow_state(AZ_IOT_SU_STATE_DOWNLOAD_STARTED);
  int calls = 0;
  fake_esp_ota_serve(image, IMAGE_SIZE);
  assert_int_equal(download_until_done(&image_file, image_url, &calls), AZ_IOT_SU_RESULT_SUCCESS);
  assert_true(calls > 2);
  assert_int_equal(g_fake_esp_ota.ota_begins, 1);
  assert_int_equal(read_all(&image_file), IMAGE_SIZE);
  assert_memory_equal(read_back, image, IMAGE_SIZE);
}

/* (8a) A zero-byte detached manifest file downloads and reads back as empty. */
static void zero_size_detached_manifest_file_is_empty(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const file = manifest_file(0);
  announce_workflow_state(AZ_IOT_SU_STATE_VERIFYING_MANIFEST);
  fake_esp_ota_serve(NULL, 0);

  assert_int_equal(
      download_until_done(&file, detached_manifest_url, NULL), AZ_IOT_SU_RESULT_SUCCESS);

  assert_int_equal(ota_context.detached_manifest_size, 0);
  assert_int_equal(read_all(&file), 0);
  assert_int_equal(g_fake_esp_ota.ota_begins, 0);
}

/* (8b) With an empty detached manifest buffer, as documented, mini manifests fail. */
static void empty_detached_manifest_buffer_fails(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const file = manifest_file(0);
  ota_context.detached_manifest_buffer = AZ_SPAN_EMPTY;
  announce_workflow_state(AZ_IOT_SU_STATE_VERIFYING_MANIFEST);
  fake_esp_ota_serve(NULL, 0);

  assert_int_equal(
      hooks.download_fn(&file, detached_manifest_url, 0, 1, hooks.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);

  assert_int_equal(g_fake_esp_ota.http_inits, 0);
}

/* (8c) A zero-byte image downloads, reads back as empty, and fails to install, leaving no OTA
 * write session open. */
static void zero_size_image_fails_install(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const file = manifest_file(0);
  announce_workflow_state(AZ_IOT_SU_STATE_DOWNLOAD_STARTED);
  download_image(NULL, 0, image_url);
  assert_int_equal(read_all(&file), 0);

  assert_int_equal(hooks.install_fn(&update_manifest, 0, hooks.user_ctx), AZ_IOT_SU_RESULT_FAILURE);

  assert_int_equal(g_fake_esp_ota.ota_set_boots, 0);
  assert_int_equal(fake_esp_ota_open_write_sessions(), 0);
  assert_false(ota_context.reboot_pending);
}

/* (9) Without az_iot_su_esp32_ota_handle_event() registered, download_fn fails, as documented,
 * before any request or OTA call. */
static void download_without_observer_fails(void** state)
{
  (void)state;
  az_iot_su_client_update_manifest_file const file = manifest_file(5000);
  fake_esp_ota_serve(detached_manifest_file, 5000);

  assert_int_equal(
      hooks.download_fn(&file, detached_manifest_url, 0, 1, hooks.user_ctx),
      AZ_IOT_SU_RESULT_FAILURE);
  assert_int_equal(
      hooks.download_fn(&file, image_url, 0, 1, hooks.user_ctx), AZ_IOT_SU_RESULT_FAILURE);

  assert_int_equal(g_fake_esp_ota.http_inits, 0);
  assert_int_equal(g_fake_esp_ota.update_partition_lookups, 0);
  assert_int_equal(g_fake_esp_ota.ota_begins, 0);
  assert_int_equal(ota_context.detached_manifest_size, 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(
        detached_manifest_file_is_stored_in_buffer_not_partition, setup, teardown),
    cmocka_unit_test_setup_teardown(
        image_after_detached_manifest_file_goes_to_partition, setup, teardown),
    cmocka_unit_test_setup_teardown(apply_waits_for_reboot_after_install, setup, teardown),
    cmocka_unit_test_setup_teardown(
        detached_manifest_file_larger_than_buffer_fails, setup, teardown),
    cmocka_unit_test_setup_teardown(
        detached_manifest_body_longer_than_buffer_fails, setup, teardown),
    cmocka_unit_test_setup_teardown(
        detached_manifest_body_longer_than_size_is_served_whole, setup, teardown),
    cmocka_unit_test_setup_teardown(
        image_body_longer_than_size_is_read_back_whole, setup, teardown),
    cmocka_unit_test_setup_teardown(
        replacement_workflow_mid_detached_manifest_download, setup, teardown),
    cmocka_unit_test_setup_teardown(replacement_workflow_mid_image_download, setup, teardown),
    cmocka_unit_test_setup_teardown(failed_workflow_releases_download, setup, teardown),
    cmocka_unit_test_setup_teardown(other_url_mid_download_aborts_stale_write, setup, teardown),
    cmocka_unit_test_setup_teardown(
        complete_uninstalled_image_is_aborted_on_next_download, setup, teardown),
    cmocka_unit_test_setup_teardown(
        resume_after_reboot_routes_downloads_by_announced_state, setup, teardown),
    cmocka_unit_test_setup_teardown(zero_size_detached_manifest_file_is_empty, setup, teardown),
    cmocka_unit_test_setup_teardown(empty_detached_manifest_buffer_fails, setup, teardown),
    cmocka_unit_test_setup_teardown(zero_size_image_fails_install, setup, teardown),
    cmocka_unit_test_setup_teardown(download_without_observer_fails, setup, teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
