// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/**
 * @file az_iot_su_esp32_ota.h
 * @brief Software updates platform hooks over the ESP-IDF OTA stack.
 *
 * - download: streams the update file over HTTPS into the inactive OTA partition, at most
 *   16 KiB and about 11 s per call (IN_PROGRESS in between), so the application's do_work()
 *   loop keeps the MQTT connection serviced.
 * - read_file: reads the written image back for the SHA-256 check against the manifest.
 * - install: validates the image, makes it the next boot partition and asks for a reboot.
 * - persist/load workflow state: NVS, so the workflow resumes after the reboot.
 *
 * ESP-IDF component: platforms/esp32/components/azure-iot-sdk-software-update-ota.
 */
#ifndef AZ_IOT_SU_ESP32_OTA_H
#define AZ_IOT_SU_ESP32_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "azure/iot/az_iot_su.h"

#include "esp_http_client.h"
#include "esp_ota_ops.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * @brief State of the OTA platform hooks. Zero-initialize, set #installed_version, and keep
   * it alive as long as the software updates client.
   */
  typedef struct az_iot_su_esp32_ota_context
  {
    /** @brief Version of the running firmware; an update with this version is installed. */
    const char* installed_version;
    /** @brief Inactive partition being written. Set by the hooks. */
    const esp_partition_t* update_partition;
    /** @brief Open OTA write session. Set by the hooks. */
    esp_ota_handle_t ota_handle;
    /** @brief Whether #ota_handle is open. Set by the hooks. */
    bool ota_in_progress;
    /** @brief Bytes written to #update_partition. Set by the hooks. */
    size_t bytes_written;
    /** @brief Open download; NULL between downloads. Set by the hooks. */
    esp_http_client_handle_t http_client;
    /** @brief Hash of the URL #http_client downloads. Set by the hooks. */
    uint32_t download_url_hash;
    /** @brief Set when an installed image waits for a reboot; the application reboots. */
    bool reboot_pending;
  } az_iot_su_esp32_ota_context;

  /**
   * @brief Get the platform hooks bound to @p context.
   *
   * @param[in] context Hook state; must outlive the software updates client.
   * @return Hooks for az_iot_su_client_config_options.hooks.
   */
  az_iot_su_platform_hooks az_iot_su_esp32_ota_get_platform_hooks(
      az_iot_su_esp32_ota_context* context);

  /**
   * @brief Software updates observer: releases a download, or an OTA write not yet
   * installed, that the software updates client abandoned without calling restore_fn
   * (a workflow replaced mid-download). Register it with az_iot_su_client_add_observer(),
   * with the same context as az_iot_su_esp32_ota_get_platform_hooks().
   *
   * @param[in] event   The event.
   * @param[in] context An #az_iot_su_esp32_ota_context.
   */
  void az_iot_su_esp32_ota_handle_event(const az_iot_su_event* event, void* context);

  /**
   * @brief Mark the running image valid, so the bootloader does not roll it back.
   *
   * No effect unless this boot is pending verification. Call once the image has proven
   * itself, for example after it connects to its hub.
   */
  void az_iot_su_esp32_ota_mark_running_image_valid(void);

#ifdef __cplusplus
}
#endif

#endif /* AZ_IOT_SU_ESP32_OTA_H */
