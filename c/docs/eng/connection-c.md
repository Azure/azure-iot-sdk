# Connection and Reconnection Lifecycle — C client

C-specific reference for the connection lifecycle: identifiers, file locations, enum values and
defaults as implemented in [c/src/core/connection_client.c](../../src/core/connection_client.c),
which is the current source of truth for the design.

The language-neutral version of this design, which both the C and .NET clients are held to, is
[connection.md](../connection.md). Read that first for the contract; read this one for the C
implementation of it.

Related documents:

- [design.md](../design.md) — overall layering and adapter model.
- [connection-impl-status.md](connection-impl-status.md) — per-client coverage of the contract.
- [certificate-management.md](certificate-management.md) — CSR / operational certificate design.
- [client-separation.md](client-separation.md) — connection profile (§2) and the ADU channel split (§8).
- [connection-state-and-error-propagation.md](connection-state-and-error-propagation.md) — observer registry and status codes.
- [dps-integration.md](../dps-integration.md), [devnotes.md](../devnotes.md) — DPS contract and the running requirements log.
- **ADUv2** — [aduv2-spec.md](aduv2-spec.md) is the device contract and the source for §7
  below; it owns the request/response shapes, error codes and trust model, which are deliberately not
  restated here. [adu-client-design.md](adu-client-design.md) covers the shared verify/download/install
  engine, which is unchanged from ADUv1.

### Status legend

This document describes the target lifecycle. Not all of it is coded yet, so every section is marked:

| Mark | Meaning |
| --- | --- |
| **[implemented]** | Present in `c/src` today and covered by tests. |
| **[planned]** | Designed and agreed, not yet in code. |

---

## 1. Vocabulary

| Term | Meaning |
| --- | --- |
| **State** | User-visible connection lifecycle value (`az_iot_connection_state`). Reported through the state callback. |
| **Connection profile** | What the device is connected to, as declared by DPS: `classic` or `mqttV5` (`az_iot_connection_profile`). Not caller-settable. |
| **Generation** | The feature-client family selected by the profile: `gen1` (classic) or `gen2` (mqttV5). |
| **Role** | Which endpoint/protocol the current MQTT session targets (`az_iot_mqtt_role`): `DPS` (v3.1.1), `HUB_CLASSIC` (v3.1.1), `HUB_NEXT` (v5). |
| **Phase** | Internal sub-step inside a state — DPS phases and presence (birth) phases. Not user-visible. |
| **Provisioning** | Obtaining a hub assignment from DPS. |
| **Onboarding** | Everything that happens against the DPS gateway under *onboarding auth*: the bootstrap update check, the CSR, and registration itself. |
| **Renewal** | The recurring, post-provisioning counterpart under *operational auth*: certificate re-issuance and the periodic ADUv2 update check. |
| **Bootstrap credential** | Initial device identity (`AZ_IOT_CRED_BOOTSTRAP`). |
| **Operational credential** | Certificate issued to the device via CSR (`AZ_IOT_CRED_OPERATIONAL`). |
| **ADU channel** | The `az_iot_adu_channel` vtable that carries update delivery and reporting, keeping `adu_core` transport-independent. |

---

## 2. Top-level state machine **[implemented]**

States are defined in
[az_iot_connection_client.h](../../inc/azure/iot/az_iot_connection_client.h); transitions are all funneled
through the internal `transition()` helper, which is also what raises the user state callback.

```mermaid
stateDiagram-v2
    direction LR
    [*] --> IDLE
    IDLE --> CONNECTING: open()
    CONNECTING --> CONNECTED: CONNACK ok, handshake done
    CONNECTING --> RECONNECTING: error, drop or timeout
    CONNECTING --> FAULTED: error, reconnect disabled
    CONNECTED --> RECONNECTING: unexpected drop
    CONNECTED --> DISCONNECTING: close()
    RECONNECTING --> CONNECTING: backoff elapsed
    RECONNECTING --> FAULTED: attempts exhausted
    RECONNECTING --> IDLE: close()
    DISCONNECTING --> IDLE: transport closed
    FAULTED --> CONNECTING: open()
    IDLE --> [*]: destroy()
```

When DPS is configured the whole provisioning exchange happens **inside** the `CONNECTING` state, so
the application never sees an intermediate `CONNECTED` for the DPS session. The DPS progress is
tracked separately as a phase:

```mermaid
stateDiagram-v2
    direction LR
    [*] --> DPS_CONNECTING: open() with id_scope
    DPS_CONNECTING --> DPS_SUBSCRIBING: CONNACK ok
    DPS_SUBSCRIBING --> DPS_REGISTERING: SUBACK
    DPS_REGISTERING --> DPS_POLLING: assigning
    DPS_POLLING --> DPS_POLLING: retry-after elapsed
    DPS_POLLING --> DPS_DONE: assigned
    DPS_REGISTERING --> DPS_DONE: assigned
    DPS_DONE --> [*]: hub host and device id applied
```

A transport drop or transient session failure during a DPS phase is handled by the same backoff path
as a hub failure, and a retry restarts provisioning from `DPS_CONNECTING`. A *registration* failure
is not: `dps_apply_deferred()` transitions to `FAULTED`. See §5.2 for the split.

---

## 3. Full connect sequence **[implemented]**

```mermaid
sequenceDiagram
    autonumber
    participant App
    participant Conn as Connection client
    participant Cert as Certificate provider
    participant DPS
    participant Hub as IoT Hub / Event Grid

    App->>Conn: open(options)
    Conn->>Conn: state = CONNECTING

    alt DPS configured (id_scope present)
        Conn->>Cert: load(BOOTSTRAP)
        Cert-->>Conn: CA + client cert/key
        opt request_operational_certificate
            Conn->>Cert: get_csr(registration_id)
            Cert-->>Conn: base64 DER CSR
        end
        Conn->>DPS: MQTT v3.1.1 CONNECT (X.509 bootstrap)
        DPS-->>Conn: CONNACK
        Conn->>DPS: SUBSCRIBE $dps/registrations/res/#
        DPS-->>Conn: SUBACK
        Conn->>DPS: PUBLISH register (payload may include the CSR)
        loop while assigning
            DPS-->>Conn: status = assigning (operation_id, retry-after)
            Conn->>DPS: PUBLISH get operation status
        end
        DPS-->>Conn: status = assigned (assignedHub, deviceId, connectionProfile, issuedCertificateChain)
        opt issued chain present
            Conn->>Cert: store_issued_certificate(chain)
            Conn-->>App: operational_cert_callback(chain)
        end
        Conn->>DPS: DISCONNECT + tear down v3.1.1 adapter
        Conn->>Conn: resolve connection profile, apply assigned host / client id, pick hub role
    end

    Conn->>Cert: load(OPERATIONAL)
    alt not found
        Conn->>Cert: load(BOOTSTRAP)
    end
    Conn->>Hub: MQTT CONNECT (role-specific username, TLS mutual auth)
    Hub-->>Conn: CONNACK

    alt role = HUB_NEXT (MQTT v5)
        Conn->>Hub: SUBSCRIBE ih/{device_id}/dev/# (QoS 1)
        Hub-->>Conn: SUBACK
        Conn->>Hub: PUBLISH birth (type=birth:1, correlation = connect nonce)
        Hub-->>Conn: birth-ack on ih/{device_id}/dev/presence (nonce echoed)
    end

    Conn->>Hub: re-SUBSCRIBE persistent feature filters (twin, methods, C2D, credentials, ADU channel)
    Conn->>Conn: state = CONNECTED
    Conn-->>App: state callback(CONNECTED)
```

Key ordering guarantees that both clients must honour:

1. `CONNECTED` is announced **after** the birth handshake (Hub-Next) and **after** persistent
   subscriptions have been re-issued, so a feature client never observes `CONNECTED` while its topic
   filters are missing.

   > **Not true of the C client today.** `announce_connected()` transitions to `CONNECTED` first and
   > only then issues the subscribes, discarding the result; SUBACKs are absorbed. So the callback
   > can fire while no filter is established, and a request published from inside it reaches the wire
   > ahead of its own SUBSCRIBE. Treat this as the *intended* contract, not a current guarantee.
   > Tracked as [AB#39366084](https://dev.azure.com/msazure/One/_workitems/edit/39366084) and fixed
   > in phase P1c of [client-separation.md](client-separation.md). The Hub-Next birth
   > handshake in the same diagram *is* correctly gated — it waits for its own SUBACK before
   > publishing birth — which is the shape the feature subscriptions are being moved to.

2. The DPS session is fully torn down before the hub session is created — they are never concurrent,
   and DPS always uses MQTT 3.1.1 even when the hub session uses v5.
3. The operational certificate is preferred over the bootstrap certificate on every connect attempt,
   including reconnects.
4. **The ADUv2 bootstrap update check runs before `open()`, not inside it.** The agent drives the
   onboarding update call against the DPS gateway until the service reports no update, and only then
   does the connection client register. The check is **advisory**: if it fails, the device proceeds
   to register anyway. See [§7](#7-aduv2-onboarding-and-renewal-planned).
5. The DPS assignment is the single delivery point for everything the device learns about its
   placement: hub, device id, connection profile and issued certificate chain.

---

## 4. Connection profile selection **[planned]**

The profile is what DPS says the device landed on. It is **reported, never selected** — there is no
caller-facing knob, and falling back from one profile to another is application logic, not SDK
behaviour. See [client-separation.md §2](client-separation.md) for the full rationale.

```mermaid
flowchart TB
    A["DPS assignment received"] --> B{"connectionProfile"}
    B -->|"classic"| C["AZ_IOT_CONNECTION_PROFILE_CLASSIC<br/>MQTT 3.1.1, role HUB_CLASSIC"]
    B -->|"absent or null"| C
    B -->|"mqttV5"| D["AZ_IOT_CONNECTION_PROFILE_MQTT_V5<br/>MQTT 5, role HUB_NEXT"]
    B -->|"anything else"| E["AZ_IOT_CONNECTION_PROFILE_UNKNOWN"]
    C --> F["gen1 feature clients"]
    D --> G["gen2 feature clients<br/>+ presence handshake<br/>+ ADUv2 channel"]
    E --> H["Connection fails:<br/>AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED<br/>raw string still readable"]
```

| Wire value | Enum | MQTT | Generation |
| --- | --- | --- | --- |
| `"classic"`, absent, or `null` | `AZ_IOT_CONNECTION_PROFILE_CLASSIC` | 3.1.1 | gen1 |
| `"mqttV5"` | `AZ_IOT_CONNECTION_PROFILE_MQTT_V5` | 5 | gen2 |
| anything else | `AZ_IOT_CONNECTION_PROFILE_UNKNOWN` | — | connection fails |

Rules both clients must implement:

- `connectionProfile` is a `readOnly` **string** on `DeviceRegistrationResult`, delivered alongside
  `assignedHub`, `deviceId` and `issuedCertificateChain`. There is no numeric `hub_version` on the wire.
- It is an **extensible union**, so the raw string must be preserved verbatim
  (`az_iot_hub_profile.connection_profile_raw`) and not collapsed into a closed enum. A value newer
  than the SDK still has to be loggable.
- An unrecognised profile **fails the connection** with `AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED`.
  The SDK will not guess which MQTT version to speak.
- The profile is readable only once `CONNECTED`; before that
  `az_iot_connection_client_get_hub_profile()` returns `AZ_IOT_ERR_NOT_CONNECTED`.
- A feature client from the wrong generation is refused with `AZ_IOT_ERR_HUB_GENERATION_MISMATCH`.
  Because of this, feature clients must be created **after** the connection is open.

> **Blocked on the api-version.** `connectionProfile` is new in DPS `2026-11-02-preview`; the SDK
> still requests `2019-03-31` via the vendored `azure-sdk-for-c`, so the field never arrives today.
> Raising it is a prerequisite for this entire section.
>
> **Open:** whether a reconnect can change the generation. If DPS can reassign a device mid-life,
> every feature client the application holds becomes invalid at that moment and it must be told.

---

## 5. Reconnection **[implemented]**

```mermaid
sequenceDiagram
    autonumber
    participant Hub
    participant Conn as Connection client
    participant App

    Hub--xConn: transport drop / CONNACK error / birth timeout
    Conn->>Conn: teardown_active() (destroy adapter, drop pending PUBACKs, reset phases)
    alt user called close()
        Conn->>Conn: state = IDLE
    else reconnect disabled (initial_delay_ms == 0)
        Conn->>Conn: state = FAULTED
        Conn-->>App: state callback(FAULTED, reason)
    else attempts exhausted (attempt > max_attempts)
        Conn->>Conn: state = FAULTED
        Conn-->>App: state callback(FAULTED, reason)
    else
        Conn->>Conn: attempt++, delay = backoff(attempt)
        Conn->>Conn: state = RECONNECTING
        Conn-->>App: state callback(RECONNECTING, reason)
        Note over Conn: do_work() waits until reconnect_due_ms
        Conn->>Conn: start_connect_attempt() -> full sequence of section 3
        Hub-->>Conn: CONNACK ok
        Conn->>Conn: attempt = 0, state = CONNECTED
    end
```

### 5.1 Backoff policy

`az_iot_reconnection_policy` in
[az_iot_connection_client.h](../../inc/azure/iot/az_iot_connection_client.h), computed by
`az_iot_reconnect_delay_ms()` in [reconnect.c](../../src/core/reconnect.c):

```text
base   = min(max_delay_ms, initial_delay_ms << min(attempt - 1, 30))
jitter = uniform(-jitter_pct%, +jitter_pct%) * base
delay  = clamp(base + jitter, 1, max_delay_ms)
```

| Field | Default | Notes |
| --- | --- | --- |
| `initial_delay_ms` | 1000 | `0` disables automatic reconnect entirely. |
| `max_delay_ms` | 30000 | Cap for the exponential term and for the jittered result. |
| `max_attempts` | 0 | `0` means retry forever. |
| `jitter_pct` | 20 | Symmetric randomization, seeded from the monotonic clock. |

The shift is clamped at 30 to avoid 32-bit overflow. The attempt counter resets to zero on every
successful CONNACK.

### 5.2 What triggers a reconnect

- CONNACK with a non-success status.
- Unexpected transport disconnect or adapter error while `CONNECTING` or `CONNECTED`.
- A reconnect attempt that cannot even start the session: `start_connect_attempt()` (or `dps_start()`
  when re-provisioning) returning non-OK schedules another reconnect rather than faulting.
- Any failure in the Hub-Next presence handshake, not only its timeout: `presence_start()` failing
  after CONNACK, the presence SUBACK arriving with a failure status, and `presence_publish_birth()`
  failing all clear the phase and reconnect.
- Hub-Next birth-ack timeout (60 s per handshake step), checked in `_do_work()`.

Not triggers, because they fault instead:

- **DPS registration failure.** `dps_apply_deferred()` transitions to `FAULTED` when the register
  status is non-OK or no assignment arrived, and again if applying the assigned host or device id
  fails. Provisioning is re-run on a reconnect only via the re-provision path below.
- **An unsupported `connectionProfile`.** Faults with
  `AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED` rather than guessing a protocol (§4).

`AZ_IOT_ERR_IDENTITY_REJECTED` on CONNACK is a special case: when DPS is configured and reconnection
is enabled, it sets `reprovision_pending`, so the next attempt runs `dps_start()` for a fresh
assignment instead of reconnecting to the same rejected credential. The flag is cleared before the
attempt, so a failure there falls back to an ordinary retry rather than looping through provisioning.

Every trigger above is conditional on `reconnect_enabled()`: with no retry policy the same conditions
transition to `FAULTED`.

§9 classifies every failure this client can see, including the ones that are retried here but
cannot succeed on retry.

A user-initiated `close()` never triggers a reconnect: it sets an internal `user_close` flag that is
checked before backoff is scheduled.

### 5.3 What is preserved across a reconnect

| Item | Preserved | Behaviour |
| --- | --- | --- |
| Persistent subscriptions | Yes | Re-issued on reconnect. Intended to complete before `CONNECTED` is announced; not yet gated on the SUBACK — see the caveat under §4's ordering guarantees. |
| Assigned hub host / device id | Yes | Cached after the first DPS assignment. |
| Connection profile | Yes | Re-resolved from the new assignment; a change invalidates held feature clients (open question, §4). |
| Operational certificate | Yes | Owned by the certificate provider, reloaded on each attempt. |
| ADU workflow state | Yes | Owned by `adu_core` and persisted, so an install survives a reconnect and a reboot. |
| Reconnect attempt counter | Reset on success | Incremented per failed attempt. |
| In-flight QoS 1 PUBACKs | No | Packet ids belong to the destroyed adapter; callers must re-send. |
| Twin GET/PATCH, method responses, telemetry in flight | No | Feature clients must re-issue. |
| ADU status report not yet acked | Yes | Held in durable storage and retried until acked; idempotent on `workflowId`. |
| Presence (birth) phase | No | Restarted with a freshly generated nonce. |
| DPS phase | No | Restarted from `CONNECTING` if DPS is configured. |
| In-flight CSR operation | No | Abandoned; the callback fires with a failure/timeout result. |

---

## 6. Certificate management: onboarding and renewal **[implemented]**

Two distinct issuance paths exist; both end with the certificate provider owning the operational
credential, and neither forces an immediate reconnect.

```mermaid
flowchart TD
    A["open with DPS +<br/>request_operational_certificate"] --> B[get_csr from provider]
    B --> C["DPS register with csr in payload"]
    C --> D{assigned?}
    D -->|"yes, chain present"| E["store_issued_certificate +<br/>operational_cert_callback"]
    D -->|"yes, no chain"| F["continue with bootstrap credential"]
    E --> G["Hub CONNECT using operational credential"]
    F --> G

    G --> H[CONNECTED]
    H --> I["send_csr renewal<br/>classic hub only"]
    I --> J["PUBLISH issueCertificate request"]
    J --> K{credentials response}
    K -->|202 accepted| L["CSR_ACCEPTED callback,<br/>keep waiting"]
    L --> K
    K -->|200 issued| M["CSR_ISSUED callback with chain,<br/>app persists"]
    K -->|"error or 120s timeout"| N["CSR_FAILED callback,<br/>slot released"]
    M --> O["New certificate is used on<br/>the NEXT connect attempt"]
```

Renewal topics (classic hub):

| Direction | Topic |
| --- | --- |
| Publish | `$iothub/credentials/POST/issueCertificate/?$rid={request_id}` |
| Subscribe | `$iothub/credentials/res/#` |
| Response | `$iothub/credentials/res/{status}/{request_id}` |

Rules that apply to both clients:

- Only one CSR operation may be in flight; a second request fails fast with a *busy* result.
- The issued chain is delivered leaf-first as base64 DER and is only valid for the duration of the
  callback — the application must copy or persist it.
- A successful renewal does **not** tear down the live session. The new credential takes effect on
  the next connect, whether that is a reconnect or an explicit reopen.
- At connect time the provider is asked for `OPERATIONAL` first and falls back to `BOOTSTRAP` when
  the operational credential is absent or uninitialized.
- CSR-based DPS enrollment uses the `2025-07-01-preview` DPS API version and requires a
  caller-provided CSR payload buffer of at least `AZ_IOT_CSR_PAYLOAD_BUFFER_MIN` bytes.

---

## 7. ADUv2: onboarding and renewal **[planned]**

**ADUv1 is cut.** Its Twin-based public API is being removed; what survives is everything that has
nothing to do with transport. ADU is re-layered into a transport-independent **`adu_core`** —
manifest v5 parsing, JWS/SJWK verification, root keys, SHA-256 integrity, the
download/backup/install/apply state machine, and reboot/resume persistence — plus an
**`az_iot_adu_channel`** vtable carrying delivery and reporting.

ADUv2 is a **device-initiated pull protocol**, and its device-facing delivery moved **off** a
dedicated ADU HTTPS endpoint. The agent calls an updating operation on a gateway it already talks to,
reusing the credential it already has; the gateway is an authenticated pass-through to ADR and then
ADU. The device never talks to ADU directly, needs no ADU-specific credential, and there is no twin,
no subscription and no unsolicited offer.

| Phase | Gateway | Operation (working name) | ADU route |
| --- | --- | --- | --- |
| First-time / bootstrap (**before** provisioning) | DPS | `GetOnboardingDeviceUpdate` | `POST /devices/requestOnboardingUpdates` |
| Regular / operational (**after** provisioning) | DPS *(Ignite '26 interim)*, IoT Hub *(post-Ignite)* | `GetDeviceUpdate` | `POST /devices/requestUpdates` |
| Reporting, either phase | same gateway as the fetch | `ReportDeviceUpdateStatus` | `POST /devices/reportStatus` |

The device selects onboarding vs regular **by which operation it calls**; the gateway does not infer
or validate the choice.

> **The Hub updating API is not ready for Ignite '26.** In preview, DPS fronts both the bootstrap and
> the operational flow, using the existing DPS device credential (X.509 in phase 1). The operational
> path moves to IoT Hub afterwards **with no device-contract change** — same request and response, a
> different gateway. Treat the gateway as a channel parameter, not a constant.

### 7.1 Onboarding — bootstrap update, before provisioning

The critical ordering fact for this document: **the bootstrap update check happens before the device
registers.** The agent, not the connection client, drives it. On the happy path `open()` waits until
the check reports no update, so several updates can chain before provisioning.

The check is **advisory and must never block provisioning.** If it errors, times out, or the account
is not linked, the device proceeds to register anyway.

```mermaid
sequenceDiagram
    autonumber
    participant ADU as adu_core + DPS channel
    participant Conn as Connection client
    participant DPS
    participant Hub

    loop until "no update" or an advisory failure
        ADU->>DPS: GetOnboardingDeviceUpdate (agentInfo, installedUpdateId, ETags)
        alt update available
            DPS-->>ADU: serviceConfiguration + updateMetadata (workflowId, manifest, signature, fileUrls)
            ADU->>ADU: verify signature, download fileUrls, install (reboot if required)
            ADU->>DPS: ReportDeviceUpdateStatus (workflowId, installedUpdateId, lastInstallResult)
        else no update
            DPS-->>ADU: 200 with updateMetadata omitted
        end
    end

    Note over ADU,Conn: provisioning proceeds - on success or on an advisory failure
    Conn->>DPS: Register (unchanged, CSR optional)
    DPS-->>Conn: assignedHub, deviceId, connectionProfile, issuedCertificateChain
    Conn->>Hub: CONNECT with operational auth
```

- The loop is genuine: after installing a bootstrap update the agent re-checks, because a bootstrap
  deployment can chain.
- Bootstrap progress is stored **in the bootstrap update job**, not on the device's ADR attributes —
  the device resource does not exist yet.
- Trust comes from the **root-key package** at `serviceConfiguration.rootKeyDownloadUrl` returned by
  the same call. Account scoping (`accountId` bound into the manifest signature) is **deferred past
  Ignite '26** — DPS returns no `accountId`, so the device verifies provenance-from-ADU but not
  account scoping. Base signature validation stays required.
- `fileUrls` are **not** covered by the signed manifest; payloads are downloaded straight from blob
  storage and integrity comes from the per-file hashes inside the manifest.
- Bootstrap orchestration is entirely the agent's responsibility for Ignite '26. DPS does **not**
  enforce that a device is on a given update version before provisioning it.

### 7.2 Renewal — operational update check, after `CONNECTED`

```mermaid
sequenceDiagram
    autonumber
    participant Conn as Connection client
    participant ADU as adu_core + channel
    participant GW as Gateway (Hub, or DPS in preview)
    participant Store as Durable ADU state

    Conn-->>ADU: CONNECTED
    ADU->>Store: load_state()
    Store-->>ADU: installedUpdateId, ETags, unsent report
    opt report pending from a previous session
        ADU->>GW: ReportDeviceUpdateStatus (workflowId, lastInstallResult)
    end

    loop poll at the agent's own cadence
        ADU->>GW: GetDeviceUpdate (agentInfo, installedUpdateId, ETags)
        Note over GW: ADU derives the device class from agentProfile + compatibilityProperties
        alt update available
            GW-->>ADU: serviceConfiguration + updateMetadata (workflowId, manifest, signature, fileUrls)
            ADU->>ADU: verify, download, backup, install, apply
            ADU->>Store: persist_state()
            ADU->>GW: ReportDeviceUpdateStatus (workflowId, installedUpdateId, lastInstallResult)
        else no update
            GW-->>ADU: 200 with updateMetadata omitted
        end
    end

    Conn--xADU: connection drop
    Note over ADU: install continues, report held in durable storage
    Conn-->>ADU: CONNECTED again
    ADU->>GW: retry the report until acked, then resume polling
```

`lastInstallResult` carries the terminal outcome, its failure origin, the hex `extendedResultCodes`
list and a per-step `stepResults` map — see [aduv2-spec.md](aduv2-spec.md) for the field-level
shape.

### 7.3 Rules both clients must implement

- **Poll, never wait.** The agent owns the cadence. A missed poll is not an error and there is no
  offer to lose, which is why a reconnect needs no replay of ADU subscriptions — there are none.
- **`workflowId` is the correlation key.** It arrives in `updateMetadata` and is echoed on the
  report. Reporting is **idempotent on `workflowId` alone**; a conflicting terminal result for the
  same id is rejected as a conflict.
- **The device is the sole retrier.** The gateway fails fast with one attempt per hop. The agent
  honours `Retry-After` on throttling and retries `ReportDeviceUpdateStatus` until it is acked — a
  report is a durable write and must not be lost.
- **Drive behaviour from the machine-readable error code, never the HTTP status.** A stale
  `agentInfoETag` means resend the full `agentInfo`; a stale `serviceConfigETag` means re-ask without
  it; an unlinked update account means "no update service configured", which is not a failure.
- **"No update" is a success.** It is a 200 with the update metadata omitted, not an error.
- **ADU never drives the connection.** It does not open, close, or force a reconnect. It does
  sequence ahead of the *first* `open()`, via the advisory bootstrap check in §7.1.
- **Compatibility properties are opaque key/value pairs** (1–5) reported by the agent alongside an
  opaque `agentProfile`; the service combines them into a device class. The agent assigns them no
  meaning.
- **The gateway is a channel parameter.** Bootstrap always uses DPS; operational uses DPS in the
  Ignite '26 preview and IoT Hub afterwards, with no device-contract change. This SDK plans to
  expose the operational channel as a gen2 feature client
  ([client-separation.md](client-separation.md) §8), but the service contract binds ADU to
  the updating operations, not to a connection profile.

---

## 8. Combined reference diagram

One picture of the whole device lifecycle: connection and reconnection, connection-profile selection,
certificate management (onboarding + renewal) and ADUv2 (onboarding + renewal). Dotted edges are
deferred effects — they do not happen inline.

```mermaid
flowchart TB
    BOOT["Agent boot"] --> BCHK["ADUv2 bootstrap check<br/>GetOnboardingDeviceUpdate via DPS"]
    BCHK -->|"update available"| BINST["Verify, download, install,<br/>report, re-check"]
    BINST --> BCHK
    BCHK -->|"no update, or advisory failure"| IDLE["IDLE"]

    IDLE -->|"open() with id_scope"| REG["DPS register<br/>CSR optional"]
    IDLE -->|"open() with host"| CRED

    REG --> ASSIGN["Assignment:<br/>assignedHub, deviceId,<br/>connectionProfile,<br/>issuedCertificateChain"]
    ASSIGN --> STORE1["Store issued chain"]
    STORE1 --> PROFILE{"connectionProfile"}

    PROFILE -->|"unknown profile"| FAULTED
    PROFILE -->|"classic - gen1"| CRED
    PROFILE -->|"mqttV5 - gen2"| CRED["Load credential:<br/>operational, else bootstrap"]

    CRED --> CONNECTING["CONNECTING<br/>MQTT CONNECT + mutual TLS"]
    CONNECTING --> BIRTH["Presence handshake<br/>gen2 only"]
    BIRTH --> SUBS["Replay persistent subscriptions"]
    CONNECTING --> SUBS
    SUBS --> CONNECTED["CONNECTED"]

    CONNECTED --> CRENEW["Cert renewal:<br/>send_csr, 202 then 200"]
    CRENEW -.->|"new chain used on<br/>the next connect"| CRED
    CONNECTED --> ARENEW["ADUv2 operational check:<br/>poll GetDeviceUpdate,<br/>ReportDeviceUpdateStatus"]

    CONNECTED -->|"close()"| DISC["DISCONNECTING"] --> IDLE
    CONNECTED --> DROP{"drop or error"}
    DROP -->|"reconnect disabled<br/>or attempts exhausted"| FAULTED["FAULTED"]
    DROP -->|"reconnect enabled"| RECON["RECONNECTING<br/>exponential backoff + jitter"]
    RECON -->|"DPS configured"| REG
    RECON -->|"direct host"| CRED
    ARENEW -.->|"workflowId and unsent<br/>report persisted"| RECON
```

Reading it as four overlapping concerns:

| Concern | Onboarding (DPS gateway, onboarding auth) | Renewal (post-`CONNECTED`, operational auth) |
| --- | --- | --- |
| **Certificates** | CSR in the registration, issued chain in the assignment | `send_csr` over the hub; new chain applies on the next connect |
| **ADUv2** | `GetOnboardingDeviceUpdate` loop **before** registration, advisory | Polled `GetDeviceUpdate` / `ReportDeviceUpdateStatus` (DPS in preview, Hub afterwards) |
| **Connection profile** | Declared in the assignment; selects MQTT version and generation | Re-resolved on every reconnect that goes through DPS |
| **Connection** | DPS phases inside `CONNECTING` | Backoff-driven reconnect replays the whole path |

The two onboarding concerns are **not** symmetric, and that asymmetry is the thing to remember:
the CSR travels *inside* registration, while the ADU bootstrap check happens *before* it and must
complete first.

---

## 9. Connection failure realization (C) **[partly implemented]**

The C realization of the taxonomy in [connection.md §9](../connection.md#9-connection-failure-taxonomy).
That section says what *any* client must do; this one says what the C client **actually does today**,
which result value it produces, and which component decides.

Read the two together. Where a row's class in the generic table and the SDK action here disagree,
that is a gap, and it is called out in the notes rather than smoothed over.

Per-client status — what C does versus what .NET does — is not repeated here; it lives in
[connection-impl-status.md](connection-impl-status.md).

### 9.1 Result vocabulary

The full set is in [az_iot_result.h](../../inc/azure/iot/az_iot_result.h). The values that appear on a
connection failure path:

| Value | Meaning on the connection path |
| --- | --- |
| `AZ_IOT_OK` | Success. |
| `AZ_IOT_ERR_INVALID_ARG` | A caller argument was rejected at the SDK boundary, or a bound was exceeded that is a caller error. |
| `AZ_IOT_ERR_NOT_INITIALIZED` | The operation needs a live adapter and there is none. |
| `AZ_IOT_ERR_NOT_CONNECTED` | Substituted as the reason when a disconnect carries no status of its own. |
| `AZ_IOT_ERR_TIMEOUT` | A deadline expired — birth-ack, CSR operation. |
| `AZ_IOT_ERR_PROTOCOL` | A service payload could not be parsed. |
| `AZ_IOT_ERR_MQTT` | The catch-all for transport and broker failures. **The great majority of wire failures land here**, including every CONNACK code that is not an identity refusal, every refused SUBACK, and every failed PUBACK. |
| `AZ_IOT_ERR_DPS` | Registration returned a failed or disabled status. |
| `AZ_IOT_ERR_NOT_SUPPORTED` | No adapter factory for the required protocol version; a fixed-size registry is full; a service-supplied string is longer than its buffer. |
| `AZ_IOT_ERR_BUSY` | A single-slot operation is already in flight, or the service is throttling. |
| `AZ_IOT_ERR_NOT_ENOUGH_SPACE` | A compile-time buffer bound was exceeded. |
| `AZ_IOT_ERR_NOT_FOUND` | A required field was absent from a service payload. |
| `AZ_IOT_ERR_INTERNAL` | A dependency call failed in a way the SDK cannot attribute. |
| `AZ_IOT_ERR_IDENTITY_REJECTED` | The broker refused *who the device claims to be*. The only result that drives re-provisioning. |
| `AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED` | DPS assigned a `connectionProfile` this build does not know. |

Two values exist in the enum but are **never produced on the connection path**:
`AZ_IOT_ERR_TLS` and `AZ_IOT_ERR_AUTH`. Every TLS failure — handshake, certificate, cipher — reaches
the core as `AZ_IOT_ERR_MQTT`, because the Paho adapter signals its own failures with negative codes
and [`az_iot_mqtt_connack_result()`](../../src/core/mqtt_iface.c) maps every negative code to
`AZ_IOT_ERR_MQTT` by design (a failure that never reached a broker carries no verdict about the
identity). The consequence is recorded in [§9.5](#95-known-gaps).

### 9.2 CONNACK mapping

`az_iot_mqtt_connack_result(version, connack_code)` in
[mqtt_iface.c](../../src/core/mqtt_iface.c) is the single decision point. Adapters are **required** to
call it rather than reporting a blanket `AZ_IOT_ERR_MQTT` — the rule is normative in
[how_to_byo_mqtt_client.md](../how_to_byo_mqtt_client.md).

| Input | Result | Why |
| --- | --- | --- |
| `0` (either version) | `AZ_IOT_OK` | Accepted. |
| Any **negative** code | `AZ_IOT_ERR_MQTT` | The adapter's own failure — refused socket, TLS handshake, client-library error. It never reached a broker, so it says nothing about the identity. |
| v3.1.1 `2 identifier rejected`, `4 bad user name or password`, `5 not authorized` | `AZ_IOT_ERR_IDENTITY_REJECTED` | The broker refused the identity. |
| v3.1.1 `1 unacceptable protocol version` | `AZ_IOT_ERR_MQTT` | **Deliberately excluded** from the identity set: it says nothing about who the device claims to be. |
| v3.1.1 `3 Connection Refused, Server unavailable` | `AZ_IOT_ERR_MQTT` | **Deliberately excluded**: the canonical transient failure. |
| v5 `0x85 Client Identifier not valid`, `0x86 Bad User Name or Password`, `0x87 Not authorized`, `0x8C Bad authentication method` | `AZ_IOT_ERR_IDENTITY_REJECTED` | `0x86` is included alongside the three the core strictly needs because it is the v5 spelling of v3.1.1's `4`; treating the same refusal differently per protocol version would make the re-provisioning trigger depend on the hub flavour. |
| Every other non-zero v5 code | `AZ_IOT_ERR_MQTT` | Not an identity verdict. |
| Any code, with a version the function does not know | `AZ_IOT_ERR_MQTT` | The two schemes overlap numerically — `2`, `4` and `5` are identity refusals in v3.1.1 and mean something else entirely in v5 — so guessing a scheme would be guessing whether to abandon a credential. This is a public entry point that adapters call with a version they supply, so the value is genuinely untrusted. |

`AZ_IOT_ERR_IDENTITY_REJECTED` is the **only** result that changes where the next attempt goes: with
DPS configured and reconnection enabled it sets `reprovision_pending`, so the retry runs `dps_start()`
for a fresh assignment (§5.2). Everything else retries against the same endpoint.

### 9.3 Compile-time bounds

Buffers are fixed-size struct members, not allocations, so exceeding one is a hard failure rather than
a slow path. Every constant is `#ifndef`-guarded and can be raised at build time. From
[az_iot_connection_client.h](../../inc/azure/iot/az_iot_connection_client.h) unless noted.

| Constant | Value | What it bounds | Result when exceeded |
| --- | --- | --- | --- |
| `AZ_IOT_MAX_MQTT_FACTORIES` | 4 | Registered adapter factories | `AZ_IOT_ERR_NOT_SUPPORTED` |
| `AZ_IOT_MAX_PENDING_PUBACKS` | 16 | QoS-1 publishes awaiting a PUBACK **with an ack callback** | `AZ_IOT_ERR_NOT_SUPPORTED` — the publish itself already succeeded |
| `AZ_IOT_MAX_PERSISTENT_SUBS` | 8 | Persistent subscription registry | `AZ_IOT_ERR_NOT_ENOUGH_SPACE` |
| `AZ_IOT_PERSISTENT_SUB_TOPIC_MAX` | 128 | Persistent topic-filter string | `AZ_IOT_ERR_INVALID_ARG` |
| `AZ_IOT_MAX_SESSION_HANDLERS` | 4 | Session-end handlers (re-registering the same context upserts) | `AZ_IOT_ERR_NOT_SUPPORTED` |
| `AZ_IOT_MAX_INBOUND_HANDLERS` | 8 | Inbound dispatch table ([az_iot_dispatch.h](../../inc/azure/iot/az_iot_dispatch.h)) | `AZ_IOT_ERR_NOT_SUPPORTED` |
| `AZ_IOT_DISPATCH_PREFIX_MAX` | 128 | Dispatch topic prefix | `AZ_IOT_ERR_NOT_SUPPORTED` |
| `AZ_IOT_DPS_HOST_BUF` | 128 | Assigned hub hostname | `AZ_IOT_ERR_NOT_SUPPORTED` on the DPS path; `AZ_IOT_ERR_NOT_ENOUGH_SPACE` from `__set_host()` |
| `AZ_IOT_DPS_DEVICE_ID_BUF` | 128 | Assigned device id | as above |
| `AZ_IOT_DPS_TOPIC_BUF` | 256 | DPS register / query publish topic | `AZ_IOT_ERR_INTERNAL` |
| `AZ_IOT_MQTT_USERNAME_BUF` | 256 | Hub username | Hub-Next: `AZ_IOT_ERR_NOT_ENOUGH_SPACE`. Classic: **no explicit error** — the connect proceeds without a username. See [§9.5](#95-known-gaps). |
| `AZ_IOT_PRESENCE_TOPIC_BUF` | 256 | Presence topics | `AZ_IOT_ERR_NOT_ENOUGH_SPACE` |
| `AZ_IOT_CONNECTION_PROFILE_RAW_BUF` | 64 | Raw `connectionProfile` string | Truncated, resolves to UNKNOWN, then `AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED` |
| `AZ_IOT_CSR_PAYLOAD_BUFFER_MIN` | 8448 | Minimum caller-supplied CSR payload buffer | `AZ_IOT_ERR_NOT_ENOUGH_SPACE` |
| `CSR_MAX_BASE64` | 8192 | Base64 CSR body ([connection_client.c](../../src/core/connection_client.c)) | `AZ_IOT_ERR_INVALID_ARG` |
| `AZ_IOT_TWIN_MAX_PENDING` | 8 | Pending twin requests ([az_iot_twin_client.h](../../inc/azure/iot/az_iot_twin_client.h)) | `AZ_IOT_ERR_NOT_SUPPORTED` |
| `AZ_IOT_DM_MAX_INFLIGHT` | 4 | In-flight direct-method requests ([az_iot_direct_method_client.h](../../inc/azure/iot/az_iot_direct_method_client.h)) | **No result** — the invocation is dropped and a warning is logged. See [§9.5](#95-known-gaps). |
| CSR slot | 1 | In-flight hub CSR renewals | `AZ_IOT_ERR_BUSY` |
| `AZ_IOT_DEFAULT_CONNECT_TIMEOUT_SECONDS` | 30 | Connect attempt, hub and DPS alike | Adapter-reported failure → `AZ_IOT_ERR_MQTT` |
| `AZ_IOT_DEFAULT_KEEP_ALIVE_SECONDS` | 30 | MQTT keep-alive in CONNECT | — |
| `AZ_IOT_PRESENCE_BIRTH_ACK_TIMEOUT_MS` | 60000 | Each presence handshake step | `AZ_IOT_ERR_TIMEOUT` |
| `CSR_OP_TIMEOUT_MS` | 120000 | Hub CSR renewal, re-armed on each `202 Accepted` | `AZ_IOT_ERR_TIMEOUT` |

There is **no subscription-gate deadline**: the gate itself does not exist yet (P1c).

### 9.4 The realization table

**Mapped by** names the component that decides the result: the adapter, the CONNACK mapper, the
connection client itself, or a feature client. Rows marked **[P1c]** describe scoped-but-unmerged
behaviour; rows marked **unverified** are ones where current C behaviour was not established, and are
left as gaps rather than guesses.

#### 9.4.1 Phase 1 — host, network and OS

| Phase | Trigger | Surfaced as | Mapped by | SDK action | Notes / limits |
| --- | --- | --- | --- | --- | --- |
| Name resolution | Hostname does not resolve | `AZ_IOT_ERR_MQTT` | Paho → negative code → `az_iot_mqtt_connack_result()` | `DEFER_RECONNECT` if a policy is configured, else `DEFER_FAULT` | Indistinguishable from every other adapter-side failure. The Paho code is logged but not carried. |
| Address selection | Dual-stack / IPv6-only failure | `AZ_IOT_ERR_MQTT` | as above | as above | Address iteration is Paho's, not the SDK's. |
| Socket connect | Connection refused, unreachable, or connect timeout | `AZ_IOT_ERR_MQTT` | as above | as above | The 30 s connect timeout is passed to the adapter; expiry arrives as an ordinary connect failure. |
| Established session | Reset by peer, or write to a half-closed socket | `AZ_IOT_ERR_NOT_CONNECTED` (substituted when the event carries no status) | Paho `connectionLost` → `AZ_IOT_MQTT_EVT_DISCONNECTED` | `DEFER_RECONNECT` unless `user_close` or no policy, in which case `DEFER_IDLE` | `teardown_active()` drops pending PUBACKs and resets the presence phase. |
| Session bytes | Captive portal returns non-MQTT bytes | `AZ_IOT_ERR_MQTT` | Paho | reconnect | Paho rejects the bytes; the SDK sees an ordinary connect failure. |
| Host clock | Certificate outside its validity window because the clock is wrong | `AZ_IOT_ERR_MQTT` | Paho → negative code | reconnect until the policy is exhausted | **Gap:** classified Terminal generically, retried here. The OpenSSL reason is available in the trace log via `ssl_error_cb`, but not in the result. |
| Host clock | Wall clock steps backwards | none | — | none | Correct today: every deadline uses `az_iot_time_mono_ms()`, a monotonic source. Backoff jitter is seeded from it, never driven by wall time. |

#### 9.4.2 Phase 2 — TLS

| Phase | Trigger | Surfaced as | Mapped by | SDK action | Notes / limits |
| --- | --- | --- | --- | --- | --- |
| Handshake | Any TLS failure — expired, untrusted CA, hostname mismatch, revoked, version or cipher mismatch | `AZ_IOT_ERR_MQTT` | Paho negative code → `az_iot_mqtt_connack_result()` | reconnect if a policy is configured, else fault | **All five collapse to one value.** `AZ_IOT_ERR_TLS` exists in the enum and is never produced. |
| Handshake | TLS alert detail | not in the result | `paho_ssl_error_callback` | logged only | The OpenSSL error queue is drained line by line to the trace log when `AZ_IOT_PAHO_SSL` is built and tracing is enabled. It is the only place the concrete reason appears. |
| Handshake | **Client certificate rejected during the handshake** | `AZ_IOT_ERR_MQTT` | Paho negative code | reconnect | No MQTT session exists, so no CONNACK code is available. Correctly **not** treated as an identity rejection: the negative-code rule exists for exactly this. Consequence: a device whose operational certificate has been revoked retries forever instead of re-provisioning. |
| CONNACK | **Client certificate accepted by TLS, identity refused at CONNACK** (`rc=5` / `0x87 Not authorized`) | `AZ_IOT_ERR_IDENTITY_REJECTED` | `az_iot_mqtt_connack_result()` | sets `reprovision_pending`; the next attempt runs `dps_start()` | The distinction between this row and the previous one is exactly the distinction the negative-code rule encodes, and it is the reason adapters must not flatten codes. |
| Configuration | TLS is only enabled when a client certificate, key or `verify_server` is set | — | `paho_iface_create` | scheme selected as `ssl://` or `tcp://` | Keying off the credential means an unconfigured device connects in the clear rather than failing. |

#### 9.4.3 Phase 3 — CONNECT / CONNACK

| Phase | Trigger | Surfaced as | Mapped by | SDK action | Notes / limits |
| --- | --- | --- | --- | --- | --- |
| CONNACK | Accepted | `AZ_IOT_OK` | adapter | Hub-Next: start the presence handshake. Classic: replay persistent subscriptions, announce `CONNECTED`. Attempt counter reset. | |
| CONNACK | Identity refused — v3 `2`/`4`/`5`, v5 `0x85`/`0x86`/`0x87`/`0x8C` | `AZ_IOT_ERR_IDENTITY_REJECTED` | `az_iot_mqtt_connack_result()` | DPS configured and reconnection enabled → `reprovision_pending`, retry via `dps_start()`. Otherwise an ordinary retry or fault. | The flag is cleared before the attempt, so a failure inside re-provisioning degrades to an ordinary retry rather than looping. |
| CONNACK | v3 `1 unacceptable protocol version` | `AZ_IOT_ERR_MQTT` | `az_iot_mqtt_connack_result()` | **retried** under policy | **Known defect.** Deterministic and can never succeed on retry; the generic table classes it Terminal. The exclusion from the identity set is correct — `1` says nothing about the identity — but the result should be a fatal classification, not a retry. Fixing it needs the fatal-failure classification that is `planned` for C. |
| CONNACK | v3 `3 Server unavailable` | `AZ_IOT_ERR_MQTT` | `az_iot_mqtt_connack_result()` | retried | Correct: the canonical transient refusal. |
| CONNACK | v5 deterministic refusals — `0x81`, `0x82`, `0x84`, `0x95`, `0x8A`, `0x90`, `0x99`, `0x9A`, `0x9B` | `AZ_IOT_ERR_MQTT` | `az_iot_mqtt_connack_result()` | **retried** | Same defect as v3 `1`. The four Will-related codes cannot arise: no client here sends a Will. |
| CONNACK | v5 transient refusals — `0x88`, `0x89`, `0x97`, `0x9F`, `0x80`, `0x83` | `AZ_IOT_ERR_MQTT` | `az_iot_mqtt_connack_result()` | retried with jitter | Correct. |
| CONNACK | v5 redirection — `0x9C Use another server`, `0x9D Server moved` | `AZ_IOT_ERR_MQTT` | `az_iot_mqtt_connack_result()` | **retried against the same host** | **Known gap.** The Server Reference property is not read. Retrying the same endpoint repeats the redirection until the policy is exhausted. |
| CONNECT | No CONNACK within the connect timeout | `AZ_IOT_ERR_MQTT` | Paho | ordinary failed attempt | 30 s by default, configurable; the same value is used for the DPS bootstrap connect. |
| CONNACK | Arrives after `close()` | ignored | connection client | logged at debug, `break` — the pending DISCONNECTED event settles the session to `IDLE` | Guarded on `user_close || state == DISCONNECTING`. Correct. |
| CONNECT | No factory registered for the version the role requires | `AZ_IOT_ERR_NOT_SUPPORTED` | `find_factory()` | `open()` transitions back to `IDLE` and returns the error | Role → version: DPS and Hub-Classic are v3.1.1, Hub-Next is v5. |

#### 9.4.4 Phase 4 — DPS provisioning

| Phase | Trigger | Surfaced as | Mapped by | SDK action | Notes / limits |
| --- | --- | --- | --- | --- | --- |
| Registering | Registration status is failed or disabled | `AZ_IOT_ERR_DPS` | connection client | `dps_finalize()` → `FAULTED` | Not a reconnect trigger, by design (§5.2). |
| Registering | Response payload empty | `AZ_IOT_ERR_PROTOCOL` | connection client | `FAULTED` | Checked **before** calling the parser: the dependency's precondition on an empty span would spin, because this build ships with precondition checking on and no handler installed. |
| Registering | Response payload unparsable | `AZ_IOT_ERR_PROTOCOL` | dependency parser | `FAULTED` | The body is logged. |
| Registering | Assigned hostname or device id longer than its 128-byte buffer | `AZ_IOT_ERR_NOT_SUPPORTED` | connection client | `FAULTED` | |
| Polling | `operation_id` longer than its buffer | `AZ_IOT_ERR_NOT_SUPPORTED` | connection client | `FAULTED` | |
| Assignment | `issuedCertificateChain` absent when a CSR was sent | `AZ_IOT_ERR_NOT_FOUND` | connection client | `FAULTED` | |
| Assignment | No handler to store the issued chain | `AZ_IOT_ERR_NOT_SUPPORTED` | connection client | `FAULTED` | Neither the provider vtable hook nor the callback was supplied. |
| Assignment | `registrationState` key absent | `AZ_IOT_ERR_NOT_FOUND`, treated as OK | connection client | continues, profile stays classic | Deliberate: the stock api-version does not carry the key. |
| Polling | `assigning` with a `retry-after` | `AZ_IOT_OK` | connection client | `dps_poll_due_ms = now + retry_after_seconds * 1000`; the pump re-publishes the query when it elapses | The service-supplied delay is honoured verbatim, with no reconnect backoff on top and no SDK-side cap on the number of polls. |
| Assignment | Unrecognised `connectionProfile`, or one longer than 64 bytes | `AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED` | `connection_profile_set()`, detected in `dps_apply_deferred()` | `FAULTED` | The raw string stays readable through the profile getter even in `FAULTED`. **[planned]** — the field never arrives at the current api-version. |
| Registration SUBACK | The `$dps/registrations/res/#` subscription is refused | `AZ_IOT_ERR_MQTT` | adapter | **unverified** | The SUBACK correlation path only matches the presence packet id; a DPS SUBACK failure is absorbed by the `default` arm. What the client does next has not been established — most likely it waits for the register response until the connect times out. |
| Any DPS phase | DPS message arrives in the wrong phase | ignored | connection client | dropped | Guarded on `dps_phase` being REGISTERING or POLLING. |
| Hub CONNACK | Identity rejected on a DPS-provisioned device | `AZ_IOT_ERR_IDENTITY_REJECTED` | `az_iot_mqtt_connack_result()` | `reprovision_pending` → `dps_start()` on the next attempt | See [§9.2](#92-connack-mapping). |

#### 9.4.5 Phase 5 — presence handshake (gen2)

| Phase | Trigger | Surfaced as | Mapped by | SDK action | Notes / limits |
| --- | --- | --- | --- | --- | --- |
| Subscribing | Presence SUBACK carries a failure | `AZ_IOT_ERR_MQTT` | adapter (reason code flattened, see [§9.5](#95-known-gaps)) | clear the phase, `DEFER_RECONNECT` or `DEFER_FAULT` | A deterministic refusal — `0x87`, `0x8F`, `0xA2` — is retried until the policy is exhausted. |
| Subscribing | `presence_start()` fails after CONNACK | its own result | connection client | clear the phase, reconnect | |
| Birth | `presence_publish_birth()` fails | its own result | connection client | clear the phase, reconnect | |
| Birth | No birth-ack within 60 s | `AZ_IOT_ERR_TIMEOUT` | `_do_work()` | reconnect, or `FAULTED` with no policy | The deadline is armed at CONNACK and re-armed after the birth publish, so each step gets its own 60 s. |
| Birth | Birth-ack arrives while still subscribing | ignored as a birth-ack | connection client | falls through to `az_iot_dispatch_route()`, and is dropped if no prefix matches | The interception is guarded on `presence.phase == PRESENCE_PHASE_BIRTH`. Correct. |
| Birth | Birth-ack carries a correlation value from an earlier attempt | ignored as a birth-ack | `presence_is_birth_ack()` | routed to dispatch, then dropped | A fresh 16-byte nonce is generated per attempt and compared with `memcmp`. This is the epoch guard; there is no separate generation counter. |
| Birth | Birth-ack arrives after `close()` | ignored | connection client | logged at debug, `break` | Same guard as the late CONNACK. |
| SUBACK | Any SUBACK whose packet id is not the presence subscribe | absorbed | connection client | nothing | Only `presence.sub_packet_id` is correlated. This is what makes the feature-subscription rows below unverifiable today. |

#### 9.4.6 Phase 6 — subscription gate

The gate does not exist yet. `CONNECTED` is announced first and the persistent subscribes are issued
afterwards with their results discarded, so a request published from inside the state callback can
reach the wire ahead of its own SUBSCRIBE.

| Phase | Trigger | Surfaced as | Mapped by | SDK action | Notes / limits |
| --- | --- | --- | --- | --- | --- |
| SUBACK | Granted, at or below the requested QoS | `AZ_IOT_OK` | adapter | absorbed | The adapter treats anything `< 0x80` as granted, which is correct. This SDK never requests QoS 2, so a downgrade does not arise. |
| SUBACK | Refused, feature subscription | `AZ_IOT_ERR_MQTT` | adapter | **nothing** — absorbed by the `default` arm | The connection is announced `CONNECTED` with a filter that is not live, and no one is told. |
| SUBSCRIBE | The subscribe call fails synchronously | `AZ_IOT_ERR_MQTT` | adapter | **discarded** — the replay loop ignores the return value | |
| SUBACK | No SUBACK arrives | — | — | **nothing** | There is no deadline; `CONNECTED` has already been announced. |
| Registry | Persistent-subscription registry full (9th filter) | `AZ_IOT_ERR_NOT_ENOUGH_SPACE` | connection client | the registration call fails; the connection is unaffected | Correctly Contained. |
| Registry | Topic filter longer than 127 bytes | `AZ_IOT_ERR_INVALID_ARG` | connection client | as above | Rejected before the transport is touched; never truncated. |
| SUBACK | Refusal class and failure scope | `AZ_IOT_ERR_SUBSCRIPTION_REFUSED`, with `az_iot_subscription_failure_scope` ∈ { `AZ_IOT_SUBSCRIPTION_FAILS_SESSION`, `AZ_IOT_SUBSCRIPTION_FAILS_SELF` } | `az_iot_mqtt_suback_result()` | fail the connect, or fail only the requesting feature | **[planned — P1c]**. Scoped, not merged: none of these identifiers exists in the tree today. Attributed to the P1c phase in [client-separation.md §12](client-separation.md), which also carries the gate itself. |
| SUBACK | Gate deadline | `AZ_IOT_ERR_TIMEOUT` | connection client | reconnect | **[planned — P1c]**. |

#### 9.4.7 Phase 7 — steady state

| Phase | Trigger | Surfaced as | Mapped by | SDK action | Notes / limits |
| --- | --- | --- | --- | --- | --- |
| PUBACK | v5 PUBACK `< 0x80`, including `0x10 No matching subscribers` | `AZ_IOT_OK` | `paho_publish_success5` | the ack callback fires with success | Correct — `0x10` is a success. Paho routes reason codes `>= 0x80` to the failure callback, so success5 only sees grants. |
| PUBACK | v5 PUBACK `>= 0x80` — `0x87 Not authorized`, `0x90 Topic Name invalid`, `0x97 Quota exceeded`, `0x99 Payload format invalid` | `AZ_IOT_ERR_MQTT` | `paho_publish_failure5` | the ack callback fires with the failure; the connection survives | Correctly **Contained**, but the reason code is **flattened**: `response->reasonCode` is not inspected, so a caller cannot tell a deterministic refusal from a quota it should back off on. |
| PUBACK | v3.1.1 PUBACK | `AZ_IOT_OK` / `AZ_IOT_ERR_MQTT` | `paho_publish_success` / `_failure` | as above | v3.1.1 PUBACK carries no reason code; there is nothing to flatten. |
| PUBACK | Unknown packet id | dropped | connection client | nothing | Deliberate: a publish issued without an ack callback has no table entry. |
| DISCONNECT | Server-initiated v5 DISCONNECT, any reason code | `AZ_IOT_OK` on `AZ_IOT_MQTT_EVT_DISCONNECTED` | `paho_disconnected` | `DEFER_RECONNECT` — the substituted reason is `AZ_IOT_ERR_NOT_CONNECTED` | **The reason code is logged and then discarded.** `0x8E Session taken over` and `0x9D Server moved` are indistinguishable from a routine drop, and all three reconnect. Wiring these up is the single highest-value fix in this table. |
| Keep-alive | Local keep-alive expiry | `AZ_IOT_OK` on DISCONNECTED | Paho `connectionLost` | reconnect | Keep-alive is 30 s by default. |
| Transport | Adapter raises `AZ_IOT_MQTT_EVT_ERROR` | the event's status, or `AZ_IOT_ERR_MQTT` | adapter | `DEFER_RECONNECT` or `DEFER_FAULT` | |
| Inbound | Message matching no dispatch prefix | dropped | `az_iot_dispatch_route()` | nothing; the return value is explicitly discarded | Correct and deliberate — the behaviour brokers rely on for filters that outlive their subscriber. |
| Twin | Service status `400` | `AZ_IOT_ERR_INVALID_ARG` | `status_to_result()` in the twin client | that request completes with the failure | Contained. |
| Twin | Service status `404` | `AZ_IOT_ERR_NOT_FOUND` | as above | as above | Contained. |
| Twin | Service status `429` | `AZ_IOT_ERR_BUSY` | as above | as above | Deliberately **not** `AZ_IOT_ERR_NOT_SUPPORTED`, which is what a full pending table returns: a caller must be able to tell "the service is throttling me" from "I have too many requests in flight locally". |
| Twin | `5xx` or an unrecognised status | `AZ_IOT_ERR_MQTT` | as above | as above | |
| Any feature | The MQTT session ends with requests pending | the session-end result | session-end handlers | every pending request is completed with an error and its slot released | Without this the slots would leak one per outage until the pool is exhausted and every later request is refused. |

#### 9.4.8 Phase 8 — framework, resource and programming errors

| Phase | Trigger | Surfaced as | Mapped by | SDK action | Notes / limits |
| --- | --- | --- | --- | --- | --- |
| Any | Runtime allocation failure | not applicable to the core | — | — | The core state machine performs **no** allocation: every buffer is an in-struct fixed array. The only `malloc` on any core path is a Windows-only environment-variable read used by the mock endpoints in dev and test builds; on failure it returns `AZ_IOT_ERR_INTERNAL` or `AZ_IOT_ERR_INVALID_ARG`, not an out-of-memory value. The adapter does allocate, for the server URI and duplicated option strings. |
| Publish / subscribe | A bound in [§9.3](#93-compile-time-bounds) is exceeded | see that table | connection client | the call fails before the transport is touched | Never truncated. |
| Publish | Pending-PUBACK table full (17th unacknowledged publish with a callback) | `AZ_IOT_ERR_NOT_SUPPORTED` | connection client | the publish already succeeded; the error exists so the caller can apply backpressure | Contained. The value overlaps "no factory registered", which is a poor fit for a capacity condition. |
| Twin | Pending-request pool full (9th) | `AZ_IOT_ERR_NOT_SUPPORTED` | twin client | the request is rejected | Contained. Distinct from the `429` row above, which is the point. |
| Direct methods | In-flight pool full (5th) | **none** | direct-method client | **the invocation is dropped**, with a warning logged | **Known gap.** A slot is released only by responding; a handler that returns without responding leaks one permanently, and after four leaks every invocation disappears. The generic contract requires this to be visible, not silent — a warning in a log is the weakest form of visible there is. |
| Connect | No factory for the required version | `AZ_IOT_ERR_NOT_SUPPORTED` | `find_factory()` | back to `IDLE`, error returned from `open()` | |
| Registry | Session-end handler registry full (5th distinct context) | `AZ_IOT_ERR_NOT_SUPPORTED` | connection client | the registration fails | Re-registering the same context upserts rather than consuming a slot. |
| Registry | Inbound dispatch table full (9th) | `AZ_IOT_ERR_NOT_SUPPORTED` | dispatch | the registration fails | |
| Init | Invalid or missing arguments | `AZ_IOT_ERR_INVALID_ARG` | explicit guards at each public entry point | the call returns | No layer aborts. Every public entry point checks its arguments explicitly. |
| Init | An invalid value reaches a dependency's precondition | undefined | — | would spin | This build ships with precondition checking enabled and **no handler installed**, so the default handler is an infinite loop: a misconfigured device would hang inside `open()` rather than get an error back. The SDK therefore pre-validates everything it passes in — the empty-identity-scope and empty-payload guards above exist for exactly this reason. Any path that misses a check is a hang, not an error. |
| CSR | A second CSR while one is in flight | `AZ_IOT_ERR_BUSY` | connection client | rejected | Single slot. |
| CSR | No response within 120 s, re-armed on each `202 Accepted` | `AZ_IOT_ERR_TIMEOUT` | `_do_work()` | the slot is released and the callback fires with a failure | |
| Threading | An adapter delivers a callback off the pump thread | undefined | — | none | The single-threaded contract is normative in [how_to_byo_mqtt_client.md](../how_to_byo_mqtt_client.md) and **not enforced**: nothing detects a violation. The Paho adapter honours it by pushing events onto a small FIFO from the Paho I/O thread and draining it inside `process_loop()`. A byo adapter that skips this corrupts state with no error at the point of corruption. |

#### 9.4.9 Phase 9 — teardown

| Phase | Trigger | Surfaced as | Mapped by | SDK action | Notes / limits |
| --- | --- | --- | --- | --- | --- |
| `close()` while `IDLE` | — | `AZ_IOT_OK` | connection client | idempotent no-op | |
| `close()` while `CONNECTING` | — | `AZ_IOT_ERR_NOT_INITIALIZED` | connection client | nothing | **Gap.** `active_client` is still NULL until the connect callback populates it, so the call fails **and does not set `user_close`**. The late-CONNACK guard therefore does not arm, and the attempt continues to completion. The generic contract classes this Benign and requires the close intent to be recorded. |
| `close()` while `CONNECTED` | — | `AZ_IOT_OK`, or the adapter's disconnect error | connection client | sets `user_close`, transitions to `DISCONNECTING`, calls the adapter's `disconnect()` | |
| `close()` while `DISCONNECTING` | — | `AZ_IOT_OK`, or the adapter's error | connection client | sets `user_close` again and re-issues `disconnect()` | Harmless, but not a no-op. |
| `close()` while `RECONNECTING` | — | `AZ_IOT_OK` | connection client | cancels the schedule (`reconnect_attempt = 0`, `reconnect_due_ms = 0`), clears `user_close`, transitions straight to `IDLE` | No adapter exists to disconnect. |
| `close()` while `FAULTED` | — | `AZ_IOT_ERR_NOT_INITIALIZED` | connection client | nothing | **Gap.** `teardown_active()` already cleared `active_client`, so the call reports an error for what the contract classes as a benign no-op. `open()` from `FAULTED` still works. |
| `destroy()` with PUBACKs pending | — | none | connection client | the table is zeroed **without** invoking the callbacks | Deliberate: on destroy the context those callbacks close over may already be gone, and calling into it would turn cleanup into a use-after-free. Contrast session teardown, where the callbacks **do** fire. |
| `destroy()` with session handlers registered | — | none | connection client | cleared without invoking them | Same reasoning. |
| `destroy()` with a CSR in flight | — | none | connection client | no callback fires | The slot is irrelevant after destruction. |
| `destroy()` mid-handshake | — | none | `teardown_active()` | the presence phase is reset; the hub and DPS adapters are destroyed; each registered factory's `destroy` hook runs | |

### 9.5 Known gaps

Filling the table above surfaced these. They are recorded here rather than smoothed over, because an
honest gap is the point of the exercise.

1. **A deterministic CONNACK refusal is retried.** `1 Connection Refused, unacceptable protocol version`
   is the clearest case — it can never succeed on retry — and so are v5 `0x81`, `0x82`, `0x84`, `0x8A`
   and `0x95`. All become `AZ_IOT_ERR_MQTT` and are retried until the policy is exhausted. The
   CONNACK mapper is right to exclude them from the identity set; what is missing is the fatal
   classification alongside it. Tracked as the fatal-failure classification in
   [connection-impl-status.md](connection-impl-status.md).
2. **The server DISCONNECT reason code is discarded.** `paho_disconnected` logs it and enqueues
   `AZ_IOT_OK`, so `0x8E Session taken over` — where reconnecting makes things actively worse — is
   indistinguishable from a routine drop.
3. **The SUBACK reason code is flattened.** The adapter subscribes one filter at a time, so Paho
   routes a refusal to the failure callback; that callback reports `AZ_IOT_ERR_MQTT` without reading
   `response->reasonCode`, and the defensive `>= 0x80` check on the success callback discards it too.
   Either way the core cannot tell `0x87 Not authorized` from `0x97 Quota exceeded`. The v3.1.1 path
   does see a refusal — Paho routes a single-filter `0x80 Failure` to the failure callback — but
   3.1.1 carries no reason code, so there is genuinely nothing to preserve there. This is the same
   "do not flatten codes" rule that [how_to_byo_mqtt_client.md](../how_to_byo_mqtt_client.md) already
   makes normative for CONNACK; `az_iot_mqtt_suback_result()` is the SUBACK counterpart and is
   **[planned — P1c]**.
4. **The PUBACK reason code is flattened.** `paho_publish_failure5` ignores `response->reasonCode`, so
   a caller cannot separate `0x87 Not authorized` (do not retry) from `0x97 Quota exceeded` (back off
   and retry).
5. **No TLS-specific result.** `AZ_IOT_ERR_TLS` and `AZ_IOT_ERR_AUTH` are in the enum and are never
   produced on the connection path. Every certificate, chain, hostname and cipher failure arrives as
   `AZ_IOT_ERR_MQTT`; the concrete reason exists only in the trace log.
6. **A v5 redirection is retried against the same host.** `0x9C Use another server` and
   `0x9D Server moved` carry a Server Reference property that is not read.
7. **A dropped direct-method invocation is silent to the application.** Only a log warning marks it.
8. **`close()` returns an error in `CONNECTING` and in `FAULTED`,** and in `CONNECTING` it also fails
   to record the close intent, so the late-CONNACK guard does not arm.
9. **The classic-hub username is not bounds-checked.** Exceeding
   `AZ_IOT_MQTT_USERNAME_BUF` on the Hub-Next path returns `AZ_IOT_ERR_NOT_ENOUGH_SPACE`; on the
   classic path the connect proceeds with no username instead of failing.
10. **Unverified: a refused DPS registration SUBACK.** The SUBACK correlation path matches only the
    presence packet id, so a refusal of `$dps/registrations/res/#` is absorbed. What the client does
    next has not been established and is deliberately left unfilled rather than guessed.
11. **The single-threaded contract is unenforced.** Nothing detects an adapter callback delivered off
    the pump thread.
12. **The default reconnection policy is not applied.** `az_iot_reconnection_policy_default()` exists
    and is never called by the connection client, so a caller that zero-initialises the options gets
    `initial_delay_ms == 0`, which means reconnect disabled — every Retryable row above then faults on
    the first occurrence. A separate workstream is changing this; the shipped default should not be
    read as the intended behaviour.

---

## 10. Implementation index (C SDK)

| Topic | Location | Status |
| --- | --- | --- |
| State enum, policy, options | [az_iot_connection_client.h](../../inc/azure/iot/az_iot_connection_client.h) | implemented |
| State transitions, connect attempt, event handling | [connection_client.c](../../src/core/connection_client.c) | implemented |
| Backoff computation and defaults | [reconnect.c](../../src/core/reconnect.c) | implemented |
| Certificate provider contract | [az_iot_certificate_provider.h](../../inc/azure/iot/az_iot_certificate_provider.h) | implemented |
| Managed OpenSSL provider | [az_iot_certificate_provider_managed.c](../../adapters/cert_openssl/az_iot_certificate_provider_managed.c) | implemented |
| Connection profile enum, `az_iot_hub_profile`, `get_hub_profile()` | [client-separation.md](client-separation.md) §2 | planned — blocked on the DPS api-version |
| `adu_core` / `az_iot_adu_channel` split | [client-separation.md](client-separation.md) §8 | planned |
| ADU engine internals reused by ADUv2 | [c/src/features/adu](../../src/features/adu) | implemented (ADUv1 API to be removed) |
| ADUv2 device contract | [aduv2-spec.md](aduv2-spec.md) | planned — DPS fronts both flows for Ignite '26 |
| CONNACK code mapping | [mqtt_iface.c](../../src/core/mqtt_iface.c) | implemented |
| Paho adapter event and code mapping | [az_iot_mqtt_paho.c](../../adapters/paho/az_iot_mqtt_paho.c) | implemented — SUBACK, PUBACK and DISCONNECT codes flattened (§9.5) |
| Inbound dispatch table | [dispatch.c](../../src/core/dispatch.c) | implemented |
| Subscription gate, `az_iot_mqtt_suback_result()`, subscription failure scope | [client-separation.md](client-separation.md) §12, P1c | planned |
