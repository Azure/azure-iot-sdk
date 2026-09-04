// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license
// information.

/* SPDX-License-Identifier: MIT */
/* Real ESP32 OTA platform hooks for the ADU client.
 *
 * Implements az_iot_adu_platform_hooks with the native ESP-IDF OTA stack:
 *   - download_fn  : streams the ADU-provided HTTPS blob straight into the
 *                    inactive OTA partition (esp_http_client + esp_ota_write).
 *   - read_file_fn : reads the written bytes back so ADU core can run its real
 *                    SHA-256 check against the signed manifest.
 *   - install_fn   : finalizes/validates the image, makes it the next boot
 *                    target, and returns REBOOT_REQUIRED.
 *   - persist/load : stores the ADU resume blob in NVS so the workflow survives
 *                    the reboot into the freshly flashed firmware.
 */
#ifndef ADU_ESP32_OTA_H
#define ADU_ESP32_OTA_H

#include "azure/iot/az_iot_adu.h"

#include "esp_ota_ops.h"

#ifdef __cplusplus
extern "C"
{
#endif

  typedef struct adu_ota_ctx
  {
    const char* installed_version; /* this firmware's baked-in version */

    const esp_partition_t* update_partition; /* inactive slot being written */
    esp_ota_handle_t ota_handle;
    bool ota_in_progress;
    size_t written; /* bytes written to the partition */

    bool reboot_pending; /* install asked for a reboot */
  } adu_ota_ctx;

  /* Build the platform-hooks vtable bound to @p ctx. The caller owns @p ctx and
   * must keep it alive for the lifetime of the ADU client. */
  az_iot_adu_platform_hooks adu_esp32_ota_hooks(adu_ota_ctx* ctx);

  /* Confirm the running image is healthy so the bootloader does not roll it back
   * (no-op unless this boot is pending verification). Call once the device has
   * proven itself — e.g. after it connects to IoT Hub. */
  void adu_esp32_ota_mark_valid(void);

#ifdef __cplusplus
}
#endif

#endif /* ADU_ESP32_OTA_H */
