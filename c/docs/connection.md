# Connection and Reconnection Lifecycle

Language-neutral reference for the device connection lifecycle in this repository. It defines the
contract that **every** client in this SDK family must honour — the C99 client under
[c/](../../c) and the .NET client under [dotnet/](../../dotnet) today — covering connection states,
provisioning, connection-profile selection, reconnection, certificate onboarding and renewal, and
device update (ADU).

This document owns the *behaviour*. It deliberately names no types, functions or files: each client
maps the concepts onto its own idioms, and those mappings are collected in
[§10](#10-language-mapping).

Language-specific detail:

- **C** — [eng/connection-c.md](eng/connection-c.md): enum names, headers, source locations, exact
  defaults.
- **.NET** — no separate design doc yet; the mapping table in [§10](#10-language-mapping) is the
  current record.

Related documents:

- [design.md](design.md) — layering and adapter model (C).
- [eng/certificate-management.md](eng/certificate-management.md) — CSR / operational certificate design.
- [eng/client-separation.md](eng/client-separation.md) — connection profile (§2) and the ADU channel split (§8).
- [eng/connection-state-and-error-propagation.md](eng/connection-state-and-error-propagation.md) — observer registry and status codes.
- [dps-integration.md](dps-integration.md), [devnotes.md](devnotes.md) — DPS contract and the running requirements log.
- [eng/aduv2-spec.md](eng/aduv2-spec.md) — the ADUv2 device contract, and the source for [§7](#7-device-update-onboarding-and-renewal).
  It owns the request/response shapes, error codes and trust model, which are deliberately not restated
  here. [eng/adu-client-design.md](eng/adu-client-design.md) covers the shared verify/download/install
  engine, which is unchanged from ADUv1.

### Status legend

This document describes the target lifecycle. Coverage differs per client, so each section carries a
status line using these marks:

| Mark | Meaning |
| --- | --- |
| **implemented** | Present in that client's code today. |
| **partial** | Present, but with a known gap called out in the section. |
| **planned** | Designed and agreed, not yet in code. |
| **none** | Not implemented and not started in that client. |

---

## 1. Vocabulary

| Term | Meaning |
| --- | --- |
| **State** | User-visible connection lifecycle value, reported to the application through a callback, event or observable property. |
| **Connection profile** | What the device is connected to, as declared by DPS: `classic` or `mqttV5`. Reported, never caller-selected. |
| **Generation** | The feature-client family selected by the profile: `gen1` (classic) or `gen2` (mqttV5). |
| **Role** | Which endpoint and protocol the current MQTT session targets: DPS (v3.1.1), classic hub (v3.1.1), or next-generation hub (v5). |
| **Phase** | Internal sub-step inside a state — DPS phases and presence (birth) phases. Not user-visible. |
| **Provisioning** | Obtaining a hub assignment from DPS. |
| **Onboarding** | Everything that happens against the DPS gateway under *onboarding auth*: the bootstrap update check, the CSR, and registration itself. |
| **Renewal** | The recurring, post-provisioning counterpart under *operational auth*: certificate re-issuance and the periodic update check. |
| **Bootstrap credential** | Initial device identity, used to reach DPS. |
| **Operational credential** | Certificate issued to the device in response to a CSR. |
| **Update channel** | The transport-carrying abstraction for update delivery and reporting, which keeps the update engine transport-independent. |

---

## 2. Top-level state machine

**C:** implemented · **.NET:** partial — the same lifecycle is driven internally, but it is surfaced
as connect/disconnect events rather than as a single user-visible state value; see
[§10](#10-language-mapping).

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
the application never sees an intermediate `CONNECTED` for the DPS session. DPS progress is tracked
separately as a phase:

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

Any failure or drop in a DPS phase is handled by the same backoff path as a hub failure, and a retry
restarts provisioning from `DPS_CONNECTING`.

---

## 3. Full connect sequence

**C:** implemented · **.NET:** implemented (CSR-in-registration excepted, see below)

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

    alt DPS configured (id scope present)
        Conn->>Cert: load(BOOTSTRAP)
        Cert-->>Conn: CA + client cert/key
        opt operational certificate requested
            Conn->>Cert: get_csr(registration id)
            Cert-->>Conn: base64 DER CSR
        end
        Conn->>DPS: MQTT v3.1.1 CONNECT (X.509 bootstrap)
        DPS-->>Conn: CONNACK
        Conn->>DPS: SUBSCRIBE $dps/registrations/res/#
        DPS-->>Conn: SUBACK
        Conn->>DPS: PUBLISH register (payload may include the CSR)
        loop while assigning
            DPS-->>Conn: status = assigning (operation id, retry-after)
            Conn->>DPS: PUBLISH get operation status
        end
        DPS-->>Conn: status = assigned (assignedHub, deviceId, connectionProfile, issuedCertificateChain)
        opt issued chain present
            Conn->>Cert: store issued certificate(chain)
            Conn-->>App: operational certificate callback(chain)
        end
        Conn->>DPS: DISCONNECT + tear down the v3.1.1 session
        Conn->>Conn: resolve connection profile, apply assigned host / client id, pick hub role
    end

    Conn->>Cert: load(OPERATIONAL)
    alt not found
        Conn->>Cert: load(BOOTSTRAP)
    end
    Conn->>Hub: MQTT CONNECT (role-specific username, TLS mutual auth)
    Hub-->>Conn: CONNACK

    alt gen2 hub (MQTT v5)
        Conn->>Hub: SUBSCRIBE ih/{device_id}/dev/# (QoS 1)
        Hub-->>Conn: SUBACK
        Conn->>Hub: PUBLISH birth (type=birth:1, correlation = connect nonce)
        Hub-->>Conn: birth-ack on ih/{device_id}/dev/presence (nonce echoed)
    end

    Conn->>Hub: re-SUBSCRIBE persistent feature filters (twin, methods, C2D, credentials, update channel)
    Conn->>Conn: state = CONNECTED
    Conn-->>App: state callback(CONNECTED)
```

Key ordering guarantees that every client must honour:

1. `CONNECTED` is announced **after** the birth handshake (gen2) and **after** persistent
   subscriptions have been re-issued, so a feature client never observes `CONNECTED` while its topic
   filters are missing.

   > **Client status.** The .NET client honours this: the connect call does not complete, and feature
   > traffic is latched, until the subscriptions (classic) or the presence flow (gen2) have completed.
   > The C client does **not** yet: it transitions to `CONNECTED` first and only then issues the
   > subscribes, discarding the result, so a request published from inside the callback can reach the
   > wire ahead of its own SUBSCRIBE. Treat the guarantee as the *intended* contract; see
   > [eng/connection-c.md §3](eng/connection-c.md) for the tracking detail and the fix phase. The
   > gen2 birth handshake *is* correctly gated in both clients — it waits for its own SUBACK before
   > publishing birth — which is the shape the feature subscriptions are being moved to.

2. The DPS session is fully torn down before the hub session is created — they are never concurrent,
   and DPS always uses MQTT 3.1.1 even when the hub session uses v5.
3. The operational certificate is preferred over the bootstrap certificate on every connect attempt,
   including reconnects.
4. **The bootstrap update check runs before `open()`, not inside it.** The agent drives the onboarding
   update call against the DPS gateway until the service reports no update, and only then does the
   connection client register. The check is **advisory**: if it fails, the device proceeds to register
   anyway. See [§7](#7-device-update-onboarding-and-renewal).
5. The DPS assignment is the single delivery point for everything the device learns about its
   placement: hub, device id, connection profile and issued certificate chain.

> **CSR in registration is C-only today.** The .NET client always registers with an empty CSR field,
> so it can only obtain an operational certificate through the post-connect renewal path of
> [§6](#6-certificate-management-onboarding-and-renewal).

---

## 4. Connection profile selection

**C:** planned · **.NET:** partial — the branch exists and drives protocol and feature selection, but
it is a locally-set boolean rather than the DPS-declared string.

The profile is what DPS says the device landed on. It is **reported, never selected** — there is no
caller-facing knob, and falling back from one profile to another is application logic, not SDK
behaviour. See [eng/client-separation.md §2](eng/client-separation.md) for the full rationale.

```mermaid
flowchart TB
    A["DPS assignment received"] --> B{"connectionProfile"}
    B -->|"classic"| C["Classic profile<br/>MQTT 3.1.1, classic hub role"]
    B -->|"absent or null"| C
    B -->|"mqttV5"| D["MQTT v5 profile<br/>MQTT 5, next-generation hub role"]
    B -->|"anything else"| E["Unknown profile"]
    C --> F["gen1 feature clients"]
    D --> G["gen2 feature clients<br/>+ presence handshake<br/>+ update channel"]
    E --> H["Connection fails:<br/>unsupported-profile error,<br/>raw string still readable"]
```

| Wire value | Profile | MQTT | Generation |
| --- | --- | --- | --- |
| `"classic"`, absent, or `null` | classic | 3.1.1 | gen1 |
| `"mqttV5"` | MQTT v5 | 5 | gen2 |
| anything else | unknown | — | connection fails |

Rules every client must implement:

- `connectionProfile` is a `readOnly` **string** on the DPS registration result, delivered alongside
  `assignedHub`, `deviceId` and `issuedCertificateChain`. There is no numeric hub version on the wire.
- It is an **extensible union**, so the raw string must be preserved verbatim and not collapsed into a
  closed enum. A value newer than the SDK still has to be loggable.
- An unrecognised profile **fails the connection** with a dedicated unsupported-profile error. The SDK
  will not guess which MQTT version to speak.
- The profile is readable only once `CONNECTED`; before that, querying it fails with a not-connected
  error.
- A feature client from the wrong generation is refused with a generation-mismatch error. Because of
  this, feature clients must be created **after** the connection is open.

> **Blocked on the DPS api-version, in both clients.** `connectionProfile` is new in DPS
> `2026-11-02-preview`; both clients still request `2019-03-31`, so the field never arrives today.
> Raising it is a prerequisite for this entire section. Until then the .NET client carries a
> locally-set boolean placeholder in the registration result, and the C client has no profile at all.
>
> **Open:** whether a reconnect can change the generation. If DPS can reassign a device mid-life,
> every feature client the application holds becomes invalid at that moment and it must be told.

---

## 5. Reconnection

**C:** implemented · **.NET:** implemented

```mermaid
sequenceDiagram
    autonumber
    participant Hub
    participant Conn as Connection client
    participant App

    Hub--xConn: transport drop / CONNACK error / birth timeout
    Conn->>Conn: tear down the active session (drop pending PUBACKs, reset phases)
    alt user called close()
        Conn->>Conn: state = IDLE
    else reconnect disabled
        Conn->>Conn: state = FAULTED
        Conn-->>App: state callback(FAULTED, reason)
    else attempts exhausted, or the failure is fatal
        Conn->>Conn: state = FAULTED
        Conn-->>App: state callback(FAULTED, reason)
    else
        Conn->>Conn: attempt++, delay = backoff(attempt)
        Conn->>Conn: state = RECONNECTING
        Conn-->>App: state callback(RECONNECTING, reason)
        Note over Conn: wait out the backoff delay
        Conn->>Conn: restart the full sequence of section 3
        Hub-->>Conn: CONNACK ok
        Conn->>Conn: attempt = 0, state = CONNECTED
    end
```

### 5.1 Backoff policy

The shape is the same in both clients — exponential growth, capped, jittered, reset on every
successful CONNACK — but the parameters and their defaults are **not** currently aligned:

| Property | C | .NET |
| --- | --- | --- |
| Growth | `initial_delay << (attempt - 1)`, shift clamped at 30 | `2^(baseExponent + attempt)` ms, exponent clamped at 32 |
| First delay (default) | 1 s | 128 ms (base exponent 6) |
| Cap (default) | 30 s | 60 s as configured by the connection client; 30 min for the bare policy default |
| Max attempts (default) | unlimited | unlimited |
| Jitter (default) | ±20 % of the computed delay | 95–105 % of the computed delay, skipped below 50 ms |
| Disable reconnect | zero initial delay | a no-retry policy |
| Policy is caller-replaceable | no — parameters only | yes — the policy itself is an interface |

Both clients also cap a single connect attempt with a timeout and treat the expiry as a failed
attempt.

> **Divergence to close.** Aligning the defaults, and deciding whether C should also accept a
> caller-supplied policy object, is open work. Applications must not depend on the current numbers
> being the same across languages.

### 5.2 What triggers a reconnect

- CONNACK with a non-success status.
- Unexpected transport disconnect or adapter error while `CONNECTING` or `CONNECTED`.
- Any failure during a DPS phase (a reconnect restarts provisioning from the beginning).
- gen2 birth-ack timeout (60 s per handshake step, in both clients).

A user-initiated `close()` never triggers a reconnect: the intent to close is recorded and checked
before any backoff is scheduled.

Some failures are **fatal** and must not be retried, because retrying cannot succeed — protocol
errors, malformed packets, authorization failures, session-taken-over, invalid topic filters and
server-moved among them. A fatal failure goes straight to `FAULTED` and is reported to the
application. This classification is implemented in the .NET client and is the intended behaviour for
the C client.

### 5.3 What is preserved across a reconnect

| Item | Preserved | Behaviour |
| --- | --- | --- |
| Persistent subscriptions | Yes | Re-issued on reconnect. Intended to complete before `CONNECTED` is announced; see the caveat in [§3](#3-full-connect-sequence). |
| Assigned hub host / device id | Yes | Cached after the first DPS assignment. |
| Connection profile | Yes | Re-resolved from the new assignment; a change invalidates held feature clients (open question, [§4](#4-connection-profile-selection)). |
| Operational certificate | Yes | Owned by the certificate provider, reloaded on each attempt. |
| Update workflow state | Yes | Owned by the update engine and persisted, so an install survives a reconnect and a reboot. |
| Reconnect attempt counter | Reset on success | Incremented per failed attempt. |
| In-flight QoS 1 PUBACKs | No | Packet ids belong to the destroyed session; callers must re-send. |
| Twin GET/PATCH, method responses, telemetry in flight | No | Feature clients must re-issue. |
| Update status report not yet acked | Yes | Held in durable storage and retried until acked; idempotent on the workflow id. |
| Presence (birth) phase | No | Restarted with a freshly generated nonce. |
| DPS phase | No | Restarted from `CONNECTING` if DPS is configured. |
| In-flight CSR operation | No | Abandoned; the caller is notified with a failure or timeout result. |

---

## 6. Certificate management: onboarding and renewal

**C:** implemented · **.NET:** partial — renewal is implemented for the classic hub only; the CSR is
not yet sent during registration, and renewal is not available on a gen2 hub.

Two distinct issuance paths exist; both end with the certificate provider owning the operational
credential, and neither forces an immediate reconnect.

```mermaid
flowchart TD
    A["open with DPS +<br/>operational certificate requested"] --> B[get CSR from provider]
    B --> C["DPS register with the CSR in the payload"]
    C --> D{assigned?}
    D -->|"yes, chain present"| E["store issued certificate +<br/>notify the application"]
    D -->|"yes, no chain"| F["continue with the bootstrap credential"]
    E --> G["Hub CONNECT using the operational credential"]
    F --> G

    G --> H[CONNECTED]
    H --> I["CSR renewal over the hub<br/>classic hub only"]
    I --> J["PUBLISH issueCertificate request"]
    J --> K{credentials response}
    K -->|202 accepted| L["accepted notification,<br/>keep waiting"]
    L --> K
    K -->|200 issued| M["issued notification with the chain,<br/>the application persists it"]
    K -->|"error or timeout"| N["failed notification,<br/>slot released"]
    M --> O["The new certificate is used on<br/>the NEXT connect attempt"]
```

Renewal topics (classic hub), identical in both clients:

| Direction | Topic |
| --- | --- |
| Publish | `$iothub/credentials/POST/issueCertificate/?$rid={request_id}` |
| Subscribe | `$iothub/credentials/res/#` |
| Response | `$iothub/credentials/res/{status}/{request_id}` |

Rules that apply to every client:

- Only one CSR operation per request id may be in flight; a duplicate fails fast with a *busy* result.
- The issued chain is delivered leaf-first as base64 DER. Where the client hands it over in a callback
  it is only valid for the duration of that callback, so the application must copy or persist it.
- A successful renewal does **not** tear down the live session. The new credential takes effect on the
  next connect, whether that is a reconnect or an explicit reopen.
- At connect time the provider is asked for the operational credential first and falls back to the
  bootstrap credential when it is absent or uninitialized.
- CSR-based DPS enrollment requires the `2025-07-01-preview` DPS api-version.
- Renewal over a gen2 hub is not yet a supported service feature; a client that is asked for it must
  fail with an explicit unsupported error rather than silently doing nothing.

---

## 7. Device update: onboarding and renewal

**C:** planned (the ADUv1 engine internals are implemented and reused; the ADUv1 twin-based public API
is being removed) · **.NET:** none — no update support exists in the .NET client today, and this
section is the contract it will have to meet when it is added.

What survives from ADUv1 is everything that has nothing to do with transport. Update support is
re-layered into a transport-independent **update engine** — manifest parsing, signature and root-key
verification, integrity hashing, the download/backup/install/apply state machine, and reboot/resume
persistence — plus an **update channel** abstraction carrying delivery and reporting.

ADUv2 is a **device-initiated pull protocol**, and its device-facing delivery moved **off** a
dedicated update HTTPS endpoint. The agent calls an updating operation on a gateway it already talks
to, reusing the credential it already has; the gateway is an authenticated pass-through. The device
never talks to the update service directly, needs no update-specific credential, and there is no twin,
no subscription and no unsolicited offer.

| Phase | Gateway | Operation (working name) |
| --- | --- | --- |
| First-time / bootstrap (**before** provisioning) | DPS | `GetOnboardingDeviceUpdate` |
| Regular / operational (**after** provisioning) | DPS *(interim)*, IoT Hub *(later)* | `GetDeviceUpdate` |
| Reporting, either phase | same gateway as the fetch | `ReportDeviceUpdateStatus` |

The device selects onboarding vs regular **by which operation it calls**; the gateway does not infer
or validate the choice.

> **The hub-side updating API is not available in the first release.** In preview, DPS fronts both the
> bootstrap and the operational flow, using the existing DPS device credential (X.509 in phase 1). The
> operational path moves to IoT Hub afterwards **with no device-contract change** — same request and
> response, a different gateway. Treat the gateway as a channel parameter, not a constant.

### 7.1 Onboarding — bootstrap update, before provisioning

The critical ordering fact for this document: **the bootstrap update check happens before the device
registers.** The agent, not the connection client, drives it. On the happy path `open()` waits until
the check reports no update, so several updates can chain before provisioning.

The check is **advisory and must never block provisioning.** If it errors, times out, or the account
is not linked, the device proceeds to register anyway.

```mermaid
sequenceDiagram
    autonumber
    participant ADU as Update engine + DPS channel
    participant Conn as Connection client
    participant DPS
    participant Hub

    loop until "no update" or an advisory failure
        ADU->>DPS: GetOnboardingDeviceUpdate (agent info, installed update id, ETags)
        alt update available
            DPS-->>ADU: service configuration + update metadata (workflow id, manifest, signature, file URLs)
            ADU->>ADU: verify signature, download files, install (reboot if required)
            ADU->>DPS: ReportDeviceUpdateStatus (workflow id, installed update id, last install result)
        else no update
            DPS-->>ADU: 200 with update metadata omitted
        end
    end

    Note over ADU,Conn: provisioning proceeds - on success or on an advisory failure
    Conn->>DPS: Register (unchanged, CSR optional)
    DPS-->>Conn: assignedHub, deviceId, connectionProfile, issuedCertificateChain
    Conn->>Hub: CONNECT with operational auth
```

- The loop is genuine: after installing a bootstrap update the agent re-checks, because a bootstrap
  deployment can chain.
- Bootstrap progress is stored **in the bootstrap update job**, not on the device's own attributes —
  the device resource does not exist yet.
- Trust comes from the **root-key package** whose download URL is returned by the same call. Account
  scoping (an account id bound into the manifest signature) is **deferred**: DPS returns no account id,
  so the device verifies provenance but not account scoping. Base signature validation stays required.
- File URLs are **not** covered by the signed manifest; payloads are downloaded straight from blob
  storage and integrity comes from the per-file hashes inside the manifest.
- Bootstrap orchestration is entirely the agent's responsibility in the first release. DPS does **not**
  enforce that a device is on a given update version before provisioning it.

### 7.2 Renewal — operational update check, after `CONNECTED`

```mermaid
sequenceDiagram
    autonumber
    participant Conn as Connection client
    participant ADU as Update engine + channel
    participant GW as Gateway (Hub, or DPS in preview)
    participant Store as Durable update state

    Conn-->>ADU: CONNECTED
    ADU->>Store: load state
    Store-->>ADU: installed update id, ETags, unsent report
    opt report pending from a previous session
        ADU->>GW: ReportDeviceUpdateStatus (workflow id, last install result)
    end

    loop poll at the agent's own cadence
        ADU->>GW: GetDeviceUpdate (agent info, installed update id, ETags)
        Note over GW: the service derives the device class from the agent profile + compatibility properties
        alt update available
            GW-->>ADU: service configuration + update metadata (workflow id, manifest, signature, file URLs)
            ADU->>ADU: verify, download, backup, install, apply
            ADU->>Store: persist state
            ADU->>GW: ReportDeviceUpdateStatus (workflow id, installed update id, last install result)
        else no update
            GW-->>ADU: 200 with update metadata omitted
        end
    end

    Conn--xADU: connection drop
    Note over ADU: install continues, report held in durable storage
    Conn-->>ADU: CONNECTED again
    ADU->>GW: retry the report until acked, then resume polling
```

The last install result carries the terminal outcome, its failure origin, the list of extended result
codes and a per-step result map — see [eng/aduv2-spec.md](eng/aduv2-spec.md) for the field-level shape.

### 7.3 Rules every client must implement

- **Poll, never wait.** The agent owns the cadence. A missed poll is not an error and there is no offer
  to lose, which is why a reconnect needs no replay of update subscriptions — there are none.
- **The workflow id is the correlation key.** It arrives in the update metadata and is echoed on the
  report. Reporting is **idempotent on the workflow id alone**; a conflicting terminal result for the
  same id is rejected as a conflict.
- **The device is the sole retrier.** The gateway fails fast with one attempt per hop. The agent honours
  `Retry-After` on throttling and retries the status report until it is acked — a report is a durable
  write and must not be lost.
- **Drive behaviour from the machine-readable error code, never the HTTP status.** A stale agent-info
  ETag means resend the full agent info; a stale service-config ETag means re-ask without it; an
  unlinked update account means "no update service configured", which is not a failure.
- **"No update" is a success.** It is a 200 with the update metadata omitted, not an error.
- **Update never drives the connection.** It does not open, close, or force a reconnect. It does
  sequence ahead of the *first* `open()`, via the advisory bootstrap check in
  [§7.1](#71-onboarding--bootstrap-update-before-provisioning).
- **Compatibility properties are opaque key/value pairs** (1–5) reported by the agent alongside an
  opaque agent profile; the service combines them into a device class. The agent assigns them no
  meaning.
- **The gateway is a channel parameter.** Bootstrap always uses DPS; operational uses DPS in preview and
  IoT Hub afterwards, with no device-contract change. This SDK plans to expose the operational channel
  as a gen2 feature client ([eng/client-separation.md](eng/client-separation.md) §8), but the service
  contract binds update delivery to the updating operations, not to a connection profile.

---

## 8. Combined reference diagram

One picture of the whole device lifecycle: connection and reconnection, connection-profile selection,
certificate management (onboarding + renewal) and device update (onboarding + renewal). Dotted edges
are deferred effects — they do not happen inline.

```mermaid
flowchart TB
    BOOT["Agent boot"] --> BCHK["Bootstrap update check<br/>GetOnboardingDeviceUpdate via DPS"]
    BCHK -->|"update available"| BINST["Verify, download, install,<br/>report, re-check"]
    BINST --> BCHK
    BCHK -->|"no update, or advisory failure"| IDLE["IDLE"]

    IDLE -->|"open() with id scope"| REG["DPS register<br/>CSR optional"]
    IDLE -->|"open() with host"| CRED

    REG --> ASSIGN["Assignment:<br/>assignedHub, deviceId,<br/>connectionProfile,<br/>issuedCertificateChain"]
    ASSIGN --> STORE1["Store the issued chain"]
    STORE1 --> PROFILE{"connectionProfile"}

    PROFILE -->|"unknown profile"| FAULTED
    PROFILE -->|"classic - gen1"| CRED
    PROFILE -->|"mqttV5 - gen2"| CRED["Load credential:<br/>operational, else bootstrap"]

    CRED --> CONNECTING["CONNECTING<br/>MQTT CONNECT + mutual TLS"]
    CONNECTING --> BIRTH["Presence handshake<br/>gen2 only"]
    BIRTH --> SUBS["Replay persistent subscriptions"]
    CONNECTING --> SUBS
    SUBS --> CONNECTED["CONNECTED"]

    CONNECTED --> CRENEW["Cert renewal:<br/>CSR over the hub, 202 then 200"]
    CRENEW -.->|"new chain used on<br/>the next connect"| CRED
    CONNECTED --> ARENEW["Operational update check:<br/>poll GetDeviceUpdate,<br/>ReportDeviceUpdateStatus"]

    CONNECTED -->|"close()"| DISC["DISCONNECTING"] --> IDLE
    CONNECTED --> DROP{"drop or error"}
    DROP -->|"reconnect disabled,<br/>attempts exhausted<br/>or fatal failure"| FAULTED["FAULTED"]
    DROP -->|"reconnect enabled"| RECON["RECONNECTING<br/>exponential backoff + jitter"]
    RECON -->|"DPS configured"| REG
    RECON -->|"direct host"| CRED
    ARENEW -.->|"workflow id and unsent<br/>report persisted"| RECON
```

Reading it as four overlapping concerns:

| Concern | Onboarding (DPS gateway, onboarding auth) | Renewal (post-`CONNECTED`, operational auth) |
| --- | --- | --- |
| **Certificates** | CSR in the registration, issued chain in the assignment | CSR over the hub; the new chain applies on the next connect |
| **Device update** | `GetOnboardingDeviceUpdate` loop **before** registration, advisory | Polled `GetDeviceUpdate` / `ReportDeviceUpdateStatus` (DPS in preview, Hub afterwards) |
| **Connection profile** | Declared in the assignment; selects MQTT version and generation | Re-resolved on every reconnect that goes through DPS |
| **Connection** | DPS phases inside `CONNECTING` | Backoff-driven reconnect replays the whole path |

The two onboarding concerns are **not** symmetric, and that asymmetry is the thing to remember: the
CSR travels *inside* registration, while the bootstrap update check happens *before* it and must
complete first.

---

## 9. Cross-client status summary

| Area | C | .NET |
| --- | --- | --- |
| Top-level states ([§2](#2-top-level-state-machine)) | implemented, user-visible state value | partial — internal lifecycle, surfaced as events, no single state value |
| DPS provisioning inside connect ([§3](#3-full-connect-sequence)) | implemented | implemented |
| CSR carried in the registration ([§3](#3-full-connect-sequence)) | implemented | planned — the field is always sent empty |
| Subscriptions established before `CONNECTED` ([§3](#3-full-connect-sequence)) | partial — not gated on SUBACK | implemented — connect completes, and feature traffic is latched, on readiness |
| gen2 birth handshake, 60 s timeout ([§3](#3-full-connect-sequence)) | implemented | implemented |
| Connection profile from DPS ([§4](#4-connection-profile-selection)) | planned — blocked on the api-version | partial — local boolean placeholder, blocked on the api-version |
| Exponential backoff with jitter ([§5](#5-reconnection)) | implemented, fixed policy | implemented, caller-replaceable policy |
| Fatal-failure classification ([§5.2](#52-what-triggers-a-reconnect)) | planned | implemented |
| Certificate renewal over the hub ([§6](#6-certificate-management-onboarding-and-renewal)) | implemented (classic) | implemented (classic); explicit unsupported error on gen2 |
| Device update ([§7](#7-device-update-onboarding-and-renewal)) | planned — engine internals implemented and reused | none |

---

## 10. Language mapping

How the vocabulary of this document maps onto each client. Concept names in the left column are the
normative ones; the language columns are informative and follow the code.

| Concept | C | .NET |
| --- | --- | --- |
| Connection client | `az_iot_connection_client` | `Unified.Connection.ConnectionClient` (dispatches by profile) and `Gen2.Connection.ConnectionClient` |
| Open / close | `az_iot_connection_client_open()` / `_close()` | `ConnectAsync()` / `ProvisionAndConnectAsync()` / `DisconnectAsync()` |
| State value | `az_iot_connection_state` (`IDLE`…`FAULTED`) | none — `ConnectingAsync` / `ConnectedAsync` / `DisconnectedAsync` events, plus an internal presence-complete signal |
| Session maintenance and reconnect | `connection_client.c` + `reconnect.c` | `MqttConnectionManager` |
| Retry policy | `az_iot_reconnection_policy` (initial delay, max delay, max attempts, jitter %) | `IRetryPolicy`, default `ExponentialBackoffRetryPolicy`, `NoRetry` to disable |
| Connection profile | `az_iot_connection_profile` / `az_iot_hub_profile` (planned) | `ConnectionContext.IsGen2Hub` (placeholder boolean) |
| Provisioning settings | id scope + global endpoint in the connection options | `ProvisioningSettings` |
| Registration result | DPS assignment struct | `DeviceRegistrationResult` |
| Credentials | `az_iot_certificate_provider` (`BOOTSTRAP` / `OPERATIONAL`) | `X509AuthenticationProvider` + `ConnectionContext.IssuedClientCertificates` |
| CSR request / response | `az_iot_connection_client_send_csr()` and its callbacks | `SendCertificateSigningRequestAsync()` returning a `CertificateSigningOperation` |
| MQTT abstraction | adapter vtable (`how_to_byo_mqtt_client.md`) | `IMqttClient`, default backed by MQTTnet |
| Update engine / channel | `adu_core` + `az_iot_adu_channel` (planned) | not present |

For the C-side detail behind this table — headers, source files, exact enum spellings and defaults —
see [eng/connection-c.md](eng/connection-c.md).
