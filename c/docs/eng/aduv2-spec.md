# ADUv2 — Device Update via DPS (design summary)

> **Status: design summary of a DRAFT, Microsoft-internal spec** (api-version `2026-11-02-preview`).
> Device-SDK-oriented digest of the DPS *"ADU first-time update"* design. Contract details are still
> settling and DPS re-syncs on ADU revisions — treat field-level specifics as provisional.
> For SDK status/cost, see [adu-client-plan.md](adu-client-plan.md); for the shared engine/crypto core,
> see [adu-client-design.md](adu-client-design.md).

## What changed

ADUv2's **device-facing delivery moved off a dedicated ADU HTTPS endpoint**. Instead, **DPS** exposes three
new device-facing update APIs and acts as an **authenticated pass-through** to Azure Device Registry (ADR) →
ADU. The device **reuses its existing DPS auth and endpoint** and **never talks to ADU directly**. (Post-Ignite,
IoT Hub will front the *operational* flow the same way; for Ignite '26 **DPS fronts both** bootstrap and
operational as an interim.)

| | Previous ADUv2 sketch | **Now (DPS-fronted)** |
|---|---|---|
| Device endpoint | Dedicated ADU HTTPS data-plane endpoint (discovered) | **The device's existing DPS endpoint** (already provisioned) |
| Auth | mTLS with a customer ADU cert | **Reuse DPS device auth** — X.509 (Phase 1); SAS / TPM later |
| Credentials on device | ADU endpoint + creds burned in | **None new** — DPS discovers the ADU endpoint from ADR (late binding) |
| Who talks to ADU | Device → ADU directly | **Device → DPS → ADR → ADU** (DPS proxies; ADR resolves the device) |
| Primary scenario | Operational polling | **Update *before* first Register** (bootstrap), plus operational (interim) |

## The three device-facing DPS operations

The device **selects** onboarding vs regular by *which endpoint it calls* — DPS does not infer or validate the
choice; it passes the ADR/ADU response (or error) straight through.

| DPS operation (working name) | ADU route | When the device uses it |
|---|---|---|
| `GetOnboardingDeviceUpdate` | `POST /devices/requestOnboardingUpdates` | **Before provisioning** (not yet in ADR) — bootstrap / day-zero |
| `GetDeviceUpdate` | `POST /devices/requestUpdates` | **Operational** (already provisioned) — interim, until Hub ships its API |
| `ReportDeviceUpdateStatus` | `POST /devices/reportStatus` | After an install attempt (**required** so ADU can reconcile) |

## Architecture

```mermaid
flowchart LR
    Dev["Device (SDK)"] -->|"getUpdate / reportStatus (DPS device auth)"| DPS["DPS gateway"]
    DPS -->|"authenticated proxy (S2S)"| ADR["Azure Device Registry"]
    ADR -->|internal| ADU["Azure Device Update"]
    DPS -. "endpoint discovery" .-> ADR
    Dev -. "download firmware (fileUrls)" .-> Blob["Blob / CDN"]
```

- **DPS is a synchronous pass-through** — no orchestration, no per-device state, `Register` untouched.
- **Advisory, never blocking:** if an update check fails, the device just proceeds to `Register`.
- **Firmware is downloaded directly from blob** (`fileUrls`), outside the signed manifest.

## Flows

### Bootstrap — update before first Register

```mermaid
sequenceDiagram
    participant Dev as Device (SDK)
    participant DPS as DPS
    participant ADU as ADR to ADU
    loop until "no update"
      Dev->>DPS: GetOnboardingDeviceUpdate (agentInfo, installedUpdateId)
      DPS->>ADU: proxy (externalDeviceId)
      ADU-->>DPS: serviceConfiguration [+ updateMetadata]
      DPS-->>Dev: 200 (no updateMetadata = no update)
      alt update available
        Dev->>Dev: verify signature, download fileUrls, install
        Dev->>DPS: ReportDeviceUpdateStatus (workflowId, result)
        DPS->>ADU: proxy report
        DPS-->>Dev: 200
      end
    end
    Dev->>DPS: Register (unchanged)
    DPS-->>Dev: IoT Hub assignment
```

The device **re-checks after each successful install** and only proceeds to `Register` once it gets
*"no update"* — so multiple updates can chain before provisioning completes.

### Operational (interim)

An already-provisioned device polls `GetDeviceUpdate` on the same shape. This DPS-fronted operational path is a
**time-boxed interim**; post-Ignite it moves to **IoT Hub's** own updating API (no device-contract change —
same request/response, different gateway).

## Contract summary

**Request** (`RequestUpdatesRequest`, both fetches):

- `agentInfo` — `{ agentSdkVersion, agentProfile (opaque capability id), compatibilityProperties (1–5 KVPs) }`;
  the service combines `agentProfile` + compat props into the **device class**. Required unless a still-current
  `agentInfoETag` is supplied (onboarding always requires it).
- `installedUpdateId` — currently installed `{ provider, name, version }`, or `null`.
- `agentInfoETag`, `serviceConfigETag` — optional; let the service skip re-processing / omit unchanged config.

**Response** (`RequestUpdatesResponse`):

- `serviceConfiguration.rootKeyDownloadUrl` — root-key package URL for signature verification (omitted when the
  supplied `serviceConfigETag` still matches).
- `serviceConfigETag`, `agentInfoETag` — always present.
- `updateMetadata` — **present only when an update applies**: `{ workflowId, updateManifest (opaque JSON string),
  updateManifestSignature (JWS), fileUrls (fileId → URL) }`. **Omitted ⇒ "no update" (HTTP 200, not an error).**

**Report** (`ReportStatusRequest`): `{ workflowId, installedUpdateId, lastInstallResult }` where
`lastInstallResult` = `{ outcome ∈ IN_PROGRESS|SUCCEEDED|FAILED|CANCELED|SKIPPED, failureOrigin, resultCode,
extendedResultCodes (comma-sep hex), resultDetails, stepResults{ step_0, step_1, … } }`. **Idempotent on
`workflowId` alone**; a conflicting terminal for the same id ⇒ `409 REPORT_CONFLICT`.

> Identity headers (`x-ms-external-device-id` = registrationId; `x-ms-device-id` = ADR UUID on the regular path)
> are **gateway-populated — the device sets none of them**.

## Auth & transport

- **Auth:** reuse the existing DPS device credential. **Phase 1: X.509.** Symmetric key and TPM follow (TPM is a
  two-phase 401-challenge, individual-only). No ADU-specific credentials.
- **Transport:** **Phase 1 HTTP + MQTT**; Phase 2 AMQP. (TPM works on HTTP/AMQP only, not MQTT.)
- **api-version:** `2026-11-02-preview`.

## Trust model

- The device **validates `updateManifestSignature`** (nested JWS / RS256) against the **root-key package** at
  `rootKeyDownloadUrl` (hardcoded ADU root key → SJWK signing key → manifest hash). Root-key rotation +
  `disabledSigningKeys` revocation are supported. `fileUrls` are **not** in the signed manifest.
- **Ignite defers the `accountId`-in-signature binding** (manifest-signature-v2): DPS returns **no** `accountId`,
  so the device verifies *provenance-from-ADU* but not *account scoping*. **Base signature validation stays
  required.**

## Error handling (device / SDK)

Drive behavior from the machine-readable **`error.code`** (`x-ms-error-code` header), never the HTTP status.

| Case | Code / status | SDK action |
|---|---|---|
| No update | 200, `updateMetadata` omitted | Nothing to apply; proceed to `Register`. **Not an error.** |
| ADU not linked | 409 `UPDATE_ACCOUNT_NOT_LINKED` | Treat as "no update service configured" (distinct from *no update*); proceed. Don't retry. |
| Agent-info stale/unknown | 400 `OUTDATED_AGENT_INFO` / `UNKNOWN_AGENT_INFO_VERSION` | **Resend the full `agentInfo`** and retry (handle **both** codes). |
| Service-config stale | 400 `OUTDATED_SERVICE_CONFIG` | Re-check **without** the stale `serviceConfigETag`; response returns fresh config. |
| Throttled | 429 + `Retry-After` | Wait `Retry-After`, then retry. |
| Transient upstream | 503 `UPSTREAM_UNAVAILABLE` / `INTERNAL_SERVER_ERROR` | **Get:** proceed to `Register` (advisory), retry later. **Report:** retry (must not be lost). |
| Bad request / auth / disabled | 400 / 401 / 403 | Fix request or credentials; don't retry unchanged. |

**The device is the sole retrier** (DPS fails fast, one attempt per hop) and honors `Retry-After`. `reportStatus`
is a durable write — retry until acked; safe because ADU is idempotent on `workflowId`.

## Ignite '26 scope

- **In:** the 3 device APIs · X.509 auth · HTTP + MQTT · DPS fronts **bootstrap + operational (interim)** ·
  advisory pass-through · stateless.
- **Deferred (post-Ignite):** `accountId` delivery + signature binding (manifest-sig-v2) · symmetric-key & TPM
  auth · AMQP · per-enrollment-group enablement toggle · operational path moving to IoT Hub.

## What this means for the ADU client SDK

The **verify → download → install → report engine is unchanged** from ADUv1 (shared core). New client work is the
**transport binding + orchestration**:

1. Call the three DPS ops over the device's **existing DPS transport/auth** (X.509, HTTP/MQTT) — no ADU endpoint,
   no mTLS, no identity headers to set.
2. **Onboarding-vs-regular endpoint selection** by provisioning state.
3. **Bootstrap orchestration:** update-check → install → report → **re-check loop** → then `Register` (advisory).
4. **ETag + agent-info/service-config handling**, including the *resend* codes.
5. **Root-key-package fetch** from `rootKeyDownloadUrl`; keep the existing JWS/RS256 verification.
6. **Advisory/retry rules:** never block `Register`; device is the sole retrier; honor `Retry-After`.

## References

- **Public REST API (TypeSpec, draft):** [Azure/azure-rest-api-specs#44617](https://github.com/Azure/azure-rest-api-specs/pull/44617) — the three device-update operations, api-version `2026-11-02-preview`.
- **Design spec (Microsoft-internal):** DPS *"ADU first-time update"* spec package — [Azure-IoT-Hub-DeviceRegistrationService `/specs/002-adu-first-time-update`](https://dev.azure.com/msazure/One/_git/Azure-IoT-Hub-DeviceRegistrationService?path=/specs/002-adu-first-time-update).
- Related SDK docs: [adu-client-plan.md](adu-client-plan.md), [adu-client-design.md](adu-client-design.md).
