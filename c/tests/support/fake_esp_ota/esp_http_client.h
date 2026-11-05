// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Desktop stand-in for ESP-IDF's esp_http_client.h. Behaviour: fake_esp_ota.h. */
#ifndef FAKE_ESP_HTTP_CLIENT_H
#define FAKE_ESP_HTTP_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct esp_http_client* esp_http_client_handle_t;

/** @brief Client configuration; only the fields the OTA hooks set. */
typedef struct esp_http_client_config_t
{
  const char* url;
  int timeout_ms;
  bool keep_alive_enable;
  int buffer_size;
  esp_err_t (*crt_bundle_attach)(void* conf);
} esp_http_client_config_t;

/** @brief Create a client serving the current body; records the URL. */
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config);
/** @brief Open the connection; always succeeds. */
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
/** @brief Content length: the body size. */
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
/** @brief fake_esp_ota_state::http_status_code. */
int esp_http_client_get_status_code(esp_http_client_handle_t client);
/** @brief Read the next body bytes; 0 at the end. */
int esp_http_client_read(esp_http_client_handle_t client, char* buffer, int len);
/** @brief Close the connection; counted. */
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
/** @brief Free the client; counted. */
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

#endif /* FAKE_ESP_HTTP_CLIENT_H */
