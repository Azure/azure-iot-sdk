# Connection Lifecycle — implementation status

Per-client coverage of the contract in [../connection.md](../connection.md). That document defines
the behaviour every client must honour and deliberately says nothing about how far each one has got;
this one is the record of how far each one has got. Section numbers below refer to it.

C-specific detail — headers, source files, exact enum spellings and defaults — is in
[connection-c.md](connection-c.md).

Keep this file in step with the code. It is expected to change often; `connection.md` is not.

## Status legend

| Mark | Meaning |
| --- | --- |
| **implemented** | Present in that client's code today. |
| **partial** | Present, but with a known gap called out below. |
| **planned** | Designed and agreed, not yet in code. |
| **none** | Not implemented and not started in that client. |

---

## By section

| Section | C | .NET |
| --- | --- | --- |
| [§2 Top-level state machine](../connection.md#2-top-level-state-machine) | implemented | partial — the same lifecycle is driven internally, but it is surfaced as connect/disconnect events rather than as a single user-visible state value |
| [§3 Full connect sequence](../connection.md#3-full-connect-sequence) | implemented | implemented |
| [§4 Connection profile selection](../connection.md#4-connection-profile-selection) | implemented | implemented |
| [§5 Reconnection](../connection.md#5-reconnection) | implemented | implemented |
| [§6 Certificate management](../connection.md#6-certificate-management-onboarding-and-renewal) | implemented | partial — the CSR now rides the registration, but renewal over the hub is classic-only |
| [§7 Device update](../connection.md#7-device-update-onboarding-and-renewal) | implemented over the provisioning gateway — the hub channel is not written | none — no update support exists in the .NET client today, and §7 is the contract it will have to meet when it is added |
| [§10 Connection topology](../connection.md#10-connection-topology) | partial — the three connect paths exist; no gen2 file-upload client, and a provisioning-only feature can attach to a direct-connect client and then fail every call | partial — same file-upload gap is not applicable; topology otherwise followed |

---

## By guarantee

| Area | C | .NET |
| --- | --- | --- |
| Top-level states ([§2](../connection.md#2-top-level-state-machine)) | implemented, user-visible state value | partial — internal lifecycle, surfaced as events, no single state value |
| DPS provisioning inside connect ([§3](../connection.md#3-full-connect-sequence)) | implemented | implemented |
| CSR carried in the registration ([§3](../connection.md#3-full-connect-sequence)) | implemented | implemented — `ProvisioningSettings.CertificateSigningRequest`, on its own api-version |
| Subscriptions established before `CONNECTED` ([§3](../connection.md#3-full-connect-sequence)) | implemented — the gate waits for every `FAILS_SESSION` SUBACK, with its own deadline | implemented — connect completes, and feature traffic is latched, on readiness |
| gen2 birth handshake, 60 s timeout ([§3](../connection.md#3-full-connect-sequence)) | implemented | implemented |
| Connection profile from DPS ([§4](../connection.md#4-connection-profile-selection)) | implemented — `az_iot_connection_profile`, reported by `get_hub_profile()`; still resolves to classic until the api-version carrying it is deployed | implemented — `ConnectionProfile` enum on the registration result; same api-version dependency |
| Exponential backoff with jitter ([§5](../connection.md#5-reconnection)) | implemented, fixed policy | implemented, caller-replaceable policy |
| Fatal-failure classification ([§5.2](../connection.md#52-what-triggers-a-reconnect)) | partial — a refused SUBACK is terminal regardless of policy; a deterministic CONNACK refusal is still retried | implemented — `ErrorRetryability` { `Terminal`, `IdentityTerminal`, `Retryable` } on every classified failure |
| Failure taxonomy — MQTT reason-code fidelity ([§9](../connection.md#9-connection-failure-taxonomy)) | partial — CONNACK and SUBACK codes are classified by their own mappers; PUBACK and server-DISCONNECT codes are still flattened | implemented — codes are preserved as typed values and classified per §9.3.3, §9.3.7, §9.4.1, §9.4.2 and §9.4.8, which the source cites by section number |
| Certificate renewal over the hub ([§6](../connection.md#6-certificate-management-onboarding-and-renewal)) | implemented (classic) | partial (classic) — no busy rejection for a duplicate in-flight request; explicit unsupported error on gen2 |
| gen2 file upload ([§10.3](../connection.md#103-what-a-classic-sunset-would-cost)) | **absent** — gen1 only | n/a |
| Device update ([§7](../connection.md#7-device-update-onboarding-and-renewal)) | implemented over the provisioning gateway; hub channel not written | none |

---

## Known gaps, in detail

### Duplicate in-flight CSR request id is not rejected (.NET)

[§6](../connection.md#6-certificate-management-onboarding-and-renewal) requires a second request
carrying an already in-flight request id to fail fast with a busy result.

The C client enforces it: it keeps a single CSR slot and returns a busy error.

The .NET client does not. It keeps a map keyed by request id, ignores the insert result and publishes
regardless, so a duplicate in-flight request id is sent and the caller gets an operation that never
completes.

### Connection profile is not yet reported by DPS (both)

[§4](../connection.md#4-connection-profile-selection) is implemented in both clients — a real
profile value, resolved from the service-reported string, driving protocol and feature selection.
What is still missing is the **service**: `connectionProfile` is new in a preview api-version that
is not deployed, so the property never arrives and both clients resolve the contract default,
`classic`. The C client carries a development override that applies only when the property is
absent, so an actual wire value always wins and enabling it cannot mask the rollout.

### Fatal-failure classification (C)

[§5.2](../connection.md#52-what-triggers-a-reconnect) requires failures that cannot succeed on retry
— protocol errors, malformed packets, authorization failures, session-taken-over, invalid topic
filters, server-moved — to go straight to `FAULTED` rather than being retried.

The .NET client classifies all of them, through `ErrorRetryability`.

The C client classifies **one family**: a SUBACK refusal the broker will repeat is terminal even
when a reconnection policy is configured. Everything else — including `rc=1 unacceptable protocol
version` and the deterministic v5 CONNACK codes — still retries until the policy is exhausted.
The SUBACK path is the shape the CONNACK path needs; see [connection-c.md §9.6](connection-c.md#96-known-gaps).

### Failure taxonomy — where each client stands

[§9](../connection.md#9-connection-failure-taxonomy) classifies every failure as terminal, retryable,
contained or benign.

**.NET follows it.** `MqttConnectionManager` classifies CONNACK codes, server-DISCONNECT codes,
socket failures, TLS handshake failures and programming errors into `ErrorRetryability`, and the
source cites the taxonomy by section number. The two divergences recorded in the previous revision
of this file — socket failures treated as fatal, and `0x83 Implementation specific error` treated
as fatal — have both been corrected to the contract.

**C is partway.** CONNACK and SUBACK each have a mapper that preserves the wire code and classifies
it. PUBACK failures and server-initiated DISCONNECTs still collapse to one generic transport
result, so `0x8E Session taken over` is indistinguishable from a routine drop and a caller cannot
separate a publish refusal it must not retry from a quota it should back off on. The full list is
[connection-c.md §9.6](connection-c.md#96-known-gaps).

> **This document is a dependency of shipped .NET code.** `MqttConnectionManager.cs` cites
> `connection.md` §9.3.3, §9.3.7, §9.4.1, §9.4.2 and §9.4.8 as the reason for each branch. Those
> section numbers are therefore a stable reference: content may be corrected, but §9 must not be
> renumbered without updating the citations.

