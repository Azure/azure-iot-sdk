# Azure/azure-iot-sdk — client feature matrix

Repo: `Azure/azure-iot-sdk` (**private** mono-repo), `main` @ `a453698`, 2026-10-08. Verified by reading the code, not docs or the public SDKs.

**This is not the old public SDK.** It ships two client libraries only — **C** (`/c`, C99, `AZ_IOT_VERSION_STRING "1.0.0-preview"`) and **.NET** (`/dotnet`, `net10.0`, `Microsoft.Azure.Iot.Device` **1.1.0**, published to **GitHub Packages**, not nuget.org). No Java/Node/Python/embedded columns exist.

### Reading the columns

The columns are **MQTT protocol versions**, because that is what determines wire capability. Each version targets a different IoT Hub:

| | Hub | MQTT | Encoding | C symbols | .NET types |
|---|---|---|---|---|---|
| **mqttv3** | the current Azure IoT Hub | v3.1.1 only | `$iothub/...` topics, percent-encoded property bag | `az_iot_mqttv3_*`, `inc/azure/iot/mqttv3/` | the mqttv3 path inside `Unified/*` |
| **mqttv5** | the new Azure IoT Hub | v5 only | `ih/{deviceId}/...` topics, protobuf + v5 user properties | `az_iot_mqttv5_*`, `inc/azure/iot/mqttv5/` | **not on `main`** — see below |

**The .NET mqttv5 column is `N/A` throughout.** The mqttv5 API set was split out of `main` to the `releases/public-preview` branch, so `dotnet/src` ships only the mqttv3 path inside `Unified/*`; there is no `MQTTv5/` directory on `main`, and `DoesClientSupportHubType` accepts Classic only. Rows describing .NET mqttv5 behaviour therefore describe a branch this matrix does not track. The protobuf contracts are still compiled into the package.

**`Unified` is not the .NET equivalent of C mqttv3** — it is a facade that *was* designed to span both versions. On `main` only its mqttv3 half is reachable.

- **C** exposes two sibling API families and makes the **application** branch: read `az_iot_connection_client_get_hub_profile()`, then instantiate either `az_iot_mqttv3_telemetry_client` or `az_iot_mqttv5_telemetry_client`. Choosing the wrong one is refused with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH`.
- **.NET** branched internally on `ConnectionProfile` at runtime. With the mqttv5 half on the preview branch, that branch resolves to mqttv3 on `main`.

- **DPS always speaks MQTT v3.1.1**, whichever hub follows, and is **not a separate client** — it runs inside the connection client, which then picks the MQTT version from the DPS-returned `connectionProfile`.
- C keeps **one** connection client and splits only the feature clients (`c/docs/eng/client-separation.md`).
- Shared protobuf contracts live in `common/Protos/{presence,directmethods,twin}.proto`; C hand-rolls the codec (`src/mqttv5/direct_method_codec.c`), .NET compiles them with Grpc.Tools.

Legend: **Yes** supported · **Partial** partial/caveated · **No** absent · **N/A** not applicable.

---

## 1. Architecture & lifecycle

| Feature | C mqttv3 | C mqttv5 | .NET mqttv3 | .NET mqttv5 | Notes |
|---|---|---|---|---|---|
| Maturity | Partial | Partial | Partial | N/A | C `1.0.0-preview` (header is the single source of truth, gated by `eng/check-version.sh`); .NET 1.1.0 to GitHub Packages |
| Single connection client, DPS internal | Yes | Yes | Yes | N/A | `az_iot_connection_client` / `AbstractConnectionClient`; provisioning is not an app step |
| Per-feature clients over one connection | Yes | Yes | Yes | N/A | telemetry / c2d / direct method / twin (+ file upload mqttv3 only) |
| Generation selected at | client init (app picks the API) | ← | N/A — mqttv3 only | N/A | C refuses a mismatch with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH`. On `main` .NET has one path, so there is nothing to select |
| Runtime generation query | Yes | Yes | Yes | N/A | `az_iot_connection_client_get_hub_profile()` / `ConnectionProfile` |
| MQTTv3↔MQTTv5 fallback handled by | application | ← | N/A | N/A | C app must instantiate the other API family. No fallback exists on `main` for .NET — the mqttv5 half is on the preview branch |
| API model | single-threaded `do_work()` pump, callbacks on caller thread, no internal threads | ← | Task-based async, `CancellationToken` on every op, `IDisposable` | N/A | .NET has **no** `IAsyncDisposable` |
| Nullable / strictness | C99-strict, `-pedantic`, banned-construct + layering CI gates | ← | `<Nullable>enable</Nullable>` | N/A | |
| Struct/ABI versioning | Partial | Partial | N/A | N/A | `_internal_size` on events + cert-provider vtable v2; `struct_versioning.md` still a proposal |
| Preview/experimental markers | Partial | Partial | No | N/A | No per-API attributes; C README carries the status banner |

## 2. Transport

| Feature | C mqttv3 | C mqttv5 | .NET mqttv3 | .NET mqttv5 | Notes |
|---|---|---|---|---|---|
| MQTT 3.1.1 | Yes | N/A | Yes | N/A | Required for MQTTv3 **and** for DPS in both generations |
| MQTT 5.0 | N/A | Yes | N/A | N/A | v5 user properties, correlation data, content type, message expiry, reason codes |
| v5 user properties end-to-end | N/A | Yes | N/A | N/A | C now sets outbound and extracts inbound, with conformance round-trip cases (#205) |
| MQTT over WebSockets | Yes | Yes | Yes | N/A | C `samples/websockets`; .NET mqttv5 websocket URI added (#269) |
| HTTP CONNECT proxy | Yes | Yes | Partial | N/A | C `az_iot_mqtt_proxy_options` + `samples/proxy`; .NET websocket-only and not forwarded to MQTTv5 |
| AMQP | No | No | No | N/A | Deliberate — see `c/docs/amqp_vs_mqtt_for_new_sdk_client.md`. Test-only AMQP under `c/tests/deps/amqp` |
| HTTPS | Partial | No | Partial | N/A | Only the file-upload SAS PUT/notify; C ships **no** HTTP client (app supplies a transport hook) |
| BYO MQTT client | Yes | Yes | Yes | N/A | C `az_iot_mqtt_iface` vtable + published conformance suite; .NET `IMqttClient` + `CustomMqttClientSample` |
| Shipped adapters | Paho v3.1.1 + v5 (default) | ← | MQTTnet 5.1.0.1559 (`MqttNetAdapter`) | N/A | C adds **az_mqtt** (vendored, zero-allocation, v3+v5, `AZ_IOT_WITH_AZ_MQTT=OFF` by default, conformance-tested) and a Rust adapter that delegates to a runtime-installed FFI table, refusing only WebSockets and proxy connects |
| Allocation profile | Partial | Partial | N/A | N/A | Core is static/caller-allocated; PEM cert provider and the Paho adapter malloc |
| TLS version control | No | No | Yes | N/A | C exposes only `use_tls`; .NET forces Tls12/Tls13. Neither can disable validation |
| Reconnect / connection maintenance | Yes | Yes | Yes | N/A | C `src/core/retry_policy.c` + public `az_iot_retry_policy.h`; .NET `MqttConnectionManager.MaintainConnectionAsync` |
| Keep-alive | Yes | Yes | Yes | N/A | C default 30 s; .NET default 60 s |
| Clean start / session expiry / LWT | Yes | Yes | Yes | N/A | C `resolve_session_options()` sets per role (#204); MQTTv5 `//TODO subscribe elide logic` |
| Subscription-ack gating | Yes | Yes | **Yes** | N/A | Both check SUBACK reason codes. C failure scope is configurable (`az_iot_subscription_failure_scope`); .NET always disconnects and reconnects |
| MQTT DISCONNECT reason code | N/A | **Yes** | **N/A** | N/A | v5 only — v3.1.1 has no reason-code field. C sets 0x04 on the mqttv5 hub. `MqttDisconnect.Reason` exists as a model property but is unreachable on .NET `main`, which is mqttv3-only |

## 3. Authentication

| Feature | C | .NET | Notes |
|---|---|---|---|
| X.509 client certificates | Yes | Yes | First auth source tried in C (`az_iot_auth_source`); the only one in .NET |
| SAS token from symmetric key | **Partial** | **No** | C: Yes for DPS and the mqttv3 hub (base64 primary/secondary key, enrollment-group derivation); **the mqttv5 hub does not accept SAS yet**. .NET: `Password` is always an empty array |
| Application-supplied SAS tokens | **Partial** | No | C `on_sas_token_required` + `az_iot_connection_client_update_sas_token()`; needs `sas_buffer`, no crypto backend. **DPS and mqttv3 hub only** — the mqttv5 hub refuses SAS |
| SAS renewal before expiry | **Partial** | N/A | C renews at `renewal_percent` of lifetime (default 80) and reconnects; surfaced as `is_credential_renewal`. **DPS and mqttv3 hub only**, as above |
| Credential fallback on rejection | **Yes** | No | C order: X.509 certificate indexes → primary key → secondary key → application token, without a backoff delay |
| Multiple certificates per role | **Yes** | No | C `load(index)`, up to `AZ_IOT_MAX_CERTS_PER_ROLE` (4); each index is its own auth source |
| TPM attestation | No | No | Explicitly out of scope (`c/docs/eng/certificate-management.md`) |
| Entra ID / token credential | No | No | |
| Certificate-provider abstraction | Yes | Partial | C `az_iot_certificate_provider.h` vtable (load/release + csr/sign, v2); .NET has `X509AuthenticationProvider` only |
| CSR at DPS enrollment | Yes | Yes | Both send it |
| CSR renewal against the hub | Partial | Yes | C: **mqttv3 only**. .NET `SendCertificateSigningRequestAsync` |
| Issued-cert callback / persistence | Yes | Yes | C `store_issued_certificate`; .NET swaps the auth provider and reconnects |
| Credential rotation without app restart | Yes | Yes | C via CSR reissue **and** SAS renewal; .NET swaps the provider, `LocalCertificateSelectionCallback` can pick the new cert per handshake |
| Non-extractable keys (PKCS#11 / HSM / TPM URI) | Yes | Yes | C `client_key_uri` + `crypto_engine_id`, Linux e2e leg; .NET `LocalCertificateSelectionCallback` + HSM-backed `X509Certificate2`, SoftHSM-tested |
| Custom signing callback | Partial | No | C vtable `sign` hook with a `samples/authentication/hsm_sign_callback` sample, but **Paho refuses a sign()-only credential** — needs a BYO/mbedTLS/az_mqtt adapter |
| Trust bundle / custom CA | Partial | Yes | C single CA path/PEM, no rotation API; .NET `RemoteCertificateValidationCallback` supports pinning and private roots |
| Secret zeroization | **Partial** | No | C `az_iot_crypto__wipe()` across HMAC scratch, SAS tokens, key slots and the whole `sas_buffer` on teardown, plus `OPENSSL_cleanse` in key custody |

## 4. Device features

| Feature | C mqttv3 | C mqttv5 | .NET mqttv3 | .NET mqttv5 | Notes |
|---|---|---|---|---|---|
| D2C telemetry | Yes | Yes | Yes | N/A | |
| Message properties | Yes | Yes | Yes | N/A | mqttv3 percent-encoded topic bag; mqttv5 v5 user properties |
| Content type / encoding | Yes | Yes | Yes | N/A | .NET serializes `ContentType` and `ContentEncoding` into the mqttv3 topic; a `//TODO` there covers only filling them from user properties |
| Message expiry | Partial | Partial | Partial | N/A | Present on the MQTT publish, not surfaced on the telemetry API |
| Configurable QoS | Partial | Partial | No | N/A | .NET `//TODO do we want configurable QoS here?` |
| C2D receive | Yes | **N/A** | **No** | N/A | Service supports C2D on mqttv3 only. C mqttv5 C2D client was **removed** (#272); .NET dropped C2D from the unified API (#255) |
| C2D settlement (accept/reject/abandon) | No | N/A | N/A | N/A | C: "design C2D strict-settlement state machine" still open (mqttv3 only) |
| Direct methods | Yes | Yes | Yes | N/A | mqttv5 adds the MQTTv5 **probe / exec / abandon** protobuf handshake |
| Slow / async method responses | Yes | Yes | Partial | N/A | C has a `direct_method_slow_responder` sample under both `samples/unified/` and `samples/mqttv5/` (#260) |
| Twin get | Yes | Yes | Yes | N/A | .NET mqttv5 supports selective/ETag (`getReported`, `ifNotMatch`) |
| Reported-properties patch | Yes | Yes | Yes | N/A | |
| Desired-properties patch events | Yes | Yes | Yes | N/A | C mqttv5 now delivers the real version, SNAPSHOT vs PATCH kind, and resyncs when behind (#240) |
| Twin push (MQTTv5 birth-driven) | N/A | **Yes** | N/A | N/A | Both consume it. C `on_twin_push()` in `src/mqttv5/twin_client.c`; opt-in via `push_desired`/`push_reported`, default false. .NET `TwinPushReceived`/`TwinPushOptions` |
| MQTTv5 presence / birth handshake | N/A | Yes | N/A | N/A | `common/Protos/presence.proto`; C `presence_encode_birth()` |
| Custom topics | N/A | **No** | N/A | N/A | mqttv5-only service feature, **publish-only in public preview** (one topic group, 10 templates, `{deviceId}` the only variable). Absent from `main` in both libraries; .NET has a `CustomTopicsClient` on the preview branch, C has a design but no code |
| File upload (SAS URI + notify) | Yes | **N/A** | **No** | N/A | Not offered on mqttv5. C mqttv3 has it (app supplies the HTTP hook); .NET **removed** file upload from the unified API (#255) |
| Device update (software updates) | Partial | Partial | **No** | N/A | C only: `az_iot_su.h`, su-over-DPS (renamed from ADU in #270). **.NET has no software-update code at all** |
| Connection state / error propagation | **Yes** | **Yes** | Yes | N/A | C now has an observer registry, scoped (DPS or HUB) state, `is_retriable` + `{source,code,message}` (#221/#224/#235) |
| Recover from FAULTED via close() | **Yes** | **Yes** | N/A | N/A | C: close() is a legal exit from FAULTED (#213) — was a permanent deadlock |
| Retry: exponential backoff + jitter | Yes | Yes | Yes | N/A | C 1 s→**60 s** default cap (#273), ∞, ±20 %, jitter not clamped to the cap (#219), per-scope ladders (#214); .NET `ExponentialBackoffRetryPolicy` (cap 60 s, jitter 95–105 %) |
| Retry-after honoured | Partial | Partial | Partial | N/A | Both honour DPS polling retry-after; C also honours it on the software-update topic. CSR `RetryAfterSeconds` is surfaced, not auto-applied |
| Identity-rejection recovery | Yes | Yes | Yes | N/A | C now selectable: `REPROVISION`, `RETRY_HUB` (retry the cached hub) or `NONE`, bounded by `max_hub_connect_attempts_before_reprovision` |
| Offline queueing / persistence | No | No | No | N/A | Publishing while disconnected fails (`AZ_IOT_ERR_NOT_CONNECTED` / `MqttClientNotConnectedException`) |
| Modules / IoT Edge | No | No | No | N/A | No ModuleClient, no edgeHub/workload HSM, no gateway support anywhere in the repo |
| Service-side client | No | No | No | N/A | No registry/jobs/query/digital-twin/C2D-send. .NET tests consume the **old** v1 packages for the service side |

## 5. DPS / provisioning

| Feature | C | .NET | Notes |
|---|---|---|---|
| Separate provisioning client | No | No | By design: a phase inside the connection client (`c/docs/dps-integration.md`) |
| Register + poll + assignment | Yes | Yes | .NET now pins api-version **2026-11-02-preview** for all DPS device sessions (#262); both poll with retry-after + jitter |
| X.509 attestation | Yes | Yes | The only attestation type in .NET; C also supports symmetric key (see the row below) |
| Symmetric key (SAS) attestation | **Yes** | No | C derives the device key from an enrollment-group key, or takes a device key directly |
| TPM attestation | No | No | |
| CSR-based provisioning | Yes | Yes | .NET now serializes the CSR and requests a CSR-capable api-version (#202) |
| Custom registration payload | **Yes** | **Yes** | C `dps.registration_payload`, composes with the CSR body (#206); .NET `ProvisioningPayload` |
| DPS-returned payload surfaced to the app | **Yes** | **Yes** | C `set_registration_payload_callback()` (#206); .NET `DeviceRegistrationResult.Payload` |
| Provision-only (device with no IoT Hub) | **Yes** | **No** | C `dps.provision_only`: settles at DPS:CONNECTED + HUB:IDLE (#232) |
| Global endpoint override | Yes | Yes | |
| `connectionProfile` selects the MQTT version | Yes | Yes | Unknown value → `ERR_CONNECTION_PROFILE_UNSUPPORTED` |
| Transports | MQTT 3.1.1 over TCP / WS / proxy | MQTT 3.1.1 over TCP 8883 or wss 443 | No HTTPS or AMQP provisioning |
| Hold feature clients until registered | Yes | N/A | C `AZ_IOT_DPS_PHASE_HOLD`, `dps_hold_timeout_ms` 60 s |

## 6. Plug and Play

| Feature | C | .NET | Notes |
|---|---|---|---|
| Model ID on connect | Partial | No | C sets `opts.model_id` **only on the mqttv3 username** — `connection_client.c:3080` excludes the mqttv5 role, so it is never announced there. .NET does not set it |
| Model ID via DPS payload | No | Partial | .NET references a `ModelIdPayload` type that is not in the repo |
| Components | Partial | No | C exposes only the `$.sub` component-name property; no component APIs |
| Digital twin / PnP conventions | No | No | |

## 7. Observability

| Feature | C | .NET | Notes |
|---|---|---|---|
| Log sink + levels | Yes | Partial | C `az_iot_log.h` (TRACE→OFF, pluggable sink) plus a **file sink** and a defined line format; **.NET only writes to `System.Diagnostics.Trace`** |
| MQTT-level logging | Yes | Yes | .NET `EnableMqttLogging` → `MqttNetTraceLogger` |
| Per-component log routing | **Partial** | No | C tags every record with one of 16 CI-checked components (`az_iot_log_components.h`); sinks route on it, but the level filter is still a single `min_level` |
| ILogger / EventSource | N/A | No | |
| Activity / OpenTelemetry / metrics | No | No | Neither library emits traces or metrics |

## 8. Platform, build & packaging

| Feature | C | .NET | Notes |
|---|---|---|---|
| Language / TFM | C99-strict | **net10.0 only** | No netstandard/net472 multi-targeting |
| Language-standard conformance | **Yes** | N/A | C builds `-pedantic -Werror` under **c99, c11, c17 and c23** in CI (#282); Paho pinned to C99 (its v1.3.13 `typedef unsigned int bool` breaks C23) |
| Dependencies | azure-sdk-for-c 1.5.0 (FetchContent, mandatory), Paho | MQTTnet 5.1.0.1559, Google.Protobuf 3.34.1, Google.Protobuf.Tools + Grpc.Tools 2.80.0 (build-only) | `az::core`/`az::iot::hub`/`az::iot::provisioning`; MQTTv5 protocol logic lives in this repo |
| Dependency acquisition | vcpkg manifest (primary) or CPM.cmake | NuGet | `azure-sdk-for-c` is a git submodule **only** under the ESP32 sample |
| Build options | 16 CMake options (`cmake/az_iot_options.cmake`) | — | PAHO, RUST_MQTT, KEY_CUSTODY, software-update crypto, cert provider, tests, e2e, conformance, coverage |
| Platforms built | Linux + Windows + Yocto | Linux + Windows | C adds a Yocto/bitbake consumer leg (qemux86-64, scarthgap). **No macOS in either CI** |
| AOT / trimming | N/A | No | No `IsAotCompatible`/`IsTrimmable` |
| Packaging | **Partial** | Yes | C installs as a CMake package with export targets and pkg-config (`AZ_IOT_INSTALL`); **no vcpkg port published**. .NET publishes nightly to GitHub Packages |

## 9. Testing & CI

| Feature | C | .NET | Notes |
|---|---|---|---|
| Unit tests | Yes (46 cmocka files) | Yes (xunit.v3 + Moq, 75 facts + 9 theories) | C runs the suite under each of four language standards |
| Known-answer crypto vectors | Yes | N/A | C software-update adapters: FIPS 180-4 SHA-256, RS256 good/bad, root→SJWK→manifest chain |
| MQTT-interface conformance suite | Yes | No | C `tests/conformance/` for BYO adapters; Paho v3 + v5 **and** az_mqtt v3 + v5, against a Mosquitto service container |
| Integration tests | Yes | Yes | |
| E2E against live Azure | Yes | Yes | Resources provisioned per run via OIDC, torn down after; Windows + Linux legs |
| SAS e2e | **Yes** | N/A | C `e2e_sas_test.c`, `e2e_csr_sas_test.c`: DPS and hub over SAS, and SAS onboarding with a DPS-issued certificate |
| **MQTTv5 e2e actually executed** | Yes (vs a mock mqttv5 hub) | **No** | .NET `Setup.cs` skips 2×, both "No MQTTv5 hub to test against yet" |
| Dedicated software-update / CSR / PKCS#11 e2e | Yes | **Yes** | C: `ci-c-e2e-adu.yml`, `ci-c-e2e-csr.yml` (Linux only), PKCS#11 on the Linux leg. .NET CSR tests re-enabled and split into `ci-dotnet-e2e-csr.yml` |
| Software-update e2e vs the real service | Yes | N/A | `az_iot_tests_e2e_su_offer`: real offered update, libcurl download, engine hash check |
| Fault injection | Partial | Yes | .NET covers faults by **unit test** by design — `ConnectionFaultedUnitTests.cs`, 28 facts/theories |
| Sanitizers | Yes | No | valgrind (Linux) + MSVC ASan, plus a race-detector job (helgrind/DRD) |
| Style / layering gates | Yes | No | `check-banned-constructs.sh`, `check-layering.sh`, `check-log-components.sh`, `check-hardening.sh`, clang-format |
| **Static analysis** | **Yes** | **No** | C: clang-tidy (`c/.clang-tidy`), MSVC `/analyze`, CodeQL `c-cpp`, plus an Azure Pipelines SDL build (CodeQL, BinSkim, antimalware, SBOM). Repo-wide: CodeQL for `actions`, dependency review, workflow lint. **There is no C# CodeQL job** — the .NET workflow scans for vulnerable packages, which is dependency scanning, not static code analysis |
| **Build hardening** | **Yes** | Partial | C: stack protector, FORTIFY, PIE/RELRO on GCC/Clang; `/guard:cf`, `/CETCOMPAT`, `/sdl` on MSVC, enforced by `check-hardening.sh`. .NET: strong-name signing |
| Coverage | Yes | Yes | C: gcovr with per-component floors, patch coverage, and a zero-gcda failure. **No overall percentage is published any more.** .NET: XPlat + CodeCoverageSummary |
| Fuzzing | No | No | |

## 10. Cross-cutting client concerns

Areas that decide whether a device client is adoptable, distinct from protocol features.

| Concern | C | .NET | Notes |
|---|---|---|---|
| Installable / linkable package | **Yes** | Yes | C: `install()`/`export()` targets, CMake package config and pkg-config behind `AZ_IOT_INSTALL`; builds correctly as a CMake subproject. **No vcpkg port published** |
| Generated API reference | **No** | **No** | Public C headers are Doxygen-formatted but there is still **no Doxyfile and no doc build**, so no API reference is produced |
| Static analysis | **Yes** | **No** | See §9 — .NET has dependency scanning only, no C# CodeQL |
| Supply chain / SBOM | **Partial** | **Partial** | C `cgmanifest.json` pins every third-party component, SBOM generated by the official pipeline, installed deps ship LICENSE/NOTICE. Repo-wide dependency review; .NET vulnerable-package scan. Not confirmed whether the SBOM is published as an artifact |
| Secret hygiene in memory | **Partial** | **No** | C `az_iot_crypto__wipe()` covers HMAC scratch, SAS tokens, key slots and `sas_buffer` on teardown; `OPENSSL_cleanse` in key custody. .NET relies on the GC |
| Log redaction guarantees | Partial | Partial | C redacts PKCS#11 URIs and truncates Paho traces; neither library **documents** what must never reach a sink |
| Measured footprint (ROM/RAM) | **Partial** | N/A | Published for the az_mqtt adapter only: per-module `.text`/`.rodata`/`.data`, per-config deltas, RAM per client. No figures for the Paho build |
| Portable time source | Partial | N/A | `az_iot_time_mono_ms()` is POSIX `clock_gettime` or Win32 `GetTickCount64` behind `#if defined(_WIN32)`. **Still no platform hook**, so an RTOS port must patch `src/core/mono_time.c` |
| Thread-safety contract | Partial | Partial | C is a single-threaded `do_work()` pump with callbacks on the caller's thread, stated in `README.md`/`design.md` but not enforced by a test |
| Reboot persistence / session resumption | Partial | **No** | Only the software-update client persists (`persist_state_fn`/`load_state_fn`, CRC-32 guarded). MQTT session, twin version and in-flight operations are cold-started |
| Backpressure / in-flight bounds | **Yes** | Partial | C has one connection-wide `AZ_IOT_MAX_PENDING_PUBACKS` table; selected feature clients may reserve part of it and every other caller shares the remainder. A slot is taken before the publish, so a full table refuses with `AZ_IOT_ERR_BUSY` instead of failing an already-sent publish |
| Credential expiry handling | **Yes** | Partial | C renews a SAS token at `renewal_percent` before expiry and falls back across credentials on rejection; neither library warns ahead of client-certificate expiry |
| Clock-skew tolerance | Partial | Partial | Monotonic time drives backoff; SAS needs a real clock, and the samples check for one. No guidance on wall-clock skew against certificate validity |
| API deprecation policy | N/A | N/A | Both are pre-release; no policy is written |

## 11. Concrete open gaps (from code markers)

> **Source change.** Earlier revisions of this section were derived from `c/docs/TODO.md`, which **no longer exists**. The per-phase counts previously quoted here ("21 open / 37 done") are unsourced and have been removed. What follows is from the code and docs as they stand.

**C**
- **mqttv5 hub does not accept SAS.** SAS covers DPS and the mqttv3 hub only; `connecting.md` records the hub side as not yet accepting it.
- **Hub-side CSR renewal is mqttv3-only.**
- **MQTT v5 fields accepted but never serialized**: `az_iot_mqtt_connect_options.user_properties` on CONNECT, and `response_topic` / `topic_alias` on PUBLISH. Silent loss, no error.
- **PnP model id is announced on the mqttv3 username only** — never on the mqttv5 path, and not placed in the DPS payload.
- **Software updates: operational (post-registration) poll** still returns `AZ_IOT_ERR_NOT_CONNECTED`; root-key rotation remains unimplemented; no event for a successful "no update available".
- **`max_attempts` is per-ladder and misnamed** (`az_iot_retry_policy.h`); renaming is cheap only before release.
- **No generated API reference** (no Doxyfile), and **no vcpkg port**.
- **No platform hook for monotonic time** — an RTOS port patches `src/core/mono_time.c`.
- **C2D strict settlement** (accept/reject/abandon) is still not designed; C2D is mqttv3-only.
- Three non-production env-var overrides ship in library code: `AZ_IOT_HUB_MQTT_V5_MOCK_ENDPOINT` (skips DPS entirely), `AZ_IOT_DEVICE_ID`, `AZ_IOT_DPS_CONNECTION_PROFILE_OVERRIDE`. The last is a self-described development bridge pending a DPS api-version rollout.
- Naming: one file still references the retired `gen1`/`gen2`/`Classic` terms — `c/eng/check-layering.sh`, where they are the guard pattern itself.

**.NET**
- **The mqttv5 API set is not on `main`** — it lives on `releases/public-preview`. Everything mqttv5 in this matrix is therefore untracked here.
- `Unified` twin error mapping always returns `Result.Ok` (`Unified/Twin/TwinClient.cs:261-262`).
- Content type/encoding carries a `//TODO` on the unified telemetry path (`Unified/Telemetry/TelemetryClient.cs:65`).
- Telemetry QoS is not configurable (`Models/Telemetry/DeviceToCloudTelemetry.cs:13`).
- CSR QoS/ack correctness TODOs remain (`Unified/Connection/ConnectionClient.cs:130`).
- No offline queueing: publishing while disconnected throws (`MqttNetAdapter/MqttNetClient.cs:199`).
- Proxy support is websocket-only on the MQTTnet adapter.
- No software-update/ADU code at all.

**Closed since the last revision** — GitHub issue #258 (a full pending-PUBACK table reporting failure for an already-sent publish) is fixed: the slot is reserved *before* the publish and a full pool refuses without sending.

## 12. Net-new vs the old public SDKs

Present here, no analogue in `azure-iot-sdk-c` / `azure-iot-sdk-csharp`:
- mqttv5/MQTTv5 clients over MQTT v5 with a presence/birth handshake.
- Protobuf wire contracts shared across languages (`common/Protos/`), incl. the direct-method probe/exec/abandon flow.
- Certificate-provider abstraction with CSR issuance and hub-side renewal/rotation.
- Non-extractable key custody (PKCS#11 / engine URI, sign hook) — C only.
- BYO MQTT client as a first-class, conformance-tested contract.
- software updates (su) carried over DPS — C only.
- DPS-returned `connectionProfile` choosing the MQTT version at runtime.

Old-SDK staples deliberately **absent**: AMQP and multiplexing, HTTPS transport, SAS/symmetric-key and TPM auth, connection strings, modules/IoT Edge, service SDK (registry, jobs, query, digital twin, C2D send, feedback/file-upload notification receivers), PnP conventions and digital twin, device streams.

## 13. Caveats on this report

- Read from a local clone at `a453698` (2026-10-08); nothing was built or executed, so "Yes" means the code path exists and is wired, not that it was run.
- **The .NET mqttv5 API set is on `releases/public-preview`, not `main`.** This matrix tracks `main`, so every .NET mqttv5 cell reads N/A. That is a statement about where the code lives, not about whether the feature works.
- **Cut is not the same as missing.** C2D and file upload are absent on mqttv5 because the service does not offer them there; both libraries deleted their mqttv5 implementations rather than ship ahead of the service.
- Both libraries are **pre-release** — C `1.0.0-preview`, .NET `1.1.0` to GitHub Packages — so API shapes are still free to change.
- **No overall coverage percentage is published any more**; `code-coverage.md` now defines per-component floors and patch coverage instead. The 72.5 % / 49.9 % figures quoted in earlier revisions are withdrawn, not superseded.
- `c/docs/TODO.md` has been deleted; see the note in §11.
- Terminology follows the `mqttv3` / `mqttv5` convention.

## 14. What changed since the 2026-09-28 revision

Rechecked at `a453698` after roughly 65 merged PRs (#315–#387). The largest movements:

- **SAS authentication landed for C** and is first-class alongside X.509: symmetric keys (primary/secondary, enrollment-group derivation), application-supplied tokens, renewal before expiry, and ordered fallback on rejection. **This retires the "X.509 is the only device auth" statement that headed earlier revisions.** The mqttv5 hub still refuses SAS.
- **.NET mqttv5 moved off `main`** to `releases/public-preview`, so its column is N/A throughout.
- **Static analysis and build hardening arrived**, from nothing: clang-tidy, MSVC `/analyze`, CodeQL (C and actions), dependency review, workflow lint, an SDL pipeline, and enforced hardening flags. Earlier revisions listed this as absent.
- **The C library is installable** (CMake export + pkg-config + subproject support) and gained a **Yocto consumer leg**.
- **A second MQTT adapter** — vendored `az_mqtt`, zero-allocation, v3 and v5, opt-in, conformance-tested — with the first **published memory-footprint figures**.
- Version moved to `1.0.0-preview` (C) and `1.1.0` (.NET); C `README` and a client-configuration reference replaced the old build docs.
- Connection work: generalized retry policy (`retry_policy.c`, `az_iot_retry_policy.h`; `reconnect.c` is gone), `SETTING_UP` state, `RETRY_PENDING` rename, a `LOCAL` error source, selectable identity-rejection recovery, and pending-PUBACK slots taken before the publish — which closes GitHub issue #258.
- Logging gained a file sink, a defined line format and 16 CI-checked components.
- Secret zeroization went from a single `OPENSSL_cleanse` to a general `az_iot_crypto__wipe()` across tokens, keys and buffers.
