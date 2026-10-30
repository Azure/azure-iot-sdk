<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# C SDK samples

Every sample has a README next to its source with what it shows, what it needs, how to build and
run it, and what to expect.

## Choose a sample

| Folder | Serves | Use when |
| --- | --- | --- |
| [`unified/`](unified/) | mqttv3 (MQTT 3.1.1) **or** mqttv5 (MQTT 5), whichever DPS assigns | Default. The device does not control which hub it is provisioned to. |
| [`mqttv5/`](mqttv5/) | mqttv5 only | The device is known to be on an mqttv5 hub. |
| [`authentication/`](authentication/README.md) | Either | Certificate providers, certificates issued by DPS, renewal, keys in hardware. |
| [`software_update/`](software_update/) | Either | Software updates agent. |

| Sample | What it shows |
| --- | --- |
| [unified/telemetry](unified/telemetry/README.md) | Telemetry to either hub generation. **Start here**: the build-before-open pattern most `unified/` samples follow. |
| [unified/connect_first](unified/connect_first/README.md) | The same, building the client after connecting, from the assigned generation. |
| [unified/twin_get_patch](unified/twin_get_patch/README.md) | Device twin: GET, reported PATCH, desired updates. |
| [unified/direct_method_responder](unified/direct_method_responder/README.md) | Direct methods answered inside the handler. |
| [unified/direct_method_slow_responder](unified/direct_method_slow_responder/README.md) | Direct methods answered after the handler returned. |
| [unified/c2d_receiver](unified/c2d_receiver/README.md) | Cloud-to-device messages (mqttv3 only). |
| [unified/file_upload](unified/file_upload/README.md) | File upload to Azure Storage (mqttv3 only). |
| [unified/websockets](unified/websockets/README.md) | Telemetry over MQTT over WebSockets (port 443). |
| [unified/proxy](unified/proxy/README.md) | Telemetry through an HTTP proxy. |
| [unified/multithreaded_telemetry](unified/multithreaded_telemetry/README.md) | Telemetry with readings from other threads, handed to the one thread that calls the SDK. |
| [mqttv5/telemetry](mqttv5/telemetry/README.md) | Telemetry on an mqttv5 hub. |
| [mqttv5/twin_get_patch](mqttv5/twin_get_patch/README.md) | Device twin on an mqttv5 hub. |
| [mqttv5/direct_method_responder](mqttv5/direct_method_responder/README.md) | Direct methods on an mqttv5 hub, with a probe handler. |
| [mqttv5/direct_method_slow_responder](mqttv5/direct_method_slow_responder/README.md) | Direct methods answered later, on an mqttv5 hub. |
| [mqttv5/custom_topic_publisher](mqttv5/custom_topic_publisher/README.md) | Publish to a custom topic on an mqttv5 hub. |
| [authentication/dps_csr_managed](authentication/dps_csr_managed/README.md) | DPS issues an operational certificate from a CSR. |
| [authentication/dps_persist_assignment](authentication/dps_persist_assignment/README.md) | Save the DPS assignment; later runs connect to the hub without registering. |
| [authentication/custom_certificate_provider](authentication/custom_certificate_provider/README.md) | The same, with the CSR built by application code. |
| [authentication/hub_renew](authentication/hub_renew/README.md) | Renew the operational certificate over an mqttv3 hub. |
| [authentication/hsm_pkcs11](authentication/hsm_pkcs11/README.md) | Private key in a PKCS#11 token or TPM. |
| [authentication/hsm_sign_callback](authentication/hsm_sign_callback/README.md) | Private key reachable only through a `sign()` callback. |
| [authentication/custom_provider_template](authentication/custom_provider_template/README.md) | Skeleton of a certificate provider. |
| [authentication/dps_tpm](authentication/dps_tpm/README.md) | DPS TPM attestation over HTTPS, then hub SAS tokens signed by a simulated TPM. |
| [software_update/pc/simulated_onboarding](software_update/pc/simulated_onboarding/README.md) | Software updates before registration, simulated install; no IoT Hub needed. |
| [software_update/pc/simulated_regular](software_update/pc/simulated_regular/README.md) | Software updates for a registered device, simulated install. |
| [software_update/pc/http_download](software_update/pc/http_download/README.md) | Software updates with a real, non-blocking HTTPS download (libcurl) through a proxy; simulated install. |
| [software_update/pc/multi_step](software_update/pc/multi_step/README.md) | A multi-step update: per-step hooks, rollback, failure codes, step result details and the final report. |
| [software_update/pc/reboot_resume](software_update/pc/reboot_resume/README.md) | An update that needs a reboot: atomic state persistence, when to reboot, and resume. |
| [software_update/esp32](software_update/esp32/README.md) | Software updates with a real over-the-air install on an ESP32. |

## How a unified sample handles the hub generation

DPS returns the hub's connection profile with the assignment, and it can change while a device
runs: a device moved to another hub re-provisions and may land on the other generation. A feature
client pins its generation at `init()`. If DPS assigns the other one, the connection stops with
`AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH` before that hub is reached, and the state event carries
the assigned profile. The `unified/` samples respond in one of three ways:

- `telemetry`, `twin_get_patch`, `direct_method_responder`, `direct_method_slow_responder`,
  `websockets`, `proxy`: build their clients before `open()`; on a mismatch they destroy them,
  build the other generation's, and call `close()` and `open()`.
- `connect_first`: opens with no feature client, builds once `CONNECTED` from the assigned profile,
  and on a later mismatch destroys the client and reopens without one.
- `c2d_receiver`, `file_upload`: mqttv3 features with no mqttv5 counterpart. On an mqttv5
  assignment they report it and exit 1.

See [Architecture](../docs/architecture.md#hub-generations-mqttv3-and-mqttv5).

## Prerequisites

### Azure

The samples provision through the Azure IoT Hub Device Provisioning Service (DPS) with an X.509
certificate. Each README lists what else it needs. For an mqttv3 hub,
[Quickstart: Provision an X.509 certificate simulated device](https://learn.microsoft.com/azure/iot-dps/quick-create-simulated-device-x509)
walks through creating the DPS instance, the linked IoT Hub and the enrollment.

### Build tools

The build fetches Paho MQTT C and azure-sdk-for-c itself.

**Linux** (Debian/Ubuntu package names):

```sh
sudo apt-get update
sudo apt-get install -y git build-essential cmake ninja-build libssl-dev ca-certificates pkg-config
```

- A C99 compiler (GCC or Clang), CMake 3.21+, Ninja.
- OpenSSL 3.0 to 4.1 development files (`libssl-dev`), for TLS and for the samples that need
  OpenSSL.
- `/etc/ssl/certs/ca-certificates.crt` (from `ca-certificates`) works as `AZ_IOT_TRUSTED_CA`.

**Windows:**

- [Visual Studio 2022](https://visualstudio.microsoft.com/vs/) with the *Desktop development with
  C++* workload, or the
  [Build Tools for Visual Studio 2022](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022).
- [CMake 3.21+](https://cmake.org/download/) (bundled with Visual Studio).
- OpenSSL 3.0 to 4.1 visible to CMake, either:
  - the full Win64 installer (not *Light*, which has no headers or libraries) from
    [OpenSSL for Windows](https://slproweb.com/products/Win32OpenSSL.html). CMake finds its default
    install folder; otherwise add `-DOPENSSL_ROOT_DIR=<folder>`. If the installer put the DLLs in
    `<folder>\bin`, add that folder to `PATH` before running the samples. Or
  - vcpkg, through the manifest in `c/vcpkg.json`: add
    `-DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_MANIFEST_FEATURES=paho`
    to the configure command. vcpkg then builds OpenSSL itself; `vcpkg install` alone is not
    enough.

  Or build without OpenSSL: see [Windows without OpenSSL](#windows-without-openssl).
- Run the build from a *Developer PowerShell for VS 2022*.

### Windows without OpenSSL

The az_mqtt adapter uses Schannel on Windows. Built alone, it needs no OpenSSL:

```powershell
cmake --preset windows-msvc-debug -DAZ_IOT_WITH_PAHO=OFF -DAZ_IOT_WITH_AZ_MQTT=ON -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
```

`CMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON` keeps an installed OpenSSL out of the build. Devices
authenticate with X.509 PEM files (`AZ_IOT_CLIENT_CERT`, `AZ_IOT_CLIENT_KEY`); with a client
certificate, Schannel connects with TLS 1.2. Not available, because they need OpenSSL:

- The Paho adapter, whose TLS is OpenSSL.
- Key references (`tls.client_key_uri`, e.g. `pkcs11:`), refused with `AZ_IOT_ERR_NOT_SUPPORTED`;
  the `hsm_pkcs11` sample is not built. The sign callback (`tls.sign`) and encrypted keys
  (`tls.client_key_password`) are Paho-only.
- The OpenSSL crypto backend. SAS tokens and Software Update signature checks need a crypto
  backend: the mbedTLS one (`AZ_IOT_WITH_CRYPTO_MBEDTLS`, built when mbedTLS is found) or your own.
  Without one, `dps_sas_key` and the Software Update samples are not built.
- The managed certificate provider and the samples' OpenSSL certificate, CSR, token and TPM code:
  `custom_certificate_provider`, `dps_csr_managed`, `dps_persist_assignment`,
  `dps_sas_key_issued_cert`, `hub_renew`, `user_provided_sas_token` and `dps_tpm` are not built.

## Configuration

The `unified/` and `mqttv5/` samples read:

| Variable | Required | Meaning |
| --- | --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | yes | DPS ID scope. |
| `AZ_IOT_DPS_REGISTRATION_ID` | yes | Registration ID; must equal the device certificate's common name. |
| `AZ_IOT_CLIENT_CERT` | yes | Device certificate chain (PEM file, leaf first). |
| `AZ_IOT_CLIENT_KEY` | yes | Device private key (PEM file). |
| `AZ_IOT_TRUSTED_CA` | yes | CA bundle (PEM file) that validates the DPS and IoT Hub server certificates. |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | Provisioning endpoint. Default `global.azure-devices-provisioning.net`. |
| `AZ_IOT_SAMPLE_LOG_TO_FILE` | no | Any non-empty value writes SDK logs to `azure-iot-sdk-c.log` in the working directory, rotated, instead of stderr. See [Logging](../docs/logging.md). |

> **Production.** Reading the private key from a file is for development: a key file can be
> copied, and the device's identity with it. In production, keep device keys in an HSM, TPM or
> secure element; see [Production keys](../docs/connecting.md#production-keys) and the
> [`hsm_pkcs11`](authentication/hsm_pkcs11/README.md) and
> [`hsm_sign_callback`](authentication/hsm_sign_callback/README.md) samples.

A sample with a missing required variable names it and exits 1. Other samples add or replace
variables; their READMEs list them. `AZ_IOT_SAMPLE_LOG_TO_FILE` applies to every sample except
`authentication/hsm_sign_callback`, `authentication/custom_provider_template` and the ESP32 one.

## Build

From the repository's `c/` directory, all samples at once:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
```

Use `windows-msvc-debug` on Windows. Each README gives the single-sample target and the path of
the binary: `build/<preset>/samples/<folder>/` (Windows: `.../<folder>/Debug/`).

Each sample that connects is built once per MQTT adapter in the build, from the same `main.c`:

| Adapter | Option | Target | Binary |
| --- | --- | --- | --- |
| Paho | `AZ_IOT_WITH_PAHO=ON` (default) | `az_iot_sample_<...>` | `samples/<folder>/` |
| az_mqtt | `AZ_IOT_WITH_AZ_MQTT=ON` | `az_iot_sample_<...>_az_mqtt` | `samples/<folder>/az_mqtt/` |

`main.c` selects the adapter's header and factories with `AZ_IOT_SAMPLE_ADAPTER_PAHO` or
`AZ_IOT_SAMPLE_ADAPTER_AZ_MQTT`, which the build defines. With samples on, at least one of the two
adapters is required. The SAS-key and Software Update samples use the OpenSSL crypto backend, or
mbedTLS when OpenSSL is not built (`AZ_IOT_SAMPLE_CRYPTO_OPENSSL`, `AZ_IOT_SAMPLE_CRYPTO_MBEDTLS`).
