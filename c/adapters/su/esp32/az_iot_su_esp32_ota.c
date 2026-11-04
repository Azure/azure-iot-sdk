// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "az_iot_su_esp32_ota.h"

#include <inttypes.h>
#include <string.h>

#include "azure/iot/az_iot_log.h"
#include "azure/iot/az_iot_log_components.h"

#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "nvs_flash.h"

#define SU_NVS_NAMESPACE "su_sample"
#define SU_NVS_STATE_KEY "wf_state"

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

/* Copy an az_span into a NUL-terminated C string buffer. Returns false on
 * overflow. */
static bool span_to_cstr(az_span s, char* out, size_t cap)
{
  int32_t n = az_span_size(s);
  if (n < 0 || (size_t)n + 1 > cap)
  {
    return false;
  }
  if (n > 0)
  {
    memcpy(out, az_span_ptr(s), (size_t)n);
  }
  out[n] = '\0';
  return true;
}

/* ------------------------------------------------------------------------- */
/* download                                                                  */
/* ------------------------------------------------------------------------- */

/* Per download_fn call: at most this many bytes, and no new read once this much
 * time has passed. With OTA_HTTP_READ_TIMEOUT_MS, a call holds do_work() for at
 * most about 11 s, well inside the MQTT keep-alive. */
#define OTA_DOWNLOAD_BYTES_PER_CALL (16 * 1024)
#define OTA_DOWNLOAD_TIME_PER_CALL_US (1000 * 1000)
/* Longest a single read waits for data. */
#define OTA_HTTP_READ_TIMEOUT_MS 10000

/** @brief FNV-1a hash of a download URL: tells a new download from the one in progress. */
static uint32_t ota_download_url_hash(az_span url)
{
  uint32_t hash = 2166136261u;
  for (int32_t i = 0; i < az_span_size(url); ++i)
  {
    hash = (hash ^ az_span_ptr(url)[i]) * 16777619u;
  }
  return hash;
}

/** @brief Close the HTTP download and abort an unfinished OTA write, if open. */
static void ota_download_close(az_iot_su_esp32_ota_context* c, bool abort_ota_write)
{
  if (c->http_client != NULL)
  {
    esp_http_client_close(c->http_client);
    esp_http_client_cleanup(c->http_client);
    c->http_client = NULL;
  }
  if (abort_ota_write && c->ota_in_progress)
  {
    esp_ota_abort(c->ota_handle);
    c->ota_in_progress = false;
  }
}

/**
 * @brief Whether the client is fetching a mini manifest's detached manifest file, which it does
 * only in AZ_IOT_SU_STATE_VERIFYING_MANIFEST, rather than a step's file.
 */
static bool ota_is_detached_manifest_phase(const az_iot_su_esp32_ota_context* c)
{
  return c->workflow_state == AZ_IOT_SU_STATE_VERIFYING_MANIFEST;
}

/**
 * @brief Open the HTTP download, and the OTA write unless @p detached_manifest. Returns
 * IN_PROGRESS or FAILURE.
 */
static int32_t ota_download_open(
    az_iot_su_esp32_ota_context* c,
    const az_iot_su_client_update_manifest_file* file,
    az_span download_url,
    bool detached_manifest)
{
  char url[1024];
  if (!span_to_cstr(download_url, url, sizeof(url)))
  {
    AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_SU, "ota: download URL too long");
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  if (detached_manifest)
  {
    if (file->size_in_bytes < 0
        || file->size_in_bytes > (int64_t)az_span_size(c->detached_manifest_buffer))
    {
      AZ_IOT_LOG_ERRORF(
          AZ_IOT_LOG_COMPONENT_SU,
          "ota: detached manifest file of %lld bytes exceeds detached_manifest_buffer (%" PRId32
          " bytes)",
          (long long)file->size_in_bytes,
          az_span_size(c->detached_manifest_buffer));
      return AZ_IOT_SU_RESULT_FAILURE;
    }
    AZ_IOT_LOG_INFOF(
        AZ_IOT_LOG_COMPONENT_SU,
        "ota: downloading %lld-byte detached manifest file",
        (long long)file->size_in_bytes);
  }
  else
  {
    c->update_partition = esp_ota_get_next_update_partition(NULL);
    if (!c->update_partition)
    {
      AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_SU, "ota: no free OTA partition");
      return AZ_IOT_SU_RESULT_FAILURE;
    }
    AZ_IOT_LOG_INFOF(
        AZ_IOT_LOG_COMPONENT_SU,
        "ota: downloading %lld bytes -> partition '%s' @0x%08" PRIx32,
        (long long)file->size_in_bytes,
        c->update_partition->label,
        c->update_partition->address);
  }

  /* timeout_ms bounds each blocking read; it must stay well below the MQTT keep-alive. */
  esp_http_client_config_t http = {
    .url = url,
    .crt_bundle_attach = esp_crt_bundle_attach,
    .timeout_ms = OTA_HTTP_READ_TIMEOUT_MS,
    .keep_alive_enable = true,
    .buffer_size = 2048,
  };
  c->http_client = esp_http_client_init(&http);
  if (!c->http_client)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  esp_err_t err = esp_http_client_open(c->http_client, 0);
  if (err != ESP_OK)
  {
    AZ_IOT_LOG_ERRORF(AZ_IOT_LOG_COMPONENT_SU, "ota: http open failed: %s", esp_err_to_name(err));
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  esp_http_client_fetch_headers(c->http_client);
  int status = esp_http_client_get_status_code(c->http_client);
  if (status != 200 && status != 206)
  {
    AZ_IOT_LOG_ERRORF(AZ_IOT_LOG_COMPONENT_SU, "ota: http status %d", status);
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  if (detached_manifest)
  {
    c->detached_manifest_size = 0;
  }
  else
  {
    if (esp_ota_begin(c->update_partition, OTA_SIZE_UNKNOWN, &c->ota_handle) != ESP_OK)
    {
      AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_SU, "ota: esp_ota_begin failed");
      return AZ_IOT_SU_RESULT_FAILURE;
    }
    c->ota_in_progress = true;
    c->bytes_written = 0;
  }
  c->download_url_hash = ota_download_url_hash(download_url);
  return AZ_IOT_SU_RESULT_IN_PROGRESS;
}

/**
 * @brief Store @p size downloaded bytes: appended to #detached_manifest_buffer when
 * @p detached_manifest, else written to the OTA partition. Returns false on failure.
 */
static bool ota_download_store(
    az_iot_su_esp32_ota_context* c,
    bool detached_manifest,
    const uint8_t* chunk,
    size_t size)
{
  if (detached_manifest)
  {
    size_t const capacity = (size_t)az_span_size(c->detached_manifest_buffer);
    if (size > capacity - c->detached_manifest_size)
    {
      AZ_IOT_LOG_ERRORF(
          AZ_IOT_LOG_COMPONENT_SU,
          "ota: detached manifest file exceeds detached_manifest_buffer (%zu bytes)",
          capacity);
      return false;
    }
    memcpy(az_span_ptr(c->detached_manifest_buffer) + c->detached_manifest_size, chunk, size);
    c->detached_manifest_size += size;
    return true;
  }
  if (esp_ota_write(c->ota_handle, chunk, size) != ESP_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU, "ota: esp_ota_write failed at %zu", c->bytes_written);
    return false;
  }
  c->bytes_written += size;
  return true;
}

/**
 * @brief Store up to OTA_DOWNLOAD_BYTES_PER_CALL with ota_download_store(), starting no read
 * after OTA_DOWNLOAD_TIME_PER_CALL_US. Returns IN_PROGRESS, SUCCESS or FAILURE.
 */
static int32_t ota_download_continue(az_iot_su_esp32_ota_context* c, bool detached_manifest)
{
  uint8_t buf[2048];
  size_t read_this_call = 0;
  int64_t const started_us = esp_timer_get_time();
  while (read_this_call < OTA_DOWNLOAD_BYTES_PER_CALL
         && esp_timer_get_time() - started_us < OTA_DOWNLOAD_TIME_PER_CALL_US)
  {
    int r = esp_http_client_read(c->http_client, (char*)buf, sizeof(buf));
    if (r < 0)
    {
      AZ_IOT_LOG_ERROR(AZ_IOT_LOG_COMPONENT_SU, "ota: http read error");
      return AZ_IOT_SU_RESULT_FAILURE;
    }
    if (r == 0)
    {
      AZ_IOT_LOG_INFOF(
          AZ_IOT_LOG_COMPONENT_SU,
          "ota: download complete: %zu bytes written",
          detached_manifest ? c->detached_manifest_size : c->bytes_written);
      return AZ_IOT_SU_RESULT_SUCCESS;
    }
    if (!ota_download_store(c, detached_manifest, buf, (size_t)r))
    {
      return AZ_IOT_SU_RESULT_FAILURE;
    }
    read_this_call += (size_t)r;
  }
  return AZ_IOT_SU_RESULT_IN_PROGRESS;
}

/**
 * @brief download_fn: a mini manifest's detached manifest file into #detached_manifest_buffer,
 * else the app image into the OTA partition. Called again on each do_work() while it returns
 * IN_PROGRESS.
 */
static int32_t ota_download(
    const az_iot_su_client_update_manifest_file* file,
    az_span download_url,
    uint32_t file_index,
    uint32_t file_count,
    void* user_ctx)
{
  az_iot_su_esp32_ota_context* c = (az_iot_su_esp32_ota_context*)user_ctx;
  bool const detached_manifest = ota_is_detached_manifest_phase(c);

  /* The single payload is the new ESP32 app image; multi-file updates would need a
   * partition/file map. */
  if (!detached_manifest && file_index != 0)
  {
    AZ_IOT_LOG_WARNF(
        AZ_IOT_LOG_COMPONENT_SU,
        "ota: ignoring extra file %" PRIu32 "/%" PRIu32,
        file_index + 1u,
        file_count);
    return AZ_IOT_SU_RESULT_SUCCESS;
  }

  if (c->http_client != NULL && c->download_url_hash != ota_download_url_hash(download_url))
  {
    /* A different download replaced the one in progress. */
    ota_download_close(c, true);
  }
  int32_t result = c->http_client == NULL
      ? ota_download_open(c, file, download_url, detached_manifest)
      : ota_download_continue(c, detached_manifest);
  if (result != AZ_IOT_SU_RESULT_IN_PROGRESS)
  {
    /* Keep a complete image's OTA write open for install_fn. */
    ota_download_close(c, result != AZ_IOT_SU_RESULT_SUCCESS);
  }
  return result;
}

/* ------------------------------------------------------------------------- */
/* read back for hash verification                                           */
/* ------------------------------------------------------------------------- */

/**
 * @brief read_file_fn: reads the detached manifest file from #detached_manifest_buffer, else the
 * written image from the OTA partition.
 */
static int32_t ota_read_file(
    const az_iot_su_client_update_manifest_file* file,
    uint32_t file_index,
    size_t offset,
    uint8_t* buffer,
    size_t buffer_size,
    size_t* out_read,
    void* user_ctx)
{
  (void)file_index;
  az_iot_su_esp32_ota_context* c = (az_iot_su_esp32_ota_context*)user_ctx;
  if (ota_is_detached_manifest_phase(c))
  {
    size_t const remaining
        = offset < c->detached_manifest_size ? c->detached_manifest_size - offset : 0;
    size_t const copied = remaining < buffer_size ? remaining : buffer_size;
    if (copied > 0)
    {
      memcpy(buffer, az_span_ptr(c->detached_manifest_buffer) + offset, copied);
    }
    *out_read = copied;
    return AZ_IOT_SU_RESULT_SUCCESS;
  }
  if (!c->update_partition)
  {
    *out_read = 0;
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  size_t size = c->bytes_written ? c->bytes_written
                                 : (file->size_in_bytes > 0 ? (size_t)file->size_in_bytes : 0);
  if (offset >= size)
  {
    *out_read = 0;
    return AZ_IOT_SU_RESULT_SUCCESS;
  } /* EOF */

  size_t remain = size - offset;
  size_t n = remain < buffer_size ? remain : buffer_size;
  if (esp_partition_read(c->update_partition, offset, buffer, n) != ESP_OK)
  {
    *out_read = 0;
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  *out_read = n;
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* install / apply / rollback                                                */
/* ------------------------------------------------------------------------- */

static int32_t ota_is_installed(const az_iot_su_client_update_manifest* manifest, void* user_ctx)
{
  az_iot_su_esp32_ota_context* c = (az_iot_su_esp32_ota_context*)user_ctx;
  if (c->installed_version
      && az_span_is_content_equal(
          manifest->update_id.version, az_span_create_from_str((char*)c->installed_version)))
  {
    return AZ_IOT_SU_RESULT_ALREADY_INSTALLED;
  }
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t ota_backup(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  (void)user_ctx;
  /* The currently-running partition IS the backup: the bootloader rolls back
   * to it automatically if the new image fails to confirm itself healthy. */
  AZ_IOT_LOG_INFOF(
      AZ_IOT_LOG_COMPONENT_SU,
      "ota: backup step %" PRIu32 " (implicit A/B rollback partition)",
      step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t ota_install(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  az_iot_su_esp32_ota_context* c = (az_iot_su_esp32_ota_context*)user_ctx;
  if (!c->ota_in_progress)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU, "ota: install step %" PRIu32 " with no downloaded image", step);
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  /* Finalize + validate the written image (checks magic + SHA-256 footer). */
  esp_err_t err = esp_ota_end(c->ota_handle);
  c->ota_in_progress = false;
  if (err != ESP_OK)
  {
    AZ_IOT_LOG_ERRORF(AZ_IOT_LOG_COMPONENT_SU, "ota: esp_ota_end failed: %s", esp_err_to_name(err));
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  err = esp_ota_set_boot_partition(c->update_partition);
  if (err != ESP_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "ota: esp_ota_set_boot_partition failed: %s",
        esp_err_to_name(err));
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  AZ_IOT_LOG_INFOF(
      AZ_IOT_LOG_COMPONENT_SU,
      "ota: install step %" PRIu32 " complete; reboot required to apply",
      step);
  c->reboot_pending = true;
  return AZ_IOT_SU_RESULT_REBOOT_REQUIRED;
}

static int32_t ota_apply(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  (void)user_ctx;
  /* Reached after the reboot, when the workflow resumes inside the freshly
   * booted image: the new firmware is already running, so apply is a no-op. */
  AZ_IOT_LOG_INFOF(
      AZ_IOT_LOG_COMPONENT_SU, "ota: apply step %" PRIu32 " (new image already running)", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t ota_restore(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  az_iot_su_esp32_ota_context* c = (az_iot_su_esp32_ota_context*)user_ctx;
  AZ_IOT_LOG_WARNF(
      AZ_IOT_LOG_COMPONENT_SU,
      "ota: restore step %" PRIu32 " (rollback to running partition)",
      step);
  ota_download_close(c, true);
  /* Rolled back: the reboot install asked for is no longer wanted. */
  c->reboot_pending = false;
  /* Point boot back at the currently-running (good) partition. */
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_err_t err = (running != NULL) ? esp_ota_set_boot_partition(running) : ESP_FAIL;
  if (err != ESP_OK)
  {
    AZ_IOT_LOG_ERRORF(
        AZ_IOT_LOG_COMPONENT_SU,
        "ota: restore: could not select the running partition: %s",
        esp_err_to_name(err));
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  return AZ_IOT_SU_RESULT_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* resume-state persistence (NVS)                                            */
/* ------------------------------------------------------------------------- */

static int32_t ota_persist(const uint8_t* blob, size_t len, void* user_ctx)
{
  (void)user_ctx;
  nvs_handle_t h;
  if (nvs_open(SU_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  int32_t result = AZ_IOT_SU_RESULT_FAILURE;
  if (len == 0)
  {
    /* Invalidation: erase the key so a later boot finds no checkpoint. */
    esp_err_t err = nvs_erase_key(h, SU_NVS_STATE_KEY);
    if ((err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK)
    {
      AZ_IOT_LOG_INFO(AZ_IOT_LOG_COMPONENT_SU, "ota: cleared persisted workflow state from NVS");
      result = AZ_IOT_SU_RESULT_SUCCESS;
    }
  }
  else if (nvs_set_blob(h, SU_NVS_STATE_KEY, blob, len) == ESP_OK && nvs_commit(h) == ESP_OK)
  {
    AZ_IOT_LOG_INFOF(
        AZ_IOT_LOG_COMPONENT_SU, "ota: persisted %zu bytes of workflow state to NVS", len);
    result = AZ_IOT_SU_RESULT_SUCCESS;
  }
  nvs_close(h);
  return result;
}

static int32_t ota_load(uint8_t* blob, size_t cap, size_t* out_len, void* user_ctx)
{
  (void)user_ctx;
  nvs_handle_t h;
  if (nvs_open(SU_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
  {
    return 1; /* nothing persisted */
  }
  size_t len = cap;
  esp_err_t err = nvs_get_blob(h, SU_NVS_STATE_KEY, blob, &len);
  nvs_close(h);
  if (err != ESP_OK)
  {
    return 1;
  }
  *out_len = len;
  return 0;
}

/* ------------------------------------------------------------------------- */
/* public                                                                    */
/* ------------------------------------------------------------------------- */

void az_iot_su_esp32_ota_release_download(az_iot_su_esp32_ota_context* context)
{
  if (context->http_client != NULL || context->ota_in_progress)
  {
    AZ_IOT_LOG_WARN(AZ_IOT_LOG_COMPONENT_SU, "ota: releasing an unfinished download");
  }
  ota_download_close(context, true);
}

void az_iot_su_esp32_ota_handle_event(const az_iot_su_event* event, void* context)
{
  az_iot_su_esp32_ota_context* c = (az_iot_su_esp32_ota_context*)context;
  c->workflow_state = event->state;
  /* A workflow replaced or ended outside download_fn and restore_fn. */
  if (event->kind == AZ_IOT_SU_EVENT_WORKFLOW_STATE_CHANGED
      && (event->state == AZ_IOT_SU_STATE_MANIFEST_RECEIVED || event->state == AZ_IOT_SU_STATE_IDLE
          || event->state == AZ_IOT_SU_STATE_FAILED))
  {
    az_iot_su_esp32_ota_release_download(c);
  }
}

void az_iot_su_esp32_ota_mark_running_image_valid(void)
{
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK
      && state == ESP_OTA_IMG_PENDING_VERIFY)
  {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
    {
      AZ_IOT_LOG_INFO(AZ_IOT_LOG_COMPONENT_SU, "ota: image confirmed valid; rollback cancelled");
    }
  }
}

az_iot_su_platform_hooks az_iot_su_esp32_ota_get_platform_hooks(
    az_iot_su_esp32_ota_context* context)
{
  az_iot_su_platform_hooks h;
  memset(&h, 0, sizeof(h));
  h.download_fn = ota_download;
  h.read_file_fn = ota_read_file;
  h.install_fn = ota_install;
  h.apply_fn = ota_apply;
  h.backup_fn = ota_backup;
  h.restore_fn = ota_restore;
  h.is_installed_fn = ota_is_installed;
  h.persist_workflow_state_fn = ota_persist;
  h.load_workflow_state_fn = ota_load;
  h.user_ctx = context;
  return h;
}
