<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Device twin (`unified/twin_get_patch`)

On every connect, requests the device twin (GET) and reports a property
(`{"sample":"hello"}`, PATCH); in between it prints desired-property updates. Runs about 60 seconds,
on whichever IoT Hub DPS assigns. For an mqttv5-only device, see
[`mqttv5/twin_get_patch`](../../mqttv5/twin_get_patch/README.md).

## Sample features

- Hub generations: mqttv3 (MQTT 3.1.1) and mqttv5 (MQTT 5), whichever DPS assigns, including after the device is moved to a hub of the other generation.
- The two generations differ in protocol, so each client has its own callbacks:
-   - GET: mqttv3 returns one document; mqttv5 returns the desired and reported sections separately, each with its own version.
-   - PATCH: mqttv3 reports the new version; mqttv5 also reports the service's verdict. An mqttv5 patch can complete and still be refused (another writer moved the version first); the sample counts that as a failure.
-   - The mqttv5 client encodes patches into an application-supplied buffer and needs its own `az_iot_mqttv5_twin_client_do_work()` in the pump.
- Rebuilds its clients when DPS assigns the other generation, as [`unified/telemetry`](../telemetry/README.md) does.
- Platforms: Linux and Windows.

## Service requirements

- An Azure IoT Hub Device Provisioning Service (DPS) instance linked to an IoT Hub.
- An X.509 enrollment (individual or group) for the device. For an mqttv3 hub,
  [Quickstart: Provision an X.509 certificate simulated device](https://learn.microsoft.com/azure/iot-dps/quick-create-simulated-device-x509) walks through the setup.
- To exercise mqttv5, the device's enrollment must assign it to an mqttv5 IoT Hub.

To see a desired-property update on an mqttv3 hub, update the twin with the Azure CLI
(`azure-iot` extension) while the sample runs:

```sh
az iot hub device-twin update -n <hub-name> -d <device-id> --desired '{"color":"blue"}'
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
cmake --build --preset linux-gcc-debug --target az_iot_sample_unified_twin_get_patch
./build/linux-gcc-debug/samples/unified/az_iot_sample_twin_get_patch
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_unified_twin_get_patch
.\build\windows-msvc-debug\samples\unified\Debug\az_iot_sample_twin_get_patch.exe
```

## How it ends

Runs about 60 seconds. Exit code 0 when at least one GET + PATCH round succeeded, none failed, and the connection never settled at `FAULTED`; otherwise 1.

## Expected output

On an mqttv3 hub (the document content depends on your twin):

```
twin GET: {"desired":{"$version":1},"reported":{"$version":1}}
twin desired (version 2): {"color":"blue","$version":2}
twin_get: AZ_IOT_OK, patch_reported: AZ_IOT_OK version=3
```

On an mqttv5 hub, the GET prints `twin GET desired (version N): ...` and
`twin GET reported (version N): ...`, and a refused patch prints
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
| Register both MQTT adapters (DPS always uses MQTT 3.1.1) | `az_iot_paho_factory_create_v3_1_1()`, `az_iot_paho_factory_create_v5()` |
| Build or rebuild the twin client for a generation | `clients_build()`, `clients_destroy()` |
| mqttv3 GET, PATCH, desired updates | `az_iot_mqttv3_twin_client_get()`, `az_iot_mqttv3_twin_client_patch_reported()`; `on_get_mqttv3()`, `on_patch_mqttv3()`, `on_desired_mqttv3()` |
| mqttv5 GET, PATCH, desired updates | `az_iot_mqttv5_twin_client_get()`, `az_iot_mqttv5_twin_client_patch_reported()`; `on_get_mqttv5()`, `on_patch_mqttv5()`, `on_desired_mqttv5()` |
| mqttv5 encode buffer and pump | `az_iot_mqttv5_twin_client_set_encode_buffer()`, `az_iot_mqttv5_twin_client_do_work()` |
