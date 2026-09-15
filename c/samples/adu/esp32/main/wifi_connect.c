// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
#include "wifi_connect.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char* TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRY 10

static EventGroupHandle_t s_wifi_events;
static int s_retry_count;

static void wifi_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
  (void)arg;
  (void)data;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
  {
    esp_wifi_connect();
  }
  else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
  {
    if (s_retry_count < WIFI_MAX_RETRY)
    {
      esp_wifi_connect();
      s_retry_count++;
      ESP_LOGW(TAG, "retrying Wi-Fi connect (%d/%d)", s_retry_count, WIFI_MAX_RETRY);
    }
    else
    {
      xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
    }
  }
  else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t* evt = (ip_event_got_ip_t*)data;
    ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
    s_retry_count = 0;
    xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
  }
}

esp_err_t wifi_connect_blocking(void)
{
  s_wifi_events = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

  wifi_config_t cfg = { 0 };
  strncpy((char*)cfg.sta.ssid, CONFIG_ADU_WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
  strncpy((char*)cfg.sta.password, CONFIG_ADU_WIFI_PASSWORD, sizeof(cfg.sta.password) - 1);
  cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "connecting to SSID '%s'", CONFIG_ADU_WIFI_SSID);
  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

  return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}
