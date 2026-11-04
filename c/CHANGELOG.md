# Release History

## 2.0.0-preview.1 (2026-11-05)

Public preview of the C SDK for Azure IoT Hub (mqttv3 and mqttv5), the Azure IoT Hub Device
Provisioning Service (DPS) and software updates through Azure Device Update: a C99 device SDK for
constrained and embedded devices. It has everything in the GA release 1.0.0 (mqttv3, DPS), which is
on [`main`](https://github.com/Azure/azure-iot-sdk/tree/main/c), plus mqttv5 hubs, MQTT 5 and
software updates. APIs may change before a stable release.

### Features Added

#### Connecting a device

- Connection client (`az_iot_connection_client.h`): registers the device with DPS and connects it
  to the assigned IoT Hub over MQTT 3.1.1 (mqttv3) or MQTT 5 (mqttv5), or connects to a known hub
  directly. See [docs/connecting.md](docs/connecting.md).
- Both IoT Hub generations: DPS returns the hub's `connectionProfile` with the assignment and the
  SDK picks the MQTT version from it (`az_iot_connection_client_get_hub_profile()`). A connection
  serves one generation at a time: a feature client of the other one fails with
  `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH`, and an unknown profile with
  `AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED`. Without DPS, an mqttv5 hub is reached with
  `connection_profile = AZ_IOT_CONNECTION_PROFILE_MQTT_V5`. See
  [docs/architecture.md](docs/architecture.md#hub-generations-mqttv3-and-mqttv5).
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
- One MQTT session at a time: the provisioning session and the hub session do not overlap. With
  software updates, an update check or status report made while the hub is connected ends the hub
  session, which reconnects to its assignment once the check is done; this planned end
  (`AZ_IOT_CONNECTION_SESSION_END_PLANNED`) does not count toward reconnection backoff or
  re-provisioning.

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
  and TPM software stack.
- Crypto backends: OpenSSL 3 (`az_iot_crypto_openssl.h`), mbedTLS (`az_iot_crypto_mbedtls.h`), or
  the application's own (`az_iot_crypto.h`). See [docs/connecting.md](docs/connecting.md#crypto-backend).
- mqttv5 hubs need X.509 (keys in hardware included); they do not accept SAS yet. Certificate
  renewal over IoT Hub is mqttv3 only.

#### IoT Hub features (mqttv3)

- Telemetry, with a documented delivery contract ([docs/telemetry.md](docs/telemetry.md)).
- Cloud-to-device messages, with their properties read on demand.
- Direct methods, answered in the handler or later.
- Device twin: GET, reported properties and desired property updates.
- File upload to Azure Storage, through an HTTP transport the application provides.

#### IoT Hub features (mqttv5)

- Telemetry (`az_iot_mqttv5_*` clients) with the same delivery contract: application properties
  are sent as MQTT 5 user properties, `$.ct` as the Content Type and `$.cid` as the Correlation
  Data. See [docs/telemetry.md](docs/telemetry.md#mqttv5-message-properties).
- Device twin: GET, reported properties and desired property updates; GETs are sent again on the
  next session when unanswered. With `twin_push.push_desired`, the hub pushes the desired
  snapshot on connect and each desired patch; with `push_desired` or `push_reported`, a requested
  push that does not arrive ends the session and reconnects.
- Direct methods, answered in the handler or later, with a probe handler for the hub's delivery
  probes. Probe-acks, results and abandons are held while the hub is not `CONNECTED` and published
  again after a lost PUBACK, until their Message Expiry or result deadline; a result is never
  published at or after its deadline (`respond()` then returns `AZ_IOT_ERR_TIMEOUT`).
  `az_iot_connection_client_options::max_unconfirmed_delivery_timeout_ms` bounds messages that
  carry no Message Expiry.
- Custom topics (`az_iot_custom_topic_client.h`): publish at QoS 0 or 1, with content type, user
  properties and message expiry, to the topics the hub's topic templates allow
  (`az_iot_mqttv5_custom_topic_client_format_topic()` fills in `{deviceId}`). Publish only.
- Presence handshake: after CONNACK the client subscribes to its device topics and publishes a
  birth message, and announces `CONNECTED` only on a matching birth-ack (a malformed one is
  ignored). A presence SUBACK missing after `subscription_ack_timeout_seconds`, or a birth-ack
  after 5 to 15 minutes (longer after consecutive misses), ends the session with a DISCONNECT and
  reconnects.
- WebSockets use the hub's path `/mqtt` (`AZ_IOT_MQTT_HUB_V5_WEBSOCKET_PATH`).
- Not on mqttv5: cloud-to-device messages, file upload and certificate renewal over IoT Hub.

#### MQTT clients

- Eclipse Paho C adapter (`az_iot_adapter_paho.h`), the default.
- az_mqtt adapter (`AZ_IOT_WITH_AZ_MQTT=ON`, `az_iot_adapter_az_mqtt.h`): MQTT 3.1.1 and MQTT 5
  over the bundled [deps/az_mqtt](deps/az_mqtt/README.md) client, with no thread of its own; TLS
  through OpenSSL 3, mbedTLS, or Schannel on Windows.
- Bring your own MQTT client through `az_iot_mqtt_adapter` (`az_iot_mqtt_iface.h`), with a
  conformance test suite for it. See [docs/how_to_byo_mqtt_client.md](docs/how_to_byo_mqtt_client.md).
- MQTT 5 in both adapters and the adapter interface: user properties read through a view
  (`az_iot_mqtt_user_properties_next()` / `_find()`), with no fixed count; Correlation Data and
  Message Expiry; Reason Strings of a refused CONNACK (az_mqtt only) or SUBACK and of a server
  DISCONNECT, reported in state events (`error->message`) and to
  `az_iot_subscription_failed_callback`; a refused PUBACK leaves the session up, and its Reason
  String goes to the publish's result callback (`az_iot_publish_result::reason_string`, e.g. that
  of `az_iot_mqttv5_custom_topic_client_publish()`); PUBACK reason codes mapped by
  `az_iot_mqtt_puback_result()` (`AZ_IOT_ERR_PUBLISH_REFUSED`, `AZ_IOT_ERR_BUSY` for quota). The
  conformance suite covers MQTT 5.

#### Logging

- Log levels and components, application sinks and a rotating file sink. See
  [docs/logging.md](docs/logging.md).

#### Software updates

- Software updates client (`az_iot_su.h`) for
  [Azure Device Update](https://learn.microsoft.com/azure/iot-hub-device-update/), for mqttv3 and
  mqttv5 hubs: the device pulls updates over its DPS connection with its X.509 identity, verifies
  them, installs them through the application and reports the result. Update checks before
  registration (`az_iot_su_client_request_onboarding_update()`) or for a registered device
  (`az_iot_su_client_request_update()`), each bounded by a timeout
  (`AZ_IOT_SU_REQUEST_DEFAULT_TIMEOUT_MS`, 120 s). See
  [docs/software-update.md](docs/software-update.md).
- Three integrations: managed, with platform hooks for download, install, apply and persistence;
  library mode, where the application installs each verified update and reports it
  (`az_iot_su_client_report_status()`, with 0 to `AZ_IOT_SU_MAX_STEP_COUNT` step results, as the
  Device Update device API allows); and transport-free primitives
  (`az_iot_su_verify_manifest()`, `az_iot_su_parse_update_request()`,
  `az_iot_su_verify_file_hash()`, `az_iot_su_build_report()`,
  `az_iot_su_decode_manifest_string()`).
- Trust: signed manifests verified against root keys (Microsoft's compiled in) and files against
  their SHA-256. Updates with up to 10 steps and 10 files (`AZ_IOT_SU_MAX_STEP_COUNT`,
  `AZ_IOT_SU_MAX_FILE_COUNT`, `AZ_IOT_SU_MAX_FILE_COUNT_PER_STEP`); a manifest over a limit, or
  whose steps name a missing file, is refused before any step runs.
- Supported manifests: inline steps, and in managed mode detached (mini) manifests
  (`detachedManifestFileId`, sent when a manifest exceeds 4,096 bytes): the client downloads the
  detached manifest file with `download_fn`, reads it into the update copy with `read_file_fn`,
  checks its size and SHA-256 against the signed mini manifest and installs the full manifest it
  carries; it is persisted, so resume does not download it again
  (`AZ_IOT_SU_MANIFEST_ERROR_DETACHED_*` on failure; `AZ_IOT_SU_FILE_URL_CAPACITY()` counts its
  URL). `reference` steps, and mini manifests in library mode, are not supported in this preview.
  A verified manifest with no step, a `reference` step, or a mini manifest in library mode is
  refused before any hook runs: the workflow fails with `AZ_IOT_SU_MANIFEST_ERROR_NO_STEPS`,
  `AZ_IOT_SU_MANIFEST_ERROR_REFERENCE_STEP` or `AZ_IOT_SU_MANIFEST_ERROR_DETACHED_MANIFEST`,
  library mode does not offer it, and
  `az_iot_su_parse_update_request()` returns `AZ_IOT_ERR_INVALID_ARG` or
  `AZ_IOT_ERR_NOT_SUPPORTED`. The parsed manifest exposes `detached_manifest_file_id`, and each
  step its `type` (`az_iot_su_client_update_manifest_step_type`) and `detached_manifest_file_id`.
- Multi-step workflows with per-step results, rollback, result codes and step result details
  (`az_iot_su_client_set_step_result_details()`). Reboot and resume: the workflow state is
  persisted through the platform hooks, `az_iot_su_client_resume()` continues it after a restart,
  and `az_iot_su_client_get_reboot_state()` says when a reboot is safe. See
  [docs/software-update.md](docs/software-update.md#reboot-and-resume).
- Delta updates: download handlers (`az_iot_su_download_handler`) build a file from a related
  file, with fallback to a full download (`AZ_IOT_SU_FACILITY_DELTA` result codes, outcome in
  `workflow_ended_fn`). The reference
  `microsoft/delta:1` handler (`az_iot_su_delta.h`, `AZ_IOT_WITH_SU_DELTA=ON`, POSIX) uses the
  Device Update delta processor, loaded at run time. See
  [docs/software-update.md](docs/software-update.md#delta-updates).
- Device properties (`az_iot_su_client_update_device_properties()`) and, when set, the installed
  update id are sent with every check; the applied update id is adopted after a successful update.
- Events for each stage (`AZ_IOT_SU_EVENT_*`), including no update, duplicate and refused offers,
  accepted reports, a completed workflow and a retry wait. A failed workflow offered again is
  retried after 1 h, doubling up to 24 h; cancellation is service-side only. Service errors and
  report conflicts are handled as the service requires.
- Memory from one application-provided workspace, sized by run-time limits (`az_iot_su_limits`,
  `az_iot_su_workspace_size()`, `AZ_IOT_SU_WORKSPACE_SIZE()`); `az_iot_su_client_init()` refuses
  a client whose layout differs from the SDK build. See
  [docs/software-update.md](docs/software-update.md#memory-and-limits).

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
- Preview additions: the `mqttv5` and `su_delta` package components; the ESP-IDF software updates
  OTA platform hooks (`az_iot_su_esp32_ota.h`); libcurl 7.55.0+ for the software update download
  sample. The installed package accepts the same minor version only (`SameMinorVersion`) while in
  preview.

#### Samples

- [`unified/`](samples/README.md): telemetry, device twin, direct methods, cloud-to-device
  messages, file upload, WebSockets, HTTP proxy, multithreaded telemetry, and an ESP32 sample.
- [`authentication/`](samples/authentication/README.md): DPS with SAS keys, CSR-issued
  certificates, saved assignments and TPM attestation; certificate renewal over IoT Hub; keys in a
  PKCS#11 token or behind a `sign()` callback; user-provided SAS tokens; custom certificate
  providers.
- `unified/` and `authentication/` samples serve whichever hub generation DPS assigns, except the
  mqttv3-only `c2d_receiver`, `file_upload`, `hub_renew` and `dps_csr_managed`, and those that
  authenticate to the hub with SAS (`dps_sas_key`, `user_provided_sas_token`, `dps_tpm`).
- [`mqttv5/`](samples/README.md): telemetry, device twin, direct methods (in the handler or later)
  and custom topics on an mqttv5 hub.
- [`software_update/`](docs/software-update.md#samples): onboarding and regular checks, HTTPS
  download through a proxy, multi-step updates, reboot and resume, and library mode with simulated
  installs on a PC; a real over-the-air install on an ESP32, also run in QEMU.

#### Tests

- Unit tests (cmocka), and the MQTT adapter conformance suite run against an MQTT broker for the
  Paho and az_mqtt adapters.
- End-to-end tests against IoT Hub and DPS over both adapters: SAS keys, X.509, DPS-issued
  certificates, keys in a PKCS#11 token, TPM attestation, file upload, and the ESP32 sample in
  QEMU. See [docs/eng/end-to-end-tests.md](docs/eng/end-to-end-tests.md).
- End-to-end tests on an mqttv5 hub (connection, transports, telemetry, twin, direct methods,
  custom topics, key custody) and for software updates against the service, delta updates
  included. See [docs/eng/end-to-end-tests.md](docs/eng/end-to-end-tests.md#software-updates-e2e).
- Install and consumer tests (CMake and pkg-config), Valgrind memcheck, MSVC AddressSanitizer,
  race detection (helgrind, DRD, ThreadSanitizer), and code coverage. See
  [docs/eng/test-coverage.md](docs/eng/test-coverage.md).
