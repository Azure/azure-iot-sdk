# Connection and Reconnection Lifecycle

Reference document for the connection lifecycle shared by the **C99** and **.NET** Azure IoT device
clients. The C SDK implementation in [c/src/core/connection_client.c](../src/core/connection_client.c)
is the current source of truth; the .NET client is expected to expose the same observable states,
the same ordering guarantees, and the same retry semantics, even where the internal structure
differs.

Related documents:

- [design.md](design.md) — overall layering and adapter model.
- [eng/certificate-management.md](eng/certificate-management.md) — CSR / operational certificate design.
- [eng/client-separation.md](eng/client-separation.md) — connection profile (§2) and the ADU channel split (§8).
- [eng/connection-state-and-error-propagation.md](eng/connection-state-and-error-propagation.md) — observer registry and status codes.
- [dps-integration.md](dps-integration.md), [devnotes.md](devnotes.md) — DPS contract and the running requirements log.
- **ADUv2 device protocol** — *Azure Device Update v2 — Public Preview (Ignite 2026)*, Leo Lie /
  Joe Heiniger / Darko Aleksic, 7/6/2026
  ([SharePoint](https://microsoft.sharepoint.com/:w:/r/teams/DigitalOperations/_layouts/15/Doc.aspx?sourcedoc=%7B0f2203ff-bda7-4f97-b2a5-468fcb95f7f0%7D&action=default&share=cQr_AyIPp72XT7KlRo_LlffwEgUCJRfZ3zpZHPh4PbcI-NloUw)).
  §7 below is derived from it. [eng/adu-client-design.md](eng/adu-client-design.md) and
  [eng/adu-feature-support.md](eng/adu-feature-support.md) still describe ADUv1 and the superseded
  HTTPS-RPC-to-ADR shape, and have **not** been reconciled with it.

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
[az_iot_connection_client.h](../inc/azure/iot/az_iot_connection_client.h); transitions are all funneled
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

Any failure or drop in a DPS phase is handled by the same backoff path as a hub failure, and a
retry restarts provisioning from `DPS_CONNECTING`.

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
        DPS-->>Conn: status = assigned (assignedHub, deviceId, connectionProfile, issuedCertificateChain, ADU accountId)
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
2. The DPS session is fully torn down before the hub session is created — they are never concurrent,
   and DPS always uses MQTT 3.1.1 even when the hub session uses v5.
3. The operational certificate is preferred over the bootstrap certificate on every connect attempt,
   including reconnects.
4. **The ADUv2 bootstrap update check runs before `open()`, not inside it.** The agent drives
   `RequestBootstrapUpdate` against the DPS gateway until the service reports no update, and only
   then does the connection client register. See [§7](#7-aduv2-onboarding-and-renewal-planned).
5. The DPS assignment is the single delivery point for everything the device learns about its
   placement: hub, device id, connection profile, issued certificate chain, and the ADU account id
   used to validate update downloads.

---

## 4. Connection profile selection **[planned]**

The profile is what DPS says the device landed on. It is **reported, never selected** — there is no
caller-facing knob, and falling back from one profile to another is application logic, not SDK
behaviour. See [eng/client-separation.md §2](eng/client-separation.md) for the full rationale.

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
[az_iot_connection_client.h](../inc/azure/iot/az_iot_connection_client.h), computed by
`az_iot_reconnect_delay_ms()` in [reconnect.c](../src/core/reconnect.c):

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
- Any failure during a DPS phase (a reconnect restarts provisioning from the beginning).
- Hub-Next birth-ack timeout (60 s per handshake step).

A user-initiated `close()` never triggers a reconnect: it sets an internal `user_close` flag that is
checked before backoff is scheduled.

### 5.3 What is preserved across a reconnect

| Item | Preserved | Behaviour |
| --- | --- | --- |
| Persistent subscriptions | Yes | Re-issued before `CONNECTED` is announced. |
| Assigned hub host / device id | Yes | Cached after the first DPS assignment. |
| Connection profile | Yes | Re-resolved from the new assignment; a change invalidates held feature clients (open question, §4). |
| Operational certificate | Yes | Owned by the certificate provider, reloaded on each attempt. |
| ADU workflow state | Yes | Owned by `adu_core` and persisted, so an install survives a reconnect and a reboot. |
| Reconnect attempt counter | Reset on success | Incremented per failed attempt. |
| In-flight QoS 1 PUBACKs | No | Packet ids belong to the destroyed adapter; callers must re-send. |
| Twin GET/PATCH, method responses, telemetry in flight | No | Feature clients must re-issue. |
| ADU status report not yet delivered | Yes | Held in durable storage and sent on the next successful `ReportStatus`. |
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

ADUv2 is a **device-initiated pull protocol**. Nothing is pushed: the agent calls an updating API on
a gateway, and the gateway forwards to ADR and then ADU. There is no twin, no subscription, and no
unsolicited offer. Which gateway depends on the phase and the milestone:

| Phase | Gateway | Auth | Operations |
| --- | --- | --- | --- |
| First-time / bootstrap (**before** provisioning) | DPS | onboarding | `RequestBootstrapUpdate(aduAgentConfig, installedUpdateId)`, `ReportStatus(installedUpdateId, installResult)` |
| Regular / operational (**after** provisioning) | IoT Hub *(post-Ignite)* | operational | `RequestUpdate(aduAgentConfig, installedUpdateId)`, `ReportStatus(...)`, agent property reporting |
| Regular / operational, **Ignite '26 preview** | DPS *(interim)* | onboarding only | `RequestUpdate(...)`, `ReportStatus(...)` |

> **The Hub updating API is not ready for Ignite '26.** In preview, the operational update check runs
> against DPS with onboarding auth and is deprecated as soon as the Hub API ships. Treat the gateway
> as a channel parameter, not a constant.

### 7.1 Onboarding — bootstrap update, before provisioning

The critical ordering fact for this document: **the bootstrap update check happens before the device
registers.** The agent, not the connection client, drives it, and `open()` must not be called until
the check reports no update.

```mermaid
sequenceDiagram
    autonumber
    participant ADU as adu_core + DPS channel
    participant Conn as Connection client
    participant DPS
    participant Hub

    loop until "no update"
        ADU->>DPS: RequestBootstrapUpdate(aduAgentConfig, installedUpdateId)
        alt update available
            DPS-->>ADU: update manifest, signature, download URLs
            ADU->>ADU: verify, download, install (reboot if required)
            ADU->>DPS: ReportStatus(installedUpdateId, installResult)
        else no update
            DPS-->>ADU: no update
        end
    end

    Note over ADU,Conn: only now is the device allowed to provision
    Conn->>DPS: Register (CSR optional)
    DPS-->>Conn: assignedHub, deviceId, connectionProfile, issuedCertificateChain, ADU accountId
    Conn->>Hub: CONNECT with operational auth
```

- The loop is genuine: after installing a bootstrap update the agent re-checks, because a bootstrap
  deployment can chain.
- Bootstrap progress is stored **in the bootstrap update job**, not on the device's ADR attributes —
  the device resource does not exist yet.
- DPS returns the **ADU account id** in the registration result so the device can validate the update
  source before downloading.
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
    ADU->>GW: report properties (agentInfo.compatibilityProperties)
    Note over GW: ADU computes device class, creates the ADR device attribute
    ADU->>Store: load_state()
    Store-->>ADU: installedUpdateId, unsent installResult
    opt report pending from a previous session
        ADU->>GW: ReportStatus(installedUpdateId, installResult)
    end

    loop poll at the agent's own cadence
        ADU->>GW: RequestUpdate(aduAgentConfig, installedUpdateId)
        alt update available
            GW-->>ADU: update manifest, signature, download URLs
            ADU->>ADU: verify, download, backup, install, apply
            ADU->>Store: persist_state()
            ADU->>GW: ReportStatus(installedUpdateId, installResult)
        else no update
            GW-->>ADU: no update
        end
    end

    Conn--xADU: connection drop
    Note over ADU: install continues, installResult held in durable storage
    Conn-->>ADU: CONNECTED again
    ADU->>GW: ReportStatus, then resume polling
```

`installResult` is the terminal report shape:

```jsonc
{
  "outcome": "SUCCEEDED|FAILED|CANCELED",
  "failureOrigin": "ADU_MANAGED_RESOURCE|AGENT_CORE|DEVICE|...",
  "resultCode": -1,
  "extendedResultCodes": "c00e5011",   // comma-separated hex string
  "resultDetails": "Failed to download update payload from CDN."
}
```

### 7.3 Rules both clients must implement

- **Poll, never wait.** The agent owns the cadence. A missed poll is not an error and there is no
  offer to lose, which is why a reconnect needs no replay of ADU subscriptions — there are none.
- **`installedUpdateId` is the correlation key**, sent on every check. There is no workflow id on the
  device-facing contract and no per-device retry: once a device reaches terminal failure, the user
  must cancel and reschedule the run.
- **Report status is terminal-only and durable.** A result produced while disconnected is held and
  sent on the next successful call, so an install that completes across a reboot still reports.
- **ADU never drives the connection.** It does not open, close, or force a reconnect. It does gate
  the *first* `open()`, via the bootstrap check in §7.1.
- **Compatibility properties are opaque key/value pairs** reported by the agent; ADU uses them to
  compute a device class and match deployments. The agent assigns no meaning to them.
- **The gateway is a channel parameter.** Bootstrap always uses DPS; operational uses DPS in the
  Ignite '26 preview and IoT Hub afterwards. This SDK plans to expose the operational channel as a
  gen2 feature client ([eng/client-separation.md](eng/client-separation.md) §8), but the service
  contract binds ADU to the updating APIs, not to a connection profile.
- **Known preview gap:** the interim DPS operational path depends on the DPS enrollment group
  continuing to exist. Deleting it breaks operational updating.

---

## 8. Combined reference diagram

One picture of the whole device lifecycle: connection and reconnection, connection-profile selection,
certificate management (onboarding + renewal) and ADUv2 (onboarding + renewal). Dotted edges are
deferred effects — they do not happen inline.

```mermaid
flowchart TB
    BOOT["Agent boot"] --> BCHK["ADUv2 bootstrap check<br/>RequestBootstrapUpdate via DPS<br/>onboarding auth"]
    BCHK -->|"update available"| BINST["Verify, download, install,<br/>ReportStatus, re-check"]
    BINST --> BCHK
    BCHK -->|"no update"| IDLE["IDLE"]

    IDLE -->|"open() with id_scope"| REG["DPS register<br/>CSR optional"]
    IDLE -->|"open() with host"| CRED

    REG --> ASSIGN["Assignment:<br/>assignedHub, deviceId,<br/>connectionProfile,<br/>issuedCertificateChain,<br/>ADU accountId"]
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
    CONNECTED --> ARENEW["ADUv2 operational check:<br/>report properties, then poll<br/>RequestUpdate / ReportStatus"]

    CONNECTED -->|"close()"| DISC["DISCONNECTING"] --> IDLE
    CONNECTED --> DROP{"drop or error"}
    DROP -->|"reconnect disabled<br/>or attempts exhausted"| FAULTED["FAULTED"]
    DROP -->|"reconnect enabled"| RECON["RECONNECTING<br/>exponential backoff + jitter"]
    RECON -->|"DPS configured"| REG
    RECON -->|"direct host"| CRED
    ARENEW -.->|"installedUpdateId and<br/>unsent installResult persisted"| RECON
```

Reading it as four overlapping concerns:

| Concern | Onboarding (DPS gateway, onboarding auth) | Renewal (post-`CONNECTED`, operational auth) |
| --- | --- | --- |
| **Certificates** | CSR in the registration, issued chain in the assignment | `send_csr` over the hub; new chain applies on the next connect |
| **ADUv2** | `RequestBootstrapUpdate` loop **before** registration; ADU account id returned in the assignment | Agent property report, then polled `RequestUpdate` / `ReportStatus` (Hub post-Ignite, DPS in preview) |
| **Connection profile** | Declared in the assignment; selects MQTT version and generation | Re-resolved on every reconnect that goes through DPS |
| **Connection** | DPS phases inside `CONNECTING` | Backoff-driven reconnect replays the whole path |

The two onboarding concerns are **not** symmetric, and that asymmetry is the thing to remember:
the CSR travels *inside* registration, while the ADU bootstrap check happens *before* it and must
complete first.

---

## 9. Implementation index (C SDK)

| Topic | Location | Status |
| --- | --- | --- |
| State enum, policy, options | [az_iot_connection_client.h](../inc/azure/iot/az_iot_connection_client.h) | implemented |
| State transitions, connect attempt, event handling | [connection_client.c](../src/core/connection_client.c) | implemented |
| Backoff computation and defaults | [reconnect.c](../src/core/reconnect.c) | implemented |
| Certificate provider contract | [az_iot_certificate_provider.h](../inc/azure/iot/az_iot_certificate_provider.h) | implemented |
| Managed OpenSSL provider | [az_iot_certificate_provider_managed.c](../adapters/cert_openssl/az_iot_certificate_provider_managed.c) | implemented |
| Connection profile enum, `az_iot_hub_profile`, `get_hub_profile()` | [eng/client-separation.md](eng/client-separation.md) §2 | planned — blocked on the DPS api-version |
| `adu_core` / `az_iot_adu_channel` split | [eng/client-separation.md](eng/client-separation.md) §8 | planned |
| ADU engine internals reused by ADUv2 | [c/src/features/adu](../src/features/adu) | implemented (ADUv1 API to be removed) |
| ADUv2 device contract | *Azure Device Update v2 — Public Preview*, linked above | planned — Hub gateway post-Ignite |
