<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Azure IoT C SDK

[![ci-c](https://github.com/Azure/azure-iot-sdk/actions/workflows/ci-c.yml/badge.svg?branch=main)](https://github.com/Azure/azure-iot-sdk/actions/workflows/ci-c.yml)

A C99 device SDK for Azure IoT Hub and the Azure IoT Hub Device Provisioning Service (DPS),
built for constrained and embedded devices.

> **Preview** (1.0.0-preview). APIs may change before the first stable release. See the
> [changelog](CHANGELOG.md).

## Table of Contents

- [Features](#features)
- [Getting Started](#getting-started)
  - [Getting the SDK](#getting-the-sdk)
  - [Quickstart](#quickstart)
  - [Samples](#samples)
- [Documentation](#documentation)
- [Platforms and Porting](#platforms-and-porting)
- [Getting Help](#getting-help)
- [Contributing](#contributing)
  - [Reporting Security Issues](#reporting-security-issues)
  - [License](#license)

## Features

- **Both IoT Hub generations:** mqttv3 (MQTT 3.1.1) and mqttv5 (MQTT 5). DPS tells the device which
  one it was assigned to, and the SDK picks the protocol.
- **X.509 authentication:** certificates from files, certificates issued by DPS from a CSR,
  renewal over IoT Hub (mqttv3 only), and private keys held in a PKCS#11 token or TPM.
- **Resilient connections:** reconnection with backoff, re-provisioning, WebSockets and HTTP
  proxy support.
- **Embedded-friendly API:** callbacks run on the thread that calls into the SDK, and the
  connection and feature clients do no dynamic allocation.
- **Pluggable MQTT:** Eclipse Paho C by default; bring your own MQTT client through a small
  adapter interface.

Device features by IoT Hub generation:

| Feature | mqttv3 | mqttv5 |
| --- | --- | --- |
| Telemetry | Yes | Yes |
| Device twin | Yes | Yes |
| Direct methods | Yes | Yes |
| Cloud-to-device messages | Yes | No |
| File upload | Yes | No |
| Certificate renewal over IoT Hub | Yes | No |
| Software updates | Yes, over DPS | Yes, over DPS |

## Getting Started

### Getting the SDK

Build it from source. Add `c/` to your CMake project with `add_subdirectory()` or `FetchContent`,
or install it and use `find_package(azure-iot-sdk)`. See
[Building and installing](docs/eng/building.md#install-and-consume).

### Quickstart

On Linux, with the [build tools](samples/README.md#build-tools) installed (a C compiler, CMake
3.21+, Ninja, OpenSSL 3 development files), from this directory:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_unified_telemetry
./build/linux-gcc-debug/samples/unified/az_iot_sample_telemetry
```

The sample needs a DPS enrollment for the device and five environment variables. The
[telemetry sample](samples/unified/telemetry/README.md) explains both.

### Samples

The [samples overview](samples/README.md) lists every sample, with Windows build steps. Each
sample has its own README.

## Documentation

| I want to... | Read |
| --- | --- |
| Understand how the SDK fits together | [Architecture](docs/architecture.md) |
| Connect a device: states, provisioning, reconnection, proxies, certificates | [Connecting a device](docs/connecting.md) |
| Use my own MQTT client library | [Bring your own MQTT client](docs/how_to_byo_mqtt_client.md) |
| Know what stays compatible between releases | [Struct versioning](docs/struct_versioning.md) |
| Install the SDK, consume it from CMake or pkg-config, or harden builds | [Building and installing](docs/eng/building.md) |
| Read the design and engineering notes | [docs/eng](docs/eng/) |

API reference: the public headers in [inc/azure/iot](inc/azure/iot/); each function is documented
in its header.

## Platforms and Porting

- CI builds and tests the SDK on Linux (GCC, Clang) and Windows (MSVC), and builds it in strict
  C99, C11, C17 and C23 modes.
- A [Yocto layer](platforms/yocto/meta-azure-iot-sdk/README.md) (scarthgap) builds the libraries,
  headers, CMake package and pkg-config files.
- To port to another platform, supply an MQTT adapter
  ([Bring your own MQTT client](docs/how_to_byo_mqtt_client.md)) and, for software updates, the
  platform and crypto hooks ([ESP32 sample](samples/software_update/esp32/README.md)).

## Getting Help

To get help, or to post a suggestion or comment, please file a
[GitHub issue](https://github.com/Azure/azure-iot-sdk/issues/new).

## Contributing

See [Contributing](../README.md#contributing).

### Reporting Security Issues

Please do not report security vulnerabilities through public GitHub issues. See
[SECURITY.md](../SECURITY.md).

### License

Licensed under the [MIT](../LICENSE) license.
