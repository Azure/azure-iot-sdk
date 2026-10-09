<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Cloud-to-device messages

Receives cloud-to-device (C2D) messages for about 60 seconds and prints each one with its
properties. C2D is an **mqttv3** IoT Hub feature; there is no mqttv5 counterpart.

## Sample features

- Hub generation: mqttv3 (MQTT 3.1.1) only. Registers only the MQTT 3.1.1 adapter.
- The client subscribes to `devices/<id>/messages/devicebound/#`. Message properties travel in the topic; the client percent-decodes them before the handler runs.
- The client pins mqttv3 at `init()`, before `open()`. If DPS assigns an mqttv5 hub, on the first connect or after a move, the connection stops with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH`; the sample reports that and exits 1. A move to another mqttv3 hub needs nothing from the application.
- Platforms: Linux and Windows.

## Service requirements

- An Azure IoT Hub Device Provisioning Service (DPS) instance linked to an IoT Hub.
- An X.509 enrollment (individual or group) for the device. For an mqttv3 hub,
  [Quickstart: Provision an X.509 certificate simulated device](https://learn.microsoft.com/azure/iot-dps/quick-create-simulated-device-x509) walks through the setup.

To send a message, use the Azure CLI (`azure-iot` extension) while the sample runs:

```sh
az iot device c2d-message send -n <hub-name> -d <device-id> --data 'hello' --props 'key0=value0'
```

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
cmake --build --preset linux-gcc-debug --target az_iot_sample_unified_c2d_receiver
./build/linux-gcc-debug/samples/unified/az_iot_sample_c2d_receiver
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_unified_c2d_receiver
.\build\windows-msvc-debug\samples\unified\Debug\az_iot_sample_c2d_receiver.exe
```

## How it ends

Runs about 60 seconds. Exit code 0 when the hub connection was reached at least once, never settled at `FAULTED`, and the hub is mqttv3; otherwise 1.

## Expected output

One line per message, then a summary:

```
C2D #1: 5 bytes key0=value0 => hello
Received 1 message(s).
```

On an mqttv5 hub it prints
`DPS assigned MQTTv5 (MQTT v5). Cloud-to-device messages are not available on this hub generation.`

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
| Register the MQTT 3.1.1 adapter only | `az_iot_paho_factory_create_v3_1_1()` |
| Create the client and set the handler | `az_iot_mqttv3_c2d_client_init()`, `az_iot_mqttv3_c2d_client_set_handler()` |
| Handle a message and its properties | `on_c2d()` |
| Report an mqttv5 assignment | `on_conn_state()` |
