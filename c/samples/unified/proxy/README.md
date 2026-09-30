<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Telemetry through an HTTP proxy (`unified/proxy`)

[`unified/telemetry`](../telemetry/README.md), with every MQTT session tunnelled through an HTTP
`CONNECT` proxy, for networks where a proxy is the only way out. Everything else is identical, so
the difference between the two `main.c` files is exactly the feature.

## Sample features

- Hub generations: mqttv3 (MQTT 3.1.1) and mqttv5 (MQTT 5), whichever DPS assigns, including after the device is moved to a hub of the other generation.
- The whole feature is `copts.proxy.host` and `copts.proxy.port`, plus optional Basic credentials.
- TLS is unaffected: it runs end to end with the service inside the tunnel. The proxy sees only ciphertext, and certificate and hostname validation are unchanged.
- Applies to the DPS connect as well as the hub connect.
- If the proxy cannot be reached, the connect fails. The SDK never falls back to a direct connection.
- Stays on TCP (8883 through the tunnel). Combine with [`websockets`](../websockets/README.md) for a network that also only passes 443.
- Platforms: Linux and Windows.

## Service requirements

- An Azure IoT Hub Device Provisioning Service (DPS) instance linked to an IoT Hub.
- An X.509 enrollment (individual or group) for the device. For an mqttv3 hub,
  [Quickstart: Provision an X.509 certificate simulated device](https://learn.microsoft.com/azure/iot-dps/quick-create-simulated-device-x509) walks through the setup.
- To exercise mqttv5, the device's enrollment must assign it to an mqttv5 IoT Hub.
- An HTTP proxy that allows `CONNECT` to the DPS and IoT Hub hosts on port 8883.

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
| `AZ_IOT_PROXY_HOST` | yes | Proxy host name or IP address. |
| `AZ_IOT_PROXY_PORT` | no | Proxy port. Default 8080. |
| `AZ_IOT_PROXY_USERNAME` | no | User name for a proxy that requires HTTP Basic authentication. |
| `AZ_IOT_PROXY_PASSWORD` | no | Password; used only with `AZ_IOT_PROXY_USERNAME`. |

```sh
export AZ_IOT_DPS_ID_SCOPE='<id-scope>'
export AZ_IOT_DPS_REGISTRATION_ID='<registration-id>'
export AZ_IOT_CLIENT_CERT="$PWD/device-cert.pem"
export AZ_IOT_CLIENT_KEY="$PWD/device-key.pem"
export AZ_IOT_TRUSTED_CA='/etc/ssl/certs/ca-certificates.crt'
export AZ_IOT_PROXY_HOST='<proxy-host>'
```

PowerShell:

```powershell
$env:AZ_IOT_DPS_ID_SCOPE        = '<id-scope>'
$env:AZ_IOT_DPS_REGISTRATION_ID = '<registration-id>'
$env:AZ_IOT_CLIENT_CERT         = "$PWD\device-cert.pem"
$env:AZ_IOT_CLIENT_KEY          = "$PWD\device-key.pem"
$env:AZ_IOT_TRUSTED_CA          = "$PWD\ca.pem"
$env:AZ_IOT_PROXY_HOST          = '<proxy-host>'
```

## Build and run

Build prerequisites (toolchain, CMake, OpenSSL) are listed in the
[samples overview](../../README.md#prerequisites). Run from the repository's `c/` directory.

Linux:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_unified_proxy
./build/linux-gcc-debug/samples/unified/az_iot_sample_proxy
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_unified_proxy
.\build\windows-msvc-debug\samples\unified\Debug\az_iot_sample_proxy.exe
```

## How it ends

Runs about 60 seconds. Exit code 0 when at least one message was sent and the connection never settled at `FAULTED`; otherwise 1. A `FAULTED` connection ends the run early. Without `AZ_IOT_PROXY_HOST` it prints `AZ_IOT_PROXY_HOST is not set; nothing to demonstrate.` and exits 1.

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
| Connect fails while the other samples connect | The proxy is unreachable, refuses `CONNECT` to port 8883, or needs credentials. |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| Configure the proxy | `copts.proxy` in `main()` |
| Everything else | as in [`unified/telemetry`](../telemetry/README.md#where-to-look-in-mainc) |
