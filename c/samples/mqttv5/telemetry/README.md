<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Telemetry, mqttv5 only

Provisions through DPS, connects to an mqttv5 IoT Hub, sends one telemetry message, and closes.
A device that must serve either hub generation should follow
[`unified/telemetry`](../../unified/telemetry/README.md) instead.

## Sample features

- Hub generation: mqttv5 (MQTT 5) only. An assignment to an mqttv3 hub fails; use the `unified/` sample instead.
- The MQTT adapter builds an MQTT 3.1.1 client for DPS, then an MQTT 5 one for the hub, in turn in one workspace.
- Builds its client before `open()`: an mqttv5 client records the generation it needs instead of reading it from a live connection.
- Message properties, all in one array: system properties use the `AZ_IOT_MSG_PROP_*` names (`$.ct` is sent as the MQTT v5 Content Type, `$.cid` as Correlation Data, the others as user properties); any other name is an application property, sent as a user property. No limit on their number. An mqttv5 hub sends no cloud-to-device messages, so there are no inbound properties to read; for mqttv3, see [`unified/c2d_receiver`](../../unified/c2d_receiver/README.md).
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
cmake --build --preset linux-gcc-debug --target az_iot_sample_mqttv5_telemetry
./build/linux-gcc-debug/samples/mqttv5/az_iot_sample_telemetry
```

With az_mqtt (`-DAZ_IOT_WITH_AZ_MQTT=ON`), target `az_iot_sample_mqttv5_telemetry_az_mqtt` builds the same sample in
`az_mqtt/` next to that binary ([adapters](../../README.md#build)).

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
| MQTT adapter and the workspace it builds each MQTT client in | `mqtt_adapter` (`az_iot_paho_get_adapter()`; az_mqtt: `az_iot_az_mqtt_get_adapter()`), `mqtt_limits`, `mqtt_workspace`; sizes in [`sample_utils.h`](../../common/sample_utils.h) |
| Create the client before `open()` | `az_iot_mqttv5_telemetry_client_init()` |
| Send and wait for completion | `az_iot_mqttv5_telemetry_client_send()`, `on_send_done()` |
