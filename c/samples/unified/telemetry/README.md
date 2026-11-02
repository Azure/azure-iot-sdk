<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Telemetry

Sends a telemetry message (`{"temp":23}`) every 5 seconds for about 60 seconds, to the mqttv3
IoT Hub DPS assigns the device. It is the shortest example of the build-before-open pattern that
most `unified/` samples follow; read it first. [`connect_first`](../connect_first/README.md)
builds after connecting instead.

## Sample features

- Hub: mqttv3 (MQTT 3.1.1). A DPS assignment to a hub profile this SDK does not support faults with `AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED` before the hub is reached.
- Builds the telemetry client **before** `open()`.
- Building before `open()` means `CONNECTED` also says the client's subscriptions were granted. [`connect_first`](../connect_first/README.md) shows the alternative.
- Platforms: Linux and Windows.

## Service requirements

- An Azure IoT Hub Device Provisioning Service (DPS) instance linked to an IoT Hub.
- An X.509 enrollment (individual or group) for the device. For an mqttv3 hub,
  [Quickstart: Provision an X.509 certificate simulated device](https://learn.microsoft.com/azure/iot-dps/quick-create-simulated-device-x509) walks through the setup.

## Configure

The sample reads these environment variables:

| Variable | Required | Meaning |
| --- | --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | yes | DPS ID scope. |
| `AZ_IOT_DPS_REGISTRATION_ID` | yes | Registration ID; must equal the device certificate's common name. |
| `AZ_IOT_CLIENT_CERT` | yes | Device certificate chain (PEM file, leaf first). |
| `AZ_IOT_CLIENT_KEY` | yes | Device private key (PEM file). A key file is for development; see [Production keys](../../../docs/connecting.md#production-keys). |
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

With az_mqtt (`-DAZ_IOT_WITH_AZ_MQTT=ON`), target `az_iot_sample_unified_telemetry_az_mqtt` builds the same sample in
`az_mqtt/` next to that binary ([adapters](../../README.md#build)).

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
Sent 11 message(s), 0 failed.
```

The count depends on how long connecting took.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `Required env var <NAME> not set.`, exit 1 | A required variable is missing. |
| SDK line `dps register: errorCode=<code> errorMessage=<text>` | DPS refused the registration: no matching enrollment, the certificate's common name differs from `AZ_IOT_DPS_REGISTRATION_ID`, or the enrollment is disabled. DPS verdicts are retried until the sample's run time ends. |
| Repeated `paho: connect failed: ... TCP/TLS connect failure` | DPS or the hub is unreachable (network, proxy, `AZ_IOT_DPS_GLOBAL_ENDPOINT`), or server certificate validation fails (`AZ_IOT_TRUSTED_CA`). |
| `Unsupported hub generation "<value>". Upgrade the SDK.` | DPS assigned a connection profile this SDK does not support. |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| MQTT adapter and the workspace it builds each MQTT client in | `mqtt_adapter` (`az_iot_paho_get_adapter()`; az_mqtt: `az_iot_az_mqtt_get_adapter()`), `mqtt_limits`, `mqtt_workspace`; sizes in [`sample_utils.h`](../../common/sample_utils.h) |
| Build the client before `open()` | `telemetry_build()` |
| Detect an unsupported hub profile | `on_conn_state()`: `AZ_IOT_CONN_STATE_FAULTED` with `AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED`; `event->profile` holds the assigned one |
| Send | `az_iot_mqttv3_telemetry_client_send()`, completion in `on_send_done()` |
