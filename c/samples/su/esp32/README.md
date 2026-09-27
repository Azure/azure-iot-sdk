# ESP32-WROOM real software updates (OTA) sample

This sample performs a **genuine over-the-air firmware update** on an ESP32-WROOM
using Azure software updates. Unlike [`samples/su/pc`](../pc), which simulates
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
samples/su/esp32/
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
    ├── su_version.h           compiled-in firmware version (rewritten by the script)
    ├── Kconfig.projbuild       Wi-Fi + DPS menuconfig options
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

- Azure CLI with the `azure-iot` extension, logged in (`az login`).
- The shared software updates sample environment created by
  [`samples/common/scripts/Initialize-SuSampleEnvironment.ps1`](../../common/scripts/Initialize-SuSampleEnvironment.ps1).
  It provisions the IoT Hub, DPS, Device Update account/instance, storage, and a device
  X.509 certificate, and exports the `AZ_IOT_SU_*` environment variables that
  the deployment script reuses.

> **Native components.** esp-mqtt is pulled from the ESP Component Registry as
> the managed component `espressif/mqtt` (it left the IDF core tree in v6.0).
> Crypto uses **PSA Crypto** (`psa/crypto.h`) from mbedTLS 4.x — the native API
> in IDF v6.0, since the legacy `mbedtls_rsa_*` API is now private. The Azure
> SDK for C is built from the in-repo submodule via the
> [`components/azure-sdk-for-c`](components/azure-sdk-for-c) component.

## 1. Set up the Azure environment

```powershell
cd samples/common/scripts
./Initialize-SuSampleEnvironment.ps1
```

Note the printed **DPS ID scope** and **registration id (device id)**, and find
the generated device certificate/key PEMs.

## 2. Configure the device identity + Wi-Fi

Run the configuration script from an ESP-IDF-capable PowerShell. It reuses the
device certificate, DPS ID scope, and registration id exported by step 1, embeds
the certificate/key into the firmware, and prompts only for the Wi-Fi SSID and
password (the two values the init script can't know):

```powershell
cd samples/common/scripts
./Set-SuEsp32Config.ps1
```

You'll be prompted for the **Wi-Fi SSID** and **password**; everything else is
taken from the `AZ_IOT_*` environment variables. The Wi-Fi credentials and device
settings are written into the git-ignored `sdkconfig`, so nothing secret is
committed. Pass `-WifiSsid` / `-WifiPassword` to skip the prompts.

<details>
<summary>You can also do the sample configuration manually instead.</summary>

Copy the device certificate and key into the embedded `certs/` files (the build
embeds them into the firmware):

```powershell
Copy-Item <path>\su-sim-device-cert.pem samples/su/esp32/main/certs/device_cert.pem
Copy-Item <path>\su-sim-device-key.pem  samples/su/esp32/main/certs/device_key.pem
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
cd samples/su/esp32
idf.py set-target esp32
idf.py menuconfig   # → "Software Updates ESP32 Sample Configuration"
```

Set `SU_WIFI_SSID`, `SU_WIFI_PASSWORD`, `SU_DPS_ID_SCOPE` (the ID scope from
step 1) and `SU_DPS_REGISTRATION_ID` (must equal the certificate CN / device id).

</details>

## 3. Flash the initial firmware and watch it connect

```powershell
idf.py build flash monitor
```

The device joins Wi-Fi, provisions through DPS (model id
`dtmi:azure:iot:deviceUpdateContractModel;2`), reports its device properties
(manufacturer `Espressif`, model `ESP32-WROOM`, installed version from
`su_version.h`), and starts pumping the software updates workflow. Leave the monitor running.

## 4. Build a new image and deploy it as an update

From an ESP-IDF-capable PowerShell:

```powershell
cd samples/common/scripts
./New-SuEsp32Image.ps1
```

This script (the real-OTA counterpart of `New-SuSampleDeployment.ps1`):

1. resolves the next version (auto-bump, or `-UpdateVersion 2.0.0`),
2. rewrites `main/su_version.h` so the firmware reports that version,
3. `idf.py build`s the sample to produce `build/su_esp32.bin`,
4. generates a v5 import manifest with `--compat manufacturer=Espressif
   model=ESP32-WROOM` referencing the real binary,
5. stages + imports the update, tags the device into its group, and creates the
   deployment.

Within a minute or two the running device downloads the image, flashes the
inactive OTA slot, reboots, marks the new app valid, and reports the new version.
Track service-side status with the command the script prints at the end.

## 5. Tear down

```powershell
cd samples/common/scripts
./Remove-SuSampleEnvironment.ps1
```

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
