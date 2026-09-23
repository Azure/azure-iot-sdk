# Azure/azure-iot-sdk — client feature matrix

Repo: `Azure/azure-iot-sdk` (**private** mono-repo), `main` @ `75e281ab`, 2026-09-23. Verified by reading the code, not docs or the public SDKs.

**This is not the old public SDK.** It ships two client libraries only — **C** (`/c`, C99, `AZ_IOT_VERSION_STRING "0.0.1"`, status "early bootstrap") and **.NET** (`/dotnet`, `net10.0`, `Microsoft.Azure.Iot.Device` 2.0.0 — renamed from `Microsoft.Azure.Devices.Client` in #226 — published to **GitHub Packages**, not nuget.org). No Java/Node/Python/embedded columns exist.

### Reading the columns

The columns are **hub generations**, because that is what determines wire capability. The two libraries package those generations differently:

| | Hub | MQTT | Encoding | C symbols | .NET types |
|---|---|---|---|---|---|
| **gen1 / Classic** | Azure IoT Hub Classic | v3.1.1 only | `$iothub/...` topics, percent-encoded property bag | `az_iot_gen1_*` | the Classic path inside `Unified/*` |
| **gen2 / Next (AEG)** | Azure IoT / AEG Hub | v5 only | `ih/{deviceId}/...` topics, protobuf + v5 user properties | `az_iot_gen2_*` | `Gen2/*`, directly or via `Unified/*` |

**`Unified` is not the .NET equivalent of C gen1** — it is a facade spanning *both* generations, so it does not belong on a generation axis:

- **C** exposes two sibling API families and makes the **application** branch: read `az_iot_connection_client_get_hub_profile()`, then instantiate either `az_iot_gen1_telemetry_client` or `az_iot_gen2_telemetry_client`. Choosing the wrong one is refused with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH`. There is no C facade that hides the choice.
- **.NET** branches **internally**. Every `Unified` feature client owns a nested `Gen2` counterpart and switches on `ConnectionProfile` at runtime — `Unified/Telemetry/TelemetryClient.cs:24,78` (Stub at `:44`), `Unified/Twin/TwinClient.cs`, `Unified/DirectMethods/DirectMethodClient.cs`, `Unified/FileUpload/FileUploadClient.cs`. `Unified/Connection/ConnectionClient.cs` does the same for the CONNECT packet and hands the gen2 client its own connection via `Stub.cs`. So a .NET app writes one code path; a C app writes two.
- The `Gen2` classes are also usable on their own, which is why gen2 rows can differ depending on whether the gen2 path is reached directly or through the facade — those cases are called out in the Notes.

So: **C gen1/gen2 = two APIs you choose between; .NET gen1/gen2 = two wire behaviours one API chooses for you.**

- **DPS always speaks MQTT v3.1.1**, in both generations, and is **not a separate client** — it runs inside the connection client, which then picks the MQTT version from the DPS-returned `connectionProfile`/`hub_version`.
- C keeps **one** connection client and splits only the feature clients (`c/docs/eng/client-separation.md`, which supersedes `split-client.md`).
- Shared protobuf contracts live in `common/Protos/{presence,directmethods,twin}.proto`; C hand-rolls the codec (`src/gen2/direct_method_codec.c`), .NET compiles them with Grpc.Tools.

Legend: **Yes** supported · **Partial** partial/caveated · **No** absent · **N/A** not applicable.

---

## 1. Architecture & lifecycle

| Feature | C gen1 | C gen2 | .NET gen1 | .NET gen2 | Notes |
|---|---|---|---|---|---|
| Maturity | Partial | Partial | Partial | Partial | C `0.0.1`, README says "early bootstrap"; .NET 2.0.0 prerelease to GitHub Packages |
| Single connection client, DPS internal | Yes | Yes | Yes | Yes | `az_iot_connection_client` / `AbstractConnectionClient`; provisioning is not an app step |
| Per-feature clients over one connection | Yes | Yes | Yes | Yes | telemetry / c2d / direct method / twin (+ file upload gen1 only) |
| Generation selected at | client init (app picks the API) | ← | runtime, per connection | ← | C refuses a mismatch with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH`; .NET switches on `ConnectionProfile` internally |
| Runtime generation query | Yes | Yes | Yes | Yes | `az_iot_connection_client_get_hub_profile()` / `ConnectionProfile` |
| Classic↔Next fallback handled by | application | ← | library | ← | C app must instantiate the other API family; .NET `Unified` facade branches for you |
| API model | single-threaded `do_work()` pump, callbacks on caller thread, no internal threads | ← | Task-based async, `CancellationToken` on every op, `IDisposable` | ← | .NET has **no** `IAsyncDisposable` |
| Nullable / strictness | C99-strict, `-pedantic`, banned-construct + layering CI gates | ← | `<Nullable>enable</Nullable>` | ← | |
| Struct/ABI versioning | Partial | Partial | N/A | N/A | `_internal_size` on events + cert-provider vtable v2; `struct_versioning.md` still a proposal |
| Preview/experimental markers | Partial | Partial | No | No | No per-API attributes; C README carries the status banner |

## 2. Transport

| Feature | C gen1 | C gen2 | .NET gen1 | .NET gen2 | Notes |
|---|---|---|---|---|---|
| MQTT 3.1.1 | Yes | N/A | Yes | N/A | Required for Classic **and** for DPS in both generations |
| MQTT 5.0 | N/A | Yes | N/A | Yes | v5 user properties, correlation data, content type, message expiry, reason codes |
| v5 user properties end-to-end | N/A | Yes | N/A | Yes | C now sets outbound and extracts inbound, with conformance round-trip cases (#205) |
| MQTT over WebSockets | Yes | Yes | Yes | Partial | C `samples/websockets`; .NET `UseWebsocket` **not forwarded Unified→Gen2** |
| HTTP CONNECT proxy | Yes | Yes | Partial | Partial | C `az_iot_mqtt_proxy_options` + `samples/proxy`; .NET websocket-only and not forwarded to Gen2 |
| AMQP | No | No | No | No | Deliberate — see `c/docs/amqp_vs_mqtt_for_new_sdk_client.md`. Test-only AMQP under `c/tests/deps/amqp` |
| HTTPS | Partial | No | Partial | No | Only the file-upload SAS PUT/notify; C ships **no** HTTP client (app supplies a transport hook) |
| BYO MQTT client | Yes | Yes | Yes | Yes | C `az_iot_mqtt_iface` vtable + published conformance suite; .NET `IMqttClient` + `CustomMqttClientSample` |
| Shipped adapters | Paho v3.1.1 + v5 (default) | ← | MQTTnet 5.1.0.1559 (`MqttNetAdapter`) | ← | C also has a **shell** Rust-MQTT adapter (refuses WS/proxy) and an ESP-IDF adapter inside the ADU sample |
| Allocation profile | Partial | Partial | N/A | N/A | Core is static/caller-allocated; PEM cert provider and the Paho adapter malloc |
| TLS version control | No | No | Yes | Yes | C exposes only `use_tls`; .NET forces Tls12/Tls13. Neither can disable validation |
| Reconnect / connection maintenance | Yes | Yes | Yes | Yes | C `src/core/reconnect.c`; .NET `MqttConnectionManager.MaintainConnectionAsync` |
| Keep-alive | Yes | Yes | Yes | Yes | C default 30 s; .NET default 60 s |
| Clean start / session expiry / LWT | Yes | Yes | Yes | Partial | C `resolve_session_options()` sets per role (#204); Gen2 `//TODO subscribe elide logic` |
| Subscription-ack gating | Yes | Yes | **Yes** | **Yes** | Both check SUBACK reason codes. C failure scope is configurable (`az_iot_subscription_failure_scope`); .NET always disconnects and reconnects |
| MQTT DISCONNECT reason code | N/A | **Yes** | **Yes** | **Yes** | Both expose it (.NET `MqttDisconnect.Reason`, incl. `DisconnectWithWillMessage`=4). C sets 0x04 on HUB_NEXT (#204); .NET always sends NormalDisconnection |

## 3. Authentication

| Feature | C | .NET | Notes |
|---|---|---|---|
| X.509 client certificates | Yes | Yes | The **only** device auth in either library |
| SAS / symmetric key | No | No | .NET: `Password` "should always be empty array in x509 only world" |
| TPM attestation | No | No | Explicitly out of scope (`c/docs/eng/certificate-management.md`) |
| Entra ID / token credential | No | No | |
| Certificate-provider abstraction | Yes | Partial | C `az_iot_certificate_provider.h` vtable (load/release + csr/sign, v2); .NET has `X509AuthenticationProvider` only |
| CSR at DPS enrollment | Yes | **Yes** | Both send it. .NET fixed in #202 (`AbstractConnectionClient.cs:621`), was previously dead code |
| CSR renewal against the hub | Yes | Yes | C `az_iot_connection_client_send_csr()`/`_cancel_csr()`; .NET `SendCertificateSigningRequestAsync` — **Gen2 throws `NotImplementedException`** |
| Issued-cert callback / persistence | Yes | Yes | C `store_issued_certificate`; .NET swaps the auth provider and reconnects |
| Credential rotation without app restart | Yes | Yes | .NET file-upload `HttpClient` vs rotated cert is an open TODO |
| Non-extractable keys (PKCS#11 / HSM / TPM URI) | Yes | No — **in progress** | C `client_key_uri` + `crypto_engine_id`, `az_iot_paho_key_custody.c`, Linux e2e leg. .NET: SoftHSM support in draft PR #236 |
| Custom signing callback | Partial | No | C vtable `sign` hook exists but **Paho refuses it** — needs a BYO adapter |
| Trust bundle / custom CA | Partial | No — **in progress** | C single CA path/PEM, no rotation API. .NET `RemoteCertificateValidationCallback` lands with draft PR #236 |

## 4. Device features

| Feature | C gen1 | C gen2 | .NET gen1 | .NET gen2 | Notes |
|---|---|---|---|---|---|
| D2C telemetry | Yes | Yes | Yes | Yes | |
| Message properties | Yes | Yes | Yes | Yes | gen1 percent-encoded topic bag; gen2 v5 user properties |
| Content type / encoding | Yes | Yes | Yes | Partial | .NET Gen2 has `//TODO fill in content type` |
| Message expiry | Partial | Partial | Partial | Partial | Present on the MQTT publish, not surfaced on the telemetry API |
| Configurable QoS | Partial | Partial | No | No | .NET `//TODO do we want configurable QoS here?` |
| C2D receive | Yes | Yes | Yes | **No** | C gen2 client exists and is ahead of the service; **AEG C2D is cut service-side for Ignite 2026**, returns after. .NET Gen2 "Not supported yet" |
| C2D settlement (accept/reject/abandon) | No | No | Partial | No | C: "design C2D strict-settlement state machine" still open. .NET never auto-acks (`AutoAcknowledge=false`) |
| Direct methods | Yes | Yes | Yes | Yes | gen2 adds the AEG **probe / exec / abandon** protobuf handshake |
| Slow / async method responses | Yes | Yes | Partial | Partial | C has dedicated `direct_method_slow_responder_gen1/gen2` samples |
| Twin get | Yes | Yes | Yes | Yes | .NET Gen2 supports selective/ETag (`getReported`, `ifNotMatch`) |
| Reported-properties patch | Yes | Yes | Yes | Yes | |
| Desired-properties patch events | Yes | Yes | Yes | Yes | C gen2 now delivers the real version, SNAPSHOT vs PATCH kind, and resyncs when behind (#240) |
| Twin push (AEG birth-driven) | No | No | No | Yes | C options default false and the dispatch is not consumed; .NET has `TwinPushReceived`/`TwinPushOptions` |
| AEG presence / birth handshake | N/A | Yes | N/A | Yes | `common/Protos/presence.proto`; C `presence_encode_birth()` |
| File upload (SAS URI + notify) | Yes | **No** | Yes | **No** | **Cut from AEG by design**, not an SDK gap; gen2 clients refuse/throw. C requires an app-supplied HTTP hook |
| Device update (ADU v2) | Partial | Partial | **No** | **No** | C only: `az_iot_adu.h`, ADU-over-DPS. **.NET has no ADU code at all** |
| Connection state / error propagation | **Yes** | **Yes** | Yes | Yes | C now has an observer registry, scoped (DPS or HUB) state, `is_retriable` + `{source,code,message}` (#221/#224/#235) |
| Recover from FAULTED via close() | **Yes** | **Yes** | N/A | N/A | C: close() is a legal exit from FAULTED (#213) — was a permanent deadlock |
| Retry: exponential backoff + jitter | Yes | Yes | Yes | Yes | C 1 s→30 s, ∞, ±20 %, jitter no longer clamped to the cap (#219), per-scope ladders (#214); .NET `ExponentialBackoffRetryPolicy` (cap 60 s, jitter 95–105 %) |
| Retry-after honoured | Partial | Partial | Partial | Partial | Both honour DPS polling retry-after; C also honours it on the ADU topic. CSR `RetryAfterSeconds` is surfaced, not auto-applied |
| Re-provision on identity rejection | Yes | Yes | Yes | Yes | C `max_hub_connect_attempts_before_reprovision` (default 50) |
| Offline queueing / persistence | No | No | No | No | Publishing while disconnected fails (`AZ_IOT_ERR_NOT_CONNECTED` / `MqttClientNotConnectedException`) |
| Modules / IoT Edge | No | No | No | No | No ModuleClient, no edgeHub/workload HSM, no gateway support anywhere in the repo |
| Service-side client | No | No | No | No | No registry/jobs/query/digital-twin/C2D-send. .NET tests consume the **old** v1 packages for the service side |

## 5. DPS / provisioning

| Feature | C | .NET | Notes |
|---|---|---|---|
| Separate provisioning client | No | No | By design: a phase inside the connection client (`c/docs/dps-integration.md`) |
| Register + poll + assignment | Yes | Yes | .NET: api-version 2021-10-01, or 2025-07-01-preview for CSR; both poll with retry-after + jitter |
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
| Model ID on connect | Partial | No | C uses `opts.model_id` for the **Classic username only**; gen2 path unverified. .NET does not set it |
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
| Dependencies | azure-sdk-for-c 1.5.0 (FetchContent, mandatory), Paho | MQTTnet 5.1.0.1559, Google.Protobuf 3.34.1, Google.Protobuf.Tools + Grpc.Tools 2.80.0 (build-only) | `az::core`/`az::iot::hub`/`az::iot::provisioning`; Next protocol logic lives in this repo |
| Dependency acquisition | vcpkg manifest (primary) or CPM.cmake | NuGet | `azure-sdk-for-c` is a git submodule **only** under the ESP32 sample |
| Build options | 16 CMake options (`cmake/az_iot_options.cmake`) | — | PAHO, RUST_MQTT, KEY_CUSTODY, ADU crypto, cert provider, tests, e2e, conformance, coverage |
| Platforms built | Linux + Windows | Linux + Windows | **No macOS in either CI**; ESP32/ESP-IDF exists as an uncompiled sample component |
| AOT / trimming | N/A | No | No `IsAotCompatible`/`IsTrimmable` |
| Packaging | No | **Broken** | No vcpkg port. **.NET CD has failed on every run since #226**: `cd-dotnet.yml:33` still points at the pre-rename csproj → `MSB1009: Project file does not exist` |

## 9. Testing & CI

| Feature | C | .NET | Notes |
|---|---|---|---|
| Unit tests | Yes (38 cmocka files) | Yes (xunit.v3 + Moq, 57 facts) | C added `paho_event_queue_race_test.c`, `connection_dps_payload_test.c` |
| MQTT-interface conformance suite | Yes | No | C `tests/conformance/` for BYO adapters (Paho v3 + v5), needs a broker |
| Integration tests | Yes | Yes | C `reconnect_real_stack_test.c` |
| E2E against live Azure | Yes | Yes | Resources provisioned per run via OIDC, torn down after; Windows + Linux legs |
| **Gen2 e2e actually executed** | Yes (vs mock Hub-Next) | **No** | .NET `Setup.cs` skips 5×: 3 "No test infrastructure setup for Gen2 client testing yet.", 2 "No AEG hub to test against yet" |
| Dedicated ADU / CSR / PKCS#11 e2e | Yes | **Disabled** | C: `ci-c-e2e-adu.yml`, `ci-c-e2e-csr.yml` (Linux only), PKCS#11 on the Linux leg. .NET CSR/cert-mgmt tests exist but are `Skip`-ped (#229) |
| Fault injection | Partial | **Yes** | .NET covers faults by **unit test** by design — `ConnectionFaultedUnitTests.cs`, 12 facts (identity fault, reprovision, terminal fault, pending-op cancellation). The empty `MqttNetFaultInjectionIntegrationTests.cs` is dead and is being deleted |
| Sanitizers | Yes | No | valgrind (Linux) + MSVC ASan, plus a **race-detector job** (helgrind/DRD) added with #205 |
| Style / layering gates | Yes | No | `check-banned-constructs.sh`, `check-layering.sh`, clang-format |
| Coverage | Yes | Yes | C: gcovr + gate, combined unit+e2e; **72.5 % line / 49.9 % branch**. .NET: XPlat + CodeCoverageSummary |
| Fuzzing | No | No | |

## 10. Concrete open gaps (from TODOs and code markers)

**C** (`c/docs/TODO.md` + source) — Phase 3 and Phase 4 are now closed out.
- Paho v5: CONNECT user properties, and PUBLISH `response_topic` / `topic_alias`, are still not serialized (residual Phase 3).
- gen2 direct-method ready-token sweep still runs only on inbound messages.
- C2D: no strict-settlement state machine; no e2e against a real Classic hub.
- Classic username still sent for HUB_NEXT (Phase 6); no Next-mock CI job, v5 conformance not passing against mosquitto (Phase 7).
- ADU: operational (post-registration) poll returns `AZ_IOT_ERR_NOT_CONNECTED` (`adu_channel_dps.c:191-196`); root-key rotation (Option B) deferred, 8 sub-items. ADUv1 twin channel cut. No event for a successful "no update available".
- `connection_client.c:5562` — "Hub-Next (AEG) path not defined yet" for one credentials flow (was `:4145`).
- Error-check audit outstanding.
- `max_attempts` is per-ladder and misnamed; renaming it gets harder after release.
- Allocation model is an open pre-release design decision.

**.NET**
- `Gen2/FileUpload/FileUploadClient.cs:26,37` and `Gen2/Connection/ConnectionClient.cs:219` — `NotImplementedException`.
- **CD pipeline is broken**: `cd-dotnet.yml:33` still references the pre-rename `Microsoft.Azure.Devices.Client.csproj`; every run since #226 fails with `MSB1009`.
- Placeholder exceptions shipped in the connection path: `new DeviceException("TODO")` ×3 in `MqttConnectionManager.cs` (697/716/735), plus `throw new Exception("todo")` at `AbstractConnectionClient.cs:778`.
- `MqttClient`/websocket/proxy options not forwarded Unified→Gen2 (`Unified/Connection/ConnectionClient.cs:53`).
- Unified twin error mapping always returns `Result.Ok` (`Unified/Twin/TwinClient.cs:287-288`).
- CSR / certificate-management integration tests are disabled via `Skip`, and CI sets `enable-certificate-management: 'false'` (#229).
- Unhandled twin-GET-across-reconnect and disconnect-mid-method cases; CSR QoS/ack correctness TODOs.
- File-upload `HttpClient` vs rotated certificate unresolved.
- `dotnet/README.md` is 0 bytes.

## 11. Net-new vs the old public SDKs

Present here, no analogue in `azure-iot-sdk-c` / `azure-iot-sdk-csharp`:
- gen2/AEG clients over MQTT v5 with a presence/birth handshake.
- Protobuf wire contracts shared across languages (`common/Protos/`), incl. the direct-method probe/exec/abandon flow.
- Certificate-provider abstraction with CSR issuance and hub-side renewal/rotation.
- Non-extractable key custody (PKCS#11 / engine URI, sign hook) — C only.
- BYO MQTT client as a first-class, conformance-tested contract.
- ADU v2 carried over DPS — C only.
- DPS-returned `connectionProfile` choosing the MQTT version at runtime.

Old-SDK staples deliberately **absent**: AMQP and multiplexing, HTTPS transport, SAS/symmetric-key and TPM auth, connection strings, modules/IoT Edge, service SDK (registry, jobs, query, digital twin, C2D send, feedback/file-upload notification receivers), PnP conventions and digital twin, device streams.

## 12. Caveats on this report

- Read from a local clone at `75e281ab` (2026-09-23); nothing was built or executed, so "Yes" means the code path exists and is wired, not that it was run. The one exception is the .NET CD failure, which was confirmed from the workflow logs (`MSB1009`).
- Generation-specific rows for .NET describe the **wire behaviour** of each generation. Because `Unified` implements gen1 itself but delegates gen2 to nested `Gen2` clients, some gen2 cells differ depending on whether `Gen2` is used directly or through the facade; those cases are flagged in the Notes.
- **Cut ≠ missing.** gen2 C2D and gen2 file upload are absent because the *service* cut them from AEG — file upload permanently by design, C2D temporarily for Ignite 2026 (the C gen2 C2D client is already written and is ahead of the service). Neither is an SDK defect, and the gen2 C2D code should not be deleted as dead.
- These libraries are **unreleased** — `git tag` is empty, so there is no shipped ABI and no back-compat constraint on any of the shapes described here.
- `model_id` on the gen2/AEG path, QoS 2 usage, and message ordering remain **unverified** — the code does not settle them.
- Cross-checked against the connection-client design review held by the peer session (`connection-client-design-review.md`, last verified 2026-09-23 09:58), which is the authority for the connection/DPS/retry rows.

### Corrections from Tim (owner of `dotnet/`), 2026-09-23

- **Subscription-ack gating is Yes for .NET, both generations** — this matrix previously said No. Verified: `Unified/Connection/ConnectionClient.cs:116-133` and `Gen2/Connection/ConnectionClient.cs:61-88` both check SUBACK reason codes and reconnect on refusal. The remaining difference is policy, not capability: C lets you choose the failure scope, .NET always disconnects and reconnects.
- **Non-extractable keys and trust bundle / custom CA are in progress**, not simply absent — draft PR #236 adds SoftHSM-backed keys and a `RemoteCertificateValidationCallback`. Still No on `main`.
- **.NET fault-injection coverage is by unit test, by design** — `ConnectionFaultedUnitTests.cs`, 12 facts. The empty `MqttNetFaultInjectionIntegrationTests.cs` (41 lines, 0 test attributes) is dead code pending deletion, not a coverage gap.
- Found while re-auditing on the back of the above: **.NET does expose a DISCONNECT reason code** (`MqttDisconnect.Reason`, enum includes `DisconnectWithWillMessage`=4) — previously recorded as No. It is always sent as `NormalDisconnection`.

## 13. What changed since the 2026-09-17 revision

Rechecked at `8801f126` after ~40 merged PRs (#204–#242). Now **Yes** that were not before: C v5 user properties end-to-end (#205); C clean start / session expiry / LWT per role and the DISCONNECT reason code (#204); C custom DPS registration payload and the returned payload (#206); C connection-state observer registry, scoped state and structured error detail (#221/#224/#235); C `dps.provision_only` (#232); C gen2 twin desired-version tracking (#240); **.NET DPS CSR, which was dead code and now works** (#202). Newly **broken**: the .NET CD pipeline, since the #226 namespace migration. Renamed: the .NET package/namespace to `Microsoft.Azure.Iot.Device`.
