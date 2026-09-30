<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Telemetry (mqttv5) (`mqttv5/telemetry`)

Provisions through DPS, connects to an mqttv5 IoT Hub, sends one telemetry message, and closes.
A device that must serve either hub generation should follow
[`unified/telemetry`](../../unified/telemetry/README.md) instead.

## Sample features

- Hub generation: mqttv5 (MQTT 5) only. An assignment to an mqttv3 hub fails; use the `unified/` sample instead.
- Registers both Paho adapters: the hub uses MQTT 5, but DPS always uses MQTT 3.1.1. Registering only the MQTT 5 adapter makes provisioning fail with `AZ_IOT_ERR_NOT_SUPPORTED`.
- Builds its client before `open()`: an mqttv5 client records the generation it needs instead of reading it from a live connection.
- Platforms: Linux and Windows.

## Service requirements

- An Azure IoT Hub Device Provisioning Service (DPS) instance linked to an **mqttv5** IoT Hub.
- An X.509 enrollment (individual or group) that assigns the device to that hub. DPS then returns
  the connection profile `mqttV5`.

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
cmake --build --preset linux-gcc-debug --target az_iot_sample_mqttv5_telemetry
./build/linux-gcc-debug/samples/mqttv5/az_iot_sample_telemetry
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_mqttv5_telemetry
.\build\windows-msvc-debug\samples\mqttv5\Debug\az_iot_sample_telemetry.exe
```

## How it ends

Exit code 0 when the send completion reports success; otherwise 1.

## Expected output

The sample prints nothing on success; check the exit code. SDK warnings and errors go to stderr.
On an mqttv3 hub it prints
`This device is assigned to an MQTTv3 hub. Run the unified telemetry sample instead.` and exits 1.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `Required env var <NAME> not set.`, exit 1 | A required variable is missing. |
| SDK line `dps register: errorCode=<code> errorMessage=<text>` | DPS refused the registration: no matching enrollment, the certificate's common name differs from `AZ_IOT_DPS_REGISTRATION_ID`, or the enrollment is disabled. DPS verdicts are retried until the sample's run time ends. |
| Repeated `paho: connect failed: ... TCP/TLS connect failure` | DPS or the hub is unreachable (network, proxy, `AZ_IOT_DPS_GLOBAL_ENDPOINT`), or server certificate validation fails (`AZ_IOT_TRUSTED_CA`). |
| `Unsupported hub generation "<value>". Upgrade the SDK.` | DPS assigned a connection profile this SDK version does not know. |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| Register both MQTT adapters | `az_iot_paho_factory_create_v3_1_1()`, `az_iot_paho_factory_create_v5()` |
| Create the client before `open()` | `az_iot_mqttv5_telemetry_client_init()` |
| Send and wait for completion | `az_iot_mqttv5_telemetry_client_send()`, `on_send_done()` |
