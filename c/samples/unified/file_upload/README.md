<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# File upload

Uploads a small text file (`sample-data/test.txt`) to the Azure Storage account linked to the
IoT Hub, then notifies the hub. File upload is an **mqttv3** IoT Hub feature; there is no mqttv5
counterpart.

## Sample features

- Hub generation: mqttv3 (MQTT 3.1.1) only.
- Three steps: `az_iot_mqttv3_file_upload_client_get_sas_uri()` returns a SAS URI and a correlation ID; the application PUTs the file to that URI on Azure Storage; `az_iot_mqttv3_file_upload_client_notify_complete()` reports the result to the hub.
- The SDK ships no HTTP client. The hub calls go through an application HTTP hook, required at `init()`; this sample implements it, and the Storage PUT, with libcurl.
- The hub calls authenticate with the device's X.509 certificate (mutual TLS), the same as MQTT. The Storage PUT is authenticated by the SAS token in the URI.
- `init()` needs the assigned hub, so the client is built once `CONNECTED`, the way [`connect_first`](../connect_first/README.md) does. On an mqttv5 hub the sample reports that file upload is unavailable and exits 1.
- One-shot: it uploads once and exits.
- Platforms: Linux and Windows.

## Service requirements

- An Azure IoT Hub Device Provisioning Service (DPS) instance linked to an IoT Hub.
- An X.509 enrollment (individual or group) for the device. For an mqttv3 hub,
  [Quickstart: Provision an X.509 certificate simulated device](https://learn.microsoft.com/azure/iot-dps/quick-create-simulated-device-x509) walks through the setup.
- File upload configured on the IoT Hub, with a linked Azure Storage account: [Configure IoT Hub file uploads](https://learn.microsoft.com/azure/iot-hub/iot-hub-configure-file-upload).

## Configure

The sample reads these environment variables:

| Variable | Required | Meaning |
| --- | --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | yes | DPS ID scope. |
| `AZ_IOT_DPS_REGISTRATION_ID` | yes | Registration ID; must equal the device certificate's common name. |
| `AZ_IOT_CLIENT_CERT` | yes | Device certificate chain (PEM file, leaf first). |
| `AZ_IOT_CLIENT_KEY` | yes | Device private key (PEM file). |
| `AZ_IOT_TRUSTED_CA` | yes | CA bundle (PEM file) that validates the DPS and IoT Hub server certificates, e.g. `/etc/ssl/certs/ca-certificates.crt`. |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | Provisioning endpoint. Default `global.azure-devices-provisioning.net`. |

```sh
export AZ_IOT_DPS_ID_SCOPE='<id-scope>'
export AZ_IOT_DPS_REGISTRATION_ID='<registration-id>'
export AZ_IOT_CLIENT_CERT="$PWD/device-cert.pem"
export AZ_IOT_CLIENT_KEY="$PWD/device-key.pem"
export AZ_IOT_TRUSTED_CA='/etc/ssl/certs/ca-certificates.crt'
```

PowerShell:

```powershell
$env:AZ_IOT_DPS_ID_SCOPE        = '<id-scope>'
$env:AZ_IOT_DPS_REGISTRATION_ID = '<registration-id>'
$env:AZ_IOT_CLIENT_CERT         = "$PWD\device-cert.pem"
$env:AZ_IOT_CLIENT_KEY          = "$PWD\device-key.pem"
$env:AZ_IOT_TRUSTED_CA          = "$PWD\ca.pem"
```

## Build and run

Build prerequisites (toolchain, CMake, OpenSSL) are listed in the
[samples overview](../../README.md#prerequisites). Run from the repository's `c/` directory.

Linux:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_unified_file_upload
./build/linux-gcc-debug/samples/unified/az_iot_sample_file_upload
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_unified_file_upload
.\build\windows-msvc-debug\samples\unified\Debug\az_iot_sample_file_upload.exe
```

The upload needs **libcurl**. Linux: install `libcurl4-openssl-dev` (Debian/Ubuntu). Windows: use
vcpkg through the manifest in `c/vcpkg.json` by adding these to the configure command (vcpkg
then builds libcurl and OpenSSL; `vcpkg install curl` alone is not enough):

```powershell
cmake --preset windows-msvc-debug `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  "-DVCPKG_MANIFEST_FEATURES=paho;curl"
```

Without libcurl, configure prints
`az_iot_sample_unified_file_upload: libcurl not found; building without the HTTPS upload path.`
and the sample connects but cannot upload.

## How it ends

Exit code 0 when the upload and the completion notification both succeeded; otherwise 1.

## Expected output

```
Connected. IoT Hub: <hub-name>.azure-devices.net
Requesting SAS URI for 'sample-data/test.txt'...
Received SAS URI (correlation id: <id>).
Uploading 51 bytes to Azure Storage...
Storage PUT completed (HTTP 201).
Notifying IoT Hub of completion...
File upload completed successfully.
```

If the Storage PUT fails, the sample notifies the hub of the failure, prints
`Blob upload failed; IoT Hub was notified of the failure.` and exits 1.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `Required env var <NAME> not set.`, exit 1 | A required variable is missing. |
| SDK line `dps register: errorCode=<code> errorMessage=<text>` | DPS refused the registration: no matching enrollment, the certificate's common name differs from `AZ_IOT_DPS_REGISTRATION_ID`, or the enrollment is disabled. DPS verdicts are retried until the sample's run time ends. |
| Repeated `paho: connect failed: ... TCP/TLS connect failure` | DPS or the hub is unreachable (network, proxy, `AZ_IOT_DPS_GLOBAL_ENDPOINT`), or server certificate validation fails (`AZ_IOT_TRUSTED_CA`). |
| `Unsupported hub generation "<value>". Upgrade the SDK.` | DPS assigned a connection profile this SDK version does not know. |
| `SAS URI request did not succeed: ...` | File upload is not configured on the hub, or the storage account is not linked. |
| `libcurl not built in; cannot perform HTTPS ...` | The sample was built without libcurl; see Build and run. |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| Build the client once connected, after reading the hub | `az_iot_connection_client_get_iothub_address()`, `az_iot_mqttv3_file_upload_client_init()` |
| The whole upload | `run_file_upload()` |
| Request the SAS URI | `az_iot_mqttv3_file_upload_client_get_sas_uri()`, `on_sas()` |
| Notify completion | `az_iot_mqttv3_file_upload_client_notify_complete()`, `on_notify()` |
| HTTP hook and Storage PUT (libcurl) | code under `AZ_IOT_SAMPLE_WITH_CURL` |
