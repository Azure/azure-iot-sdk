// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Desktop fakes of the ESP-IDF APIs the software updates OTA hooks call. Behaviour:
 * fake_esp_ota.h. */
#include "fake_esp_ota.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "nvs.h"

/** @brief First byte of an ESP32 app image. */
#define FAKE_ESP_IMAGE_HEADER_MAGIC 0xE9

fake_esp_ota_state g_fake_esp_ota;

static esp_ota_handle_t g_next_ota_handle;

static const esp_partition_t k_running_partition = { 0x10000u, 0x100000u, "ota_0" };
static const esp_partition_t k_update_partition
    = { 0x110000u, (uint32_t)FAKE_ESP_OTA_PARTITION_SIZE, "ota_1" };

/** @brief An HTTP client serving the body set when it was created. */
struct esp_http_client
{
  const uint8_t* body;
  size_t body_size;
  size_t offset;
};

void fake_esp_ota_reset(void)
{
  memset(&g_fake_esp_ota, 0, sizeof(g_fake_esp_ota));
  g_fake_esp_ota.http_status_code = 200;
  g_fake_esp_ota.http_read_size = 1000;
  g_next_ota_handle = 0;
}

void fake_esp_ota_serve(const void* body, size_t size)
{
  g_fake_esp_ota.http_body = (const uint8_t*)body;
  g_fake_esp_ota.http_body_size = size;
}

int fake_esp_ota_open_http_clients(void)
{
  return g_fake_esp_ota.http_inits - g_fake_esp_ota.http_cleanups;
}

int fake_esp_ota_open_write_sessions(void)
{
  int open = 0;
  for (size_t i = 0; i < FAKE_ESP_OTA_MAX_WRITE_SESSIONS; ++i)
  {
    open += g_fake_esp_ota.write_sessions[i].open ? 1 : 0;
  }
  return open;
}

const esp_partition_t* fake_esp_ota_update_partition(void) { return &k_update_partition; }

const char* esp_err_to_name(esp_err_t code)
{
  switch (code)
  {
    case ESP_OK:
      return "ESP_OK";
    case ESP_ERR_INVALID_ARG:
      return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_OTA_VALIDATE_FAILED:
      return "ESP_ERR_OTA_VALIDATE_FAILED";
    default:
      return "ESP_FAIL";
  }
}

/* ------------------------------------------------------------------------- */
/* HTTP client, certificate bundle, timer                                    */
/* ------------------------------------------------------------------------- */

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config)
{
  struct esp_http_client* client = (struct esp_http_client*)calloc(1, sizeof(*client));
  if (client == NULL)
  {
    return NULL;
  }
  client->body = g_fake_esp_ota.http_body;
  client->body_size = g_fake_esp_ota.http_body_size;
  g_fake_esp_ota.http_inits++;
  size_t url_length = strlen(config->url);
  url_length = (url_length < sizeof(g_fake_esp_ota.http_last_url))
      ? url_length
      : sizeof(g_fake_esp_ota.http_last_url) - 1u;
  memcpy(g_fake_esp_ota.http_last_url, config->url, url_length);
  g_fake_esp_ota.http_last_url[url_length] = '\0';
  return client;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len)
{
  (void)client;
  (void)write_len;
  g_fake_esp_ota.http_opens++;
  return ESP_OK;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
  return (int64_t)client->body_size;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
  (void)client;
  return g_fake_esp_ota.http_status_code;
}

int esp_http_client_read(esp_http_client_handle_t client, char* buffer, int len)
{
  size_t size = client->body_size - client->offset;
  size = (len > 0 && size > (size_t)len) ? (size_t)len : size;
  size = (size > g_fake_esp_ota.http_read_size) ? g_fake_esp_ota.http_read_size : size;
  if (size > 0)
  {
    memcpy(buffer, client->body + client->offset, size);
  }
  client->offset += size;
  return (int)size;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t client)
{
  (void)client;
  g_fake_esp_ota.http_closes++;
  return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
  g_fake_esp_ota.http_cleanups++;
  free(client);
  return ESP_OK;
}

esp_err_t esp_crt_bundle_attach(void* conf)
{
  (void)conf;
  return ESP_OK;
}

int64_t esp_timer_get_time(void) { return 0; }

/* ------------------------------------------------------------------------- */
/* OTA and partitions                                                        */
/* ------------------------------------------------------------------------- */

/** @brief The open write session of @p handle, or NULL. */
static fake_esp_ota_write_session* find_open_write_session(esp_ota_handle_t handle)
{
  for (size_t i = 0; i < FAKE_ESP_OTA_MAX_WRITE_SESSIONS; ++i)
  {
    fake_esp_ota_write_session* session = &g_fake_esp_ota.write_sessions[i];
    if (session->open && session->handle == handle)
    {
      return session;
    }
  }
  return NULL;
}

const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* start_from)
{
  (void)start_from;
  g_fake_esp_ota.update_partition_lookups++;
  return g_fake_esp_ota.no_update_partition ? NULL : &k_update_partition;
}

const esp_partition_t* esp_ota_get_running_partition(void) { return &k_running_partition; }

esp_err_t esp_ota_begin(
    const esp_partition_t* partition,
    size_t image_size,
    esp_ota_handle_t* out_handle)
{
  (void)image_size;
  if (partition != &k_update_partition || out_handle == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }
  fake_esp_ota_write_session* session = NULL;
  for (size_t i = 0; session == NULL && i < FAKE_ESP_OTA_MAX_WRITE_SESSIONS; ++i)
  {
    if (!g_fake_esp_ota.write_sessions[i].open)
    {
      session = &g_fake_esp_ota.write_sessions[i];
    }
  }
  if (session == NULL)
  {
    return ESP_FAIL;
  }
  g_fake_esp_ota.ota_begins++;
  session->handle = ++g_next_ota_handle;
  session->open = true;
  session->bytes_written = 0;
  /* OTA_SIZE_UNKNOWN erases the whole partition. */
  memset(g_fake_esp_ota.update_partition_data, 0xFF, sizeof(g_fake_esp_ota.update_partition_data));
  *out_handle = session->handle;
  return ESP_OK;
}

esp_err_t esp_ota_write(esp_ota_handle_t handle, const void* data, size_t size)
{
  fake_esp_ota_write_session* session = find_open_write_session(handle);
  if (session == NULL || data == NULL)
  {
    return ESP_ERR_INVALID_ARG;
  }
  if (size == 0)
  {
    return ESP_OK;
  }
  g_fake_esp_ota.ota_writes++;
  if (session->bytes_written == 0 && ((const uint8_t*)data)[0] != FAKE_ESP_IMAGE_HEADER_MAGIC)
  {
    return ESP_ERR_OTA_VALIDATE_FAILED;
  }
  if (size > FAKE_ESP_OTA_PARTITION_SIZE - session->bytes_written)
  {
    return ESP_ERR_INVALID_SIZE;
  }
  memcpy(g_fake_esp_ota.update_partition_data + session->bytes_written, data, size);
  session->bytes_written += size;
  g_fake_esp_ota.ota_bytes_written += size;
  return ESP_OK;
}

esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
  fake_esp_ota_write_session* session = find_open_write_session(handle);
  if (session == NULL)
  {
    return ESP_ERR_NOT_FOUND;
  }
  g_fake_esp_ota.ota_ends++;
  session->open = false;
  return (session->bytes_written == 0) ? ESP_ERR_INVALID_ARG : ESP_OK;
}

esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
  fake_esp_ota_write_session* session = find_open_write_session(handle);
  if (session == NULL)
  {
    return ESP_ERR_NOT_FOUND;
  }
  g_fake_esp_ota.ota_aborts++;
  session->open = false;
  return ESP_OK;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition)
{
  g_fake_esp_ota.ota_set_boots++;
  g_fake_esp_ota.boot_partition = partition;
  return ESP_OK;
}

esp_err_t esp_ota_get_state_partition(
    const esp_partition_t* partition,
    esp_ota_img_states_t* ota_state)
{
  (void)partition;
  *ota_state = ESP_OTA_IMG_VALID;
  return ESP_OK;
}

esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { return ESP_OK; }

esp_err_t esp_partition_read(
    const esp_partition_t* partition,
    size_t src_offset,
    void* dst,
    size_t size)
{
  if (partition != &k_update_partition || src_offset > FAKE_ESP_OTA_PARTITION_SIZE
      || size > FAKE_ESP_OTA_PARTITION_SIZE - src_offset)
  {
    return ESP_ERR_INVALID_ARG;
  }
  memcpy(dst, g_fake_esp_ota.update_partition_data + src_offset, size);
  return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* NVS: never available                                                      */
/* ------------------------------------------------------------------------- */

esp_err_t nvs_open(const char* namespace_name, nvs_open_mode_t open_mode, nvs_handle_t* out_handle)
{
  (void)namespace_name;
  (void)open_mode;
  (void)out_handle;
  return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length)
{
  (void)handle;
  (void)key;
  (void)value;
  (void)length;
  return ESP_FAIL;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length)
{
  (void)handle;
  (void)key;
  (void)out_value;
  (void)length;
  return ESP_FAIL;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key)
{
  (void)handle;
  (void)key;
  return ESP_FAIL;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
  (void)handle;
  return ESP_FAIL;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }
