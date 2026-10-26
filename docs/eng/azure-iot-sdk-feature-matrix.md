# Azure/azure-iot-sdk — client feature matrix

Repo: `Azure/azure-iot-sdk` (**private** mono-repo). Consolidated across **both** shipping branches: `main` @ `3a8be1c` (2026-10-26) and `releases/public-preview` @ `1d5c0ee` (2026-10-22). Verified by reading the code and CI workflows, not docs or the public SDKs.

**This is not the old public SDK.** It ships two client libraries only — **C** (`/c`, C99, `AZ_IOT_VERSION_STRING "1.0.0-preview"` on both branches) and **.NET** (`/dotnet`, `net10.0`, `Microsoft.Azure.Iot.Device` **1.1.0** on `main` and **2.0.0-preview3** on the preview branch, published to **nuget.org**; the GitHub Packages CD workflow was removed in #479, and no in-repo workflow pushes the package). No Java/Node/Python/embedded columns exist.

### Reading the columns

The columns are **MQTT protocol versions**, because that is what determines wire capability. Each version targets a different IoT Hub:

| | Hub | MQTT | Encoding | C symbols | .NET types |
|---|---|---|---|---|---|
| **mqttv3** | the current Azure IoT Hub | v3.1.1 only | `$iothub/...` topics, percent-encoded property bag | `az_iot_mqttv3_*`, `inc/azure/iot/mqttv3/` | the mqttv3 path inside `Unified/*` |
| **mqttv5** | the new Azure IoT Hub | v5 only | `ih/{deviceId}/...` topics, protobuf + v5 user properties | `az_iot_mqttv5_*`, `inc/azure/iot/mqttv5/` | `MQTTv5/*`, directly or via `Unified/*` |

### Branch notes

The preview branch took all of `main` up to 2026-10-22 (#520). Since then `main` is 22 commits ahead (16 C, 6 .NET); the preview branch is 20 ahead (merges plus .NET mqttv5 work). A cell states whether the capability **exists at all**; a Note says which branch when it is not on both:

- **no branch note** — present on both branches.
- **"C: main only"** / **".NET: main only"** — on `main`, not yet on the preview branch.
- **".NET: preview only"** — the .NET side is on the preview branch but not on `main`.

The split in one line: the **.NET mqttv5 client set lives only on the preview branch**; the **work merged after 2026-10-22 is only on `main`** — C: direct-method replies held until connected, mqttv3 file upload for SAS-key devices, the thread-safety contract with TSan and real-stack helgrind/DRD, software-update result/adoption changes; .NET: session handling rework, feature opt-out, `DisconnectAsync` halting reconnection, QoS 1 re-send after interruption.

**`Unified` is not the .NET equivalent of C mqttv3** — it is a facade spanning both versions. On `main` only its mqttv3 half is reachable; the preview branch carries the whole thing.

- **C** exposes two sibling API families and makes the **application** branch: read `az_iot_connection_client_get_hub_profile()`, then instantiate either `az_iot_mqttv3_telemetry_client` or `az_iot_mqttv5_telemetry_client`. A mismatch is refused with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH`.
- **.NET** branches internally on `ConnectionProfile` at runtime.

- **DPS speaks MQTT v3.1.1**, whichever hub follows, and is **not a separate client** — it runs inside the connection client, which then picks the MQTT version from the DPS-returned `connectionProfile`. Exception: C TPM attestation uses a separate, application-driven HTTPS client (§5).
- C keeps **one** connection client and splits only the feature clients (`c/docs/eng/client-separation.md`).
- Shared protobuf contracts live in `common/Protos/{presence,directmethods,twin}.proto`; C hand-rolls the codec (`src/mqttv5/direct_method_codec.c`), .NET compiles them with Grpc.Tools.

Legend: **Yes** supported · **Partial** partial/caveated · **No** absent · **N/A** not applicable.

---

## 1. Architecture & lifecycle

| Feature | C mqttv3 | C mqttv5 | .NET mqttv3 | .NET mqttv5 | Notes |
|---|---|---|---|---|---|
| Maturity | Partial | Partial | Partial | Partial | C `1.0.0-preview` on both branches (header is the single source of truth, gated by `eng/check-version.sh`); .NET 1.1.0 on `main`, 2.0.0-preview3 on the preview branch |
| Single connection client, DPS internal | Yes | Yes | Yes | Yes | `az_iot_connection_client` / `AbstractConnectionClient`; provisioning is not an app step, except C TPM attestation (app-driven HTTPS client, §5). .NET mqttv5: preview only |
| Per-feature clients over one connection | Yes | Yes | Yes | Yes | telemetry / c2d / direct method / twin (+ file upload mqttv3 only), plus custom topics on mqttv5. .NET `ConnectionClientOptions.EnableTwin` / `EnableDirectMethods` / `EnableCloudToDeviceMessages` opt a feature out (#527, **.NET: main only**). .NET mqttv5: preview only |
| Generation selected at | client init (app picks the API) | ← | runtime, per connection | ← | C refuses a mismatch with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH`; .NET switches on `ConnectionProfile`. .NET mqttv5: preview only |
| Runtime generation query | Yes | Yes | Yes | Yes | `az_iot_connection_client_get_hub_profile()` / `ConnectionProfile`. .NET mqttv5: preview only |
| MQTTv3↔MQTTv5 fallback handled by | application | ← | library | ← | C app must instantiate the other API family; the .NET `Unified` facade branches for you. .NET mqttv5: preview only |
| API model | single-threaded `do_work()` pump, callbacks on caller thread, no internal threads | ← | Task-based async, `CancellationToken` on every op, `IDisposable` | ← | .NET has **no** `IAsyncDisposable` |
| Nullable / strictness | C99-strict, `-pedantic`, banned-construct + layering CI gates | ← | `<Nullable>enable</Nullable>` | ← | |
| Struct/ABI versioning | Partial | Partial | N/A | N/A | `_internal_size` on events + cert-provider vtable v2; `struct_versioning.md` still a proposal |
| Preview/experimental markers | Partial | Partial | No | N/A | No per-API attributes; C README carries the status banner |

## 2. Transport

| Feature | C mqttv3 | C mqttv5 | .NET mqttv3 | .NET mqttv5 | Notes |
|---|---|---|---|---|---|
| MQTT 3.1.1 | Yes | N/A | Yes | N/A | Required for MQTTv3 **and** for DPS in both generations |
| MQTT 5.0 | N/A | Yes | N/A | Yes | v5 user properties, correlation data, content type, message expiry, reason codes. .NET mqttv5: preview only |
| v5 user properties end-to-end | N/A | Yes | N/A | Yes | C sets outbound and extracts inbound, with conformance round-trip cases. .NET mqttv5: preview only |
| MQTT over WebSockets | Yes | Yes | Yes | Yes | C `samples/websockets`; .NET mqttv5 websocket URI. .NET mqttv5: preview only |
| HTTP CONNECT proxy | Yes | Yes | Partial | Partial | C `az_iot_mqtt_proxy_options` + `samples/proxy`; .NET websocket-only, and not forwarded to the nested mqttv5 client. .NET mqttv5: preview only |
| AMQP | No | No | No | No | Deliberate — see `c/docs/amqp_vs_mqtt_for_new_sdk_client.md`. Test-only AMQP under `c/tests/deps/amqp` |
| HTTPS | Partial | No | Partial | No | Only the file-upload SAS PUT/notify; C ships **no** HTTP client (app supplies a transport hook) |
| BYO MQTT client | Yes | Yes | Yes | Yes | C `az_iot_mqtt_iface` vtable + published conformance suite; .NET `IMqttClient` + `CustomMqttClientSample` |
| Shipped adapters | Paho v3.1.1 + v5 (default) | ← | MQTTnet 5.1.0.1559 (`MqttNetAdapter`) | ← | C adds **az_mqtt** (vendored, zero-allocation, v3+v5, `AZ_IOT_WITH_AZ_MQTT=OFF` by default, conformance-tested; **every C e2e suite runs over both Paho and az_mqtt** (#491); Windows client and in-memory CA certificates (#504)) and a Rust adapter that delegates to a runtime-installed FFI table, refusing only WebSockets and proxy connects |
| Allocation profile | Partial | Partial | N/A | N/A | Core is static/caller-allocated; PEM cert provider and the Paho adapter malloc |
| TLS version control | No | No | Yes | Yes | C exposes only `use_tls`; .NET forces Tls12/Tls13 and now always uses TLS, with or without a client certificate (#470). Neither can disable validation |
| Reconnect / connection maintenance | Yes | Yes | Yes | Yes | C `src/core/retry_policy.c` + public `az_iot_retry_policy.h`; .NET `MqttConnectionManager.MaintainConnectionAsync` |
| Keep-alive | Yes | Yes | Yes | Yes | C default 30 s; .NET default 60 s |
| Clean start / session expiry / LWT | Yes | Yes | Yes | Partial | C `resolve_session_options()` sets per role. .NET: clean session configurable via `UseCleanSession` (#528, **.NET: main only**); session expiry hard-coded to 0 (`MqttConnectionManager.cs`); mqttv5 still carries a `//TODO subscribe elide logic`. .NET mqttv5: preview only |
| Subscription-ack gating | Yes | Yes | **Yes** | **Yes** | Both check SUBACK reason codes. C failure scope is configurable (`az_iot_subscription_failure_scope`); .NET always disconnects and reconnects. .NET mqttv5: preview only |
| MQTT DISCONNECT reason code | N/A | **Yes** | **N/A** | Partial | v5 only — v3.1.1 has no reason-code field, so the .NET mqttv3 cell is N/A. C sets 0x04 on the mqttv5 hub; .NET exposes `MqttDisconnect.Reason` but always sends NormalDisconnection. .NET mqttv5: preview only |

## 3. Authentication

| Feature | C | .NET | Notes |
|---|---|---|---|
| X.509 client certificates | Yes | Yes | First auth source tried in C (`az_iot_auth_source`); .NET `X509AuthenticationProvider` |
| SAS token from symmetric key | **Partial** | **Partial** | Both: Yes for DPS and the mqttv3 hub; **the mqttv5 hub does not accept SAS yet**. C: base64 primary/secondary key, enrollment-group derivation. .NET `SymmetricKeyAuthenticationProvider` (#473): raw device key, no group-key derivation in the library; hub uses SAS until a certificate is issued. C mqttv3 file upload also works for SAS-key devices (#436, **C: main only**) |
| Application-supplied SAS tokens | **Partial** | No | C `on_sas_token_required` + `az_iot_connection_client_update_sas_token()`; needs `sas_buffer`, no crypto backend. **DPS and mqttv3 hub only** — the mqttv5 hub refuses SAS |
| SAS renewal before expiry | **Partial** | **Partial** | C renews at `renewal_percent` of lifetime (default 80) and reconnects; surfaced as `is_credential_renewal`. .NET mints a fresh token on every connect attempt (configurable lifetimes: hub 1 h, DPS 10 min) and per file-upload request; no proactive renewal timer found. **DPS and mqttv3 hub only** |
| Credential fallback on rejection | **Yes** | No | C order: X.509 certificate indexes → primary key → secondary key → application token, without a backoff delay. .NET switches only from SAS to an issued certificate |
| Multiple certificates per role | **Yes** | No | C `load(index)`, up to `AZ_IOT_MAX_CERTS_PER_ROLE` (4); each index is its own auth source |
| TPM attestation | **Partial** | No | C: **DPS only, over HTTPS**, through a separate app-driven client (`az_iot_dps_tpm_client.h`, #437) — the library builds the requests; the app supplies the HTTPS transport and TPM operations. See §5 |
| Entra ID / token credential | No | No | |
| Certificate-provider abstraction | Yes | Partial | C `az_iot_certificate_provider.h` vtable (load/release + csr/sign, v2); providers wipe X.509 key material on `release()` (#433); the managed provider validates stored chains (#410) and has opt-in key rotation and EC P-384 (#415, #434). .NET has `X509AuthenticationProvider` only |
| CSR at DPS enrollment | Yes | Yes | Both send it |
| CSR renewal against the hub | Partial | Yes | C: **mqttv3 only**. .NET `SendCertificateSigningRequestAsync` |
| Issued-cert callback / persistence | Yes | Yes | C `store_issued_certificate`; .NET swaps the auth provider and reconnects |
| Credential rotation without app restart | Yes | Yes | C via CSR reissue **and** SAS renewal; .NET swaps the provider, `LocalCertificateSelectionCallback` can pick the new cert per handshake |
| Non-extractable keys (PKCS#11 / HSM / TPM URI) | Yes | Yes | C `client_key_uri` + `crypto_engine_id`, Linux e2e leg; .NET `LocalCertificateSelectionCallback` + HSM-backed `X509Certificate2`, SoftHSM-tested |
| Custom signing callback | Partial | No | C vtable `sign` hook with a `samples/authentication/hsm_sign_callback` sample, but **Paho refuses a sign()-only credential** — needs a BYO/mbedTLS/az_mqtt adapter |
| Trust bundle / custom CA | Partial | Yes | C single CA path/PEM, no rotation API; .NET `RemoteCertificateValidationCallback` supports pinning and private roots |
| Secret zeroization | **Partial** | No | C `az_iot_crypto__wipe()` across HMAC scratch, SAS tokens, key slots and the whole `sas_buffer` on teardown, `OPENSSL_cleanse` in key custody, and X.509 key material on provider release. The az_mqtt adapter wipes connect credentials before freeing them (#396, **C: main only**) |

## 4. Device features

| Feature | C mqttv3 | C mqttv5 | .NET mqttv3 | .NET mqttv5 | Notes |
|---|---|---|---|---|---|
| D2C telemetry | Yes | Yes | Yes | Yes | .NET mqttv5: preview only |
| Message properties | Yes | Yes | Yes | Yes | mqttv3 percent-encoded topic bag; mqttv5 v5 user properties. .NET mqttv5: preview only |
| Content type / encoding | Yes | Yes | Yes | Yes | .NET serializes `ContentType` and `ContentEncoding` into the mqttv3 topic; a `//TODO` there covers only filling them from user properties. .NET mqttv5: preview only |
| Message expiry | Partial | Partial | Partial | Partial | Present on the MQTT publish, not surfaced on the telemetry API (C mqttv5 by design). C mqttv5 carries the correlation id as native v5 Correlation Data and content type as native Content Type (#435). .NET mqttv5: preview only |
| Configurable QoS | Partial | Partial | No | No | .NET `//TODO do we want configurable QoS here?` |
| C2D receive | Yes | **N/A** | **Yes** | N/A | Service supports C2D on mqttv3 only. C mqttv5 C2D client was **removed** (#272). .NET re-added mqttv3 C2D (#483), covered by `TestCloudToDeviceMessages` |
| C2D settlement (accept/reject/abandon) | No | N/A | N/A | N/A | C: "design C2D strict-settlement state machine" still open (mqttv3 only) |
| Direct methods | Yes | Yes | Yes | Yes | mqttv5 adds the MQTTv5 **probe / exec / abandon** protobuf handshake. C mqttv5 holds replies until CONNECTED instead of dropping them (#516, **C: main only**); result body limit `AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX`, 512 B by default, overridable at compile time (SDK and application must use the same value). .NET mqttv5: preview only |
| Slow / async method responses | Yes | Yes | Partial | Partial | C has a `direct_method_slow_responder` sample under both `samples/unified/` and `samples/mqttv5/` (#260). .NET mqttv5: preview only |
| Twin get | Yes | Yes | Yes | Yes | .NET mqttv5 supports selective/ETag (`getReported`, `ifNotMatch`). .NET mqttv5: preview only |
| Reported-properties patch | Yes | Yes | Yes | Yes | .NET mqttv5: preview only |
| Desired-properties patch events | Yes | Yes | Yes | Yes | C mqttv5 now delivers the real version, SNAPSHOT vs PATCH kind, and resyncs when behind (#240). .NET mqttv5: preview only |
| Twin push (MQTTv5 birth-driven) | N/A | **Yes** | N/A | Yes | Both consume it. C `on_twin_push()` in `src/mqttv5/twin_client.c`; opt-in via `push_desired`/`push_reported`, default false. .NET `TwinPushReceived`/`TwinPushOptions`. .NET mqttv5: preview only |
| MQTTv5 presence / birth handshake | N/A | Yes | N/A | Yes | `common/Protos/presence.proto`; C `presence_encode_birth()`. .NET mqttv5: preview only |
| Custom topics | N/A | **Yes** | N/A | **Partial** | mqttv5-only service feature, **publish-only in public preview** (one topic group, 10 templates, `{deviceId}` the only variable). C `az_iot_custom_topic_client.h`: publish only, user properties, 256-byte (UTF-8) / 15-level topic cap, `$`-prefixed topics allowed, e2e suite. .NET `CustomTopicsClient` re-subscribes on reconnect and substitutes `{deviceId}` per send (#511), but still exposes Subscribe/Unsubscribe. **.NET: preview only** |
| File upload (SAS URI + notify) | Yes | **N/A** | **Yes** | N/A | Not offered on mqttv5. C mqttv3 (app supplies the HTTP hook), also for SAS-key devices (#436, **C: main only**). .NET re-added mqttv3 file upload (#486), covered by `FileUploadIntegrationTests` |
| Device update (software updates) | Partial | Partial | **No** | No | C only: `az_iot_su.h`, su-over-DPS. Per-step result details (#496), an update check waits for the owed terminal report (#531), the applied update is adopted as installed (#532) — **C: main only**. **.NET has no software-update code at all** |
| Connection state / error propagation | **Yes** | **Yes** | Yes | Yes | C: observer registry, scoped (DPS or HUB) state, `is_retriable` + `{source,code,message}`; mqttv5 PUBACK reason codes and Reason Strings reach the app (#400, #513); `az_iot_connection_client_get_device_id()` (#432); assignment-adopted notification (#431). .NET: re-provisioning notification (#521); `DisconnectAsync` halts reconnection and cancels pending requests (#509, **.NET: main only**). .NET mqttv5: preview only |
| Recover from FAULTED via close() | **Yes** | **Yes** | N/A | N/A | C: close() is a legal exit from FAULTED (#213) — was a permanent deadlock |
| Retry: exponential backoff + jitter | Yes | Yes | Yes | Yes | C 1 s→**60 s** default cap, ∞, ±20 %, jitter not clamped to the cap, per-scope ladders, selectable additive jitter and restart-safe `az_iot_retry_state` (#472); .NET `ExponentialBackoffRetryPolicy` (cap 60 s, jitter 95–105 %). .NET mqttv5: preview only |
| Retry-after honoured | Partial | Partial | Partial | Partial | Both honour DPS polling retry-after; C also honours it on the software-update topic. CSR `RetryAfterSeconds` is surfaced, not auto-applied. .NET mqttv5: preview only |
| Identity-rejection recovery | Yes | Yes | Yes | Yes | C selectable: `REPROVISION`, `RETRY_HUB` (retry the cached hub) or `NONE`, bounded by `max_hub_connect_attempts_before_reprovision`; a saved hub with DPS fallback is documented and tested (#429). .NET mqttv5: preview only |
| Offline queueing / persistence | No | No | **Partial** | **Partial** | No durable offline store in either library. C: publishing while disconnected fails (`AZ_IOT_ERR_NOT_CONNECTED`). .NET: feature operations submitted during automatic reconnection or re-provisioning wait for recovery instead of failing; they fail fast before the first connect or after a deliberate disconnect. .NET mqttv3 also re-sends QoS 1 publishes/subscribes/unsubscribes interrupted in flight (#524, **.NET: main only**). .NET mqttv5: preview only |
| Modules / IoT Edge | No | No | No | No | No ModuleClient, no edgeHub/workload HSM, no gateway support anywhere in the repo |
| Service-side client | No | No | No | No | No registry/jobs/query/digital-twin/C2D-send. .NET tests consume the **old** v1 packages for the service side |

## 5. DPS / provisioning

| Feature | C | .NET | Notes |
|---|---|---|---|
| Separate provisioning client | Partial | No | By design: a phase inside the connection client (`c/docs/dps-integration.md`). Exception: C TPM attestation (`az_iot_dps_tpm_client`) runs outside it |
| Register + poll + assignment | Yes | Yes | .NET now pins api-version **2026-11-02-preview** for all DPS device sessions (#262); both poll with retry-after + jitter |
| X.509 attestation | Yes | Yes | Both also support symmetric key (see the row below) |
| Symmetric key (SAS) attestation | **Yes** | **Yes** | C derives the device key from an enrollment-group key, or takes a device key directly. .NET `SymmetricKeyAuthenticationProvider` takes the device key; group-key derivation is left to the app |
| TPM attestation | **Yes** | No | C over **HTTPS** (`az_iot_dps_tpm_client.h`, #437), **not** part of `az_iot_connection_client` (DPS takes TPM over HTTPS/AMQP only). The client builds requests and parses responses; the app sends them, runs the TPM ops, hands the assignment to the connection client and handles re-registration. mqttv5 assignments need the app's own X.509 credential (the hub refuses SAS). e2e `az_iot_tests_e2e_tpm` in `ci-c-e2e` |
| CSR-based provisioning | Yes | Yes | .NET now serializes the CSR and requests a CSR-capable api-version (#202) |
| Custom registration payload | **Yes** | **Yes** | C `dps.registration_payload`, composes with the CSR body (#206); .NET `ProvisioningPayload` |
| DPS-returned payload surfaced to the app | **Yes** | **Yes** | C `set_registration_payload_callback()` (#206); .NET `DeviceRegistrationResult.Payload` |
| Provision-only (device with no IoT Hub) | **Yes** | **No** | C `dps.provision_only`: settles at DPS:CONNECTED + HUB:IDLE (#232) |
| Global endpoint override | Yes | Yes | |
| `connectionProfile` selects the MQTT version | Yes | Yes | DPS now returns `"mqttV5"` for mqttv5-hub devices, asserted by the C mqttv5 e2e. Unknown value → `ERR_CONNECTION_PROFILE_UNSUPPORTED` |
| Transports | MQTT 3.1.1 over TCP / WS / proxy; HTTPS for TPM | MQTT 3.1.1 over TCP 8883 or wss 443 | No AMQP provisioning; HTTPS only for C TPM attestation |
| Hold feature clients until registered | Yes | N/A | C `AZ_IOT_DPS_PHASE_HOLD`, `dps_hold_timeout_ms` 60 s |

## 6. Plug and Play

| Feature | C | .NET | Notes |
|---|---|---|---|
| Model ID on connect | Partial | No | C sets `opts.model_id` **only on the mqttv3 username** (`hub_build_username()`); the mqttv5 username carries only `correlationId` and `clientVersion`, so it is never announced there. .NET does not set it |
| Model ID via DPS payload | No | Partial | .NET mentions a `ModelIdPayload` type in a doc comment; no such type is in the repo |
| Components | Partial | No | C exposes only the `$.sub` component-name property; no component APIs |
| Digital twin / PnP conventions | No | No | |

## 7. Observability

| Feature | C | .NET | Notes |
|---|---|---|---|
| Log sink + levels | Yes | Partial | C `az_iot_log.h` (TRACE→OFF, pluggable sink) plus a **file sink** and a defined line format; **.NET only writes to `System.Diagnostics.Trace`** (logging docs and sample added, #487) |
| MQTT-level logging | Yes | Yes | .NET `EnableMqttLogging` → `MqttNetTraceLogger` |
| Per-component log routing | **Partial** | No | C tags every record with one of 17 CI-checked components (`az_iot_log_components.h`); sinks route on it, but the level filter is still a single `min_level` |
| ILogger / EventSource | N/A | No | |
| Activity / OpenTelemetry / metrics | No | No | Neither library emits traces or metrics |

## 8. Platform, build & packaging

| Feature | C | .NET | Notes |
|---|---|---|---|
| Language / TFM | C99-strict | **net10.0 only** | No netstandard/net472 multi-targeting |
| Language-standard conformance | **Yes** | N/A | C builds `-pedantic -Werror` under **c99, c11, c17 and c23** in CI (#282); Paho pinned to C99 (its v1.3.13 `typedef unsigned int bool` breaks C23) |
| Dependencies | azure-sdk-for-c 1.5.0, Paho | MQTTnet 5.1.0.1559, Google.Protobuf 3.36.2, Google.Protobuf.Tools 3.36.2 + Grpc.Tools 2.80.0 (build-only) | C fetches azure-sdk-for-c and Paho by default, or uses installed packages (`AZ_IOT_USE_INSTALLED_AZURE_SDK_FOR_C`, `AZ_IOT_USE_INSTALLED_PAHO`); `az::core`/`az::iot::hub`/`az::iot::provisioning`; MQTTv5 protocol logic lives in this repo |
| Dependency acquisition | vcpkg manifest (primary, with `builtin-baseline`) or CPM.cmake | NuGet | `azure-sdk-for-c` is a git submodule **only** under the ESP32 sample |
| Build options | 25 CMake options (`cmake/az_iot_options.cmake`) | — | PAHO, AZ_MQTT, RUST_MQTT, KEY_CUSTODY, crypto backend (OpenSSL/mbedTLS), managed cert provider, installed deps, hardening, MSVC `/analyze`, race annotations, tests, e2e (incl. software update, CSR, mqttv5, PKCS#11), conformance, coverage |
| Platforms built | Linux + Windows + Yocto + ESP-IDF | Linux + Windows | C adds a Yocto/bitbake consumer leg (qemux86-64, scarthgap) and ESP-IDF builds of the software-update sample (esp32, esp32c3, esp32s3, esp32c6). **No macOS in either CI** |
| AOT / trimming | N/A | No | No `IsAotCompatible`/`IsTrimmable` |
| Packaging | **Partial** | Yes | C installs as a CMake package with export targets and pkg-config (`AZ_IOT_INSTALL`); **no vcpkg port published**. .NET is on nuget.org |

## 9. Testing & CI

| Feature | C | .NET | Notes |
|---|---|---|---|
| Unit tests | Yes (57 cmocka files on `main`, 52 on preview) | Yes (xunit.v3 + Moq; 151 facts + 32 theories on `main`, 177 + 33 on preview) | C runs the suite under each of four language standards. .NET unit tests also run without Azure resources (`ci-dotnet-build.yml`, #305, **.NET: main only**) |
| Known-answer crypto vectors | Yes | N/A | C software-update adapters: FIPS 180-4 SHA-256, RS256 good/bad, root→SJWK→manifest chain |
| MQTT-interface conformance suite | Yes | No | C `tests/conformance/` for BYO adapters; Paho v3 + v5 **and** az_mqtt v3 + v5, against a Mosquitto service container |
| Integration tests | Yes | Yes | |
| E2E against live Azure | Yes | Yes | Resources provisioned per run via OIDC, torn down after; Windows + Linux legs. Every C suite runs over both Paho and az_mqtt |
| SAS e2e | **Yes** | **Yes** | C `e2e_sas_test.c`, `e2e_csr_sas_test.c`: DPS and hub over SAS, and SAS onboarding with a DPS-issued certificate. .NET `CanProvisionAndConnectWithSymmetricKeyIndividualEnrollment` and `TestFileUpload_SymmetricKey` (individual enrollment, run by `ci-dotnet`); the CSR-over-SAS path is skipped when the enrollment-group key is not configured |
| **MQTTv5 e2e actually executed** | **Yes (real mqttv5 hub)** | **No** | C `ci-c-e2e-mqttv5.yml`: provisions through real DPS and runs connection/DPS, telemetry, twin, direct method, custom topics, transport (WebSockets, proxy) and PKCS#11 custody suites. Runs on same-repo PRs, a daily schedule and on demand — **no push trigger**. .NET mqttv5 tests still skip: 3 skip sites on `main` ("No MQTTv5 hub to test against yet"), 8 on preview (adds "No test infrastructure setup…" and twin "Not implemented yet") |
| Dedicated software-update / CSR / PKCS#11 / TPM e2e | Partial | **Yes** | C: `ci-c-e2e-csr.yml` (Linux only), PKCS#11 on the Linux leg, TPM in `ci-c-e2e`. **`ci-c-e2e-adu.yml` is manual-trigger only** (no PR, push or schedule trigger). .NET: `ci-dotnet-e2e-csr.yml` |
| Software-update e2e vs the real service | **Partial** | N/A | `az_iot_tests_e2e_su_offer`: real offered update, libcurl download, engine hash check — exists, but only runs when triggered manually |
| Fault injection | Partial | Yes | .NET covers faults by **unit test** by design — `ConnectionFaultedUnitTests.cs` |
| Sanitizers | Yes | No | valgrind (Linux) + MSVC ASan; helgrind/DRD on race-labelled tests; **ThreadSanitizer over all tests (#526) and helgrind/DRD over real-stack tests (#523) — C: main only** |
| Style / layering gates | Yes | No | `check-banned-constructs.sh`, `check-layering.sh`, `check-log-components.sh`, `check-hardening.sh`, clang-format |
| **Static analysis** | **Yes** | **Yes** | C: clang-tidy (`c/.clang-tidy`), MSVC `/analyze`, CodeQL `c-cpp`, plus an Azure Pipelines 1ES build (`c/eng/pipelines/official.yml`: CodeQL, PoliCheck, secret scanning, BinSkim, antimalware, SBOM). .NET: `codeql-dotnet.yml` (`languages: csharp`) plus vulnerable-package scanning. Repo-wide: CodeQL for `actions`, dependency review, workflow lint. All on both branches |
| **Build hardening** | **Yes** | Partial | C: stack protector, FORTIFY, PIE/RELRO on GCC/Clang; `/guard:cf`, `/CETCOMPAT`, `/sdl` on MSVC, enforced by `check-hardening.sh`. .NET: strong-name signing |
| Coverage | Yes | Yes | C: gcovr with per-component floors, patch coverage, and a zero-gcda failure. **No overall percentage is published any more.** .NET: XPlat + CodeCoverageSummary |
| Fuzzing | No | No | |

## 10. Cross-cutting client concerns

Areas that decide whether a device client is adoptable, distinct from protocol features.

| Concern | C | .NET | Notes |
|---|---|---|---|
| Installable / linkable package | **Yes** | Yes | C: `install()`/`export()` targets, CMake package config and pkg-config behind `AZ_IOT_INSTALL`; builds correctly as a CMake subproject. **No vcpkg port published** |
| Generated API reference | **No** | **No** | Public C headers are Doxygen-formatted but there is still **no Doxyfile and no doc build**, so no API reference is produced |
| Static analysis | **Yes** | **Yes** | See §9. Now on both branches |
| Supply chain / SBOM | **Partial** | **Partial** | C `cgmanifest.json` pins every third-party component, SBOM generated by the official 1ES pipeline, installed deps ship LICENSE/NOTICE. Repo-wide dependency review; .NET vulnerable-package scan. Not confirmed whether the SBOM is published as an artifact |
| Secret hygiene in memory | **Partial** | **No** | C `az_iot_crypto__wipe()` covers HMAC scratch, SAS tokens, key slots and `sas_buffer` on teardown; X.509 key material wiped on provider release; `OPENSSL_cleanse` in key custody. .NET relies on the GC |
| Log redaction guarantees | Partial | Partial | C redacts PKCS#11 URIs and truncates Paho traces; neither library **documents** what must never reach a sink |
| Measured footprint (ROM/RAM) | **Partial** | N/A | Published for the az_mqtt adapter only: per-module `.text`/`.rodata`/`.data`, per-config deltas, RAM per client. No figures for the Paho build |
| Portable time source | Partial | N/A | `az_iot_time_mono_ms()` is POSIX `clock_gettime` or Win32 `GetTickCount64` behind `#if defined(_WIN32)`. **Still no platform hook**, so an RTOS port must patch `src/core/mono_time.c` |
| Thread-safety contract | **Yes** | Partial | C: single-threaded `do_work()` pump with callbacks on the caller's thread, documented in `README.md` and the architecture doc with a `multithreaded_telemetry` sample (#503), and checked by ThreadSanitizer over all tests plus helgrind/DRD over real-stack tests — **C: main only**; preview states the contract without the sample, TSan or real-stack race jobs. Paho's own races are documented as known issues (#525) |
| Reboot persistence / session resumption | Partial | **No** | Only the software-update client persists (`persist_state_fn`/`load_state_fn`, CRC-32 guarded). MQTT session, twin version and in-flight operations are cold-started |
| Backpressure / in-flight bounds | **Yes** | Partial | C has one connection-wide `AZ_IOT_MAX_PENDING_PUBACKS` table; selected feature clients may reserve part of it and every other caller shares the remainder. A slot is taken before the publish, so a full table refuses with `AZ_IOT_ERR_BUSY` instead of failing an already-sent publish |
| Credential expiry handling | **Yes** | Partial | C renews a SAS token at `renewal_percent` before expiry and falls back across credentials on rejection; neither library warns ahead of client-certificate expiry |
| Clock-skew tolerance | Partial | Partial | Monotonic time drives backoff; SAS needs a real clock, and the samples check for one. No guidance on wall-clock skew against certificate validity |
| API deprecation policy | N/A | N/A | Both are pre-release; no policy is written |

## 11. Concrete open gaps (from code markers)

**C**
- **mqttv5 hub does not accept SAS.** SAS covers DPS and the mqttv3 hub only (`c/docs/connecting.md`).
- **Hub-side CSR renewal is mqttv3-only.**
- **MQTT v5 fields accepted but never serialized by the Paho adapter**: `az_iot_mqtt_connect_options.user_properties` on CONNECT, and `response_topic` / `topic_alias` on PUBLISH. Silent loss, no error. The az_mqtt adapter serializes all three.
- **PnP model id is announced on the mqttv3 username only** — never on the mqttv5 path, and not placed in the DPS payload.
- **mqttv5 direct-method result body limit is compile-time only** (`AZ_IOT_MQTTV5_DM_RESULT_BODY_MAX`, default 512 B); SDK and application must be built with the same value.
- **mqttv5 telemetry does not expose message expiry.**
- **Software updates:** no root-key rotation API (root keys are a list supplied at init); the e2e runs only when triggered manually.
- **`max_attempts` is per-ladder and misnamed** (`az_iot_retry_policy.h`); renaming is cheap only before release.
- **No generated API reference** (no Doxyfile), and **no vcpkg port**.
- **No platform hook for monotonic time** — an RTOS port patches `src/core/mono_time.c`.
- **C2D strict settlement** (accept/reject/abandon) is still not designed; C2D is mqttv3-only.
- Naming: one file still references the retired `gen1`/`gen2`/`Classic` terms — `c/eng/check-layering.sh`, where they are the guard pattern itself.

**.NET**
- **The mqttv5 API set is on `releases/public-preview`, not `main`**, and the `main`-only work since 2026-10-22 (session rework, feature opt-out, `DisconnectAsync`, QoS 1 re-send) is not on the preview branch.
- mqttv5 integration tests are still skipped on both branches (§9).
- `CustomTopicsClient` exposes Subscribe/Unsubscribe although the service is publish-only in public preview.
- mqttv5 `//TODO subscribe elide logic` (`MQTTv5/Connection/ConnectionClient.cs`); session expiry hard-coded to 0 (`MqttConnectionManager.cs`).
- `Unified` twin error mapping always returns `Result.Ok` (`Unified/Twin/TwinClient.cs`: `main` :268, preview :288).
- Content type/encoding carries a `//TODO` on the unified telemetry path (`Unified/Telemetry/TelemetryClient.cs`: `main` :95, preview :83).
- Telemetry QoS is not configurable (`Models/Telemetry/DeviceToCloudTelemetry.cs:13`).
- CSR QoS/ack correctness TODOs remain (`Unified/Connection/ConnectionClient.cs`: `main` :209, preview :208).
- No durable offline queue: operations wait out automatic reconnection or re-provisioning, but fail before the first connect or after a deliberate disconnect; in-flight QoS 1 re-send is `main` only.
- No credential fallback across keys/certificates, and no proactive SAS renewal before expiry.
- Proxy support is websocket-only on the MQTTnet adapter.
- No TPM attestation; no software-update/ADU code at all.

**Closed since the last revision**
- C: the three non-production env-var overrides (`AZ_IOT_HUB_MQTT_V5_MOCK_ENDPOINT`, `AZ_IOT_DEVICE_ID`, `AZ_IOT_DPS_CONNECTION_PROFILE_OVERRIDE`) are gone from library code (#414); DPS returns `mqttV5` itself.
- C: custom topics implemented (publish), with e2e.
- C: software-update operational check no longer fails with `AZ_IOT_ERR_NOT_CONNECTED` — a refusal records the demand and opens a provisioning session; `AZ_IOT_SU_EVENT_NO_UPDATE` reports "no update available".
- .NET: `CustomTopicsClient` no longer loses subscriptions on reconnect and follows a device ID that changes on re-provisioning (#511).

## 12. Net-new vs the old public SDKs

Present here, no analogue in `azure-iot-sdk-c` / `azure-iot-sdk-csharp`:
- mqttv5/MQTTv5 clients over MQTT v5 with a presence/birth handshake.
- Protobuf wire contracts shared across languages (`common/Protos/`), incl. the direct-method probe/exec/abandon flow.
- Certificate-provider abstraction with CSR issuance and hub-side renewal/rotation.
- Non-extractable key custody (PKCS#11 / engine URI, sign hook) — C only.
- BYO MQTT client as a first-class, conformance-tested contract.
- software updates (su) carried over DPS — C only.
- DPS-returned `connectionProfile` choosing the MQTT version at runtime.
- Custom topics (mqttv5, publish-only in public preview).

Old-SDK staples deliberately **absent**: AMQP and multiplexing, HTTPS transport (beyond file upload and C TPM provisioning), TPM auth in .NET, connection strings, modules/IoT Edge, service SDK (registry, jobs, query, digital twin, C2D send, feedback/file-upload notification receivers), PnP conventions and digital twin, device streams. SAS/symmetric-key auth, mqttv3 C2D and mqttv3 file upload are present in both libraries.

## 13. Caveats on this report

- Read from local clones of **both** branches at `main` @ `3a8be1c` (2026-10-26) and `releases/public-preview` @ `1d5c0ee` (2026-10-22); nothing was built or executed here, so "Yes" means the code path exists and is wired, not that it was run. CI status is not tracked by this document.
- **A cell says whether the capability exists at all; the Note says which branch when it is not on both.** No branch note means both branches carry it. The preview branch took all of `main` up to 2026-10-22 (#520); `main` is 22 commits ahead, the preview branch 20.
- The split in one line: the **.NET mqttv5 client set is preview-only**, and **work merged after 2026-10-22 is main-only** (see Branch notes).
- **Cut is not the same as missing.** C2D and file upload are absent on mqttv5 because the service does not offer them there; both libraries deleted their mqttv5 implementations rather than ship ahead of the service.
- Both libraries are **pre-release** — C `1.0.0-preview` on both branches; .NET `1.1.0` on `main` and `2.0.0-preview3` on the preview branch.
- .NET publishing to nuget.org is stated by `dotnet/README.md`; the publishing pipeline is not in this repo.
- **No overall coverage percentage is published**; `code-coverage.md` defines per-component floors and patch coverage instead.
- Terminology follows the `mqttv3` / `mqttv5` convention.

## 14. What changed since the 2026-10-08 revision

Rechecked across both branches after about 120 commits on `main` and the 2026-10-20 / 2026-10-22 merges into the preview branch. The largest movements:

- **C runs e2e against a real mqttv5 hub through DPS**, and DPS returns `mqttV5` itself; the mock-hub and profile-override env vars are gone from library code.
- **C custom topics** (publish-only) landed with e2e.
- **C TPM attestation for DPS, over HTTPS**, with e2e.
- **.NET SAS** for DPS and the mqttv3 hub; **.NET C2D and file upload are back** for mqttv3 with integration tests; .NET is on nuget.org.
- **az_mqtt is on both branches**, and every C e2e suite runs over both Paho and az_mqtt.
- Branch scope shrank: SAS application tokens/renewal, credential fallback, multiple certificates per role, `SETTING_UP`, SAS e2e, the 1ES pipeline and the .NET analysis workflows are now on both branches.
- **C thread-safety contract** documented and checked by TSan and real-stack helgrind/DRD (main only).
- **.NET mqttv3 connection handling reworked** on `main`: configurable clean session, feature opt-out, `DisconnectAsync` halts reconnection, QoS 1 operations re-sent after interruption.
- C: mqttv5 direct-method replies held until connected (main only), PUBACK reason codes reach the app, additive jitter and restart-safe retry state, installed-dependency builds, ESP-IDF builds, more software-update reporting.
