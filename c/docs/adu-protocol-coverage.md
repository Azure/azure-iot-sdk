# Azure Device Update on azure-iot-sdk — Protocol Coverage & Client API

Goal: communicate which ADU **protocol and design features** the new `azure-iot-sdk` SDK plans to support, and the rationale for each include/exclude decision. This is a focused summary; it intentionally omits internal SDK architecture.

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

Legend: ✅ Planned · 🔜 Future / pending (not in v1) · ❌ Not planned

### 1.1 Core update workflow

| Feature | Support | Rationale |
|---|---|---|
| Update Manifest **v5** parsing | ✅ | The current ADU manifest schema; the only version targeted. Earlier versions are intentionally unsupported. |
| Manifest versions < v5 | ❌ | Legacy; no field demand. Reduces surface and test matrix. |
| Agent state reporting (Idle / DeploymentInProgress / Failed) | ✅ | Required for the service to track device progress. |
| Device properties reporting (manufacturer, model, aduVer, compat properties, installedUpdateId) | ✅ | Required for deployment targeting and device inventory. |
| Startup + reconnect re-reporting of device properties | ✅ | Ensures the service always has current device state after boot/reconnect. |
| Deployment **accept / reject** acknowledgement | ✅ | Honors service request/response semantics (200 accept / 406 reject). |
| **Multi-step** (composite) updates | ✅ | Real updates routinely contain multiple steps; required for parity. |
| Per-step result reporting (`resultCode` / `extendedResultCode` / `stepResults`) | ✅ | Service needs per-step diagnostics; we add a structured `extendedResultCode` (facility + raw code) for field debugging. |
| **Retry** detection (same workflow id, newer retry timestamp) | ✅ | Required to honor service-initiated retries. |
| **Replacement** detection (new workflow supersedes in-flight one) | ✅ | Required to abandon a stale deployment correctly. |
| Deployment **cancellation** | ✅ | Required; handled at phase boundaries with a cooperative cancel flag. |

### 1.2 Download & integrity

| Feature | Support | Rationale |
|---|---|---|
| File download over HTTP/HTTPS from manifest URLs | ✅ | Core delivery mechanism. Provided via a platform hook (libcurl on Linux, esp_http_client on ESP32). |
| Chunked / streaming download | ✅ | Embedded targets cannot buffer whole images; required for memory-bound devices. |
| **SHA-256** payload hash verification (streaming) | ✅ | Mandatory integrity check before install; verified per file. |
| Delta / differential updates (`relatedFiles`, `downloadHandler`) | ❌ | Large added complexity; not required for first release. Full-image updates cover the primary scenarios. |
| Delivery Optimization / peer-to-peer download | ❌ | Out of scope for a constrained-device SDK; no protocol requirement on the agent. |

### 1.3 Security & trust

| Feature | Support | Rationale |
|---|---|---|
| **JWS manifest signature** verification (RFC 7515) | ✅ | Mandatory authenticity check before any download. |
| Two-level trust chain (root key → SJWK → manifest), `kid` resolution | ✅ | Matches ADU's signing model; fully validated on device. |
| Algorithm enforcement: **RS256 only** | ✅ (RS256) | The algorithm ADU uses today. Enforced from the wire (`alg` must be `RS256`); other algorithms rejected. New algorithms would be an additive change. |
| Compiled-in + runtime-loadable **root key** store | ✅ | Microsoft roots ship by default; customers may provision their own. |
| Root key **revocation** (disable a root by `kid`) | ✅ | Supported in the in-memory key store for v1. |
| **Root Key Package** runtime rotation (fetch + verify + apply, threshold continuity) | 🔜 | Deferred. v1 rotates keys via firmware update (the same trusted update path). The full out-of-band package protocol (fetch, persistence, threshold signatures) warrants its own design pass and is not required to ship signed-update verification. Note: this is *not* the ADUv2 Day0 recovery mechanism — Day0 keeps the root keys fixed and uses them to verify a recovery update (§8), it does not rotate them. |
| HSM / PKCS#11-backed verification | ✅ (enabled) | Verification uses public keys; the crypto primitive is a customer hook, so an HSM-backed implementation is possible. |

### 1.4 Install, apply, recovery

| Feature | Support | Rationale |
|---|---|---|
| Install / Apply execution | ✅ | Provided via platform hooks (e.g. `esp_ota` on ESP32, configurable command on Linux). |
| Backup / Restore (rollback) | ✅ | Per-step backup before install; reverse-order, best-effort restore on failure. |
| Partial-failure rollback across multi-step updates | ✅ | Required so a mid-sequence failure returns the device to its pre-deployment state. |
| **Reboot coordination** + **resume after reboot** | ✅ | Updates that require a reboot persist a versioned, integrity-checked state blob and resume at the correct phase boundary on next boot. |
| Health check / auto-rollback after reboot | ✅ | The sample confirms the new image and marks it valid; failure triggers rollback (A/B partition scheme on ESP32). |

### 1.5 Manifest features explicitly not covered

| Feature | Support | Rationale |
|---|---|---|
| Reference steps (`"type": "reference"` + detached manifest file id) | ❌ | Used for proxy/nested (gateway→leaf) updates; not a target scenario for the first release. |
| Proxy / nested updates (IoT Edge parent updating leaf devices) | ❌ | Gateway topology out of scope for the device SDK's first ADU release. |
| Component-level targeting (component enumerators) | ❌ | Adds a component-model abstraction not needed for the primary single-image device scenario. |
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
az_iot_result_t az_iot_adu_client_init(
    az_iot_adu_client_t* client,
    az_iot_twin_client_t* twin,
    const az_iot_adu_platform_hooks_t* hooks,   /* download/install/apply/backup/restore/persist */
    const az_iot_adu_crypto_hooks_t*   crypto,  /* verify_rs256 + sha256 primitives */
    const az_iot_adu_root_key_t* root_keys, size_t root_key_count,
    const az_iot_adu_device_properties_t* device_props,
    uint8_t* device_props_buffer, size_t device_props_buffer_size);

void az_iot_adu_client_deinit(az_iot_adu_client_t* client);

/* Resume an interrupted workflow after a reboot (no-op if none persisted). */
az_iot_result_t az_iot_adu_client_resume(az_iot_adu_client_t* client);

/* Runtime — call from the application's do_work loop. Non-blocking. */
az_iot_result_t az_iot_adu_client_do_work(az_iot_adu_client_t* client);

/* Observe / control. */
az_iot_adu_state_t az_iot_adu_client_get_state(const az_iot_adu_client_t* client);
bool              az_iot_adu_is_cancelled(const az_iot_adu_client_t* client);

/* Update reported device properties (deep-copied; published on next do_work). */
az_iot_result_t az_iot_adu_client_update_device_properties(
    az_iot_adu_client_t* client,
    const az_iot_adu_device_properties_t* device_props);

/* Convenience: Microsoft's compiled-in ADU root public keys. */
const az_iot_adu_root_key_t* az_iot_adu_microsoft_root_keys(size_t* out_count);
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
} az_iot_adu_crypto_hooks_t;
```

---

## 3. Minimal Integration Sample

```c
/* Wire connection + twin + ADU client, then pump. */
az_iot_connection_client_init(&conn, /* hub/device credentials */ ...);
az_iot_twin_client_init(&twin, &conn);

/* Crypto primitives + root keys (Microsoft defaults shown). */
az_iot_adu_crypto_hooks_t crypto = az_iot_adu_crypto_openssl_hooks();
size_t rk_count;
const az_iot_adu_root_key_t* root_keys = az_iot_adu_microsoft_root_keys(&rk_count);

/* Platform hooks: download/install/apply/backup/restore/persist for this device. */
az_iot_adu_platform_hooks_t hooks = my_platform_hooks();

az_iot_adu_device_properties_t props = {
    .manufacturer = "Contoso",
    .model        = "Thermostat-9000",
    .installed_update_id = { .provider = "Contoso", .name = "Thermostat", .version = "1.0.0" },
};

uint8_t props_cache[256];
az_iot_adu_client_init(&adu, &twin, &hooks, &crypto,
                       root_keys, rk_count, &props, props_cache, sizeof props_cache);

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

- **Targeting the current protocol:** manifest v5, JWS/RS256 trust chain, agent
  state + per-step result reporting, accept/reject, retry/replacement,
  cancellation, multi-step, SHA-256 integrity, reboot/resume.
- **Deferred (not v1):** Root Key Package runtime rotation (firmware-delivered
  key updates used instead initially).
- **Out of scope:** delta updates, delivery optimization, reference/proxy/nested
  updates, component targeting, diagnostics/log upload.
- **Ask:** visibility into upcoming manifest schema changes so we can validate
  forward-compatibility.

---

# Part B — ADUv2 (HTTPS Pull, ADR)

ADUv2 keeps the same update *content* (manifest v5, JWS/RS256 signing, SHA-256
integrity, multi-step install/apply) but changes how a device **acquires** that
content and **reports** status: a **pull** model over a dedicated **HTTPS**
endpoint, decoupled from IoT Hub, authenticated with an **X.509** cert, with
device/updating properties stored in **Azure Device Registry (ADR)**. This part
describes the device-side differences we must address and the design options we
are considering. We are sharing this early to align before any wire contract is
locked.

## 5. Device-Side Differences to Address

| Area | ADUv1 today | ADUv2 needs | Impact on the SDK |
|---|---|---|---|
| **Delivery model** | Service **pushes** a deployment to twin desired properties | Device **pulls** by polling the HTTPS endpoint | New manifest-acquisition path + a poll scheduler; the verify/download/install pipeline is unchanged |
| **Status / property reporting** | Twin **reported properties** (MQTT) | Reported to **ADR** over HTTP | A second report-delivery path; the report *content* (agent state, device props, step results) is the same |
| **Authentication** | Carried by the IoT Hub connection (SAS / X.509) | Standalone customer-managed **X.509** cert (onboarding / operational) on the HTTP channel | Need an HTTPS+TLS client with client-certificate auth (platform hook) |
| **Connection dependency** | Requires an open IoT Hub MQTT connection + `az_iot_twin_client` | No Hub connection; ADU endpoint only | `az_iot_connection_client` / `az_iot_twin_client` are not on the ADUv2 path |
| **Cancellation & retry** | Push-based cancel + service-initiated retry via twin | Pull/poll semantics (no push channel) | State-machine triggers for cancel/retry must come from poll responses instead of twin deltas |
| **Endpoint discovery** | Implicit (Hub/twin) | Stable, versioned ADU endpoint, possibly provided via **DPS** at provisioning | Endpoint configuration input; optional DPS integration |
| **Day0 / stale device** | n/a (always via Hub) | Device too old to reach DPS/Hub/ADU can still recover via an **unauthenticated HTTP** recovery endpoint (no TLS) | Reuses the **existing manifest signing / root-key model** for trust; adds two client-side checks (see §8). Distinct from the normal ADUv2 HTTPS+X.509 operational path |

**Unchanged (shared core):** manifest v5 parse/format, JWS/SJWK parsing, `kid`
resolution, RS256 enforcement, SHA-256 integrity, and the
download/install/apply/backup/restore + reboot/resume state machine.

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
twin-backed one (v1) and an HTTPS/ADR-backed one (v2).

```c
typedef struct {
    /* Pull/receive the next assigned manifest, if any. Non-blocking. */
    az_iot_result_t (*get_manifest_fn)(void* ctx, az_span* out_manifest, bool* out_available);
    /* Deliver an agent-state / device-properties / step-results report. */
    az_iot_result_t (*report_fn)(void* ctx, az_span report_json);
    void* ctx;
} az_iot_adu_transport_t;

/* v1: twin-backed transport (push arrives as desired-property deltas) */
az_iot_adu_transport_t t = az_iot_adu_transport_twin(&twin);

/* v2: HTTPS/ADR-backed transport (poll + report over HTTP w/ X.509) */
az_iot_adu_transport_t t = az_iot_adu_transport_http(&adu_http /* endpoint, X.509, poll cfg */);

az_iot_adu_client_init(&adu, &t, &hooks, &crypto,
                       root_keys, rk_count, &props, buf, sizeof buf);
```

- **Pros:** one engine, one test surface for verify/download/install; v1 and v2
  differ only in a thin transport.
- **Cons:** the transport interface must generalize both push (twin deltas) and
  pull (polling) plus two different report channels — a slightly leaky abstraction.

### Approach 2 — Dedicated `az_adu_client` for ADUv2 (HTTP-only)

Keep `az_iot_adu_client` (twin) as the v1 client and add a separate
`az_adu_client` purpose-built for the HTTPS/ADR protocol. Both call into the
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
shared **ADU engine**. `az_iot_adu_client` (twin) and `az_adu_client` (HTTPS/ADR)
become thin transport + lifecycle wrappers over that engine.

```c
/* Shared, transport-free engine. */
az_iot_adu_engine_init(&engine, &hooks, &crypto,
                       root_keys, rk_count, &props, buf, sizeof buf);

az_iot_adu_engine_submit_manifest(&engine, manifest_span); /* whoever fetched it */
az_iot_adu_engine_do_work(&engine);                        /* verify → download → install */
az_iot_adu_engine_collect_report(&engine, &report_span);   /* transport sends it onward */
```

The v1 wrapper feeds manifests from twin desired properties and sends reports to
twin reported properties; the v2 wrapper polls the HTTPS endpoint and reports to
ADR. This combines Approach 1's clean, single engine with Approach 2's
protocol-idiomatic edges: it maximizes shared, well-tested code while letting
each transport stay natural.

- **Pros:** maximal shared/tested code (one engine, one state machine) while each
  client stays protocol-idiomatic; no leaky combined-transport abstraction.
- **Cons:** more up-front refactoring to extract the engine, and a third public
  surface (the engine) to design and version.

## 7. Shared Core (both clients)

Regardless of approach, the following is identical for ADUv1 and ADUv2:

- **Manifest v5 parse/format** — the `az_iot_adu_client` module from
  `azure-sdk-for-c`, unchanged.
- **Cryptographic verification** — JWS/SJWK parsing, `kid` resolution, **RS256**
  enforcement, and **SHA-256** integrity, via the same `verify_rs256_fn` /
  `sha256_*` hooks (§2).
- **Update state machine** — download / install / apply / backup / restore +
  reboot / resume; protocol-independent and reused verbatim.

Only **manifest acquisition** and **report delivery** differ between v1
(twin / MQTT) and v2 (HTTPS / ADR).

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

## 9. Open Questions for the Service Team

- The exact device→ADU **HTTP wire contract**: poll request/response shape
  (method, path, headers, JSON envelope), recommended polling cadence and any
  server-driven backoff/`Retry-After`, and — most important for code reuse —
  whether the manifest v5 + JWS payload is byte-identical to ADUv1. If it is, the
  shared core (§7) is untouched and only the transport differs; if it diverges,
  we need to know the deltas early to size the parser/verifier impact.
- **Authentication** lifecycle: onboarding vs operational (Cert Management) cert
  — when each is presented, and how the client cert is renewed/rotated before
  expiry (does the device fetch a new cert in-band, or is rotation tied to an
  update?). This drives whether the SDK needs a cert-renewal state, or whether
  the platform hook owns the cert store entirely.
- **Endpoint discovery**: is the operational ADU endpoint DPS-provided during
  provisioning, statically stamped at manufacturing, or returned by an earlier
  bootstrap call? This determines what configuration the device must persist and
  whether the SDK needs a discovery step before the first poll.
- **ADR reporting** schema for updating/device properties over HTTP: the exact
  payload shape and endpoint for agent state, device properties, and per-step
  results. We expect the report *content* to match ADUv1, but need to confirm
  the encoding/transport so the report path is the only thing that changes.
- **Cancellation / retry** semantics in a pull model (no push channel): how does
  a device learn a deployment was cancelled or should be retried — a flag/field
  in the poll response, a superseding workflow id, or a distinct status code? The
  SDK needs a concrete signal to map onto its existing cancel/retry state-machine
  triggers.
- **Day0 endpoint discovery / DPS coupling** (deep DPS integration vs DPS-as-façade
  vs no integration) and whether late binding of the ADU account will be
  supported — these affect what the device is stamped with at manufacturing
  (DPS endpoint + scope vs ADU account ID + endpoint) and therefore what the SDK
  must store and present on the recovery path. The device-side *trust* mechanism
  itself is settled (§8); only the addressing/identity inputs are open.
