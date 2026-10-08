<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Telemetry

Sends a telemetry message (`{"temp":23}`) every 5 seconds for about 60 seconds, to whichever
IoT Hub DPS assigns the device. It is the shortest example of the build-before-open pattern that
most `unified/` samples follow; read it first. [`connect_first`](../connect_first/README.md)
builds after connecting instead, and [`c2d_receiver`](../c2d_receiver/README.md) and
[`file_upload`](../file_upload/README.md) serve mqttv3 only. For an mqttv5-only device, see
[`mqttv5/telemetry`](../../mqttv5/telemetry/README.md).

## Sample features

- Hub generations: mqttv3 (MQTT 3.1.1) and mqttv5 (MQTT 5), whichever DPS assigns, including after the device is moved to a hub of the other generation.
- Builds the telemetry client **before** `open()` for an assumed generation (mqttv3, what DPS assigns when it names no profile). If DPS assigns the other one, the connection stops with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH` before the hub is reached; the sample destroys the client, builds the other generation's, and calls `close()` then `open()`.
- Building before `open()` means `CONNECTED` also says the client's subscriptions were granted. [`connect_first`](../connect_first/README.md) shows the alternative.
- Platforms: Linux and Windows.

## Service requirements

- An Azure IoT Hub Device Provisioning Service (DPS) instance linked to an IoT Hub.
- An X.509 enrollment (individual or group) for the device. For an mqttv3 hub,
  [Quickstart: Provision an X.509 certificate simulated device](https://learn.microsoft.com/azure/iot-dps/quick-create-simulated-device-x509) walks through the setup.
- To exercise mqttv5, the device's enrollment must assign it to an mqttv5 IoT Hub.

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
cmake --build --preset linux-gcc-debug --target az_iot_sample_unified_telemetry
./build/linux-gcc-debug/samples/unified/az_iot_sample_telemetry
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_unified_telemetry
.\build\windows-msvc-debug\samples\unified\Debug\az_iot_sample_telemetry.exe
```

## How it ends

Runs about 60 seconds. Exit code 0 when at least one message was sent and the connection never settled at `FAULTED`; otherwise 1. A `FAULTED` connection ends the run early.

## Expected output

The sample prints a summary line at the end; SDK warnings and errors go to stderr.

```
Sent 11 message(s) on MQTTv3 (MQTT v3.1.1), 0 failed.
```

The count depends on how long connecting took. When DPS assigns the other generation, the
sample first prints `DPS assigned MQTTv5 (MQTT v5); rebuilding the telemetry client.`

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
| Register both MQTT adapters (DPS always uses MQTT 3.1.1) | `az_iot_paho_factory_create_v3_1_1()`, `az_iot_paho_factory_create_v5()` |
| Build the client for an assumed generation before `open()` | `telemetry_build()` with `sample_initial_profile()` |
| Detect that DPS assigned the other generation | `on_conn_state()` → `sample_event_is_profile_mismatch()` |
| Rebuild and reconnect | main loop: `telemetry_destroy()`, `az_iot_connection_client_close()`, `telemetry_build()`, `az_iot_connection_client_open()` |
| Send | `az_iot_mqttv3_telemetry_client_send()` / `az_iot_mqttv5_telemetry_client_send()`, completion in `on_send_done()` |
