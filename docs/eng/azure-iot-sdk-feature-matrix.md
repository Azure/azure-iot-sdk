# Azure/azure-iot-sdk — client feature matrix

Repo: `Azure/azure-iot-sdk` (**private** mono-repo), `main` @ `3cd46fa`, 2026-09-28. Verified by reading the code, not docs or the public SDKs.

**This is not the old public SDK.** It ships two client libraries only — **C** (`/c`, C99, `AZ_IOT_VERSION_STRING "0.0.1"`, status "early bootstrap") and **.NET** (`/dotnet`, `net10.0`, `Microsoft.Azure.Iot.Device` 2.0.0 — renamed from `Microsoft.Azure.Devices.Client` in #226 — published to **GitHub Packages**, not nuget.org). No Java/Node/Python/embedded columns exist.

### Reading the columns

The columns are **MQTT protocol versions**, because that is what determines wire capability. Each version targets a different IoT Hub, and the two libraries package them differently:

| | Hub | MQTT | Encoding | C symbols | .NET types |
|---|---|---|---|---|---|
| **mqttv3** | the current Azure IoT Hub | v3.1.1 only | `$iothub/...` topics, percent-encoded property bag | `az_iot_mqttv3_*`, `inc/azure/iot/mqttv3/` | the mqttv3 path inside `Unified/*` |
| **mqttv5** | the new Azure IoT Hub | v5 only | `ih/{deviceId}/...` topics, protobuf + v5 user properties | `az_iot_mqttv5_*`, `inc/azure/iot/mqttv5/` | `MQTTv5/*`, directly or via `Unified/*` |

**`Unified` is not the .NET equivalent of C mqttv3** — it is a facade spanning *both* protocol versions, so it does not belong on a version axis:

- **C** exposes two sibling API families and makes the **application** branch: read `az_iot_connection_client_get_hub_profile()`, then instantiate either `az_iot_mqttv3_telemetry_client` or `az_iot_mqttv5_telemetry_client`. Choosing the wrong one is refused with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH`. There is no C facade that hides the choice.
- **.NET** branches **internally**. Each `Unified` feature client owns a nested `MQTTv5` counterpart and switches on `ConnectionProfile` at runtime — `Unified/Telemetry/TelemetryClient.cs`, `Unified/Twin/TwinClient.cs`, `Unified/DirectMethods/DirectMethodClient.cs`. `Unified/Connection/ConnectionClient.cs` does the same for the CONNECT packet and hands the mqttv5 client its own connection via `Stub.cs`. So a .NET app writes one code path; a C app writes two.
- The `MQTTv5` classes are also usable on their own, which is why mqttv5 rows can differ depending on whether that path is reached directly or through the facade — those cases are called out in the Notes.

So: **C mqttv3/mqttv5 = two APIs you choose between; .NET mqttv3/mqttv5 = two wire behaviours one API chooses for you.**

- **DPS always speaks MQTT v3.1.1**, whichever hub follows, and is **not a separate client** — it runs inside the connection client, which then picks the MQTT version from the DPS-returned `connectionProfile`.
- C keeps **one** connection client and splits only the feature clients (`c/docs/eng/client-separation.md`, which supersedes `split-client.md`).
- Shared protobuf contracts live in `common/Protos/{presence,directmethods,twin}.proto`; C hand-rolls the codec (`src/mqttv5/direct_method_codec.c`), .NET compiles them with Grpc.Tools.

Legend: **Yes** supported · **Partial** partial/caveated · **No** absent · **N/A** not applicable.

---

## 1. Architecture & lifecycle

| Feature | C mqttv3 | C mqttv5 | .NET mqttv3 | .NET mqttv5 | Notes |
|---|---|---|---|---|---|
| Maturity | Partial | Partial | Partial | Partial | C `0.0.1`, README says "early bootstrap"; .NET 2.0.0 prerelease to GitHub Packages |
| Single connection client, DPS internal | Yes | Yes | Yes | Yes | `az_iot_connection_client` / `AbstractConnectionClient`; provisioning is not an app step |
| Per-feature clients over one connection | Yes | Yes | Yes | Yes | telemetry / c2d / direct method / twin (+ file upload mqttv3 only) |
| Generation selected at | client init (app picks the API) | ← | runtime, per connection | ← | C refuses a mismatch with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH`; .NET switches on `ConnectionProfile` internally |
| Runtime generation query | Yes | Yes | Yes | Yes | `az_iot_connection_client_get_hub_profile()` / `ConnectionProfile` |
| MQTTv3↔MQTTv5 fallback handled by | application | ← | library | ← | C app must instantiate the other API family; .NET `Unified` facade branches for you |
| API model | single-threaded `do_work()` pump, callbacks on caller thread, no internal threads | ← | Task-based async, `CancellationToken` on every op, `IDisposable` | ← | .NET has **no** `IAsyncDisposable` |
| Nullable / strictness | C99-strict, `-pedantic`, banned-construct + layering CI gates | ← | `<Nullable>enable</Nullable>` | ← | |
| Struct/ABI versioning | Partial | Partial | N/A | N/A | `_internal_size` on events + cert-provider vtable v2; `struct_versioning.md` still a proposal |
| Preview/experimental markers | Partial | Partial | No | No | No per-API attributes; C README carries the status banner |

## 2. Transport

| Feature | C mqttv3 | C mqttv5 | .NET mqttv3 | .NET mqttv5 | Notes |
|---|---|---|---|---|---|
| MQTT 3.1.1 | Yes | N/A | Yes | N/A | Required for MQTTv3 **and** for DPS in both generations |
| MQTT 5.0 | N/A | Yes | N/A | Yes | v5 user properties, correlation data, content type, message expiry, reason codes |
| v5 user properties end-to-end | N/A | Yes | N/A | Yes | C now sets outbound and extracts inbound, with conformance round-trip cases (#205) |
| MQTT over WebSockets | Yes | Yes | Yes | Yes | C `samples/websockets`; .NET mqttv5 websocket URI added (#269) |
| HTTP CONNECT proxy | Yes | Yes | Partial | Partial | C `az_iot_mqtt_proxy_options` + `samples/proxy`; .NET websocket-only and not forwarded to MQTTv5 |
| AMQP | No | No | No | No | Deliberate — see `c/docs/amqp_vs_mqtt_for_new_sdk_client.md`. Test-only AMQP under `c/tests/deps/amqp` |
| HTTPS | Partial | No | Partial | No | Only the file-upload SAS PUT/notify; C ships **no** HTTP client (app supplies a transport hook) |
| BYO MQTT client | Yes | Yes | Yes | Yes | C `az_iot_mqtt_iface` vtable + published conformance suite; .NET `IMqttClient` + `CustomMqttClientSample` |
| Shipped adapters | Paho v3.1.1 + v5 (default) | ← | MQTTnet 5.1.0.1559 (`MqttNetAdapter`) | ← | C also has a **shell** Rust-MQTT adapter (refuses WS/proxy) and an ESP-IDF adapter inside the software-update sample |
| Allocation profile | Partial | Partial | N/A | N/A | Core is static/caller-allocated; PEM cert provider and the Paho adapter malloc |
| TLS version control | No | No | Yes | Yes | C exposes only `use_tls`; .NET forces Tls12/Tls13. Neither can disable validation |
| Reconnect / connection maintenance | Yes | Yes | Yes | Yes | C `src/core/reconnect.c`; .NET `MqttConnectionManager.MaintainConnectionAsync` |
| Keep-alive | Yes | Yes | Yes | Yes | C default 30 s; .NET default 60 s |
| Clean start / session expiry / LWT | Yes | Yes | Yes | Partial | C `resolve_session_options()` sets per role (#204); MQTTv5 `//TODO subscribe elide logic` |
| Subscription-ack gating | Yes | Yes | **Yes** | **Yes** | Both check SUBACK reason codes. C failure scope is configurable (`az_iot_subscription_failure_scope`); .NET always disconnects and reconnects |
| MQTT DISCONNECT reason code | N/A | **Yes** | **Yes** | **Yes** | Both expose it (.NET `MqttDisconnect.Reason`, incl. `DisconnectWithWillMessage`=4). C sets 0x04 on the mqttv5 hub (#204); .NET always sends NormalDisconnection |

## 3. Authentication

| Feature | C | .NET | Notes |
|---|---|---|---|
| X.509 client certificates | Yes | Yes | The **only** device auth in either library |
| SAS / symmetric key | No | No | .NET: `Password` "should always be empty array in x509 only world" |
| TPM attestation | No | No | Explicitly out of scope (`c/docs/eng/certificate-management.md`) |
| Entra ID / token credential | No | No | |
| Certificate-provider abstraction | Yes | Partial | C `az_iot_certificate_provider.h` vtable (load/release + csr/sign, v2); .NET has `X509AuthenticationProvider` only |
| CSR at DPS enrollment | Yes | **Yes** | Both send it. .NET fixed in #202 (`AbstractConnectionClient.cs:621`), was previously dead code |
| CSR renewal against the hub | Yes | Yes | C `az_iot_connection_client_send_csr()`/`_cancel_csr()`; .NET `SendCertificateSigningRequestAsync` — **MQTTv5 throws `NotImplementedException`** |
| Issued-cert callback / persistence | Yes | Yes | C `store_issued_certificate`; .NET swaps the auth provider and reconnects |
| Credential rotation without app restart | Yes | Yes | .NET file-upload `HttpClient` vs rotated cert is an open TODO |
| Non-extractable keys (PKCS#11 / HSM / TPM URI) | Yes | No — **in progress** | C `client_key_uri` + `crypto_engine_id`, `az_iot_paho_key_custody.c`, Linux e2e leg. .NET: SoftHSM support in draft PR #236 |
| Custom signing callback | Partial | No | C vtable `sign` hook exists but **Paho refuses it** — needs a BYO adapter |
| Trust bundle / custom CA | Partial | No — **in progress** | C single CA path/PEM, no rotation API. .NET `RemoteCertificateValidationCallback` lands with draft PR #236 |

## 4. Device features

| Feature | C mqttv3 | C mqttv5 | .NET mqttv3 | .NET mqttv5 | Notes |
|---|---|---|---|---|---|
| D2C telemetry | Yes | Yes | Yes | Yes | |
| Message properties | Yes | Yes | Yes | Yes | mqttv3 percent-encoded topic bag; mqttv5 v5 user properties |
| Content type / encoding | Yes | Yes | Yes | Partial | .NET mqttv5 has `//TODO fill in content type` |
| Message expiry | Partial | Partial | Partial | Partial | Present on the MQTT publish, not surfaced on the telemetry API |
| Configurable QoS | Partial | Partial | No | No | .NET `//TODO do we want configurable QoS here?` |
| C2D receive | Yes | **N/A** | **No** | **N/A** | Service supports C2D on mqttv3 only. C mqttv5 C2D client was **removed** (#272); .NET dropped C2D from the unified API (#255) |
| C2D settlement (accept/reject/abandon) | No | N/A | N/A | N/A | C: "design C2D strict-settlement state machine" still open (mqttv3 only) |
| Direct methods | Yes | Yes | Yes | Yes | mqttv5 adds the MQTTv5 **probe / exec / abandon** protobuf handshake |
| Slow / async method responses | Yes | Yes | Partial | Partial | C has dedicated `direct_method_slow_responder_gen1/mqttv5` samples |
| Twin get | Yes | Yes | Yes | Yes | .NET mqttv5 supports selective/ETag (`getReported`, `ifNotMatch`) |
| Reported-properties patch | Yes | Yes | Yes | Yes | |
| Desired-properties patch events | Yes | Yes | Yes | Yes | C mqttv5 now delivers the real version, SNAPSHOT vs PATCH kind, and resyncs when behind (#240) |
| Twin push (MQTTv5 birth-driven) | No | No | No | Yes | C options default false and the dispatch is not consumed; .NET has `TwinPushReceived`/`TwinPushOptions` |
| MQTTv5 presence / birth handshake | N/A | Yes | N/A | Yes | `common/Protos/presence.proto`; C `presence_encode_birth()` |
| File upload (SAS URI + notify) | Yes | **N/A** | **No** | **N/A** | Not offered on mqttv5. C mqttv3 has it (app supplies the HTTP hook); .NET **removed** file upload from the unified API (#255) |
| Device update (software updates) | Partial | Partial | **No** | **No** | C only: `az_iot_su.h`, su-over-DPS (renamed from ADU in #270). **.NET has no software-update code at all** |
| Connection state / error propagation | **Yes** | **Yes** | Yes | Yes | C now has an observer registry, scoped (DPS or HUB) state, `is_retriable` + `{source,code,message}` (#221/#224/#235) |
| Recover from FAULTED via close() | **Yes** | **Yes** | N/A | N/A | C: close() is a legal exit from FAULTED (#213) — was a permanent deadlock |
| Retry: exponential backoff + jitter | Yes | Yes | Yes | Yes | C 1 s→**60 s** default cap (#273), ∞, ±20 %, jitter not clamped to the cap (#219), per-scope ladders (#214); .NET `ExponentialBackoffRetryPolicy` (cap 60 s, jitter 95–105 %) |
| Retry-after honoured | Partial | Partial | Partial | Partial | Both honour DPS polling retry-after; C also honours it on the software-update topic. CSR `RetryAfterSeconds` is surfaced, not auto-applied |
| Re-provision on identity rejection | Yes | Yes | Yes | Yes | C `max_hub_connect_attempts_before_reprovision` (default 50) |
| Offline queueing / persistence | No | No | No | No | Publishing while disconnected fails (`AZ_IOT_ERR_NOT_CONNECTED` / `MqttClientNotConnectedException`) |
| Modules / IoT Edge | No | No | No | No | No ModuleClient, no edgeHub/workload HSM, no gateway support anywhere in the repo |
| Service-side client | No | No | No | No | No registry/jobs/query/digital-twin/C2D-send. .NET tests consume the **old** v1 packages for the service side |

## 5. DPS / provisioning

| Feature | C | .NET | Notes |
|---|---|---|---|
| Separate provisioning client | No | No | By design: a phase inside the connection client (`c/docs/dps-integration.md`) |
| Register + poll + assignment | Yes | Yes | .NET now pins api-version **2026-11-02-preview** for all DPS device sessions (#262); both poll with retry-after + jitter |
| X.509 attestation | Yes | Yes | Only attestation type supported |
| Symmetric key / TPM attestation | No | No | |
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
| Model ID on connect | Partial | No | C uses `opts.model_id` for the **MQTTv3 username only**; mqttv5 path unverified. .NET does not set it |
| Model ID via DPS payload | No | Partial | .NET references a `ModelIdPayload` type that is not in the repo |
| Components | Partial | No | C exposes only the `$.sub` component-name property; no component APIs |
| Digital twin / PnP conventions | No | No | |

## 7. Observability

| Feature | C | .NET | Notes |
|---|---|---|---|
| Log sink + levels | Yes | Partial | C `az_iot_log.h` (TRACE→OFF, pluggable sink); **.NET only writes to `System.Diagnostics.Trace`** |
| MQTT-level logging | Yes | Yes | .NET `EnableMqttLogging` → `MqttNetTraceLogger` |
| Per-classification filtering | No | N/A | C has a single `min_level` |
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
| Platforms built | Linux + Windows | Linux + Windows | **No macOS in either CI**; ESP32/ESP-IDF exists as an uncompiled sample component |
| AOT / trimming | N/A | No | No `IsAotCompatible`/`IsTrimmable` |
| Packaging | No | **Yes** | No vcpkg port. **.NET CD is fixed and green** — `cd-dotnet.yml` now points at `Microsoft.Azure.Iot.Device.csproj`; nightly runs succeeded 2026-09-25 and 2026-09-26 |

## 9. Testing & CI

| Feature | C | .NET | Notes |
|---|---|---|---|
| Unit tests | Yes (40 cmocka files) | Yes (xunit.v3 + Moq, 57 facts) | 42 ctest cases pass on each of the four C standards (#282) |
| Known-answer crypto vectors | **Yes** | N/A | C software-update adapters: FIPS 180-4 SHA-256, RS256 good/bad, root→SJWK→manifest chain (#276) |
| MQTT-interface conformance suite | Yes | No | C `tests/conformance/` for BYO adapters (Paho v3 + v5), needs a broker |
| Integration tests | Yes | Yes | C `reconnect_real_stack_test.c` |
| E2E against live Azure | Yes | Yes | Resources provisioned per run via OIDC, torn down after; Windows + Linux legs |
| **MQTTv5 e2e actually executed** | Yes (vs mock Hub-MQTTv5) | **No** | .NET `Setup.cs` skips 5×: 3 "No test infrastructure setup for MQTTv5 client testing yet.", 2 "No MQTTv5 hub to test against yet" |
| Dedicated software-update / CSR / PKCS#11 e2e | Yes | **Disabled** | C: `ci-c-e2e-adu.yml`, `ci-c-e2e-csr.yml` (Linux only), PKCS#11 on the Linux leg. .NET CSR/cert-mgmt tests exist but are `Skip`-ped (#229) |
| Software-update e2e vs the real service | **Yes** | N/A | `az_iot_tests_e2e_su_offer` (#279): real offered update, libcurl download, engine hash check; missing env **fails** rather than skips |
| Fault injection | Partial | **Yes** | .NET covers faults by **unit test** by design — `ConnectionFaultedUnitTests.cs`, 12 facts (identity fault, reprovision, terminal fault, pending-op cancellation). The empty `MqttNetFaultInjectionIntegrationTests.cs` is dead and is being deleted |
| Sanitizers | Yes | No | valgrind (Linux) + MSVC ASan, plus a **race-detector job** (helgrind/DRD) added with #205 |
| Style / layering gates | Yes | No | `check-banned-constructs.sh`, `check-layering.sh`, clang-format |
| Coverage | Yes | Yes | C: gcovr + gate, combined unit+e2e; **72.5 % line / 49.9 % branch** (recorded baseline). .NET: XPlat + CodeCoverageSummary |
| Static analysis | **No** | **No** | No clang-tidy, CodeQL, cppcheck or MISRA anywhere in the repo |
| Fuzzing | No | No | |

## 10. Cross-cutting client concerns

Areas that decide whether a device client is adoptable, distinct from protocol features. Added 2026-09-28; most were never tracked before, and several are **not implemented at all**, which is the point of listing them.

| Concern | C | .NET | Notes |
|---|---|---|---|
| Installable / linkable package | **No** | Yes | C has **no CMake `install()` or `export()` targets and no pkg-config**; consumers must vendor the tree or use FetchContent. `c/vcpkg.json` is a consumer manifest, not a published port |
| Generated API reference | **No** | **No** | Public C headers are written in Doxygen format but there is **no Doxyfile and no doc build**, so no API reference is produced or published |
| Static analysis | **No** | **No** | No clang-tidy, CodeQL, cppcheck or MISRA configuration in the repo |
| Supply chain / SBOM | **No** | **No** | No SBOM and no third-party notices file. Dependencies are version-pinned (azure-sdk-for-c 1.5.0 via FetchContent; Paho pinned), which is the mitigating half |
| Secret hygiene in memory | Partial | **No** | C zeroizes in exactly one place: `OPENSSL_cleanse` on an extractable private-key PEM in the key-custody path (`az_iot_paho_key_custody.c:532`). There is **no general zeroization** of SAS, CSR or key buffers on teardown |
| Log redaction guarantees | Partial | Partial | C redacts where it matters most — `redact_key_uri()` strips the PKCS#11 query, and the Paho trace hook truncates to `<redacted>`. But neither library **documents** what must never reach a sink, and C logs the DPS username at DEBUG |
| Measured footprint (ROM/RAM) | **No** | N/A | The C library targets constrained devices and is non-allocating on the hot path, but **no measured size figures are published**, so the claim is unverifiable by a reader |
| Portable time source | Partial | N/A | `az_iot_time_mono_ms()` is POSIX `clock_gettime(CLOCK_MONOTONIC)` or Win32 `GetTickCount64`, selected by `#if defined(_WIN32)`. **There is no platform hook**, so an RTOS/bare-metal port with neither has to patch `reconnect.c` |
| Thread-safety contract | Partial | Partial | C is a single-threaded `do_work()` pump with callbacks on the caller's thread, stated in `README.md`/`design.md` but not in a dedicated contract doc or enforced by a test |
| Reboot persistence / session resumption | Partial | **No** | The software-update client has a real checkpoint API — `persist_state_fn`/`load_state_fn` with a CRC-32 guarded blob — so an update survives a reboot. **No other feature persists**: MQTT session, twin version and in-flight operations are all cold-started |
| Backpressure / in-flight bounds | Partial | Partial | C refuses overlapping operations with `AZ_IOT_ERR_BUSY` rather than queuing; no configurable in-flight window |
| Credential expiry handling | Partial | Partial | CSR renewal exists on both; neither warns an application ahead of client-certificate expiry |
| Clock-skew tolerance | Partial | Partial | Monotonic time drives backoff; no guidance on wall-clock skew, which affects certificate validity |
| API deprecation policy | N/A | N/A | Unreleased, `git tag` empty — no policy needed yet, but none is written either |

## 11. Concrete open gaps (from TODOs and code markers)

**C** (`c/docs/TODO.md` + source) — 21 items still unticked, 37 done. See also §10 for cross-cutting gaps (packaging, API docs, static analysis, SBOM, secret zeroization, portable time source).
- Paho v5: CONNECT user properties, and PUBLISH `response_topic` / `topic_alias`, are accepted by the API but **not serialized** (residual Phase 3).
- mqttv5 direct-method ready-token sweep still runs only on inbound messages; needs a periodic tick.
- C2D (mqttv3 only): no strict-settlement state machine; **no e2e against a real MQTTv3 IoT Hub**.
- No mqttv5-mock CI job, and the conformance suite does not yet pass v5 against Mosquitto (Phase 7).
- Software updates: operational (post-registration) poll returns `AZ_IOT_ERR_NOT_CONNECTED` (`su_channel_dps.c:21-24` STATUS note; needs connection-client session-lifetime work). Root-key rotation deferred — 8 sub-items. No event for a successful "no update available".
- `connection_client.c` — one credentials flow still has no mqttv5 path.
- Error-check audit outstanding (`c/docs/TODO.md`, Code Quality).
- `max_attempts` is per-ladder and misnamed; renaming it gets harder after release.
- Allocation model is an open pre-release design decision.
- **Naming debt from #271**: 92 files under `c/` still carry `gen1`/`gen2`/`Classic`/`AEG`/`Next`/`HUB_NEXT`. Four public enum identifiers are affected — `AZ_IOT_CONNECTION_PROFILE_CLASSIC`, `AZ_IOT_MQTT_ROLE_HUB_CLASSIC`, `AZ_IOT_MQTT_ROLE_HUB_NEXT`, and the `AZ_IOT_HUB_NEXT_MOCK_ENDPOINT` env var.
- Stale TODO entry: "Skip `az_iot_hub_client_get_user_name()` for the mqttv5 hub" is **already done** — `connection_client.c` branches on `session_role` and builds the mqttv5 username via `presence_build_username()`. The checkbox, not the code, is wrong.

**.NET**
- `MQTTv5/Connection/ConnectionClient.cs:219` — `NotImplementedException` for certificate signing on the mqttv5 hub. (The file-upload `NotImplementedException`s are gone; the feature was removed outright in #255.)
- `MqttClient`/websocket/proxy options still not forwarded Unified→MQTTv5 (`Unified/Connection/ConnectionClient.cs:53`).
- Unified twin error mapping always returns `Result.Ok` (`Unified/Twin/TwinClient.cs:287-288`).
- CSR / certificate-management integration tests are disabled via `Skip`, and CI sets `enable-certificate-management: 'false'` (`ci-dotnet.yml:70`, #229).
- Unhandled twin-GET-across-reconnect and disconnect-mid-method cases; CSR QoS/ack correctness TODOs.
- `dotnet/README.md` is 0 bytes.
- Fixed since the last revision: the placeholder `DeviceException("TODO")` / `Exception("todo")` throws are **gone** (#259), and the CD pipeline is green again.

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

- Read from a local clone at `3cd46fa` (2026-09-28); nothing was built or executed, so "Yes" means the code path exists and is wired, not that it was run.
- Per-version rows for .NET describe the **wire behaviour** of each MQTT version. Because `Unified` implements mqttv3 itself but delegates mqttv5 to nested `MQTTv5` clients, some mqttv5 cells differ depending on whether `MQTTv5` is used directly or through the facade; those cases are flagged in the Notes.
- **Cut ≠ missing.** C2D and file upload are absent on mqttv5 because the *service* does not offer them there. Both libraries have now **deleted** their mqttv5 C2D/file-upload code rather than shipping clients ahead of the service (#272, #255) — a reversal of the earlier "keep it, it returns after Ignite 2026" position recorded in the previous revision.
- These libraries are **unreleased** — `git tag` is empty, so there is no shipped ABI and no back-compat constraint on any of the shapes described here.
- `model_id` on the mqttv5 path, QoS 2 usage, and message ordering remain **unverified** — the code does not settle them.
- Terminology follows the `mqttv3` / `mqttv5` convention. Where a row cites a file or symbol that still carries the old naming, that is the code's current state, not a slip in this document.

### Corrections from Tim (owner of `dotnet/`), 2026-09-23

- **Subscription-ack gating is Yes for .NET, both MQTT versions** — this matrix previously said No. Verified: `Unified/Connection/ConnectionClient.cs:116-133` and `MQTTv5/Connection/ConnectionClient.cs:61-88` both check SUBACK reason codes and reconnect on refusal. The remaining difference is policy, not capability: C lets you choose the failure scope, .NET always disconnects and reconnects.
- **Non-extractable keys and trust bundle / custom CA are in progress**, not simply absent — draft PR #236 adds SoftHSM-backed keys and a `RemoteCertificateValidationCallback`. Still No on `main`.
- **.NET fault-injection coverage is by unit test, by design** — `ConnectionFaultedUnitTests.cs`, 12 facts. The empty `MqttNetFaultInjectionIntegrationTests.cs` is dead code pending deletion, not a coverage gap.
- Found while re-auditing on the back of the above: **.NET does expose a DISCONNECT reason code** (`MqttDisconnect.Reason`, enum includes `DisconnectWithWillMessage`=4) — previously recorded as No. It is always sent as `NormalDisconnection`.

## 14. What changed since the 2026-09-27 revision

Rechecked at `3cd46fa` after #275–#282.

- **C builds strict under c99, c11, c17 and c23** in CI, `-pedantic -Werror`, 42/42 ctest on each (#282). Paho is pinned to C99 because v1.3.13 declares `typedef unsigned int bool`, which C23 rejects (fixed upstream in v1.3.15).
- **Software-update e2e against the real service** with an offered update, real libcurl download and engine hash check (#279); missing environment now fails the suite instead of skipping. Known-answer crypto vectors added for every su crypto adapter (#276).
- Caller-allocated teardown renamed `_destroy()` → `_deinit()` (#277). Adapter factories still use `_destroy()`, so both spellings are live.
- Software-update samples: regular-update sample added, ESP32 route fixed (#275).
- **New §10 "Cross-cutting client concerns"** records adoption-blocking areas the matrix never tracked — several are unimplemented.

## 15. What changed in the 2026-09-27 revision

Rechecked at `59b531a` after #255–#273.

- **Naming**: `gen1`/`gen2` renamed to `mqttv3`/`mqttv5` throughout the C library (#271); ADU renamed to software updates / `su` (#270). This document now follows that convention. 92 files under `c/` still carry the old terms, including four public identifiers — see §10.
- **mqttv5 C2D removed** from the C API (#272) and **C2D + file upload removed** from the .NET unified API (#255), because the service does not support them on mqttv5.
- **.NET CD is fixed and green** (was failing with `MSB1009` on every run since the #226 rename); nightly publishes succeeded 2026-09-25 and 2026-09-26.
- .NET placeholder `DeviceException("TODO")` throws removed (#259); DPS device sessions now pin api-version 2026-11-02-preview (#262); mqttv5 websocket URI and PUBACK handling added (#269).
- C default reconnect backoff capped at 60 s (#273). C samples regrouped into `unified/` and `mqttv5/` (#260).
- Coverage unchanged: **72.5% line / 49.9% branch** overall; the Paho adapter remains the weakest at 67.0% / 38.4%.
