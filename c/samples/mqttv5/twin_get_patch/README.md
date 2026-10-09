<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Device twin, mqttv5 only

Provisions through DPS, connects to an mqttv5 IoT Hub, requests the device twin (GET), reports
a property (PATCH), and closes. For a device that must serve either generation, see
[`unified/twin_get_patch`](../../unified/twin_get_patch/README.md).

## Sample features

- Hub generation: mqttv5 (MQTT 5) only. An assignment to an mqttv3 hub fails; use the `unified/` sample instead.
- Registers both Paho adapters: the hub uses MQTT 5, but DPS always uses MQTT 3.1.1. Registering only the MQTT 5 adapter makes provisioning fail with `AZ_IOT_ERR_NOT_SUPPORTED`.
- Builds its client before `open()`: an mqttv5 client records the generation it needs instead of reading it from a live connection.
- GET returns the desired and reported sections separately, each with its own version.
- PATCH reports the service's verdict as well as a transport status. A patch can complete and still be refused (another writer moved the version first), so the run succeeds only when the verdict is OK.
- The client encodes patches into an application-supplied buffer and needs its own `az_iot_mqttv5_twin_client_do_work()` in the pump.
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
cmake --build --preset linux-gcc-debug --target az_iot_sample_mqttv5_twin_get_patch
./build/linux-gcc-debug/samples/mqttv5/az_iot_sample_twin_get_patch
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_mqttv5_twin_get_patch
.\build\windows-msvc-debug\samples\mqttv5\Debug\az_iot_sample_twin_get_patch.exe
```

## How it ends

Exit code 0 when the GET succeeded and the PATCH completed with the verdict OK; otherwise 1.

## Expected output

```
twin GET desired (version 1): {...}
twin GET reported (version 1): {...}
twin_get:       done=1 status=AZ_IOT_OK
patch_reported: done=1 status=AZ_IOT_OK verdict=1 version=2
```

`verdict=1` is `AZ_IOT_MQTTV5_TWIN_PATCH_OK`. A refused patch prints
`twin patch refused (status <code>, current version <N>)`.

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
| Register both MQTT adapters | `az_iot_paho_factory_create_v3_1_1()`, `az_iot_paho_factory_create_v5()` |
| Create the client, encode buffer, desired handler | `az_iot_mqttv5_twin_client_init()`, `az_iot_mqttv5_twin_client_set_encode_buffer()`, `az_iot_mqttv5_twin_client_set_desired_handler()` |
| GET | `az_iot_mqttv5_twin_client_get()`, `on_get()` |
| PATCH and its verdict | `az_iot_mqttv5_twin_client_patch_reported()`, `on_patch()` |
| Pump the twin client | `az_iot_mqttv5_twin_client_do_work()` |
