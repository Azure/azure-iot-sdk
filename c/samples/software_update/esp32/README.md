# ESP32-WROOM real software updates (OTA) sample

This sample performs a **genuine over-the-air firmware update** on an ESP32-WROOM
using Azure software updates. Unlike [`samples/software_update/pc`](../pc), which simulates
the install with zero-filled payloads and a log-only "install", this sample:

- downloads the real firmware image over HTTPS into the inactive OTA partition,
- verifies the software updates manifest signature (JWS / SHA-256) and the payload hash,
- switches the boot partition and reboots,
- resumes the workflow after reboot and reports the new version to the service,
- rolls back automatically if the new image fails to validate itself.

Everything is built from **native ESP-IDF components** — `esp-mqtt` for the
transport, **mbedTLS** for crypto, and `esp_ota`/`app_update` + `esp_http_client`
for the OTA flow. The Azure IoT C SDK in this repo is consumed as an ESP-IDF
component (`components/azure-iot-sdk`).

## Layout

```
samples/software_update/esp32/
├── CMakeLists.txt              top-level ESP-IDF project
├── partitions.csv             A/B OTA partition table (ota_0 / ota_1 / otadata)
├── sdkconfig.defaults          target, flash size, rollback, MQTT v5, mbedTLS
├── components/
│   ├── azure-iot-sdk/          wraps this repo's src/ as an ESP-IDF component
│   └── azure-sdk-for-c/        builds az_core + az_iot from the submodule
└── main/
    ├── app_main.c              entry point: wifi → DPS → software updates pump
    ├── az_iot_mqtt_esp.[ch]    esp-mqtt az_iot_mqtt_iface adapter (v3.1.1 + v5)
    ├── az_iot_cert_embedded.[ch]  in-memory X.509 cert provider
    ├── wifi_connect.[ch]       station-mode Wi-Fi bring-up
    ├── su_version.h           compiled-in firmware version (raise it for each update image)
    ├── Kconfig.projbuild       Wi-Fi, DPS and poll-interval menuconfig options
    └── certs/                  device_cert.pem / device_key.pem / trusted_ca.pem
```

`main/CMakeLists.txt` also compiles two adapters from outside the sample:

- [`adapters/su/crypto_mbedtls/az_iot_su_crypto_mbedtls.[ch]`](../../../adapters/su/crypto_mbedtls/): PSA-Crypto RS256 / SHA-256 hooks.
- [`adapters/su/esp32/su_esp32_ota.[ch]`](../../../adapters/su/esp32/): real OTA platform hooks (download/install/rollback).

## Prerequisites

- ESP-IDF v6.0 installed at `C:\esp\v6.0\esp-idf` (initialize any shell with
  `C:\esp\v6.0\esp-idf\export.bat`).
- The `azure-sdk-for-c` git submodule checked out (it supplies az_core + az_iot
  used by the SDK):

  ```powershell
  git submodule update --init c/deps/azure-sdk-for-c
  ```

- A Device Provisioning Service with an X.509 enrollment for the device, linked
  to an IoT Hub (the device registers and connects to it), and a Device Update
  instance linked to that DPS.
- The device certificate and private key as PEM files. The certificate CN must
  equal the DPS registration id.

> **Native components.** esp-mqtt is pulled from the ESP Component Registry as
> the managed component `espressif/mqtt` (it left the IDF core tree in v6.0).
> Crypto uses **PSA Crypto** (`psa/crypto.h`) from mbedTLS 4.x — the native API
> in IDF v6.0, since the legacy `mbedtls_rsa_*` API is now private. The Azure
> SDK for C is built from the in-repo submodule via the
> [`components/azure-sdk-for-c`](components/azure-sdk-for-c) component.

## 1. Configure the device identity + Wi-Fi

Copy the device certificate and key into the embedded `certs/` files (the build
embeds them into the firmware):

```powershell
Copy-Item <path>\device-cert.pem samples/software_update/esp32/main/certs/device_cert.pem
Copy-Item <path>\device-key.pem  samples/software_update/esp32/main/certs/device_key.pem
```

`certs/trusted_ca.pem` can stay as the placeholder — the adapter falls back to the
ESP-IDF certificate bundle (which already trusts the Azure roots) unless the file
contains a real `-----BEGIN CERTIFICATE-----` block.

> The device key is compiled into the image. Use a per-device key and treat the
> binary as a secret. For production, source the key from a secure element /
> flash encryption instead of embedding it.

Then configure Wi-Fi and DPS via menuconfig:

```powershell
C:\esp\v6.0\esp-idf\export.bat
cd samples/software_update/esp32
idf.py set-target esp32
idf.py menuconfig   # → "Software Updates ESP32 Sample Configuration"
```

Set `SU_WIFI_SSID`, `SU_WIFI_PASSWORD`, `SU_DPS_ID_SCOPE` and
`SU_DPS_REGISTRATION_ID` (must equal the certificate CN / device id).
Optionally set `SU_POLL_INTERVAL_S` (seconds between update checks, default 60).
The values are written into the git-ignored `sdkconfig`, so nothing secret is
committed.

## 2. Flash the initial firmware and watch it connect

```powershell
idf.py build flash monitor
```

The device joins Wi-Fi and asks for an update on its first provisioning session,
reporting its device properties (manufacturer `Espressif`, model `ESP32-WROOM`,
installed version from `su_version.h`). It then registers through DPS and
connects to its hub. Leave the monitor running.

Which update-check route it uses depends on whether it has connected to its hub
before, which it records in NVS (namespace `su_app`, key `registered`):

- **Never connected:** the onboarding route
  (`az_iot_su_client_request_onboarding_update()`), for a device with no device
  record.
- **Connected before:** the regular route (`az_iot_su_client_request_update()`),
  which also sends the installed update id.

Once connected it checks on the regular route every `SU_POLL_INTERVAL_S`
(menuconfig, default 60 s) while no deployment is in flight. A check not
answered within half the interval (at most 60 s) is abandoned and asked again at
the next poll. Erasing NVS
(`idf.py erase-flash`) returns it to the onboarding route.

## 3. Build a new image and deploy it as an update

1. Raise `SU_UPDATE_VERSION` in `main/su_version.h`, so the new firmware reports
   the new version.
2. `idf.py build` to produce `build/su_esp32.bin`.
3. Import it into the Device Update instance linked to the DPS, as update
   `SU_UPDATE_PROVIDER`/`SU_UPDATE_NAME`/`SU_UPDATE_VERSION` with compatibility
   `manufacturer=Espressif`, `model=ESP32-WROOM`, and deploy it to the device.

Once a matching deployment exists, the device picks it up at its next check
(within `SU_POLL_INTERVAL_S`), downloads the image, flashes the inactive OTA
slot, reboots, marks the new app valid, and reports the new version.

## How the real OTA flow works

| Software updates platform hook | ESP32 implementation |
|---|---|
| `download`   | `esp_http_client` GET → `esp_ota_write` into the next OTA partition |
| `read_file`  | `esp_partition_read` (re-reads the staged image for hash verification) |
| `is_installed` | compares the manifest version against the compiled-in version |
| `install`    | `esp_ota_end` + `esp_ota_set_boot_partition` → returns `REBOOT_REQUIRED` |
| `apply`      | no-op (boot partition already switched) |
| `restore`    | `esp_ota_abort` / boot back to the running partition (rollback) |
| `persist`/`load` state | NVS namespace `su_sample`, key `wf_state` (resume across reboot) |

Manifest signature verification (RS256 over the SHA-256 of the manifest, plus the
SJWK chain to the compiled-in Microsoft root keys) and payload hashing are done by
the SDK core using the PSA-Crypto hooks in
[`adapters/su/crypto_mbedtls/az_iot_su_crypto_mbedtls.c`](../../../adapters/su/crypto_mbedtls/az_iot_su_crypto_mbedtls.c).

After a successful boot into the new image, `app_main` calls
`su_esp32_ota_mark_valid()` which invokes `esp_ota_mark_app_valid_cancel_rollback`
so the bootloader keeps the new slot; if the new firmware crashes or fails to mark
itself valid, the bootloader rolls back to the previous slot
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).
