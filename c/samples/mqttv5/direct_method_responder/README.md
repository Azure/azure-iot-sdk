<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Direct methods (mqttv5) (`mqttv5/direct_method_responder`)

Provisions through DPS, connects to an mqttv5 IoT Hub, and answers the direct method `echo` from
inside the handler for about 60 seconds. [`unified/direct_method_responder`](../../unified/direct_method_responder/README.md)
shows the mqttv3 route side by side; this is the feature where the generations differ most.

## Sample features

- Hub generation: mqttv5 (MQTT 5) only. An assignment to an mqttv3 hub fails; use the `unified/` sample instead.
- Registers both Paho adapters: the hub uses MQTT 5, but DPS always uses MQTT 3.1.1. Registering only the MQTT 5 adapter makes provisioning fail with `AZ_IOT_ERR_NOT_SUPPORTED`.
- Builds its client before `open()`: an mqttv5 client records the generation it needs instead of reading it from a live connection.
- mqttv5 asks before it calls: every invocation starts with a probe that names the method and carries the caller's timeout; only a device that accepts receives the arguments.
- Methods are declared with `register_method()`. A probe for any other name is answered `METHOD_NOT_FOUND` by the SDK and never reaches the application.
- The probe handler can decline a declared method with a reason (the caller gets `DEVICE_BUSY` instead of a timeout). This sample accepts every probe.
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
cmake --build --preset linux-gcc-debug --target az_iot_sample_mqttv5_direct_method_responder
./build/linux-gcc-debug/samples/mqttv5/az_iot_sample_direct_method_responder
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_mqttv5_direct_method_responder
.\build\windows-msvc-debug\samples\mqttv5\Debug\az_iot_sample_direct_method_responder.exe
```

## How it ends

Runs about 60 seconds, then closes. Exit code 0 when the hub connection was reached; otherwise 1.

## Expected output

```
Connected. Listening for 'echo' invocations (~60s)...
probe for method 'echo', caller waits 30 second(s) for a result
method 'echo' invoked, 17 byte payload
```

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
| Declare the method | `az_iot_mqttv5_direct_method_client_register_method()` |
| Probe handler | `az_iot_mqttv5_direct_method_client_set_probe_handler()`, `on_probe()` |
| Answer inline | `on_echo()` → `az_iot_mqttv5_direct_method_respond()` |
