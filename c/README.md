<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Azure IoT C SDK

A C99 device SDK for Azure IoT Hub and the Azure IoT Hub Device Provisioning Service (DPS),
built for constrained and embedded devices.

> **Preview** (1.0.0-preview). APIs may change before the first stable release. See the
> [changelog](CHANGELOG.md).

## Features

- **Both IoT Hub generations:** mqttv3 (MQTT 3.1.1) and mqttv5 (MQTT 5). DPS tells the device which
  one it was assigned to, and the SDK picks the protocol.
- **Device features:** telemetry, device twin, direct methods, cloud-to-device messages and file
  upload (mqttv3), and software updates.
- **X.509 authentication:** certificates from files, certificates issued by DPS from a CSR,
  renewal over IoT Hub (mqttv3 only), and private keys held in a PKCS#11 token or TPM.
- **Resilient connections:** reconnection with backoff, re-provisioning, WebSockets and HTTP
  proxy support.
- **Embedded-friendly API:** callbacks run on the thread that calls into the SDK, and the
  connection and feature clients do no dynamic allocation.
- **Pluggable MQTT:** Eclipse Paho C by default; bring your own MQTT client through a small
  adapter interface.

## Quickstart

On Linux, with the [build tools](samples/README.md#build-tools) installed (a C compiler, CMake
3.21+, Ninja, OpenSSL 3 development files), from this directory:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_unified_telemetry
./build/linux-gcc-debug/samples/unified/az_iot_sample_telemetry
```

The sample needs a DPS enrollment for the device and five environment variables. The
[telemetry sample](samples/unified/telemetry/README.md) explains both; the
[samples overview](samples/README.md) covers Windows and every other sample.

## Where to go next

| I want to... | Read |
| --- | --- |
| Run a sample | [Samples](samples/README.md) |
| Understand how the SDK fits together | [Architecture](docs/architecture.md) |
| Connect a device: states, provisioning, reconnection, proxies, certificates | [Connecting a device](docs/connecting.md) |
| Use my own MQTT client library | [Bring your own MQTT client](docs/how_to_byo_mqtt_client.md) |
| Know what stays compatible between releases | [Struct versioning](docs/struct_versioning.md) |
| Install the SDK, consume it from CMake or pkg-config, or harden builds | [Building and installing](docs/eng/building.md) |
| Read the design and engineering notes | [docs/eng](docs/eng/) |

## Support

- Bugs and feature requests: [GitHub issues](https://github.com/Azure/azure-iot-sdk/issues).
- Security issues: do not file a public issue; see [SECURITY.md](../SECURITY.md).

## License

[MIT](../LICENSE).
