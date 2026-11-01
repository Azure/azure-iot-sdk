<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Device twin, mqttv5 only

Provisions through DPS, connects to an mqttv5 IoT Hub, requests the device twin (GET), reports
a property (PATCH), and closes. For a device that must serve either generation, see
[`unified/twin_get_patch`](../../unified/twin_get_patch/README.md).

## Sample features

- Hub generation: mqttv5 (MQTT 5) only. An assignment to an mqttv3 hub fails; use the `unified/` sample instead.
- The MQTT adapter builds an MQTT 3.1.1 client for DPS, then an MQTT 5 one for the hub, in turn in one workspace.
- Builds its client before `open()`: an mqttv5 client records the generation it needs instead of reading it from a live connection.
- GET returns the desired and reported sections separately, each with its own version.
- PATCH reports the service's verdict as well as a transport status. A patch can complete and still be refused (another writer moved the version first), so the run succeeds only when the verdict is OK.
- The client encodes patches into an application-supplied buffer. `az_iot_connection_client_do_work()` times its requests out.
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
cmake --build --preset linux-gcc-debug --target az_iot_sample_mqttv5_twin_get_patch
./build/linux-gcc-debug/samples/mqttv5/az_iot_sample_twin_get_patch
```

With az_mqtt (`-DAZ_IOT_WITH_AZ_MQTT=ON`), target `az_iot_sample_mqttv5_twin_get_patch_az_mqtt` builds the same sample in
`az_mqtt/` next to that binary ([adapters](../../README.md#build)).

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
| SDK line `dps register: errorCode=<code> errorMessage=<text>` | DPS refused the registration: no matching enrollment, the certificate's common name differs from `AZ_IOT_DPS_REGISTRATION_ID`, or the enrollment is disabled. DPS verdicts are retried until the sample's run time ends. |
| Repeated `paho: connect failed: ... TCP/TLS connect failure` | DPS or the hub is unreachable (network, proxy, `AZ_IOT_DPS_GLOBAL_ENDPOINT`), or server certificate validation fails (`AZ_IOT_TRUSTED_CA`). |
| `Unsupported hub generation "<value>". Upgrade the SDK.` | DPS assigned a connection profile this SDK version does not know. |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| MQTT adapter and the workspace it builds each MQTT client in | `mqtt_adapter` (`az_iot_paho_get_adapter()`; az_mqtt: `az_iot_az_mqtt_get_adapter()`), `mqtt_limits`, `mqtt_workspace`; sizes in [`sample_utils.h`](../../common/sample_utils.h) |
| Create the client, encode buffer, desired handler | `az_iot_mqttv5_twin_client_init()`, `az_iot_mqttv5_twin_client_set_encode_buffer()`, `az_iot_mqttv5_twin_client_set_desired_handler()` |
| GET | `az_iot_mqttv5_twin_client_get()`, `on_get()` |
| PATCH and its verdict | `az_iot_mqttv5_twin_client_patch_reported()`, `on_patch()` |
