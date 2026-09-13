// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* adu/esp32 - real Azure Device Update (ADU) over-the-air firmware update on an
 * ESP32-WROOM, end to end:
 *
 *   Wi-Fi -> DPS provisioning (X.509) -> IoT Hub (esp-mqtt) -> twin/ADU ->
 *   manifest JWS verification (mbedTLS) -> HTTPS download straight into the
 *   inactive OTA partition -> per-file SHA-256 check -> set boot partition ->
 *   reboot -> resume the workflow in the new image -> report the new version.
 *
 * Unlike the desktop "adu/pc" sample, nothing here is simulated: the payload is
 * a genuine ESP32 app image and install_fn flashes it with the native OTA stack.
 *
 * Configure Wi-Fi + DPS via `idf.py menuconfig` (see Kconfig.projbuild). The
 * device certificate/key (and optionally the CA) are compiled into the firmware
 * from main/certs/ (EMBED_TXTFILES).
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "azure/iot/az_iot.h"
#include "azure/iot/az_iot_adu.h"

#include "adu_version.h"
#include "wifi_connect.h"
#include "az_iot_mqtt_esp.h"
#include "az_iot_cert_embedded.h"
#include "az_iot_adu_crypto_mbedtls.h"
#include "adu_esp32_ota.h"

static const char* TAG = "adu_esp32";

/* PEM material compiled into the firmware (see main/CMakeLists.txt EMBED_TXTFILES).
 * Text embedding NUL-terminates each blob, so the *_start symbol is a C string. */
extern const char device_cert_pem_start[] asm("_binary_device_cert_pem_start");
extern const char device_key_pem_start[] asm("_binary_device_key_pem_start");
extern const char trusted_ca_pem_start[] asm("_binary_trusted_ca_pem_start");

/* ------------------------------------------------------------------------- */
/* connection-state logging                                                  */
/* ------------------------------------------------------------------------- */

static az_iot_connection_state g_conn_state = AZ_IOT_CONN_STATE_IDLE;

static void on_conn_state(const az_iot_connection_state_event* event, void* ctx)
{
  az_iot_connection_state st = event->state;
  az_iot_result reason = event->reason;
  (void)ctx;
  if (st != g_conn_state)
  {
    ESP_LOGI(
        TAG,
        "connection: %s -> %s (reason=0x%08x)",
        az_iot_connection_state_to_string(g_conn_state),
        az_iot_connection_state_to_string(st),
        (unsigned)reason);
  }
  g_conn_state = st;
}

static const char* adu_state_name(az_iot_adu_state s)
{
  switch (s)
  {
    case AZ_IOT_ADU_STATE_IDLE:
      return "Idle";
    case AZ_IOT_ADU_STATE_MANIFEST_RECEIVED:
      return "ManifestReceived";
    case AZ_IOT_ADU_STATE_VERIFYING_MANIFEST:
      return "VerifyingManifest";
    case AZ_IOT_ADU_STATE_DOWNLOAD_STARTED:
      return "DownloadStarted";
    case AZ_IOT_ADU_STATE_DOWNLOAD_COMPLETE:
      return "DownloadComplete";
    case AZ_IOT_ADU_STATE_BACKUP_STARTED:
      return "BackupStarted";
    case AZ_IOT_ADU_STATE_BACKUP_COMPLETE:
      return "BackupComplete";
    case AZ_IOT_ADU_STATE_INSTALL_STARTED:
      return "InstallStarted";
    case AZ_IOT_ADU_STATE_INSTALL_COMPLETE:
      return "InstallComplete";
    case AZ_IOT_ADU_STATE_APPLY_STARTED:
      return "ApplyStarted";
    case AZ_IOT_ADU_STATE_RESTORE_STARTED:
      return "RestoreStarted";
    case AZ_IOT_ADU_STATE_FAILED:
      return "Failed";
    default:
      return "?";
  }
}

/* ------------------------------------------------------------------------- */
/* app entry                                                                 */
/* ------------------------------------------------------------------------- */

void app_main(void)
{
  ESP_LOGI(TAG, "ADU ESP32 sample starting (firmware version %s)", ADU_UPDATE_VERSION);

  /* NVS is required by Wi-Fi and used to persist the ADU resume blob. */
  esp_err_t nvs = nvs_flash_init();
  if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  if (wifi_connect_blocking() != ESP_OK)
  {
    ESP_LOGE(TAG, "Wi-Fi connect failed; rebooting");
    esp_restart();
  }

  az_iot_log_sink log = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
  az_iot_log_set_global_sink(&log);

  /* Certificate provider: hands the SDK the embedded PEM material. */
  az_iot_cert_embedded certs;
  az_iot_cert_embedded_init(
      &certs, trusted_ca_pem_start, device_cert_pem_start, device_key_pem_start);

  /* Connection client: DPS provisioning + X.509, announcing the ADU model id. */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.dps.id_scope = CONFIG_ADU_DPS_ID_SCOPE;
  copts.dps.registration_id = CONFIG_ADU_DPS_REGISTRATION_ID;
  copts.certificate_provider = &certs.base;
  copts.model_id = "dtmi:azure:iot:deviceUpdateContractModel;2";
  copts.reconnection_policy.initial_delay_ms = 2000;
  copts.reconnection_policy.max_delay_ms = 60000;
  copts.reconnection_policy.max_attempts = 0; /* retry forever */
  copts.reconnection_policy.jitter_pct = 20;

  az_iot_connection_client conn;
  if (az_iot_connection_client_init(&conn, &copts) != AZ_IOT_OK)
  {
    ESP_LOGE(TAG, "connection_client_init failed");
    esp_restart();
  }
  az_iot_connection_client_set_state_callback(&conn, on_conn_state, NULL);

  if (az_iot_connection_client_register_mqtt_factory(&conn, az_iot_esp_mqtt_factory_create_v3_1_1())
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&conn, az_iot_esp_mqtt_factory_create_v5())
          != AZ_IOT_OK)
  {
    ESP_LOGE(TAG, "register mqtt factory failed");
    esp_restart();
  }

  /* Twin client (ADU registers as a desired-property subscriber on it). */
  az_iot_twin_client twin;
  if (az_iot_twin_client_init(&twin, &conn) != AZ_IOT_OK)
  {
    ESP_LOGE(TAG, "twin_client_init failed");
    esp_restart();
  }

  /* Real OTA platform hooks + mbedTLS crypto + Microsoft root keys. */
  adu_ota_ctx ota = { 0 };
  ota.installed_version = ADU_UPDATE_VERSION;
  az_iot_adu_platform_hooks hooks = adu_esp32_ota_hooks(&ota);
  az_iot_adu_crypto_hooks crypto = az_iot_adu_crypto_mbedtls_hooks();

  size_t root_key_count = 0;
  const az_iot_adu_root_key* root_keys = az_iot_adu_microsoft_root_keys(&root_key_count);

  az_iot_adu_device_properties dp = { 0 };
  dp.manufacturer = "Espressif";
  dp.model = "ESP32-WROOM";
  dp.installed_update_id.provider = ADU_UPDATE_PROVIDER;
  dp.installed_update_id.name = ADU_UPDATE_NAME;
  dp.installed_update_id.version = ADU_UPDATE_VERSION;

  static AZ_IOT_ADU_DEVICE_PROPS_STORAGE(dp_buffer);
  az_iot_adu_client_t adu;
  az_iot_adu_client_config_options adu_opts = az_iot_adu_client_config_options_default();
  adu_opts.hooks = &hooks;
  adu_opts.crypto = &crypto;
  adu_opts.root_keys = root_keys;
  adu_opts.root_key_count = root_key_count;
  adu_opts.device_props = &dp;
  adu_opts.device_props_buffer = dp_buffer;
  adu_opts.device_props_buffer_size = sizeof(dp_buffer);
  if (az_iot_adu_client_initialize(&adu, &twin, &adu_opts) != AZ_IOT_OK)
  {
    ESP_LOGE(TAG, "adu_client_initialize failed");
    esp_restart();
  }

  /* Resume a workflow that was mid-flight before this (post-OTA) reboot. */
  if (az_iot_adu_client_resume(&adu) == AZ_IOT_OK
      && az_iot_adu_client_get_state(&adu) != AZ_IOT_ADU_STATE_IDLE)
  {
    ESP_LOGI(
        TAG,
        "resumed persisted workflow at state: %s",
        adu_state_name(az_iot_adu_client_get_state(&adu)));
  }

  if (az_iot_connection_client_open(&conn) != AZ_IOT_OK)
  {
    ESP_LOGE(TAG, "connection open failed");
    esp_restart();
  }

  /* Pump until connected (or faulted). The device-update client is ticked here
   * too: its first update check runs on the provisioning session, BEFORE the
   * device registers, so pumping only the connection client would leave that
   * check unissued and the hold would simply expire.
   *
   * The bound must exceed AZ_IOT_DPS_HOLD_TIMEOUT_MS: a fixed 1200 iterations
   * at 50 ms was exactly the hold timeout, so a stalled check would have ended
   * this loop on the same tick the hold expired and the device would have
   * looked unreachable instead of registering anyway. */
  /* Derived from the hold timeout rather than hard-coded: a build that raises
   * AZ_IOT_DPS_HOLD_TIMEOUT_MS must not have this loop give up while the
   * connection is still legitimately holding. Twice the hold leaves room for
   * the registration that follows it. */
  const unsigned tick_ms = 50u;
  const unsigned max_ticks = (2u * (unsigned)AZ_IOT_DPS_HOLD_TIMEOUT_MS) / tick_ms;
  for (unsigned i = 0; i < max_ticks && g_conn_state != AZ_IOT_CONN_STATE_CONNECTED; ++i)
  {
    (void)az_iot_connection_client_do_work(&conn, 50);
    (void)az_iot_adu_client_do_work(&adu);
    if (g_conn_state == AZ_IOT_CONN_STATE_FAULTED)
    {
      break;
    }
  }

  if (g_conn_state != AZ_IOT_CONN_STATE_CONNECTED)
  {
    ESP_LOGE(TAG, "could not connect; rebooting");
    esp_restart();
  }

  /* Connected to IoT Hub: this image works, so cancel any pending rollback. */
  adu_esp32_ota_mark_valid();

  ESP_LOGI(
      TAG, "connected; reporting Espressif/ESP32-WROOM installedUpdateId=%s", ADU_UPDATE_VERSION);
  ESP_LOGI(TAG, "waiting for a deployment...");

  az_iot_adu_state prev = az_iot_adu_client_get_state(&adu);
  for (;;)
  {
    (void)az_iot_connection_client_do_work(&conn, 50);
    (void)az_iot_adu_client_do_work(&adu);

    az_iot_adu_state cur = az_iot_adu_client_get_state(&adu);
    if (cur != prev)
    {
      ESP_LOGI(TAG, "ADU state: %s -> %s", adu_state_name(prev), adu_state_name(cur));
      prev = cur;
    }

    /* install_fn asked for a reboot to boot the freshly flashed image. The
     * ADU core has already persisted the workflow blob to NVS, so resume()
     * picks it up after the restart. */
    if (ota.reboot_pending)
    {
      ESP_LOGI(TAG, "rebooting into the new firmware to apply the update");
      vTaskDelay(pdMS_TO_TICKS(500));
      esp_restart();
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
