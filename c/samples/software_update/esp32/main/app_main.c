// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* software_update/esp32 - real Azure software updates over-the-air firmware update on an
 * ESP32-WROOM, end to end:
 *
 *   Wi-Fi -> DPS provisioning (X.509, esp-mqtt) -> software update check ->
 *   manifest JWS verification (mbedTLS) -> HTTPS download straight into the
 *   inactive OTA partition -> per-file SHA-256 check -> set boot partition ->
 *   reboot -> resume the workflow in the new image -> report the new version.
 *
 * Unlike the desktop "software_update/pc" samples, nothing here is simulated: the
 * payload is a genuine ESP32 app image and install_fn flashes it with the native
 * OTA stack.
 *
 * Route: a device that has never connected to its hub has no device record, so
 * it asks on the onboarding route; once it has (recorded in NVS), it asks on
 * the regular route. After connecting it checks every SU_POLL_INTERVAL_S.
 *
 * Configure Wi-Fi + DPS via `idf.py menuconfig` (see Kconfig.projbuild). The
 * device certificate/key (and optionally the CA) are compiled into the firmware
 * from main/certs/ (EMBED_TXTFILES).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "azure/iot/az_iot.h"
#include "azure/iot/az_iot_su.h"

#include "su_version.h"
#include "wifi_connect.h"
#include "az_iot_mqtt_esp.h"
#include "az_iot_cert_embedded.h"
#include "az_iot_su_crypto_mbedtls.h"
#include "su_esp32_ota.h"

static const char* TAG = "su_esp32";

/* PEM material compiled into the firmware (see main/CMakeLists.txt EMBED_TXTFILES).
 * Text embedding NUL-terminates each blob, so the *_start symbol is a C string. */
extern const char device_cert_pem_start[] asm("_binary_device_cert_pem_start");
extern const char device_key_pem_start[] asm("_binary_device_key_pem_start");
extern const char trusted_ca_pem_start[] asm("_binary_trusted_ca_pem_start");

/* ------------------------------------------------------------------------- */
/* connection-state logging                                                  */
/* ------------------------------------------------------------------------- */

static az_iot_connection_state g_conn_state = AZ_IOT_CONN_STATE_IDLE;
static int g_provisioning_faulted;

static void on_conn_state(const az_iot_connection_state_event* event, void* ctx)
{
  /* A rejected assignment or a failed registration faults the provisioning
   * lifecycle and leaves the hub IDLE, so the wait below must watch for it. */
  if (event->scope == AZ_IOT_CONN_SCOPE_DPS)
  {
    if (event->state == AZ_IOT_CONN_STATE_FAULTED)
    {
      g_provisioning_faulted = 1;
    }
    return;
  }

  /* Hub lifecycle only: the provisioning session reports on its own scope,
   * and storing its state here would overwrite the hub state this code acts
   * on. */
  if (event->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }

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

static const char* su_state_name(az_iot_su_state s)
{
  switch (s)
  {
    case AZ_IOT_SU_STATE_IDLE:
      return "Idle";
    case AZ_IOT_SU_STATE_MANIFEST_RECEIVED:
      return "ManifestReceived";
    case AZ_IOT_SU_STATE_VERIFYING_MANIFEST:
      return "VerifyingManifest";
    case AZ_IOT_SU_STATE_DOWNLOAD_STARTED:
      return "DownloadStarted";
    case AZ_IOT_SU_STATE_DOWNLOAD_COMPLETE:
      return "DownloadComplete";
    case AZ_IOT_SU_STATE_BACKUP_STARTED:
      return "BackupStarted";
    case AZ_IOT_SU_STATE_BACKUP_COMPLETE:
      return "BackupComplete";
    case AZ_IOT_SU_STATE_INSTALL_STARTED:
      return "InstallStarted";
    case AZ_IOT_SU_STATE_INSTALL_COMPLETE:
      return "InstallComplete";
    case AZ_IOT_SU_STATE_APPLY_STARTED:
      return "ApplyStarted";
    case AZ_IOT_SU_STATE_RESTORE_STARTED:
      return "RestoreStarted";
    case AZ_IOT_SU_STATE_FAILED:
      return "Failed";
    default:
      return "?";
  }
}

/* ------------------------------------------------------------------------- */
/* update-check route                                                        */
/* ------------------------------------------------------------------------- */

/* Separate from the adapter's "su_sample" namespace, which holds the resume blob. */
#define APP_NVS_NAMESPACE "su_app"
#define APP_NVS_REGISTERED_KEY "registered"

/**
 * @brief Whether this device has connected to its hub before, so has a device
 * record and must use the regular route.
 */
static bool app_is_registered(void)
{
  nvs_handle_t h;
  uint8_t v = 0;
  if (nvs_open(APP_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK)
  {
    (void)nvs_get_u8(h, APP_NVS_REGISTERED_KEY, &v);
    nvs_close(h);
  }
  return v != 0;
}

/** @brief Record that the device has a device record. Logged on failure only. */
static void app_set_registered(void)
{
  nvs_handle_t h;
  if (nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
  {
    ESP_LOGW(TAG, "could not record registration; the next boot asks on the onboarding route");
    return;
  }
  if (nvs_set_u8(h, APP_NVS_REGISTERED_KEY, 1) != ESP_OK || nvs_commit(h) != ESP_OK)
  {
    ESP_LOGW(TAG, "could not record registration; the next boot asks on the onboarding route");
  }
  nvs_close(h);
}

/**
 * @brief Ask for an update check on the route this device qualifies for.
 *
 * @param su Software updates client.
 * @param registered Whether the device has a device record (regular route).
 * @param timeout_ms Request timeout.
 * @return The request call's result.
 */
static az_iot_result app_request_check(az_iot_su_client* su, bool registered, uint32_t timeout_ms)
{
  ESP_LOGI(TAG, "checking for updates on the %s route", registered ? "regular" : "onboarding");
  return registered ? az_iot_su_client_request_update(su, timeout_ms)
                    : az_iot_su_client_request_onboarding_update(su, timeout_ms);
}

/* Service-requested delay before the next check, set by on_su_event. */
static uint32_t g_retry_after_ms;
/* persist_state_fn is failing: the checkpoint a reboot needs is not stored. */
static bool g_persist_failing;

/** @brief Logs abandoned operations (the poll loop asks again later) and tracks persist failures.
 */
static void on_su_event(const az_iot_su_event* event, void* ctx)
{
  (void)ctx;
  /* `service_error` is the last field read; an older library's shorter event
   * is ignored rather than read past its end. */
  if (event->_internal_size
      < offsetof(az_iot_su_event, service_error) + sizeof(event->service_error))
  {
    return;
  }
  if (event->kind == AZ_IOT_SU_EVENT_OPERATION_ABANDONED)
  {
    ESP_LOGW(
        TAG,
        "operation %d abandoned: reason=0x%08x code=%d message=\"%s\" trackingId=\"%s\"",
        (int)event->operation,
        (unsigned)event->reason,
        (int)event->service_error.code,
        event->service_error.message,
        event->service_error.tracking_id);
    if (event->operation != AZ_IOT_SU_OP_REPORT_STATUS)
    {
      g_retry_after_ms = event->service_error.retry_after_ms;
    }
  }
  else if (event->kind == AZ_IOT_SU_EVENT_PERSIST_FAILED)
  {
    ESP_LOGE(TAG, "NVS write failing; update reboot deferred");
    g_persist_failing = true;
  }
  else if (event->kind == AZ_IOT_SU_EVENT_PERSIST_RECOVERED)
  {
    g_persist_failing = false;
  }
}

/* ------------------------------------------------------------------------- */
/* app entry                                                                 */
/* ------------------------------------------------------------------------- */

void app_main(void)
{
  ESP_LOGI(TAG, "Software updates ESP32 sample starting (firmware version %s)", SU_UPDATE_VERSION);

  /* NVS is required by Wi-Fi and used to persist the software updates resume blob. */
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

  /* Connection client: DPS provisioning + X.509. */
  az_iot_connection_client_options copts = az_iot_connection_client_options_default();
  copts.dps.id_scope = CONFIG_SU_DPS_ID_SCOPE;
  copts.dps.registration_id = CONFIG_SU_DPS_REGISTRATION_ID;
  copts.certificate_provider = &certs.base;

  az_iot_connection_client conn;
  if (az_iot_connection_client_init(&conn, &copts) != AZ_IOT_OK)
  {
    ESP_LOGE(TAG, "connection_client_init failed");
    esp_restart();
  }
  az_iot_connection_client_add_state_observer(&conn, on_conn_state, NULL);

  if (az_iot_connection_client_register_mqtt_factory(&conn, az_iot_esp_mqtt_factory_create_v3_1_1())
          != AZ_IOT_OK
      || az_iot_connection_client_register_mqtt_factory(&conn, az_iot_esp_mqtt_factory_create_v5())
          != AZ_IOT_OK)
  {
    ESP_LOGE(TAG, "register mqtt factory failed");
    esp_restart();
  }

  /* Real OTA platform hooks + mbedTLS crypto + Microsoft root keys. */
  su_ota_ctx ota = { 0 };
  ota.installed_version = SU_UPDATE_VERSION;
  az_iot_su_platform_hooks hooks = su_esp32_ota_hooks(&ota);
  az_iot_su_crypto_hooks crypto = az_iot_su_crypto_mbedtls_hooks();

  size_t root_key_count = 0;
  const az_iot_su_root_key* root_keys = az_iot_su_microsoft_root_keys(&root_key_count);

  az_iot_su_device_properties dp = { 0 };
  dp.manufacturer = "Espressif";
  dp.model = "ESP32-WROOM";
  dp.installed_update_id.provider = SU_UPDATE_PROVIDER;
  dp.installed_update_id.name = SU_UPDATE_NAME;
  dp.installed_update_id.version = SU_UPDATE_VERSION;

  static AZ_IOT_SU_DEVICE_PROPERTIES_STORAGE(dp_buffer);
  /* ~14 KB: too large for the 12 KB main task stack. */
  static az_iot_su_client su;
  az_iot_su_client_config_options su_opts = az_iot_su_client_config_options_default();
  su_opts.hooks = &hooks;
  su_opts.crypto = &crypto;
  su_opts.root_keys = root_keys;
  su_opts.root_key_count = root_key_count;
  su_opts.device_properties = &dp;
  su_opts.device_properties_buffer = dp_buffer;
  su_opts.device_properties_buffer_size = sizeof(dp_buffer);
  if (az_iot_su_client_init(&su, &conn, &su_opts) != AZ_IOT_OK)
  {
    ESP_LOGE(TAG, "az_iot_su_client_init failed");
    esp_restart();
  }

  /* Registered before resume(), which replays a persisted state to observers. */
  if (az_iot_su_client_add_observer(&su, on_su_event, NULL) != AZ_IOT_OK)
  {
    ESP_LOGE(TAG, "su_client_add_observer failed");
    esp_restart();
  }

  /* Resume a workflow that was mid-flight before this (post-OTA) reboot. */
  if (az_iot_su_client_resume(&su) == AZ_IOT_OK
      && az_iot_su_client_get_state(&su) != AZ_IOT_SU_STATE_IDLE)
  {
    ESP_LOGI(
        TAG,
        "resumed persisted workflow at state: %s",
        su_state_name(az_iot_su_client_get_state(&su)));
  }

  /* Nothing is fetched unless the application asks, and only it knows the
   * route: onboarding until the device has a device record, regular after.
   * Asked before open(), so the check runs on the first provisioning session,
   * whose registration the SDK holds until the check reaches a verdict.
   *
   * The timeout bounds how long the CLIENT keeps reissuing this check before
   * giving up and raising AZ_IOT_SU_EVENT_OPERATION_ABANDONED with
   * AZ_IOT_ERR_TIMEOUT -- otherwise an unservable check is retried on every
   * do_work() for the life of the client. AZ_IOT_SU_REQUEST_NO_TIMEOUT asks
   * for exactly that, and is the wrong default on a battery-powered device.
   * It is bounded to half the poll interval: a new request resets the
   * deadline, so a longer bound would let an unanswered check outlive every
   * poll. */
  const uint32_t request_timeout_ms
      = ((uint32_t)CONFIG_SU_POLL_INTERVAL_S * 500u < AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS)
      ? (uint32_t)CONFIG_SU_POLL_INTERVAL_S * 500u
      : AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS;
  const bool registered = app_is_registered();
  /* Only from Idle: a workflow restored by resume() (e.g. after the OTA
   * reboot) is finished first, since a check now could deliver a different
   * workflow, which supersedes it. The check is then asked in the connect loop
   * below, whose verdict releases the registration hold. */
  bool check_deferred = (az_iot_su_client_get_state(&su) != AZ_IOT_SU_STATE_IDLE);
  if (!check_deferred && app_request_check(&su, registered, request_timeout_ms) != AZ_IOT_OK)
  {
    ESP_LOGE(TAG, "could not request an update check");
    esp_restart();
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
    (void)az_iot_su_client_do_work(&su);
    if (check_deferred && az_iot_su_client_get_state(&su) == AZ_IOT_SU_STATE_IDLE)
    {
      check_deferred = false;
      if (app_request_check(&su, registered, request_timeout_ms) != AZ_IOT_OK)
      {
        ESP_LOGW(TAG, "could not request an update check");
      }
    }
    if (g_conn_state == AZ_IOT_CONN_STATE_FAULTED || g_provisioning_faulted)
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
  su_esp32_ota_mark_valid();

  /* Registration created the device record: later checks use the regular route. */
  if (!registered)
  {
    app_set_registered();
  }

  ESP_LOGI(
      TAG, "connected; reporting Espressif/ESP32-WROOM installedUpdateId=%s", SU_UPDATE_VERSION);
  ESP_LOGI(TAG, "checking for updates every %d s", CONFIG_SU_POLL_INTERVAL_S);

  /* Poll on the regular route while no deployment is in flight. Tick
   * arithmetic is unsigned, so it survives the counter wrapping. */
  const TickType_t poll_ticks = pdMS_TO_TICKS((uint32_t)CONFIG_SU_POLL_INTERVAL_S * 1000u);
  TickType_t last_check = xTaskGetTickCount();
  TickType_t wait_ticks = poll_ticks;

  az_iot_su_state prev = az_iot_su_client_get_state(&su);
  for (;;)
  {
    (void)az_iot_connection_client_do_work(&conn, 50);
    (void)az_iot_su_client_do_work(&su);

    az_iot_su_state cur = az_iot_su_client_get_state(&su);
    if (cur != prev)
    {
      ESP_LOGI(TAG, "Software updates state: %s -> %s", su_state_name(prev), su_state_name(cur));
      prev = cur;
    }

    /* The service's requested delay, when longer, pushes the next check. */
    if (g_retry_after_ms > 0)
    {
      TickType_t delay = pdMS_TO_TICKS(g_retry_after_ms);
      TickType_t elapsed = xTaskGetTickCount() - last_check;
      if (elapsed + delay > wait_ticks)
      {
        wait_ticks = elapsed + delay;
      }
      g_retry_after_ms = 0;
    }

    /* FAULTED is settled until close(): no further check could be sent, so
     * restart rather than poll forever. A workflow in flight finishes first. */
    if (cur == AZ_IOT_SU_STATE_IDLE
        && (g_conn_state == AZ_IOT_CONN_STATE_FAULTED || g_provisioning_faulted))
    {
      ESP_LOGE(TAG, "connection faulted; rebooting");
      esp_restart();
    }

    if (cur == AZ_IOT_SU_STATE_IDLE && xTaskGetTickCount() - last_check >= wait_ticks)
    {
      last_check = xTaskGetTickCount();
      wait_ticks = poll_ticks;
      if (az_iot_su_client_request_update(&su, request_timeout_ms) != AZ_IOT_OK)
      {
        ESP_LOGW(TAG, "could not request an update check");
      }
    }

    /* install_fn asked for a reboot to boot the freshly flashed image. Wait
     * while the NVS write is failing: resume() needs that record. If the client
     * gives up, the rollback clears reboot_pending. */
    if (ota.reboot_pending && !g_persist_failing)
    {
      ESP_LOGI(TAG, "rebooting into the new firmware to apply the update");
      vTaskDelay(pdMS_TO_TICKS(500));
      esp_restart();
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
