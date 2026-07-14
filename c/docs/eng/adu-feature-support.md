# Azure Device Update on azure-iot-sdk — Protocol Coverage & Client API

Goal: communicate which ADU **protocol and design features** the new `azure-iot-sdk` SDK plans to support, and the rationale for each include/exclude decision. This is a focused summary; it intentionally omits internal SDK architecture.

> **Companion document.** For the internal SDK architecture, public API surface,
> state machine, crypto/hook model, source layout, and implementation phases, see
> [adu-client-design.md](adu-client-design.md).

`azure-iot-sdk` is a C99 IoT device SDK. This document is split into two parts:
**ADUv1** (the current IoT Hub device-twin protocol) and **ADUv2** (the upcoming
HTTPS pull protocol). Both reuse the same `az_iot_adu_client` manifest
parsing/formatting module from `azure-sdk-for-c` and the same cryptographic
verification core — only the way a manifest reaches the device and the way the
device reports status differ.

| | **ADUv1** | **ADUv2** |
|---|---|---|
| **Channel** | IoT Hub **device twin** (MQTT) | Dedicated **HTTPS** ADU endpoint |
| **Model** | **Push** — service writes the deployment to desired properties | **Pull** — device polls the endpoint for assigned updates |
| **Auth** | Carried by the IoT Hub connection (SAS / X.509) | Customer-managed **X.509** cert on the HTTP channel |
| **Device data / registry** | Twin reported properties | **Azure Device Registry (ADR)** |
| **Coupling** | Requires IoT Hub | Decoupled from IoT Hub (Day0 "shelf" devices can still update) |
| **Manifest + signing** | Manifest v5, JWS/RS256 | **Same** (shared core) |
| **Download / install** | HTTPS download, SHA-256, multi-step, reboot/resume | **Same** (shared core) |

**How each works:**

- **ADUv1** — the service writes a deployment into the device's twin desired
  properties; the device receives it over MQTT, verifies the manifest signature,
  acknowledges accept/reject, downloads and hash-verifies files over HTTPS,
  installs/applies per step, and reports agent state + results back via twin
  reported properties.
- **ADUv2** — the device periodically polls a stable HTTPS ADU endpoint
  (authenticating with an X.509 cert) for an assigned update; on receiving a
  manifest it runs the **same** verify → download → install → report pipeline,
  but reports updating properties to **ADR** over HTTP rather than to a twin. Its
  decoupling from IoT Hub is what enables the "Day0" scenario (a device that has
  been on the shelf too long to reach its original Hub can still update).

> **Conventions.** The key words "MUST", "MUST NOT", "REQUIRED", "SHALL",
> "SHALL NOT", "SHOULD", "SHOULD NOT", "RECOMMENDED", "NOT RECOMMENDED", "MAY",
> and "OPTIONAL" in this document are to be interpreted as described in BCP 14
> ([RFC 2119](https://datatracker.ietf.org/doc/html/rfc2119),
> [RFC 8174](https://datatracker.ietf.org/doc/html/rfc8174)) when, and only when,
> they appear in all capitals, as shown here. Most of this document is
> descriptive; normative device-side requirements (e.g. §8) use the capitalized
> keywords.

---

# Part A — ADUv1 (IoT Hub Device Twin)

## 1. Protocol & Design Feature Coverage

This is a list of features of the ADU protocol and the support status in the azure-iot-sdk library.

> **Implementation status — verified against code.** Part A reflects an audit of
> the actual `c/src/features/adu/` implementation (not just intent). Status
> values mean what is *built today*:
>
> Legend: ✅ Implemented (working in core) · 🟡 Partial (built but simplified or
> divergent from the Gen1 spec — see note) · 🔜 Planned (designed, not yet built)
> · ❌ Not planned (intentionally out of scope) · ⚙️ Architectural capability
> (enabled by the hook model; no core code required)
>
> Primary evidence: [adu_client.c](../../src/features/adu/adu_client.c),
> [adu_state_reporter.c](../../src/features/adu/adu_state_reporter.c),
> [az_iot_adu.h](../../inc/azure/iot/az_iot_adu.h), and the Phase-1 unit tests in
> [adu_client_test.c](../../tests/unit/adu_client_test.c). Internal architecture,
> the state machine, and the phased plan live in [adu-client-design.md](adu-client-design.md).

### 1.1 Core update workflow

| Feature | Support | Rationale / implementation note |
|---|---|---|
| Update Manifest **v5** parsing | ✅ | Delegated to azure-sdk-for-c's `az_iot_adu_client_parse_update_manifest()`; the only version targeted. Earlier versions are intentionally unsupported. |
| Manifest versions < v5 | ❌ | Legacy; no field demand. Reduces surface and test matrix. |
| Agent state reporting (Idle / DeploymentInProgress / Failed) | ✅ | Fine-grained internal states mapped to protocol `0 / 6 / 255` in `adu_state_reporter.c` and published via twin reported properties. |
| Device properties reporting (manufacturer, model, aduVer, compat properties, installedUpdateId) | ✅ | Deep-copied into a client-owned cache at init; serialized `installedUpdateId` built once. |
| Startup + reconnect re-reporting of device properties | ✅ | Startup report flagged at init (first `do_work`), plus an initial twin GET to catch an offline-created deployment; reconnect re-report via the connection-state observer. |
| Deployment **accept / reject** acknowledgement | ✅ | Decision made in core (accept→download, reject→idle); driven by `is_installed_fn` (already-installed ⇒ 406). Response payload formatted by upstream. No app-level `accept_deployment_fn` hook yet (battery/critical-op veto). |
| **Multi-step** (composite) updates | ✅ | Sequential per-step Download→Backup→Install→Apply loop with per-step result accumulation. |
| Per-step result reporting (`resultCode` / `extendedResultCode` / `stepResults`) | ✅ | `step_results[]` indexed per manifest step; structured `extendedResultCode` (4-bit facility + raw code) for field debugging. |
| **Retry** detection (same workflow id, newer retry timestamp) | ✅ | Core copies the active deployment's `workflow.id` + `retryTimestamp` out of the request and compares incoming patches: same id + same/empty `retryTimestamp` is a redelivery (ignored, no reprocessing); same id + a newer `retryTimestamp` restarts the deployment from scratch. (`set_active_workflow`/`same_workflow_id`/`same_retry_timestamp` in `adu_client.c`.) |
| **Replacement** detection (new workflow supersedes in-flight one) | ✅ | A patch with a **different** `workflow.id` supersedes the in-flight deployment and restarts the state machine at `MANIFEST_RECEIVED`. The active identity (`workflow.id`, `retryTimestamp`, **and** a CRC-32 fingerprint of the raw `updateManifest`) is re-established on `resume()` from the persisted snapshot, so a redelivery after reboot is recognized as a duplicate (not restarted), a forced retry still restarts, and the (anomalous) same-id/same-retry-but-changed-manifest case is treated as a replacement rather than silently ignored. (`set_active_workflow`/`same_workflow_id`/`same_retry_timestamp`/`same_manifest` in `adu_client.c`.) |
| Deployment **cancellation** | ✅ | `action: Cancel` (or a replacement) sets a cooperative flag; honored at phase boundaries in `do_work`. Hooks poll `az_iot_adu_is_cancelled()` for mid-phase cooperation; core does not forcibly interrupt a hook. |

### 1.2 Download & integrity

| Feature | Support | Rationale / implementation note |
|---|---|---|
| File download over HTTP/HTTPS from manifest URLs | ✅ | Core resolves each file's URL from the request `fileUrls` map (by file id) and drives `download_fn`. The transport itself is a platform hook. |
| Chunked / streaming download | ✅ | `download_fn` may return `IN_PROGRESS` to be re-entered on the next `do_work`, keeping the loop non-blocking. |
| **SHA-256** payload hash verification (streaming) | ✅ (opt-in) | Implemented (streaming via incremental hooks, constant-time compare) **but only runs when** `read_file_fn` **and** the incremental `sha256_*` hooks are supplied; if absent, core skips it and the platform is responsible for integrity. |
| Delta / differential updates (`relatedFiles`, `downloadHandler`) | ❌ | Large added complexity; not required for first release. Full-image updates cover the primary scenarios. |
| Delivery Optimization / peer-to-peer download | ❌ | Out of scope for a constrained-device SDK; no protocol requirement on the agent. |

### 1.3 Security & trust

| Feature | Support | Rationale / implementation note |
|---|---|---|
| **JWS manifest signature** verification (RFC 7515) | ✅ | Full six-stage `verify_manifest()` runs before any download; failure → `Failed` with facility `0x1`. |
| Two-level trust chain (root key → SJWK → manifest), `kid` resolution | ✅ | SJWK verified against the resolved root key, then the manifest JWS against the SJWK signing key, then SHA-256 binding to the deployment. |
| Algorithm enforcement: **RS256 only** | ✅ | Enforced from the wire on both the SJWK and manifest headers (`alg` must be `RS256`); anything else rejected. New algorithms would be additive. |
| Compiled-in + runtime-loadable **root key** store | ✅ | Microsoft roots ship as `const` data (`az_iot_adu_microsoft_root_keys()`); callers may pass their own array at init (bounded by `AZ_IOT_ADU_MAX_ROOT_KEYS`). |
| Root key **revocation** (disable a root by `kid`) | ✅ | `resolve_root_key()` rejects any `kid` whose entry has `disabled = true`. |
| **Root Key Package** runtime rotation (fetch + verify + apply, threshold continuity) | 🔜 | Deferred / not built. v1 rotates keys via firmware update. The out-of-band package protocol (fetch, persistence, threshold signatures) warrants its own design pass. Not the ADUv2 Day0 mechanism (Day0 keeps roots fixed; see §8). |
| HSM / PKCS#11-backed verification | ⚙️ | Verification uses only public keys and the `verify_rs256_fn` primitive is a customer hook, so an HSM/PKCS#11 backend is possible. No HSM adapter ships today. |

### 1.4 Install, apply, recovery

| Feature | Support | Rationale / implementation note |
|---|---|---|
| Install / Apply execution | ✅ (core) | State machine drives `install_fn` / `apply_fn` (chunkable, may request reboot). Real platform adapters are **not** yet factored into `adapters/adu/` — only the PC simulation sample and the ESP32 OTA sample provide hook implementations. |
| Backup / Restore (rollback) | ✅ | Optional `backup_fn` before install; on failure, reverse-order best-effort `restore_fn` (continues even if one restore fails). |
| Partial-failure rollback across multi-step updates | ✅ | Mid-sequence failure rolls back applied steps in reverse. Rollback eligibility is derived from `current_step` + the persisted `state` (both in the snapshot), so a failure *after* a reboot still rolls back pre-reboot steps — provided the platform retains its per-step backups across the reboot (a platform responsibility; the SDK calls `restore_fn` for every eligible step). |
| **Reboot coordination** + **resume after reboot** | ✅ | Persist-before-reboot + `az_iot_adu_client_resume()` are implemented (magic `ADU1`, blob **v2**, CRC-32, little-endian). The v2 trailer persists the deployment `retryTimestamp`, a CRC-32 fingerprint of the `updateManifest`, and the accumulated `install_result` (overall `result_code`/`extended_result_code` plus per-step `step_results[]`), so duplicate / retry / replacement detection and per-step result accumulation all stay correct across a reboot. Resume re-enters at the persisted phase boundary (`INSTALL_COMPLETE` → start of Apply). **Limitations:** the only persist point today is the install-requested reboot (`INSTALL_COMPLETE`); there is no mid-download resume (a partially fetched file is re-downloaded), which is intentional. |
| Health check / auto-rollback after reboot | 🟡 (sample) | Not in core. Provided by the ESP32 sample (A/B partition confirm/mark-valid); core does not re-run `is_installed_fn` on resume to confirm the new image. |

### 1.5 Manifest features & extensibility explicitly not covered

| Feature | Support | Rationale / implementation note |
|---|---|---|
| **Custom step-handler extensions** (registerable `ContentHandler` plugins; `--register-extension`) | ❌ | The reference agent's dynamically-loaded, per-update-type handler model is not supported and is incompatible with this C99/no-dynamic-loading core. The manifest `handler` type string (e.g. `microsoft/swupdate:1`) **is** parsed and passed to `install_fn` / `apply_fn` via the manifest + step index, so an integrator may dispatch on it inside their own hooks — but there is no `ContentHandler` interface, no extension registry, and no dynamic loading. |
| Per-handler-type built-in handlers (`microsoft/apt`, `microsoft/script`, `microsoft/swupdate`, …) | ❌ | Core is handler-type-agnostic: one fixed hook vtable handles every step regardless of `handler`. Handler-specific behavior is the integrator's hook responsibility. |
| Reference steps (`"type": "reference"` + detached manifest file id) | ❌ | Used for proxy/nested (gateway→leaf) updates; not a target scenario for the first release. Field defined upstream but not parsed. |
| Proxy / nested updates (IoT Edge parent updating leaf devices) | ❌ | Gateway topology out of scope for the device SDK's first ADU release. |
| Component-level targeting (component enumerators, `selectedComponents`) | ❌ | Adds a component-model abstraction not needed for the primary single-image device scenario. |
| `mimeType` handling | ❌ | Not required by the supported handlers; skipped by the parser. |
| Diagnostics / log-upload interface | ❌ | Separate feature area; not part of the core update workflow. |
| `adu-shell` / privilege escalation | ❌ | The SDK does not assume root; privilege handling is the integrator's responsibility. |


---

## 2. Public Client API

The client is callback-driven and single-threaded: the application pumps it from
its main loop via `do_work()`. Platform- and crypto-specific operations are
provided as hooks, so the core links no crypto library and no OS-specific code.

```c
/* Lifecycle */
az_iot_adu_client_config_options az_iot_adu_client_config_options_default(void);

az_iot_result az_iot_adu_client_initialize(
    az_iot_adu_client_t* client,
    az_iot_twin_client* twin,
    const az_iot_adu_client_config_options* options);  /* hooks, crypto, root keys,
                                                     device props + caller cache */

void az_iot_adu_client_destroy(az_iot_adu_client_t* client);

/* Resume an interrupted workflow after a reboot (no-op if none persisted). */
az_iot_result az_iot_adu_client_resume(az_iot_adu_client_t* client);

/* Runtime — call from the application's do_work loop. Non-blocking. */
az_iot_result az_iot_adu_client_do_work(az_iot_adu_client_t* client);

/* Observe / control. */
az_iot_adu_state az_iot_adu_client_get_state(const az_iot_adu_client_t* client);
bool             az_iot_adu_is_cancelled(const az_iot_adu_client_t* client);

/* Update reported device properties (deep-copied; published on next do_work). */
az_iot_result az_iot_adu_client_update_device_properties(
    az_iot_adu_client_t* client,
    const az_iot_adu_device_properties* device_props);

/* Convenience: Microsoft's compiled-in ADU root public keys. */
const az_iot_adu_root_key* az_iot_adu_microsoft_root_keys(size_t* out_count);
```

The crypto hook surface is deliberately minimal — pure primitives only — while
the SDK core performs all JWS/SJWK parsing, `kid` resolution, `alg` enforcement,
and revocation:

```c
typedef struct {
    int32_t (*verify_rs256_fn)(const uint8_t* modulus, size_t modulus_len,
                               const uint8_t* exponent, size_t exponent_len,
                               const uint8_t* signed_data, size_t signed_data_len,
                               const uint8_t* signature, size_t signature_len,
                               void* user_ctx);
    int32_t (*sha256_fn)(const uint8_t*, size_t, uint8_t out[32], void*);
    int32_t (*sha256_init_fn)(void** ctx_out, void*);
    int32_t (*sha256_update_fn)(void* ctx, const uint8_t*, size_t, void*);
    int32_t (*sha256_final_fn)(void* ctx, uint8_t out[32], void*);
    void* user_ctx;
} az_iot_adu_crypto_hooks;
```

---

## 3. Minimal Integration Sample

```c
/* Wire connection + twin + ADU client, then pump. */
az_iot_connection_client_init(&conn, /* hub/device credentials */ ...);
az_iot_twin_client_init(&twin, &conn);

/* Crypto primitives + root keys (Microsoft defaults shown). */
az_iot_adu_crypto_hooks crypto = az_iot_adu_crypto_openssl_hooks();
size_t rk_count;
const az_iot_adu_root_key* root_keys = az_iot_adu_microsoft_root_keys(&rk_count);

/* Platform hooks: download/install/apply/backup/restore/persist for this device. */
az_iot_adu_platform_hooks hooks = my_platform_hooks();

az_iot_adu_device_properties props = {
    .manufacturer = "Contoso",
    .model        = "Thermostat-9000",
    .installed_update_id = { .provider = "Contoso", .name = "Thermostat", .version = "1.0.0" },
};

uint8_t props_cache[256];
az_iot_adu_client_config_options adu_opts = az_iot_adu_client_config_options_default();
adu_opts.hooks = &hooks;
adu_opts.crypto = &crypto;
adu_opts.root_keys = root_keys;
adu_opts.root_key_count = rk_count;
adu_opts.device_props = &props;
adu_opts.device_props_buffer = props_cache;
adu_opts.device_props_buffer_size = sizeof props_cache;
az_iot_adu_client_initialize(&adu, &twin, &adu_opts);

az_iot_connection_client_open(&conn);
az_iot_adu_client_resume(&adu);   /* continue if a prior run rebooted mid-update */

while (running) {
    az_iot_connection_client_do_work(&conn);  /* pump MQTT */
    az_iot_adu_client_do_work(&adu);          /* drive ADU state machine (non-blocking) */
    platform_sleep_ms(100);
}
```

On a desired-property deployment, the client verifies the manifest signature,
acknowledges accept/reject, downloads and hash-verifies each file, drives
install/apply per step (with rollback on failure), coordinates any required
reboot, and reports agent state and per-step results back to the service.

---

## 4. Summary

- **Implemented today (\u2705):** manifest v5 parse/format, full JWS/RS256 two-level
  trust chain with `kid` resolution and root-key revocation, agent state + device
  properties reporting (startup + reconnect), accept/reject, multi-step with
  per-step result reporting, cancellation (phase-boundary), opt-in SHA-256
  integrity, `fileUrls` resolution, and the download/install/apply/backup/restore
  pipeline wiring.
- **Partial / simplified (\ud83d\udfe1):**
  - *Retry detection* \u2014 no `retry_timestamp` comparison; same-id redelivery reruns. (Now implemented; see Part A table.)
  - *Replacement detection* \u2014 supersedes in-flight via state reset, but no
    `workflow.id` / `manifest_sha256` compare. (Now implemented; see Part A table.)
  - *Reboot/resume* \u2014 works, but the persisted blob (magic `ADU1`) diverges from
    the design: the **v2** blob now persists `step_results[]`, `retryTimestamp`,
    and a manifest CRC fingerprint (see the Part A reboot/resume row). Eligibility
    for post-reboot rollback derives from the persisted `current_step` + `state`.
  - *Health-check / auto-rollback after reboot* \u2014 sample-only (ESP32 A/B), not core.
  - *Platform/crypto adapters* \u2014 only `adapters/adu/crypto_openssl/` is factored
    out; install/apply/download adapters live in the PC and ESP32 *samples*, not
    `adapters/`. mbedTLS exists inline in the ESP32 sample only.
- **Deferred / planned (\ud83d\udd1c):** Root Key Package runtime rotation
  (firmware-delivered key updates used instead initially).
- **Not planned (\u274c):** custom `ContentHandler` step-handler extensions / dynamic
  handler loading, per-handler-type built-in handlers, delta updates, delivery
  optimization, reference/proxy/nested updates, component targeting, `mimeType`,
  diagnostics/log upload, `adu-shell`.
- **Extensibility note:** the manifest `handler` type string is parsed and passed
  to `install_fn` / `apply_fn`; integrators dispatch on it inside their own hooks
  (static, in-process) \u2014 there is no plugin/registry/dynamic-loading model.
- **Test coverage:** Phase-1 cmocka unit tests in `tests/unit/adu_client_test.c`;
  no conformance or e2e tests yet.
- **Ask:** visibility into upcoming manifest schema changes so we can validate
  forward-compatibility.

---

# Part B — ADUv2 (HTTPS Pull Data-Plane Protocol)

> **Basis.** Part B is a **client implementation guide** for the ADU device
> data-plane protocol defined in *"ADU Device Data Plane Protocol"* (api-version
> `2026-11-02-preview`; owner: ADU protocol/API team, Darko Aleksic; integration
> contact: Leo). It describes what the protocol requires and how this SDK's
> client implements it. The manifest content (v5, JWS/RS256, SHA-256, fileUrls)
> is the same as ADUv1 and is handled by the shared core (§7).

ADUv2 keeps the same update *content* (manifest v5, JWS/RS256 signing, SHA-256
integrity, multi-step install/apply) but defines a device-initiated
**RPC-over-HTTPS** delivery model (every operation is an HTTP `POST` that names an
operation, not a REST resource), decoupled from IoT Hub, authenticated with
**mutual TLS** using a customer X.509 certificate. The agent drives three
operations — `syncConfiguration`, `requestUpdates`, `reportStatus` — and reports
the terminal result using a string `outcome` / `failureOrigin` structure (§5a).
Updating/device properties are persisted by the service in **Azure Device
Registry (ADR)**, but the device never talks to ADR directly — it only calls the
three ADU data-plane operations.

## 5. What the ADUv2 Client Must Handle

The table lists each protocol area, what the protocol defines, and how the client
implements it. The ADUv1 column is included only as orientation for readers of
Part A; the subject of Part B is the ADUv2 protocol and its client.

| Area | ADUv1 (Part A) | ADUv2 (this protocol) | Client implementation |
|---|---|---|---|
| **Delivery model** | Service **pushes** a deployment to twin desired properties | Device **pulls** via `POST /requestUpdates` at its own cadence | Poll scheduler drives `requestUpdates`; the verify/download/install pipeline is unchanged |
| **Status / property reporting** | Twin **reported properties** (MQTT), integer agent state `0/6/255` | `POST /reportStatus`, **terminal outcome only**: string `outcome` (`SUCCEEDED`/`FAILED`/`CANCELED`/`SKIPPED`), required `failureOrigin` enum, `extendedResultCodes` as a **comma-separated hex string**, `stepResults` as a **map** (`step_0`, `step_1`, …) | Client serializes the `reportStatus` payload and reports once, on terminal outcome (§5a) |
| **Config / registration handshake** | Implicit via twin | `POST /syncConfiguration` registers `agentInfo` (`agentProfile`, `compatibilityProperties`, `agentSdkVersion`) and returns `serviceConfiguration.rootKeyDownloadUrl` + `serviceConfigETag`; ETag-driven re-sync | Client performs the `syncConfiguration` handshake and caches `rootKeyDownloadUrl` + `serviceConfigETag` (§5a) |
| **Trust material** | Compiled-in root keys | Root key package **downloaded** from `serviceConfiguration.rootKeyDownloadUrl`; signature still JWS/RS256 | Client downloads and caches the root key package from `rootKeyDownloadUrl`, then verifies signatures as in Part A |
| **Authentication** | Carried by the IoT Hub connection (SAS / X.509) | **mTLS** (TLS ≥ 1.2) with a customer-managed onboarding X.509 cert; headers `x-ms-device-id` (ADR UUID), `x-ms-correlation-id`, `User-Agent` | HTTPS+mTLS client (platform hook) plus header/identity plumbing |
| **Connection dependency** | Requires an open IoT Hub MQTT connection + `az_iot_twin_client` | No Hub connection; ADU endpoint only | `az_iot_connection_client` / `az_iot_twin_client` are not on the ADUv2 path |
| **Cancellation & retry** | Push-based cancel + service-initiated retry via twin | **None on the wire.** No agent-initiated retry, no customer-visible cancel, no rollback, no intermediate progress. The service **re-offers the same `workflowId`** until a terminal outcome is reported | Client reports a terminal outcome then returns to `IDLE`; the service re-offers `workflowId` to retry. Agent-local download backoff is invisible to the wire |
| **Versioning** | n/a | `?api-version=` on every call; on `UNSUPPORTED_API_VERSION` fall back to the **eternal api-version** baked into the SDK; api-version is **per-workflow** (persist + reuse for `reportStatus`) | Client negotiates and persists the api-version per workflow (§5a) |
| **Endpoint discovery** | Implicit (Hub/twin) | DPS provides **two** base URLs (**current** + **legacy/LTS**) and the device UUID; fall back to legacy only on **transport-layer** failure | Client takes both endpoints as config and falls back on transport failure (selection rule open: §9) |
| **Load contracts** | n/a | Agent MUST NOT poll while installing; honor account-scoped **429 / `Retry-After`**; add **cold-start jitter** before first poll | Client enforces these behavior contracts (§5a) |
| **Day0 / stale device** | n/a (always via Hub) | Device too old to reach DPS/Hub/ADU recovers via a separate recovery path; trust still rests on the signed manifest + provisioned root key | Reuses the manifest signing / root-key model; see §8 |

**Unchanged (shared core):** manifest v5 parse/format, JWS/SJWK parsing, `kid`
resolution, RS256 enforcement, SHA-256 integrity, and the
download/install/apply/backup/restore + reboot/resume state machine. The manifest
is delivered as a **serialized JSON string** in `updateMetadata.updateManifest`
and MUST be verified against `updateMetadata.updateManifestSignature` before
parsing.

## 5a. ADUv2 Wire Contract — What the Client Implements

This section describes the ADUv2 wire contract and how the client implements each
operation.

### Three operations (RPC over HTTPS, all `POST`)

| Operation | Purpose | Key request fields | Key response |
|---|---|---|---|
| `syncConfiguration` | Register agent identity + fetch service config | `agentInfoETag`; full `agentInfo` block on first call / on `UNKNOWN_AGENT_INFO_VERSION` (`agentSdkVersion`, `agentProfile`, `compatibilityProperties` 1–5 entries) | `serviceConfiguration.rootKeyDownloadUrl`, `serviceConfigETag` |
| `requestUpdates` | Poll for an applicable update | `agentInfoETag`, `serviceConfigETag`, `installedUpdateId` (`null` if none) | `200` empty = no update; `200 { updateMetadata: { workflowId, updateManifest, updateManifestSignature, fileUrls } }` |
| `reportStatus` | Report **terminal** outcome (once per attempt) | `workflowId`, `installedUpdateId`, `lastInstallResult` (see below) | `200 {}` |

### New `reportStatus` payload (replaces the Gen1 reported-properties shape)

```jsonc
{
  "workflowId": "<echoed from requestUpdates>",
  "installedUpdateId": { "provider": "...", "name": "...", "version": "..." }, // or null
  "lastInstallResult": {
    "outcome": "SUCCEEDED|FAILED|CANCELED|SKIPPED",   // authoritative (string enum)
    "failureOrigin": "NOT_APPLICABLE|ADU_CLOUD_SERVICE|ADU_MANAGED_RESOURCE|AGENT_CORE|AGENT_EXTENSION|AGENT_DEPENDENCY|DEVICE|OTHER",
    "resultCode": 700,                                 // legacy int64: 0=fail, -1=cancel, >0=success
    "extendedResultCodes": "3000001C,80004005",        // comma-separated unsigned hex int32 list (string!)
    "resultDetails": "",                               // optional, diagnostics only
    "stepResults": { "step_0": { /* same fields */ } } // map keyed step_0, step_1, ...
  }
}
```

**How the client populates the report from its internal result model:**

| Internal result model | ADUv2 wire field | Action |
|---|---|---|
| internal agent state | `lastInstallResult.outcome` (string enum) | Map terminal state → `SUCCEEDED`/`FAILED`/`CANCELED`; emit `SKIPPED` for a compat-skipped step |
| failure classification | `failureOrigin` (required) | Classify each `FAILED` outcome; `NOT_APPLICABLE` on non-failures; `OTHER` when unclassifiable |
| `result_code` (`700`/`0`/`<700`) | `resultCode` (int64) | Populate; advisory only (the service uses `outcome` for state) |
| `extended_result_code` (single int32, facility nibble + 28-bit) | `extendedResultCodes` (string) | Emit the 32-bit value as the **first** hex token of a comma-separated list |
| per-step result, indexed by step | `stepResults["step_N"]` (map) | Key each step result as `step_<N>` |
| `workflowId`, `installedUpdateId` | `lastInstallResult` envelope fields | Echo `workflowId` from the offer; report the currently-installed `installedUpdateId` |

> The client's facility/sub-code layout (Part A §1.1 per-step results /
> [adu-client-design.md](adu-client-design.md) “Result-Code Mapping”) is carried
> **inside** one `extendedResultCodes` token; the wire container is a
> comma-separated hex-string list.

### Config / version / identity plumbing (new)

- **ETags:** cache `agentInfoETag` (agent-managed, runtime-generated, in writable
  storage — MUST NOT be baked into the image) and `serviceConfigETag` (service-
  managed); echo both on `requestUpdates`. Re-call `syncConfiguration` on
  `UNKNOWN_AGENT_INFO_VERSION` (full block) or `OUTDATED_SERVICE_CONFIG` (minimal).
- **`agentProfile`:** opaque integer assigned offline; selects the manifest
  versions the service may offer. The agent never infers meaning from it.
- **`compatibilityProperties`:** 1–5 entries, lowercased by the service; duplicate
  case-variant keys are rejected (`INVALID_COMPATIBILITY_PROPERTIES`).
- **api-version:** send `?api-version=` on every call; on `UNSUPPORTED_API_VERSION`
  retry with the eternal version; **persist the api-version with the durable report
  payload** and reuse it for `reportStatus` after a reboot (spec open-Q #7).
- **Headers:** `x-ms-device-id` (ADR UUID when available, else omitted),
  `x-ms-correlation-id` (UUID, required), `User-Agent` (e.g. `adu-agent/2.0.0`).

### Service error codes that drive agent control flow

`UNKNOWN_AGENT_INFO_VERSION`, `OUTDATED_SERVICE_CONFIG`, `UNSUPPORTED_API_VERSION`,
`UNSUPPORTED_AGENT_PROFILE`, `INVALID_COMPATIBILITY_PROPERTIES`,
`MAX_DEVICE_CLASSES_EXCEEDED`, `UPDATE_ACCOUNT_NOT_LINKED`, `INVALID_REQUEST`,
`INTERNAL_SERVER_ERROR`, `UPSTREAM_UNAVAILABLE`. The agent reads `error.code`
(never parses `error.message`) and maps it to the re-sync / fallback / give-up
actions in the protocol's `requestUpdates` / `syncConfiguration` response tables.

### Agent behavior contracts (mandatory)

- **No polling while installing** — only `reportStatus` is allowed in
  `CONTENT_DOWNLOADING` / `INSTALLING` / `REBOOTING`; resume `requestUpdates` only
  after returning to `IDLE`.
- **Honor 429 / `Retry-After`** (account-scoped rate cap) and back off on 500;
  respect `Retry-After` on 503 or defer to the next poll.
- **Cold-start jitter** — add a uniform random delay before the **first**
  `requestUpdates` after boot/process-start/network-restore (anti-thundering-herd).

### Recovery / durability (refines Part A reboot-resume)

- Persist the pending `reportStatus` payload in **durable** storage; re-send on the
  next `requestUpdates` when the service re-offers the same `workflowId`.
- Storage-less devices: stamp `installedUpdateId` into the image so the service can
  perform **implicit success resolution** in `requestUpdates` (close the deployment
  when the reported `installedUpdateId` matches the pending deployment's identity).

## 6. Design Approaches

ADUv1 binds the update engine directly to `az_iot_twin_client`. ADUv2 needs a
different transport, so the engine must be decoupled from "where the manifest
came from." Three options below. Approach 2 shares the least code (only the
manifest + crypto modules, two separate state machines); Approaches 1 and 3 both
share a full engine including the state machine, with Approach 3 sharing the most.

### Approach 1 — Transport-agnostic client + injected manifest/transport provider

Generalize `az_iot_adu_client` so it no longer takes an `az_iot_twin_client*`
directly. Instead it takes a small **transport** vtable that (a) supplies the
next manifest and (b) accepts status/property reports. Two providers ship: a
twin-backed one (v1) and an HTTPS data-plane one (v2).

```c
typedef struct {
    /* Pull/receive the next assigned manifest, if any. Non-blocking. */
    az_iot_result (*get_manifest_fn)(void* ctx, az_span* out_manifest, bool* out_available);
    /* Deliver the terminal report as STRUCTURED data; the provider serializes it
     * (v1: twin reported properties; v2: the reportStatus payload, §5a). */
    az_iot_result (*report_fn)(void* ctx, const az_iot_adu_report* report);
    void* ctx;
} az_iot_adu_transport;

/* v1: twin-backed transport (push arrives as desired-property deltas) */
az_iot_adu_transport t = az_iot_adu_transport_twin(&twin);

/* v2: HTTPS data-plane transport (requestUpdates poll + reportStatus, mTLS X.509) */
az_iot_adu_transport t = az_iot_adu_transport_http(&adu_http /* endpoint, X.509, poll cfg */);

/* The transport vtable replaces the twin argument; hooks/crypto/keys/props are
 * carried by az_iot_adu_client_config_options as in the shipping API. */
az_iot_adu_client_initialize(&adu, &t, &options);
```

- **Pros:** one engine, one test surface for verify/download/install; v1 and v2
  differ only in a thin transport.
- **Cons:** the transport interface must generalize both push (twin deltas) and
  pull (polling) plus two different report channels — a slightly leaky abstraction.

### Approach 2 — Dedicated `az_adu_client` for ADUv2 (HTTP-only)

Keep `az_iot_adu_client` (twin) as the v1 client and add a separate
`az_adu_client` purpose-built for the HTTPS data-plane protocol. Both call into the
shared manifest + crypto modules.

```c
az_adu_client_init(&adu2,
    &adu_endpoint,   /* HTTPS URL + protocol version */
    &adu_x509,       /* onboarding / operational cert */
    &poll_cfg,       /* polling interval */
    &hooks, &crypto, root_keys, rk_count, &props, buf, sizeof buf);

while (running) {
    az_adu_client_do_work(&adu2);   /* poll → verify → download → install → report */
    platform_sleep_ms(poll_cfg.idle_ms);
}
```

- **Pros:** each client stays simple and idiomatic for its protocol; no awkward
  shared transport abstraction; ADUv1 is left untouched.
- **Cons:** two client lifecycles/state machines to maintain — risk of behavioral
  drift unless the state machine itself is factored into a shared core (Approach 3).

### Approach 3 — Shared ADU engine + two thin clients

Factor the protocol-independent logic — manifest verification, download/integrity,
install/apply/backup/restore, and the reboot/resume **state machine** — into a
shared **ADU engine**. `az_iot_adu_client` (twin) and `az_adu_client` (HTTPS data
plane) become thin transport + lifecycle wrappers over that engine.

```c
/* Shared, transport-free engine. */
az_iot_adu_engine_init(&engine, &hooks, &crypto,
                       root_keys, rk_count, &props, buf, sizeof buf);

az_iot_adu_engine_submit_manifest(&engine, manifest_span); /* whoever fetched it */
az_iot_adu_engine_do_work(&engine);                        /* verify → download → install */
az_iot_adu_engine_collect_report(&engine, &report_span);   /* transport sends it onward */
```

The v1 wrapper feeds manifests from twin desired properties and sends reports to
twin reported properties; the v2 wrapper polls `requestUpdates` and serializes the
engine's report into the new `reportStatus` payload (§5a). This combines
Approach 1's clean, single engine with Approach 2's protocol-idiomatic edges: it
maximizes shared, well-tested code while letting each transport stay natural.

> **Report-format note:** because the ADUv2 `reportStatus` structure differs from
> the Gen1 reported-properties shape (string `outcome`/`failureOrigin`,
> string-list `extendedResultCodes`, `stepResults` map), the engine should expose
> the report as **structured data** (outcome, per-step results, ERC list) and let
> each wrapper serialize it — not emit a fixed JSON string. Otherwise the v2
> wrapper has to re-parse and rewrite the v1 JSON.

- **Pros:** maximal shared/tested code (one engine, one state machine) while each
  client stays protocol-idiomatic; no leaky combined-transport abstraction.
- **Cons:** more up-front refactoring to extract the engine, and a third public
  surface (the engine) to design and version.

## 7. Shared Core (both clients)

Regardless of approach, the following is identical for ADUv1 and ADUv2:

- **Manifest v5 parse/format** — the `az_iot_adu_client` module from
  `azure-sdk-for-c`, unchanged. The manifest arrives as a serialized JSON string
  and is verified before parsing in both generations.
- **Cryptographic verification** — JWS/SJWK parsing, `kid` resolution, **RS256**
  enforcement, and **SHA-256** integrity, via the same `verify_rs256_fn` /
  `sha256_*` hooks (§2).
- **Update state machine** — download / install / apply / backup / restore +
  reboot / resume; protocol-independent and reused verbatim.

**What differs between v1 and v2** (so it does *not* belong in the shared engine):

- **Manifest acquisition** — twin desired-property delta (v1) vs. `requestUpdates`
  poll (v2).
- **Report delivery *and shape*** — twin reported properties with integer agent
  state (v1) vs. the `reportStatus` envelope with string `outcome`/`failureOrigin`
  and a `stepResults` map (v2). This is a content delta, not just a transport one
  (§5a).
- **Trust-material acquisition** — compiled-in root keys (v1) vs. a root key
  package downloaded from `rootKeyDownloadUrl` (v2). The *verification* is shared;
  only where the keys come from differs.
- **Config/version/identity** — ETags, `agentProfile`, api-version negotiation,
  mTLS headers, and the agent behavior contracts are v2-only (§5a).

## 8. Day0 Recovery (device-side)

The ADU "Day0" recovery path (a device that can no longer reach DPS/Hub/ADU,
e.g. an outdated TLS stack) is served by a separate **unauthenticated, plain
HTTP** recovery endpoint (deliberately **no TLS** — the service trades transport
authentication for durability). Importantly for this SDK, the device-side trust
model is the one we **already implement**, so Day0 needs no new crypto:

- **Trust = existing manifest signing.** Recovery manifests reuse the Gen1
  design — signed with a Microsoft-managed key, verified on-device against the
  public root key provisioned at manufacturing. This is exactly the JWS/RS256 +
  root-key store of the shared core (§7). Because the channel is unauthenticated,
  manifest signature verification is the *only* authenticity guarantee — so it
  MUST NOT be skipped on the recovery path.
- **Account-ID binding (replay protection).** The signed manifest includes the
  **ADU account ID**; the device MUST validate it against an account ID stamped
  at manufacturing to reject cross-tenant replayed responses.
- **Compatibility-property validation.** Because a response could be replayed,
  the device MUST validate the manifest's compatibility properties against its
  own before applying.
- **Optional payload confidentiality.** The recovery payload is public;
  customers who need confidentiality encrypt the payload before import. Any such
  decryption is a platform-hook concern, not core.

Net effect on the SDK: Day0 is an additional **transport + two validation
checks** on top of the shared engine; it does not change the crypto core or the
update state machine.

> **Status in the data-plane protocol.** The data-plane protocol does not yet
> define the Day0 recovery wire contract; it references the concept indirectly
> (the “day-zero / `discoveredDevices` flow” for a device not yet in ADR). Note
> that the protocol's **legacy/LTS endpoint** is a *different* fallback: it is
> still **mTLS** and serves the eternal api-version for devices with dropped
> TLS/cipher support, whereas Day0 is an unauthenticated recovery path. The
> client must keep the two distinct. The Day0 details above (unauthenticated
> channel, account-ID binding) are provisional until the protocol covers them —
> confirm with the protocol owners before implementing.

## 9. Open Questions for the Service Team

Items the data-plane protocol leaves open and the client needs settled to
implement the ADUv2 path (tracking the protocol's own §14 where applicable):

- **Authentication lifecycle** (§14.1): how onboarding vs operational X.509 certs
  are presented and renewed/rotated before expiry — whether the client needs a
  cert-renewal state or the platform hook owns the cert store entirely. The header
  identity (`x-ms-device-id` from ADR via DPS) is defined; cert rotation is not.
- **Legacy-endpoint selection** (§14.8): distinguishing a permanent TLS/cipher
  incompatibility from a transient network failure on minimal TLS stacks
  (mbedTLS/wolfSSL surface both as a generic handshake failure). Determines whether
  the client needs a retry-threshold heuristic and endpoint persistence.
- **api-version per workflow** (§14.7): confirm the client must persist the
  api-version used for `requestUpdates` and reuse it for `reportStatus` across a
  reboot (the approach in §5a), and whether future required `reportStatus` fields
  are constrained by the pre-reboot capture problem.
- **`agentInfoETag` lifecycle** (§14.3): what triggers an agent-side ETag change
  and whether a canonical format is recommended — drives the client's
  ETag-generation logic.
- **Cold-start jitter / rate-cap values** (§14.9, §14.11, §14.12): the concrete
  account poll cap and whether jitter/cap hints move into `serviceConfiguration`
  (so the client can self-pace) or stay client-side defaults.
- **Day0 recovery wire contract**: not yet defined in the data-plane protocol (see
  §8). Confirm the channel, trust binding (account-ID), and its relationship to
  the `discoveredDevices` flow before the client implements a recovery transport.

## 10. Convergence Assessment — Does the Current Design Support Both?

**Short answer: the *content* core is already shared-ready; the *current client
is not yet decoupled*, and the state machine has gaps that must not be frozen
into a shared boundary.** Two facts from the Gen1 implementation audit (Part A)
drive the decision:

1. **The engine is still bound to the transport.** `az_iot_adu_client_initialize`
   takes an `az_iot_twin_client*` directly, and `do_work` reads desired
   properties from the twin and writes reported properties to the twin inline.
   The verify/download/install/backup/restore logic is *conceptually*
   transport-free but is **not** separated behind an interface today. Any of the
   three approaches requires that extraction first — it is unavoidable refactor
   work, not a differentiator between options.
2. **The reusable core is now largely complete.** Resume re-entry, `step_results[]`
   persistence, retry/replacement/duplicate detection (including a manifest
   fingerprint), and across-reboot identity are implemented and unit-tested —
   exactly the logic that becomes the shared engine. Extracting the engine now
   carries those behaviors into both the Gen1 and Gen2 clients rather than baking
   in gaps. **Remaining Part A items** are sample-only (health-check/auto-rollback)
   and adapter factoring.

### Mapping the user's candidates onto §6

| You asked about… | …which is | Public surface |
|---|---|---|
| `adu_update_provider_t` interface implemented by both the twin client and a future HTTPS client | **Approach 1** (`az_iot_adu_transport` vtable) | One client type + one injected vtable |
| Shared engine that takes the manifest JSON directly | **Approach 3** (`engine_submit_manifest` + `engine_collect_report` returning **structured** data) | One engine + two thin clients |
| Completely separate Gen2 client | **Approach 2** (`az_adu_client`) | Two independent clients |

### Decision matrix

| Axis | Approach 1 — provider vtable | Approach 2 — separate client | Approach 3 — shared engine |
|---|---|---|---|
| Shared/tested code | High (one engine) | **Low** (only manifest+crypto modules) | **Highest** (engine + state machine) |
| Risk of behavioral drift | Low | **High** (two state machines) | Lowest |
| Abstraction cleanliness | **Leaky** — one vtable must model *push* (twin deltas, who-calls-whom inverted) **and** *pull* (device-driven poll) plus two report channels | Clean per client | Clean — `submit`/`collect` makes no push/pull assumption |
| Coupling to the (open) v2 wire contract (§9) | Medium — vtable shape may shift as remaining items (cert rotation, legacy-endpoint rule) land | Isolated to v2 client | **Lowest** — engine takes a manifest string and returns a *structured* report; the v2 wrapper owns serialization to `reportStatus` |
| v1-only embedded build (no HTTP) | Links engine + twin provider | Links v1 client only | Links engine + twin wrapper (HTTP wrapper excluded) |
| Up-front refactor cost | Medium | **Lowest** (leave v1 alone) | **Highest** (extract + version a 3rd public surface) |
| New public API to design/version | 1 (vtable) | 1 (v2 client) | 2 (engine + v2 client) |

### Recommendation (you decide)

- **Primary: Approach 3 (shared engine, manifest-in / structured-report-out).**
  It maximizes reuse of the one thing that is genuinely hard and security-
  sensitive (the verify→install→resume state machine). The engine takes a manifest
  string and emits a **structured** result (outcome, per-step results, ERC list);
  each wrapper serializes that to its own wire shape — twin reported properties
  (v1) or the `reportStatus` payload (v2, §5a). This is important now that we know
  the v2 **report structure differs** from Gen1: a JSON-string boundary would force
  the v2 wrapper to re-parse and rewrite. The cost is the extra engine surface, but
  that surface is exactly the asset we want to harden and conformance-test once.
- **Fallback: Approach 1 (`adu_update_provider_t`)** if a single public client
  type is strongly preferred over an engine + wrappers. Functionally close to
  Approach 3, but the one vtable has to straddle push and pull — acceptable if we
  keep the vtable to `get_manifest` / `report` and let each provider own its own
  control flow internally.
- **Avoid Approach 2 as the end state.** Two hand-maintained state machines for
  the same security-critical workflow is the highest drift risk; only choose it
  if v1 must remain 100% untouched and v2 is explicitly throwaway/experimental.

**Sequencing:** (1) close the Part A 🟡 state-machine gaps; (2) extract the
transport-free engine behind a manifest-in / structured-report-out boundary
(Approach 3) — this is a pure refactor with the existing twin client as its first
consumer and the Phase-1 unit tests as the regression net; (3) add the HTTPS
data-plane wrapper (`syncConfiguration` / `requestUpdates` / `reportStatus`,
mTLS, ETags, api-version) once the §9 residual items are settled. Approaches 1
and 3 converge at step 2; the choice between them is whether step 3 adds a
*provider impl* (1) or a *thin client* (3).

---

# Part C — Using azure-iot-sdk as an ADU Agent Core Library

## 11. Goal

Beyond shipping a turnkey ADU client, this SDK should be usable as the **vetted
core** on top of which others build full ADU **agents** — the role the reference
agent [Azure/iot-hub-device-update](https://github.com/Azure/iot-hub-device-update)
fills today. That agent is a large system (communication managers, a workflow
orchestrator, content-handler extensions, component enumerators, download
handlers, diagnostics, `adu-shell`). An agent author does **not** want to
re-implement the hard, security-critical parts — manifest validation, the
JWS/RS256 trust chain, SHA-256 integrity, and result formatting — but **does**
want freedom over their own state machine, transport, threading, and extension
model.

The requirement: provide **a way to validate + parse a manifest**, then let the
consumer choose one of two modes:

- **Library mode (bring your own state machine).** The SDK hands back a
  **filled, already-verified** manifest struct; the consumer drives
  download/install/apply/report on their own.
- **Managed mode (use our state machine).** The existing `az_iot_adu_client` +
  hooks + `do_work()` orchestration owns the workflow; the consumer supplies
  platform/crypto hooks only.

Both modes MUST share **one** verified implementation of parse + trust + report
so there is no second, divergent copy of the security-critical code.

## 12. What Already Exists vs. What's Needed

| Capability | Status today | Needed for agent-core use |
|---|---|---|
| Parse a manifest into a filled struct (`az_iot_adu_client_update_manifest`: `updateId`, `compatibility[]`, `instructions.steps[]`, `files{}`) | ✅ via the upstream parser — but only **inside** our state machine | Expose a **public** parse entrypoint that returns the struct |
| JWS/RS256 + root-key + `alg`/`kid` trust verification | ✅ implemented in core (`verify_manifest`) | Make it a **public, fail-closed** step of the parse entrypoint (struct returned only after trust passes) |
| SHA-256 payload integrity | ✅ (opt-in, streaming) | Expose as a **standalone helper** callable outside the state machine |
| Result/agent-state report formatting | ✅ but internal (twin reported properties) | Expose a **report builder** that emits the structured result (and, per generation, the twin payload or the `reportStatus` body) |
| Twin coupling | `az_iot_adu_client_initialize` requires `az_iot_twin_client*` | Library mode MUST work with **no transport** dependency (same decoupling as §6/§10) |

The good news: the **"filled struct" the consumer needs already exists** — it is
the upstream `az_iot_adu_client_update_manifest`. The work is to expose a public
function that produces it *after* our trust verification, plus a report builder,
both free of the twin/state-machine.

## 13. Proposed Public Surface (two modes over one core)

Concrete prototypes live in [adu-client-design.md](adu-client-design.md) §5; the
shape is:

**Library mode — validate + parse, hand back the struct:**

```c
/* Verify (JWS/RS256 + root key + alg/kid) THEN parse. Fail-closed: out_*
 * are populated only on AZ_IOT_OK. The manifest is unescaped in place, so spans
 * in out_manifest point into `request_json`, which the caller owns and MUST keep
 * alive (and stable) while using out_manifest. No heap, no transport, no twin. */
az_iot_result az_iot_adu_parse_update_request(
    az_span request_json,
    const az_iot_adu_crypto_hooks* crypto,
    const az_iot_adu_root_key* root_keys, size_t root_key_count,
    az_iot_adu_client_update_request*  out_request,
    az_iot_adu_client_update_manifest* out_manifest);

/* Per-file SHA-256 check the consumer calls during their own download loop
 * (payload bytes are not present at parse time). */
az_iot_result az_iot_adu_verify_file_hash(
    const az_iot_adu_client_update_manifest_file* file,
    const az_iot_adu_crypto_hooks* crypto,
    int32_t (*read_chunk)(size_t offset, uint8_t* buf, size_t cap, size_t* out_read, void* ctx),
    void* read_ctx);

/* Build the report from the consumer's own outcome data. Emits structured
 * result; the gen-specific serializer turns it into the twin payload (v1) or
 * the reportStatus body (v2, Part B §5a). */
az_iot_result az_iot_adu_build_report(
    const az_iot_adu_device_properties* device_props,
    const az_iot_adu_client_install_result* result,
    const az_iot_adu_client_update_request* request,
    az_iot_adu_state state,
    uint8_t* out_json, size_t out_size, size_t* out_len);
```

**Managed mode — unchanged:** `az_iot_adu_client_initialize(...)` + hooks +
`az_iot_adu_client_do_work()`. After the §10 refactor this wrapper is implemented
**in terms of** the library-mode primitives above, so both paths share the same
verified parse/trust/report code.

## 14. Gap Analysis — What Must Be Done

> **Status (items 1–6): implemented.** The library-mode primitives ship in
> `az_iot_adu.h` and `src/features/adu/` (`az_iot_adu_parse_update_request`,
> `az_iot_adu_verify_file_hash`, `az_iot_adu_build_report`), and managed mode is
> refactored onto the shared cores (`verify_manifest_core`,
> `verify_file_hash_core`, `parse_service_request`). Items 7+ remain open.

1. **Expose parse + trust publicly (fail-closed).** Lift `verify_manifest` and
   the upstream parse call out of `do_work` into `az_iot_adu_parse_update_request`.
   The struct MUST NOT be returned if any trust stage fails.
2. **Decouple from the twin** (same as §6/§10). Library-mode functions take spans
   and structs only; no `az_iot_twin_client`, no network.
3. **Expose the SHA-256 helper** (`az_iot_adu_verify_file_hash`) so a BYO state
   machine gets the same integrity check without the managed download loop.
4. **Expose the report builder** decoupled from twin reported-properties, emitting
   the structured result so either generation's serializer can consume it.
5. **Refactor managed mode onto the primitives** (Approach 3 engine) so there is a
   single implementation, not two.
6. **Ownership/lifetime contract for library mode:** the manifest is unescaped in
   place, so spans point into the caller's `request_json` buffer; no hidden
   allocation; that buffer must outlive `out_manifest` and stay stable. (Same
   model the managed client already uses internally.)
7. **Stable handed-back struct.** Because the returned type is the upstream
   `az_iot_adu_client_update_manifest`, pin the upstream version and document the
   guaranteed fields. Unknown/forward step types MUST NOT fail the parse (the
   forward-compat item already tracked in [adu-client-design.md](adu-client-design.md) §14).
8. **Reference steps / mini-manifest (nested updates).** For agents that support
   proxy/nested updates, the parse API should surface `reference` steps (detached
   child-manifest file ids) so the agent can fetch and **recursively** parse+verify
   the child. Currently ❌ (Part A §1.5); needed only for that scenario.

## 15. Extension Model — Core-Lib vs. the Reference Agent

The reference agent's extensibility is **dynamic** (registerable, dynamically
loaded extensions under `src/extensions/`). This SDK's core is **static C99 with
no dynamic loading**, so the equivalent capabilities are the **consumer's**
responsibility, composed at build time. What core provides vs. what the agent
author owns:

| Reference-agent extension point | In azure-iot-sdk core | Agent author's responsibility |
|---|---|---|
| Step / content handlers (`microsoft/swupdate`, `apt`, `script`, …) | ❌ not dispatched | Switch on the manifest `handler` string inside their install/apply logic (Part A §1.5) |
| `update_manifest_handlers` (manifest-type dispatch) | ❌ | Consumer, in their state machine |
| Component enumerators (multi-component targeting) | ❌ | Consumer |
| Download handlers (delta / `relatedFiles`) | ❌ | Consumer |
| Content downloaders (transport) | ⚙️ via `download_fn` hook | Consumer provides the transport |
| Extension manager / dynamic loading | ❌ by design | N/A — compose handlers at build time |
| Diagnostics / log upload, `adu-shell` (privilege sep.) | ❌ out of core | Consumer / platform |
| Manifest parse + JWS/RS256 trust + SHA-256 + result format | ✅ | **Provided by core** (the whole point) |

**Net:** to make azure-iot-sdk a true ADU agent core library, the required work
is items §14.1–§14.4 (expose the four primitives, fail-closed and twin-free) on
top of the §10 engine extraction. Items §14.7–§14.8 and the extension points in
§15 are scenario-dependent and remain the agent author's domain by design.
