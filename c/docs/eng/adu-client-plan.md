# Azure Device Update (ADU) Client — Implementation Plan, Status & Feature Manual

**One-stop doc for the ADU client in `azure-iot-sdk` (C99).** It is the source for
(a) **status** to share with other teams, (b) the **implementation / testing /
manual-action queue** with cost estimates, and (c) a **feature manual** with the
design detail and caveats for each capability.

> **Supersedes `adu-feature-support.md`.** This doc replaces the old
> feature-coverage matrix and folds in the ADUv2 client action plan.
> Deep internal architecture (public API surface, hook/crypto model, source
> layout, phase plan) still lives in
> [adu-client-design.md](adu-client-design.md) — cross-linked below.

## Scope and philosophy (the ADU reference implementation)

The ADU clients have a **different goal from the rest of this library**. Beyond being
usable on-device, this API is intended to become the **canonical reference implementation
of the ADU device protocol** — the source others follow to implement ADU on *non-embedded*
platforms too.

- **Completeness is mandatory.** *Every* ADU protocol feature MUST be implemented and
  supported here. There are **no "won't implement" features** — a capability is either
  ✅ done, 🟡 partial, or 🔜 coming soon.
- **Embedded usability is a hard requirement, not a filter.** A customer MUST be able to run
  these clients in a constrained/embedded application. But **"it doesn't fit embedded" is
  never a reason to omit a feature.** Where a capability is heavy or awkward for constrained
  devices, that is **noted as a caveat** and the feature is delivered as an **optional /
  opt-in module or provider** (default-off, with a portable fallback) — it is still
  implemented, so a non-embedded integrator has a complete reference.
- **Prioritization still applies** via the Cost column: the embedded-critical core ships
  first; the broader reference-completeness features follow (they are 🔜, not dropped).

**Two generations, one core.** Both reuse the same `az_iot_adu_client` manifest
parse/format module (from `azure-sdk-for-c`) and the same crypto core; only *how a
manifest reaches the device* and *how status is reported* differ.

| | **ADUv1** (today) | **ADUv2** (in design) |
|---|---|---|
| Channel | IoT Hub **device twin** (MQTT) | Dedicated **HTTPS** ADU endpoint |
| Model | **Push** (service writes desired props) | **Pull** (device polls) |
| Auth | Carried by the Hub connection (SAS / X.509) | **mTLS** with a customer X.509 cert |
| Device data store | Twin reported properties | **Azure Device Registry (ADR)** over HTTP |
| Coupling | Requires IoT Hub | Decoupled from Hub (enables "Day0" shelf devices) |
| Manifest + signing / install | v5, JWS/RS256, SHA-256, multi-step, reboot/resume | **Same** (shared core) |

**Legend.** Support: ✅ Implemented (in core) · 🟡 Partial (built but simplified /
sample-only / not factored) · 🔜 Coming soon (planned / designed, not yet built) ·
⚙️ Architectural capability (enabled by hooks, no core code). *(There is no "not planned"
state — see [Scope and philosophy](#scope-and-philosophy-the-adu-reference-implementation).)*
**Cost** = *remaining* effort for me, Copilot-aided, in **fractional days**
(includes that feature's unit tests). `—` = already done (no remaining cost).
Estimates are planning-grade (±~50%); see [Assumptions](#assumptions-and-estimates).

---

## Status & Cost at a Glance

| Category | Support | Details | Cost |
|---|:--:|---|--:|
| Foundation | ✅ | **Connection state + error propagation** — observer registry, status/reason/source codes, lifecycle guards (Phase 0). [→](#a-foundation) | — |
| Foundation | ✅ | **Twin multi-subscriber + core state machine** — desired-prop subscriber registry, ADU client struct/`do_work` (Phase 1). [→](#a-foundation) | — |
| Core update workflow | ✅ | **Manifest v5 parsing** — delegated to `azure-sdk-for-c`; only v5 targeted. [→](#b-core-update-workflow) | — |
| Core update workflow | ✅ | **Agent state reporting** — internal states → protocol `0/6/255`. [→](#b-core-update-workflow) | — |
| Core update workflow | ✅ | **Device properties reporting** — manufacturer/model/aduVer/compat/installedUpdateId, cached at init. [→](#b-core-update-workflow) | — |
| Core update workflow | ✅ | **Startup + reconnect re-reporting** — first `do_work` + initial twin GET + reconnect observer. [→](#b-core-update-workflow) | — |
| Core update workflow | ✅ | **Accept / reject acknowledgement** — accept→download, reject/already-installed→406. [→](#b-core-update-workflow) | — |
| Core update workflow | ✅ | **Multi-step (composite) updates** — per-step Download→Backup→Install→Apply loop. [→](#b-core-update-workflow) | — |
| Core update workflow | ✅ | **Per-step result reporting** — `resultCode`/`extendedResultCode`/`stepResults`. [→](#b-core-update-workflow) | — |
| Core update workflow | ✅ | **Retry detection** — same `workflow.id` + newer `retryTimestamp` restarts; redelivery ignored. [→](#b-core-update-workflow) | — |
| Core update workflow | ✅ | **Replacement detection** — different `workflow.id` supersedes in-flight; manifest CRC fingerprint. [→](#b-core-update-workflow) | — |
| Core update workflow | ✅ | **Cancellation** — cooperative flag honored at phase boundaries. [→](#b-core-update-workflow) | — |
| Download and integrity | ✅ | **File download from manifest URLs** — resolves `fileUrls`, drives `download_fn`. [→](#c-download-and-integrity) | — |
| Download and integrity | ✅ | **Chunked / streaming download** — `download_fn` may return `IN_PROGRESS`. [→](#c-download-and-integrity) | — |
| Download and integrity | ✅ | **SHA-256 integrity (streaming, opt-in)** — runs when `read_file_fn` + incremental hooks supplied. [→](#c-download-and-integrity) | — |
| Download and integrity | 🔜 | **Delivery Optimization / peer cache** — offload download to a peer/CDN-cache provider behind the download seam; optional, default-off, direct-HTTPS fallback on constrained targets. [→](#c-download-and-integrity) | 2.5 |
| Security and trust | ✅ | **JWS manifest signature** verification (RFC 7515) — 6-stage `verify_manifest()` before any download. [→](#d-security-and-trust) | — |
| Security and trust | ✅ | **Two-level trust chain + `kid` resolution** — root key → SJWK → manifest → SHA-256 binding. [→](#d-security-and-trust) | — |
| Security and trust | ✅ | **RS256-only enforcement** — rejects any other `alg` from the wire. [→](#d-security-and-trust) | — |
| Security and trust | ✅ | **Root key store** (compiled-in Microsoft + runtime-loadable). [→](#d-security-and-trust) | — |
| Security and trust | ✅ | **Root key revocation** — `disabled` roots rejected by `kid`. [→](#d-security-and-trust) | — |
| Security and trust | ⚙️ | **HSM / PKCS#11 backend** — possible via `verify_rs256_fn`; no adapter ships. [→](#d-security-and-trust) | — |
| Security and trust | 🔜 | **Root Key Package runtime rotation** — out-of-band fetch+verify+apply, threshold continuity (deferred; firmware-delivered today). [→](#d-security-and-trust) | 2.5 |
| Install, apply, recovery | ✅ | **Install / Apply execution (core)** — chunkable `install_fn`/`apply_fn`, may request reboot. [→](#e-install-apply-recovery) | — |
| Install, apply, recovery | ✅ | **Backup / Restore (rollback)** — optional `backup_fn`; reverse-order best-effort restore. [→](#e-install-apply-recovery) | — |
| Install, apply, recovery | ✅ | **Partial-failure rollback (multi-step)** — mid-sequence failure rolls back applied steps. [→](#e-install-apply-recovery) | — |
| Install, apply, recovery | ✅ | **Reboot coordination + resume** — persist-before-reboot + `resume()`; v2 blob (CRC, step results). [→](#e-install-apply-recovery) | — |
| Install, apply, recovery | 🟡 | **Health-check / auto-rollback after reboot (core)** — sample-only today; promote to core. [→](#e-install-apply-recovery) | 0.5 |
| Platform and crypto adapters | ✅ | **`crypto_openssl` adapter** — RS256 + SHA-256, factored in `adapters/adu/`. [→](#f-platform-and-crypto-adapters) | — |
| Platform and crypto adapters | 🟡 | **`crypto_mbedtls` adapter** — inline in ESP32 sample; factor into `adapters/`. [→](#f-platform-and-crypto-adapters) | 0.5 |
| Platform and crypto adapters | 🔜 | **Linux platform adapter** — libcurl download / install cmd / file persist; factor from sample. [→](#f-platform-and-crypto-adapters) | 1 |
| Platform and crypto adapters | 🟡 | **ESP32 platform adapter** — real OTA sample exists; factor into `adapters/adu/esp32/`. [→](#f-platform-and-crypto-adapters) | 1 |
| ADUv2 transport | 🔜 | **Shared ADU engine extraction (Approach 3)** — decouple engine from twin; manifest-in / structured-report-out. [→](#g-aduv2-transport-https-pull) | 3 |
| ADUv2 transport | 🔜 | **`syncConfiguration` operation** — register `agentInfo`, cache `rootKeyDownloadUrl` + ETag. [→](#g-aduv2-transport-https-pull) | 1 |
| ADUv2 transport | 🔜 | **`requestUpdates` poll + scheduler** — cadence, `installedUpdateId`, ETags. [→](#g-aduv2-transport-https-pull) | 1.5 |
| ADUv2 transport | 🔜 | **`reportStatus` (terminal outcome)** — new payload shape (string outcome/failureOrigin, stepResults map). [→](#g-aduv2-transport-https-pull) | 1 |
| ADUv2 transport | 🔜 | **mTLS X.509 auth + headers** — HTTPS+mTLS hook, `x-ms-device-id`/correlation. [→](#g-aduv2-transport-https-pull) | 1 |
| ADUv2 transport | 🔜 | **Root key package download** — fetch/cache from `rootKeyDownloadUrl`, verify as usual. [→](#g-aduv2-transport-https-pull) | 1 |
| ADUv2 transport | 🔜 | **ETag + api-version negotiation** — per-workflow persist/reuse, eternal-version fallback. [→](#g-aduv2-transport-https-pull) | 1 |
| ADUv2 transport | 🔜 | **Endpoint discovery + fallback** — current + legacy/LTS base URLs (selection rule open). [→](#g-aduv2-transport-https-pull) | 0.5 |
| ADUv2 transport | 🔜 | **Load contracts** — 429/`Retry-After`, cold-start jitter, no-poll-while-installing. [→](#g-aduv2-transport-https-pull) | 0.5 |
| Day0 recovery | 🔜 | **Unauthenticated recovery transport** — plain-HTTP recovery endpoint (protocol not yet defined). [→](#h-day0-recovery) | 1 |
| Day0 recovery | 🔜 | **Account-ID binding** — validate signed manifest's ADU account ID (replay protection). [→](#h-day0-recovery) | 0.5 |
| Day0 recovery | 🔜 | **Compatibility-property validation** — device checks compat before applying a replayed response. [→](#h-day0-recovery) | 0.5 |
| Delta and handlers | 🔜 | **Static step/download-handler registry** — name→fn "filter" (field-requested); static, in-process. [→](#i-delta-and-handlers) | 1.5 |
| Delta and handlers | 🔜 | **Delta / differential updates** — `relatedFiles` + delta download handler; depends on registry. [→](#i-delta-and-handlers) | 3 |
| Delta and handlers | 🔜 | **Per-handler-type built-in handlers** — reference `apt`/`script`/`swupdate` handlers over the registry. [→](#i-delta-and-handlers) | 3 |
| Delta and handlers | 🔜 | **Dynamic `ContentHandler` plugin loading** — optional `dlopen`/`LoadLibrary` registrar over the static registry (non-embedded); static registry stays the portable default. [→](#i-delta-and-handlers) | 2 |
| Library / agent-core mode | ✅ | **Turnkey client** — SDK drives verify→install→report (the shipping client). [→](#j-library-and-agent-core-mode) | — |
| Library / agent-core mode | 🔜 | **Library mode** — hand back a verified+parsed manifest; consumer drives their own state machine. [→](#j-library-and-agent-core-mode) | 1 |
| Testing and conformance | ✅ | **Phase-1 unit tests** — cmocka state-machine coverage. [→](#k-testing-and-conformance) | — |
| Testing and conformance | 🟡 | **Crypto vector tests** — known-good/bad RS256 + SHA-256 vectors. [→](#k-testing-and-conformance) | 0.5 |
| Testing and conformance | 🔜 | **Adapter integration tests** — mock HTTP server + test manifest per adapter. [→](#k-testing-and-conformance) | 1 |
| Testing and conformance | 🔜 | **ADU conformance suite** — host-only `az_iot_adu_conformance`, all states + multi-step. [→](#k-testing-and-conformance) | 2 |
| Testing and conformance | 🔜 | **E2E vs real ADU service** — gated behind `AZ_IOT_ADU_E2E` (off the PR path). [→](#k-testing-and-conformance) | 1.5 |
| Advanced update model | 🔜 | **Reference steps** — `type: reference` + detached child manifest: fetch, verify, recurse. [→](#l-advanced-update-model) | 1.5 |
| Advanced update model | 🔜 | **Proxy / nested updates** — parent agent orchestrates leaf/component updates (gateway→leaf). [→](#l-advanced-update-model) | 3 |
| Advanced update model | 🔜 | **Component-level targeting** — component enumerator hook + `selectedComponents` matching. [→](#l-advanced-update-model) | 1.5 |
| Advanced update model | 🔜 | **`mimeType` handling** — parse + surface file `mimeType` to handlers. [→](#l-advanced-update-model) | 0.25 |
| Agent services | 🔜 | **Diagnostics / log-upload** — respond to a diagnostics request; collect + upload logs to the given SAS URL via an upload hook. [→](#m-agent-services) | 1.5 |
| Agent services | 🔜 | **`adu-shell` / privilege separation** — reference POSIX setuid broker so root-needing steps run out-of-process; inert on single-privilege targets. [→](#m-agent-services) | 2 |

### Cost rollup

Everything is committed (per [Scope and philosophy](#scope-and-philosophy-the-adu-reference-implementation)); the tiers are only about **ordering**.

>Note: Cost estimate is usually overblown by copilot, true estimate might range within 25% to 35% of the cost shown here.

- **Tier 1 — embedded-critical core (ship first): ≈ 21–22 days.** Adapters (E/F) ~3 · ADUv2
  transport (G) ~10.5 · Day0 (H) ~2 · Library mode (J) ~1 · Testing (K) ~5.
- **Tier 2 — reference-completeness (coming soon): ≈ 24 days.** Delta + handler registry (I)
  ~4.5 · per-handler-type handlers ~3 · dynamic loading ~2 · Delivery Optimization ~2.5 ·
  reference steps ~1.5 · proxy/nested ~3 · component targeting ~1.5 · `mimeType` ~0.25 ·
  diagnostics/log-upload ~1.5 · `adu-shell` ~2 · Root Key Package rotation (D) ~2.5.
- **All-in for full reference parity: ≈ 45–46 days.**

```mermaid
flowchart LR
    A[v1 core ✅] --> B[Finish adapters + health-check\n~3d]
    B --> C[Extract shared engine\n~3d]
    C --> D[ADUv2 transport\n~7.5d]
    D --> E[Day0 + library mode\n~3d]
    E --> F[Conformance + E2E\n~3.5d]
    F --> G[Tier 2: reference-completeness\ndelta · handlers · proxy · components\ndiagnostics · adu-shell · DO\n~24d]
```

---

# Feature Manual (design detail per category)

## A. Foundation

- **Connection state + error propagation (✅, Phase 0).** Shared observer registry
  (public + internal registration, two-pass dispatch, compile-time capacity),
  `az_iot_conn_status` / `az_iot_conn_reason` / `az_iot_error_source`, lifecycle
  guards (`DEINITIALIZING`, re-init poison guard, `AZ_IOT_ERR_DETACHED`). Detail:
  [connection-state-and-error-propagation.md](connection-state-and-error-propagation.md).
- **Twin multi-subscriber + core state machine (✅, Phase 1).** `az_iot_twin_client`
  desired-property subscriber registry; `az_iot_adu_client_t` init/destroy/`do_work`;
  device-properties cache. This is what makes the ADU client a twin subscriber today.

## B. Core update workflow

All ✅ and audited against `c/src/features/adu/`. The engine advances one phase per
`do_work()`:

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> ManifestReceived: deployment (desired props)
    ManifestReceived --> VerifyingManifest
    VerifyingManifest --> Idle: reject / already-installed (406)
    VerifyingManifest --> DownloadStarted: accept (200)
    DownloadStarted --> DownloadComplete
    DownloadComplete --> BackupStarted
    BackupStarted --> BackupComplete
    BackupComplete --> InstallStarted
    InstallStarted --> InstallComplete
    InstallComplete --> ApplyStarted: reboot? persist + resume()
    ApplyStarted --> Idle: next step / done
    DownloadStarted --> RestoreStarted: failure
    InstallStarted --> RestoreStarted: failure
    ApplyStarted --> RestoreStarted: failure
    RestoreStarted --> Idle: Failed
```

- **Manifest v5 parsing** — v5 is what the service emits today; older versions can be added
  here if a deployment ever needs them (not refused on scope grounds).
- **Agent state / device properties / re-reporting** — states map to `0/6/255`;
  device props deep-copied into a client-owned cache; re-report on startup + reconnect
  and an initial twin GET catches an offline-created deployment.
- **Accept / reject** — decided in core; driven by `is_installed_fn` (already-installed
  ⇒ 406). *Caveat:* no app-level `accept_deployment_fn` veto hook yet (e.g. battery /
  critical-op deferral) — a candidate add.
- **Multi-step / per-step results** — sequential per-step loop; `step_results[]` with a
  4-bit facility + raw-code `extendedResultCode` for field debugging.
- **Retry vs. replacement vs. duplicate** — `set_active_workflow` tracks `workflow.id`
  + `retryTimestamp` + a CRC-32 fingerprint of `updateManifest`; same-id+newer-retry
  restarts, different-id supersedes, same-id+same-retry is an ignored redelivery.
- **Cancellation** — `action: Cancel` (or a replacement) sets a cooperative flag honored
  at phase boundaries; hooks poll `az_iot_adu_is_cancelled()`. Core never force-interrupts a hook.

## C. Download and integrity

- **Download / chunked** — core resolves each file's URL from the request `fileUrls`
  map and drives `download_fn`, which may return `IN_PROGRESS` to stay non-blocking.
- **SHA-256 (opt-in)** — streaming via incremental `sha256_*` hooks + `read_file_fn`,
  constant-time compare. *Caveat:* if those hooks are absent, core **skips** the hash and
  the platform owns integrity — document this clearly for integrators.
- *No mid-download resume (yet)* — a partially fetched file is re-downloaded after a reboot;
  byte-range resume is a candidate add (not refused, just unscheduled).
- **Delivery Optimization / peer cache (🔜, ~2.5d).** Offload payload download to a
  peer-to-peer / CDN-cache provider (e.g. a Delivery-Optimization / Connected-Cache agent)
  behind the same download seam as `download_fn`, selected as an optional **download
  provider**. *Caveat:* the provider is heavy for constrained targets, so it is **default-off
  with a direct-HTTPS fallback** — but it is implemented so non-embedded integrators have the
  reference. Verification/hashing is unchanged (runs on the assembled bytes).

## D. Security and trust

- **JWS / two-level chain / RS256 / revocation (✅)** — `verify_manifest()` runs the full
  root-key → SJWK → manifest chain, enforces `alg == RS256` on both headers, binds
  SHA-256(manifest) to the deployment, and rejects `disabled` roots by `kid`. Failure ⇒
  `Failed` with facility `0x1`. This is the crown-jewel code and is already v2-ready
  (the shared engine keeps it verbatim).
- **HSM / PKCS#11 (⚙️)** — verification uses only public keys via `verify_rs256_fn`, so an
  HSM backend is a drop-in hook; none ships.
- **Root Key Package runtime rotation (🔜, ~2.5d).** Out-of-band package (fetch,
  persistence, threshold-signature continuity) to rotate roots without a firmware update.
  *Caveat:* needs its own design pass; **not** the Day0 mechanism (Day0 keeps roots fixed).
  Today roots rotate via firmware.

## E. Install, apply, recovery

- **Install/Apply, Backup/Restore, partial rollback, reboot/resume (✅).** Persist-before-
  reboot uses a versioned, CRC-checked, little-endian blob (`ADU1`, blob **v2**) carrying
  `retryTimestamp`, a manifest CRC, and the accumulated `install_result` incl.
  `step_results[]`; `resume()` re-enters at the persisted phase boundary
  (`INSTALL_COMPLETE` → Apply). *Caveats:* the only persist point today is the
  install-requested reboot; post-reboot rollback assumes the platform retained per-step
  backups across the reboot.
- **Health-check / auto-rollback after reboot (🟡 → core, ~0.5d).** Today only the ESP32
  A/B sample confirms/marks-valid the new image; core does not re-run `is_installed_fn` on
  resume. **To do:** add an optional post-reboot confirm step in core with an auto-rollback
  path when confirmation fails.

## F. Platform and crypto adapters

- **`crypto_openssl` (✅)** is the only fully factored adapter in `adapters/adu/`.
- **To do:** factor **`crypto_mbedtls`** (🟡, ~0.5d — currently inline in the ESP32 sample),
  the **Linux** adapter (🔜, ~1d — libcurl chunked/streaming-hash download, configurable
  install command, file-based persistence), and the **ESP32** adapter (🟡, ~1d —
  `esp_http_client` + `esp_ota` + NVS resume; the real-OTA sample already proves it, it
  just isn't under `adapters/adu/esp32/`). *Caveat:* install/apply/download real adapters
  currently live in **samples**, not `adapters/`.

## G. ADUv2 transport (HTTPS pull)

ADUv2 keeps the same manifest content but delivers it via a **device-initiated
RPC-over-HTTPS** data plane (every op is an HTTP `POST`), mTLS-authenticated, decoupled
from IoT Hub; updating properties are persisted by the service in **ADR** (the device only
calls the three ADU ops). Client implementation guidance is in
[adu-client-design.md](adu-client-design.md) Part B.

```mermaid
sequenceDiagram
    participant Dev as Device agent
    participant ADU as ADU HTTPS endpoint (mTLS)
    Dev->>ADU: POST syncConfiguration (agentInfo)
    ADU-->>Dev: rootKeyDownloadUrl, serviceConfigETag
    loop poll cadence (+ jitter)
      Dev->>ADU: POST requestUpdates (installedUpdateId, ETags)
      ADU-->>Dev: 200 {} (none) OR updateMetadata{workflowId, manifest, sig, fileUrls}
    end
    Note over Dev: verify → download → install → apply (shared engine)
    Dev->>ADU: POST reportStatus (workflowId, lastInstallResult)
    ADU-->>Dev: 200 {}
```

**Chosen architecture — Approach 3 (shared engine + two thin clients):** extract the
protocol-free engine (verify/download/install/apply/backup/restore + resume + step
results) and let a v1 twin wrapper and a v2 HTTPS wrapper drive it. The engine takes a
manifest **string** and returns a **structured** report; each wrapper serializes it to its
own wire shape. Decision rationale + alternatives are captured with the ADR-integration
notes.

```mermaid
flowchart TB
    ENG["Shared ADU engine<br/>verify → download → install → apply → resume<br/>step results · persistence"]
    TW["v1 twin wrapper<br/>desired/reported props"] --> ENG
    HT["v2 HTTPS wrapper<br/>sync / request / report"] --> ENG
    ENG --> CR["crypto hooks<br/>RS256 · SHA-256"]
    ENG --> PL["platform hooks<br/>download/install/apply/backup/restore/persist"]
```

Work items (all 🔜): **engine extraction** (~3d, pure refactor with the twin client as
first consumer and Phase-1 tests as the net) → **`syncConfiguration`** (~1d) →
**`requestUpdates` + poll scheduler** (~1.5d) → **`reportStatus`** new payload (~1d) →
**mTLS + headers** (~1d) → **root key package download** (~1d) → **ETag/api-version**
per-workflow (~1d) → **endpoint discovery/fallback** (~0.5d) → **load contracts** (~0.5d).

*Caveats:* the report **shape** differs from Gen1 (string `outcome`/`failureOrigin`,
comma-separated hex `extendedResultCodes`, `stepResults` map) — the engine must emit
structured data, not a fixed JSON string, so the v2 wrapper doesn't re-parse. Several wire
details are still open (see [Manual / external actions](#manual--external-actions)).

## H. Day0 recovery

A device too stale to reach DPS/Hub/ADU recovers via a separate **unauthenticated,
plain-HTTP** endpoint. *No new crypto:* trust rests on the existing signed-manifest +
provisioned-root-key model, so signature verification MUST NOT be skipped on this path.
Adds a **transport + two validation checks**: (1) **account-ID binding** — validate the
signed manifest's ADU account ID against one stamped at manufacturing (replay protection);
(2) **compatibility-property validation** before applying. *Caveat:* the Day0 wire contract
is **not yet defined** in the data-plane protocol — provisional until protocol owners
confirm; distinct from the (still-mTLS) legacy/LTS endpoint.

## I. Delta and handlers

Field/customer demand exists for **delta / differential updates** (ship only the diff
between image versions and reconstruct on-device). ADU stays **content-agnostic**: the diff
is a `relatedFiles` entry processed by a swappable **download handler**, selected by name —
*not* baked into the state machine.

- **Static step/download-handler registry (🔜, ~1.5d).** A small C99 name→function map
  ("filter") passed to the engine/sample so integrators register handlers (static,
  in-process). This is the SDK-friendly equivalent of the reference agent's extension model.
- **Delta / differential updates (🔜, ~3d).** Parse `relatedFiles`, resolve the delta file,
  and route it to the registered handler (e.g. a delta reconstruct step) before install;
  fall back to full download when the prior version is absent. Depends on the registry.
- **Per-handler-type built-in handlers (🔜, ~3d).** Ship reference handlers for the common
  types (`microsoft/apt`, `microsoft/script`, `microsoft/swupdate`) as optional modules that
  register into the handler registry and map the manifest `handler` string + step files to a
  concrete install action — turnkey parity with the reference agent on capable platforms.
- **Dynamic `ContentHandler` plugin loading (🔜, ~2d).** An **optional** runtime registrar
  (`dlopen` / `LoadLibrary`) layered over the static registry, matching the reference agent's
  `--register-extension` model so handlers can be added without recompiling. *Caveat:* dynamic
  loading is unavailable/undesirable on many embedded targets, so it is a **build-gated
  provider** and the static registry stays the portable default — but it is implemented so the
  non-embedded reference is complete. The manifest `handler` string is always parsed and passed
  to hooks, so static in-process dispatch remains available with zero loader.

## J. Library and agent-core mode

Beyond the turnkey client, the SDK should be usable as the **vetted core** others build a
full agent on. Provide a way to **validate + parse a manifest**, then let the consumer pick:

- **Turnkey (✅):** the SDK drives the whole workflow (today's client).
- **Library mode (🔜, ~1d):** hand back a **filled, already-verified** manifest struct; the
  consumer drives download/install/apply/report on their own state machine, threading and
  extension model. Reuses the same trust code so nobody re-implements JWS/RS256/SHA-256.
  Detail: [adu-client-design.md](adu-client-design.md) Part C.

## K. Testing and conformance

Per the phase plan, **L1 unit tests land with each feature** (state machine in Phase 1,
crypto vectors in Phase 2, adapter integration in Phases 3–4, persistence in Phase 5).

- **Unit tests (✅)** — cmocka state-machine coverage in `tests/unit/adu_client_test.c`.
- **Crypto vector tests (🟡, ~0.5d)** — known-good/bad RS256 + SHA-256 vectors; prove hooks
  are primitive-only.
- **Adapter integration tests (🔜, ~1d)** — mock HTTP server + test manifest per adapter.
- **Conformance suite (🔜, ~2d)** — reusable host-only `az_iot_adu_conformance` over all
  protocol states + single/multi-step manifests.
- **E2E (🔜, ~1.5d)** — against the real ADU service, gated behind `AZ_IOT_ADU_E2E` so it
  never runs on the fast PR path.

## L. Advanced update model

Full reference parity with the ADU update model. Each is 🔜 (implemented so non-embedded
integrators have the complete reference); on a single-image embedded device several are inert
at runtime, which is a **caveat, not an exclusion**.

- **Reference steps (🔜, ~1.5d).** Parse `type: reference` steps that point to a **detached
  child manifest** (by file id): fetch it, verify its signature with the same trust chain, and
  recurse into it. Prerequisite for proxy/nested updates.
- **Proxy / nested updates (🔜, ~3d).** A parent/gateway agent receives a bundle and
  orchestrates updates for **leaf** devices/components (the IoT-Edge parent→leaf topology),
  built on reference steps + component enumeration. *Caveat:* inert on a standalone device.
- **Component-level targeting (🔜, ~1.5d).** A **component-enumerator hook** lets a device
  enumerate its updatable components; the engine matches `selectedComponents` / per-component
  compatibility and iterates the workflow per selected component.
- **`mimeType` handling (🔜, ~0.25d).** Parse and surface the file `mimeType` to handlers for
  dispatch/validation (currently skipped by the parser).

## M. Agent services

Agent-level services from the reference agent, provided so the reference is complete; both are 🔜.

- **Diagnostics / log-upload (🔜, ~1.5d).** Respond to a diagnostics/log-upload request:
  collect the configured logs and upload them to the service-provided (SAS) storage URL via an
  **upload hook**. Independent of the update workflow.
- **`adu-shell` / privilege separation (🔜, ~2d).** A reference **POSIX setuid broker** so
  install/apply steps that need root run out-of-process while the SDK core stays unprivileged
  and calls the broker through a hook. *Caveat:* irrelevant on single-privilege RTOS targets
  (the core simply calls the hook directly); this is a POSIX reference, not a core requirement.

---

## Manual / external actions

Not code — things I (or the team) must do out-of-band:

- **Confirm the open ADUv2 protocol items** with the ADU protocol/API team before building
  the affected pieces: X.509 onboarding/operational cert lifecycle & rotation;
  legacy-endpoint selection heuristic; api-version persistence per workflow;
  `agentInfoETag` lifecycle; cold-start jitter / account rate-cap values; and the **Day0**
  wire contract (channel, account-ID binding).
- **Stand up a dev ADUv2 environment** and obtain the ADU endpoint URL + test **X.509
  certs** to run the SDK PoC. *(Internal setup steps are kept in local notes, not in this
  repo.)*
- **Get visibility into upcoming manifest schema changes** to validate forward-compatibility.

## Assumptions and estimates

- Costs are **remaining** effort **for one engineer, Copilot-aided**, in fractional days,
  and **include that feature's unit tests**. They are planning-grade (**±~50%**) and assume
  the shared-engine refactor (G) lands first, since most v2 items build on it.
- `✅` items are **audited against `c/src/features/adu/`**, not just intent.
- ADUv2 items assume the data-plane protocol (api-version `2026-11-02-preview`) stays stable
  on the points this SDK depends on; open items are tracked under *Manual actions*.

## References

- [adu-client-design.md](adu-client-design.md) — deep architecture: public API, hook/crypto
  model, state machine, source layout, phase plan, Part B (ADUv2 wire contract), Part C
  (library mode), test strategy.
- [connection-state-and-error-propagation.md](connection-state-and-error-propagation.md) —
  the Phase-0 foundation.
- [split-client.md](split-client.md) — packaging / client-split considerations.
- Superseded: `adu-feature-support.md` (folded into this doc).
