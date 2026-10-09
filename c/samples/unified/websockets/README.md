<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Telemetry over WebSockets

[`unified/telemetry`](../telemetry/README.md), carried as MQTT inside WebSockets on port 443
instead of MQTT over TCP on 8883, for networks that only pass HTTP(S) ports. Everything else is
identical, so the difference between the two `main.c` files is exactly the feature.

## Sample features

- Hub generations: mqttv3 (MQTT 3.1.1) and mqttv5 (MQTT 5), whichever DPS assigns, including after the device is moved to a hub of the other generation.
- The whole feature is one option: `copts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET;`. The port follows the transport (443) as long as `copts.port` stays 0.
- Applies to the DPS connect as well as the hub connect.
- The resource path defaults to `/$iothub/websocket`, which IoT Hub and DPS serve.
- Combine with [`proxy`](../proxy/README.md) for a network that needs a proxy **and** only passes 443.
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
| `AZ_IOT_MQTT_WEBSOCKET_PATH` | no | WebSocket resource path. Set only for a gateway that serves WebSockets elsewhere. Default `/$iothub/websocket`. |

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
cmake --build --preset linux-gcc-debug --target az_iot_sample_unified_websockets
./build/linux-gcc-debug/samples/unified/az_iot_sample_websockets
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_unified_websockets
.\build\windows-msvc-debug\samples\unified\Debug\az_iot_sample_websockets.exe
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
| SDK line `dps register: errorCode=<code> errorMessage=<text>` | DPS refused the registration: no matching enrollment, the certificate's common name differs from `AZ_IOT_DPS_REGISTRATION_ID`, or the enrollment is disabled. A refused request is retried until the sample's run time ends; a `failed` or `disabled` registration faults at once with `AZ_IOT_ERR_DPS_REGISTRATION_FAILED`. |
| Repeated `paho: connect failed: ... TCP/TLS connect failure` | DPS or the hub is unreachable (network, proxy, `AZ_IOT_DPS_GLOBAL_ENDPOINT`), or server certificate validation fails (`AZ_IOT_TRUSTED_CA`). |
| `Unsupported hub generation "<value>". Upgrade the SDK.` | DPS assigned a connection profile this SDK version does not know. |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| Select WebSockets | `copts.transport = AZ_IOT_MQTT_TRANSPORT_WEBSOCKET` in `main()` |
| Optional resource path | `copts.websocket_path` from `AZ_IOT_MQTT_WEBSOCKET_PATH` |
| Everything else | as in [`unified/telemetry`](../telemetry/README.md#where-to-look-in-mainc) |
