<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Direct methods, deferred response

Answers the direct method `slowEcho` **after** the handler returned, for work that does not fit
inside a callback: the handler keeps the request, and the pump loop answers about 2 seconds later
with the request payload. Read [`direct_method_responder`](../direct_method_responder/README.md)
first.

## Sample features

- Hub: mqttv3 (MQTT 3.1.1).
- Keeping the request is a struct copy: nothing is allocated. `method_name` and `payload` point into SDK buffers that do not outlive the handler, so the payload is copied out. A payload larger than that copy (256 bytes) is answered 413 at once.
- mqttv3 never learns the caller's timeout; the sample sets its own (60 s). While one invocation is held, another is answered 429 after its arguments arrived.
- Platforms: Linux and Windows.

## Service requirements

- An Azure IoT Hub Device Provisioning Service (DPS) instance linked to an IoT Hub.
- An X.509 enrollment (individual or group) for the device. For an mqttv3 hub,
  [Quickstart: Provision an X.509 certificate simulated device](https://learn.microsoft.com/azure/iot-dps/quick-create-simulated-device-x509) walks through the setup.

To invoke the method on an mqttv3 hub, use the Azure CLI (`azure-iot` extension) while the
sample runs:

```sh
az iot hub invoke-device-method -n <hub-name> -d <device-id> --method-name slowEcho --method-payload '{"hello":"world"}'
```

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
cmake --build --preset linux-gcc-debug --target az_iot_sample_unified_direct_method_slow_responder
./build/linux-gcc-debug/samples/unified/az_iot_sample_direct_method_slow_responder
```

With az_mqtt (`-DAZ_IOT_WITH_AZ_MQTT=ON`), target `az_iot_sample_unified_direct_method_slow_responder_az_mqtt` builds the same sample in
`az_mqtt/` next to that binary ([adapters](../../README.md#build)).

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_unified_direct_method_slow_responder
.\build\windows-msvc-debug\samples\unified\Debug\az_iot_sample_direct_method_slow_responder.exe
```

## How it ends

Runs about 60 seconds. Exit code 0 when the hub connection was reached at least once and never settled at `FAULTED`; otherwise 1.

## Expected output

One line per invocation:

```
method 'slowEcho' invoked, answering in ~2000 ms
```

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
| Keep the request and return | `on_method_mqttv3()`, `on_slow_echo()` |
| Answer later from the pump | `deferred_pump()` |
| mqttv3: local response timeout | `az_iot_mqttv3_direct_method_client_set_response_timeout()` |
