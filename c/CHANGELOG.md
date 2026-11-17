# Release History

## 1.0.0 (2026-11-05)

Initial release of the C SDK for Azure IoT Hub (mqttv3) and the Azure IoT Hub Device Provisioning
Service (DPS): a C99 device SDK for constrained and embedded devices.

### Features Added

#### Connecting a device

- Connection client (`az_iot_connection_client.h`): registers the device with DPS and connects it
  to the assigned IoT Hub over MQTT 3.1.1, or connects to a known hub directly. See
  [docs/connecting.md](docs/connecting.md).
- Reconnection with exponential backoff and jitter (`az_iot_retry_policy.h`), identity recovery
  and re-provisioning when the hub refuses the device.
- Saved hub with DPS fallback: the DPS assignment callbacks let an application persist the
  assignment, connect to that hub after a restart and register again only when needed. See
  [docs/connecting.md](docs/connecting.md#saved-hub-with-dps-fallback).
- Connection states and error detail in state events. See
  [docs/connecting.md](docs/connecting.md#connection-states).
- MQTT over TCP (8883) or WebSockets (443), and through an HTTP CONNECT proxy.
- No dynamic allocation in the connection and feature clients: they run in workspaces the
  application provides, sized at run time. Single-threaded contract: every SDK call comes from one
  thread or is serialized by the application. See [docs/architecture.md](docs/architecture.md) and
  [docs/client-configuration.md](docs/client-configuration.md).

#### Authentication and certificates

- X.509 from a certificate provider (several certificates per role, tried in order) and SAS
  tokens from the device's primary and secondary keys or from the application
  (`on_sas_token_required`), with fallback on rejection and SAS token renewal. SAS token helpers
  in `az_iot_sas_token.h`. See [docs/connecting.md](docs/connecting.md#authentication).
- Certificate providers: PEM files (`az_iot_certificate_provider_pem.h`), a managed provider that
  generates the operational key (EC P-256, EC P-384 or RSA 2048) and its CSR, and the ESP-IDF
  `esp_secure_cert` partition.
- Operational certificates issued by DPS from a CSR at registration, and renewed over IoT Hub
  (`az_iot_connection_client_send_csr()`). See [docs/connecting.md](docs/connecting.md#certificates).
- Private keys kept in hardware: PKCS#11 tokens or TPM 2.0 through an OpenSSL 3 provider, PSA
  Crypto keys (e.g. the ESP32 Digital Signature peripheral), or a `sign()` hook. See
  [docs/connecting.md](docs/connecting.md#production-keys).
- DPS TPM attestation over HTTPS (`az_iot_dps_tpm_client.h`), with the application's HTTPS stack
  and TPM software stack. The `dps_tpm` sample runs it on a software TPM or, through tpm2-tss
  ESAPI, on a TPM (`samples/common/sample_tpm_tss.c`), and registers again when the hub refuses
  the identity.
- Crypto backends: OpenSSL 3 (`az_iot_crypto_openssl.h`), mbedTLS (`az_iot_crypto_mbedtls.h`), or
  the application's own (`az_iot_crypto.h`). See [docs/connecting.md](docs/connecting.md#crypto-backend).

#### IoT Hub features (mqttv3)

- Telemetry, with a documented delivery contract ([docs/telemetry.md](docs/telemetry.md)).
- Cloud-to-device messages, with their properties read on demand.
- Direct methods, answered in the handler or later.
- Device twin: GET, reported properties and desired property updates.
- File upload to Azure Storage, through an HTTP transport the application provides.

#### MQTT clients

- Eclipse Paho C adapter (`az_iot_adapter_paho.h`), the default.
- az_mqtt adapter (`AZ_IOT_WITH_AZ_MQTT=ON`, `az_iot_adapter_az_mqtt.h`): MQTT 3.1.1 over the
  bundled [deps/az_mqtt](deps/az_mqtt/README.md) client, with no thread of its own; TLS through
  OpenSSL 3, mbedTLS, or Schannel on Windows.
- Bring your own MQTT client through `az_iot_mqtt_adapter` (`az_iot_mqtt_iface.h`), with a
  conformance test suite for it. See [docs/how_to_byo_mqtt_client.md](docs/how_to_byo_mqtt_client.md).

#### Logging

- Log levels and components, application sinks and a rotating file sink. See
  [docs/logging.md](docs/logging.md).

#### Platforms, building and dependencies

- Linux (GCC, Clang) and Windows (MSVC); strict C99, also built as C11, C17 and C23.
- [ESP-IDF components](platforms/esp32/README.md) for esp32, esp32c3, esp32s3 and esp32c6.
- [Yocto layer](platforms/yocto/meta-azure-iot-sdk/README.md) (scarthgap).
- CMake: `add_subdirectory()`, `FetchContent`, or an installed package
  (`find_package(azure-iot-sdk)` with components) and pkg-config files; static or shared
  libraries; installed azure-sdk-for-c and Paho instead of fetched ones; a vcpkg manifest. See
  [docs/eng/building.md](docs/eng/building.md#install-and-consume).
- Dependencies: azure-sdk-for-c 1.5.0; Eclipse Paho C 1.3.16 with upstream fixes (an installed
  Paho must be 1.3.14 or later); OpenSSL 3.0 to 4.1; mbedTLS 3.6 LTS or 4.1+. See
  [docs/dependencies.md](docs/dependencies.md).

#### Samples

- [`unified/`](samples/README.md): telemetry, device twin, direct methods, cloud-to-device
  messages, file upload, WebSockets, HTTP proxy, multithreaded telemetry, and an ESP32 sample.
- [`authentication/`](samples/authentication/README.md): DPS with SAS keys, CSR-issued
  certificates, saved assignments and TPM attestation; certificate renewal over IoT Hub; keys in a
  PKCS#11 token or behind a `sign()` callback; user-provided SAS tokens; custom certificate
  providers.

#### Tests

- Unit tests (cmocka), and the MQTT adapter conformance suite run against an MQTT broker for the
  Paho and az_mqtt adapters.
- End-to-end tests against IoT Hub and DPS over both adapters: SAS keys, X.509, DPS-issued
  certificates, keys in a PKCS#11 token, TPM attestation, file upload, and the ESP32 sample in
  QEMU. See [docs/eng/end-to-end-tests.md](docs/eng/end-to-end-tests.md).
- Install and consumer tests (CMake and pkg-config), Valgrind memcheck, MSVC AddressSanitizer,
  race detection (helgrind, DRD, ThreadSanitizer), and code coverage. See
  [docs/eng/test-coverage.md](docs/eng/test-coverage.md).
