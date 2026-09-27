// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "su_esp32_ota.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "su_ota";

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

static int32_t ota_download(
    const az_iot_su_client_update_manifest_file* file,
    az_span download_url,
    uint32_t file_index,
    uint32_t file_count,
    void* user_ctx)
{
  su_ota_ctx* c = (su_ota_ctx*)user_ctx;

  /* This sample treats the single payload as the new ESP32 app image and
   * flashes it whole; multi-file updates would loop a partition/file map. */
  if (file_index != 0)
  {
    ESP_LOGW(TAG, "ignoring extra file %u/%u", file_index + 1, file_count);
    return AZ_IOT_SU_RESULT_SUCCESS;
  }

  char url[1024];
  if (!span_to_cstr(download_url, url, sizeof(url)))
  {
    ESP_LOGE(TAG, "download URL too long");
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  c->update_partition = esp_ota_get_next_update_partition(NULL);
  if (!c->update_partition)
  {
    ESP_LOGE(TAG, "no free OTA partition");
    return AZ_IOT_SU_RESULT_FAILURE;
  }
  ESP_LOGI(
      TAG,
      "downloading %lld bytes -> partition '%s' @0x%08" PRIx32,
      (long long)file->size_in_bytes,
      c->update_partition->label,
      c->update_partition->address);

  esp_http_client_config_t http = {
    .url = url,
    .crt_bundle_attach = esp_crt_bundle_attach,
    .timeout_ms = 20000,
    .keep_alive_enable = true,
    .buffer_size = 2048,
  };
  esp_http_client_handle_t client = esp_http_client_init(&http);
  if (!client)
  {
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  int32_t result = AZ_IOT_SU_RESULT_FAILURE;
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
    goto cleanup;
  }
  esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  if (status != 200 && status != 206)
  {
    ESP_LOGE(TAG, "http status %d", status);
    goto cleanup;
  }

  if (esp_ota_begin(c->update_partition, OTA_SIZE_UNKNOWN, &c->ota_handle) != ESP_OK)
  {
    ESP_LOGE(TAG, "esp_ota_begin failed");
    goto cleanup;
  }
  c->ota_in_progress = true;
  c->written = 0;

  uint8_t buf[2048];
  for (;;)
  {
    int r = esp_http_client_read(client, (char*)buf, sizeof(buf));
    if (r < 0)
    {
      ESP_LOGE(TAG, "http read error");
      goto cleanup;
    }
    if (r == 0)
    {
      if (esp_http_client_is_complete_data_received(client))
      {
        break;
      }
      if (esp_http_client_is_chunked_response(client))
      {
        break;
      }
      break;
    }
    if (esp_ota_write(c->ota_handle, buf, (size_t)r) != ESP_OK)
    {
      ESP_LOGE(TAG, "esp_ota_write failed at %zu", c->written);
      goto cleanup;
    }
    c->written += (size_t)r;
  }

  ESP_LOGI(TAG, "download complete: %zu bytes written", c->written);
  result = AZ_IOT_SU_RESULT_SUCCESS;

cleanup:
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (result != AZ_IOT_SU_RESULT_SUCCESS && c->ota_in_progress)
  {
    esp_ota_abort(c->ota_handle);
    c->ota_in_progress = false;
  }
  return result;
}

/* ------------------------------------------------------------------------- */
/* read back for hash verification                                           */
/* ------------------------------------------------------------------------- */

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
  su_ota_ctx* c = (su_ota_ctx*)user_ctx;
  if (!c->update_partition)
  {
    *out_read = 0;
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  size_t size
      = c->written ? c->written : (file->size_in_bytes > 0 ? (size_t)file->size_in_bytes : 0);
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
  su_ota_ctx* c = (su_ota_ctx*)user_ctx;
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
  ESP_LOGI(TAG, "backup step %u (implicit A/B rollback partition)", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t ota_install(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  su_ota_ctx* c = (su_ota_ctx*)user_ctx;
  if (!c->ota_in_progress)
  {
    ESP_LOGE(TAG, "install step %u with no downloaded image", step);
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  /* Finalize + validate the written image (checks magic + SHA-256 footer). */
  esp_err_t err = esp_ota_end(c->ota_handle);
  c->ota_in_progress = false;
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  err = esp_ota_set_boot_partition(c->update_partition);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    return AZ_IOT_SU_RESULT_FAILURE;
  }

  ESP_LOGI(TAG, "install step %u complete; reboot required to apply", step);
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
  ESP_LOGI(TAG, "apply step %u (new image already running)", step);
  return AZ_IOT_SU_RESULT_SUCCESS;
}

static int32_t ota_restore(
    const az_iot_su_client_update_manifest* manifest,
    uint32_t step,
    void* user_ctx)
{
  (void)manifest;
  su_ota_ctx* c = (su_ota_ctx*)user_ctx;
  ESP_LOGW(TAG, "restore step %u (rollback to running partition)", step);
  if (c->ota_in_progress)
  {
    esp_ota_abort(c->ota_handle);
    c->ota_in_progress = false;
  }
  /* Point boot back at the currently-running (good) partition. */
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running)
  {
    esp_ota_set_boot_partition(running);
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
      ESP_LOGI(TAG, "cleared persisted workflow state from NVS");
      result = AZ_IOT_SU_RESULT_SUCCESS;
    }
  }
  else if (nvs_set_blob(h, SU_NVS_STATE_KEY, blob, len) == ESP_OK && nvs_commit(h) == ESP_OK)
  {
    ESP_LOGI(TAG, "persisted %zu bytes of workflow state to NVS", len);
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

void su_esp32_ota_mark_valid(void)
{
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK
      && state == ESP_OTA_IMG_PENDING_VERIFY)
  {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
    {
      ESP_LOGI(TAG, "image confirmed valid; rollback cancelled");
    }
  }
}

az_iot_su_platform_hooks su_esp32_ota_hooks(su_ota_ctx* ctx)
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
  h.persist_state_fn = ota_persist;
  h.load_state_fn = ota_load;
  h.user_ctx = ctx;
  return h;
}
