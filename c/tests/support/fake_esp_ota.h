// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file fake_esp_ota.h
 * @brief State of the desktop ESP-IDF OTA, partition, HTTP client and timer fakes (fake_esp_ota/).
 *
 * - HTTP: each client serves the body set when it was created, in reads of at most
 *   #fake_esp_ota_state::http_read_size bytes, then 0 (end of body).
 * - OTA: one update partition backed by #fake_esp_ota_state::update_partition_data. As in
 *   ESP-IDF, esp_ota_write() rejects an image not starting with 0xE9 and esp_ota_end() one with
 *   no bytes written; esp_ota_begin() does not refuse a second write session.
 * - Timer: a constant time, so only the byte budget ends a download_fn call.
 */
#ifndef FAKE_ESP_OTA_H
#define FAKE_ESP_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_client.h"
#include "esp_ota_ops.h"

/** @brief Capacity of the fake update partition. */
#define FAKE_ESP_OTA_PARTITION_SIZE ((size_t)64 * 1024)
/** @brief Write sessions esp_ota_begin() can track. */
#define FAKE_ESP_OTA_MAX_WRITE_SESSIONS 8

/** @brief An esp_ota_begin() write session. */
typedef struct fake_esp_ota_write_session
{
  esp_ota_handle_t handle;
  bool open;
  size_t bytes_written;
} fake_esp_ota_write_session;

/** @brief Server behaviour, OTA results and call records. */
typedef struct fake_esp_ota_state
{
  /* HTTP server. */
  const uint8_t* http_body; /**< Served by clients created from now on. */
  size_t http_body_size;
  int http_status_code;
  size_t http_read_size; /**< Most bytes one esp_http_client_read() returns. */
  int http_inits, http_opens, http_closes, http_cleanups;
  char http_last_url[256];

  /* OTA. */
  bool no_update_partition; /**< esp_ota_get_next_update_partition() returns NULL. */
  int update_partition_lookups;
  int ota_begins, ota_writes, ota_ends, ota_aborts, ota_set_boots;
  size_t ota_bytes_written; /**< Over all write sessions. */
  fake_esp_ota_write_session write_sessions[FAKE_ESP_OTA_MAX_WRITE_SESSIONS];
  const esp_partition_t* boot_partition;
  uint8_t update_partition_data[FAKE_ESP_OTA_PARTITION_SIZE];
} fake_esp_ota_state;

/** @brief The fakes' state; reset with fake_esp_ota_reset(). */
extern fake_esp_ota_state g_fake_esp_ota;

/** @brief Reset: no body, status 200, 1000-byte reads, nothing recorded. */
void fake_esp_ota_reset(void);

/** @brief Serve @p body, of @p size bytes, to clients created from now on. */
void fake_esp_ota_serve(const void* body, size_t size);

/** @brief HTTP clients created and not cleaned up. */
int fake_esp_ota_open_http_clients(void);

/** @brief OTA write sessions begun and neither ended nor aborted. */
int fake_esp_ota_open_write_sessions(void);

/** @brief The partition esp_ota_get_next_update_partition() returns. */
const esp_partition_t* fake_esp_ota_update_partition(void);

#endif /* FAKE_ESP_OTA_H */
