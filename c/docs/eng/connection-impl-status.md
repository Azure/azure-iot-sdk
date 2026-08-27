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
| [§3 Full connect sequence](../connection.md#3-full-connect-sequence) | implemented | implemented, except the CSR carried in the registration |
| [§4 Connection profile selection](../connection.md#4-connection-profile-selection) | planned | partial — the branch exists and drives protocol and feature selection, but it is a locally-set boolean rather than the DPS-declared string |
| [§5 Reconnection](../connection.md#5-reconnection) | implemented | implemented |
| [§6 Certificate management](../connection.md#6-certificate-management-onboarding-and-renewal) | implemented | partial — renewal is implemented for the classic hub only; the CSR is not yet sent during registration, and renewal is not available on a gen2 hub |
| [§7 Device update](../connection.md#7-device-update-onboarding-and-renewal) | planned — the ADUv1 engine internals are implemented and reused; the ADUv1 twin-based public API is being removed | none — no update support exists in the .NET client today, and §7 is the contract it will have to meet when it is added |

---

## By guarantee

| Area | C | .NET |
| --- | --- | --- |
| Top-level states ([§2](../connection.md#2-top-level-state-machine)) | implemented, user-visible state value | partial — internal lifecycle, surfaced as events, no single state value |
| DPS provisioning inside connect ([§3](../connection.md#3-full-connect-sequence)) | implemented | implemented |
| CSR carried in the registration ([§3](../connection.md#3-full-connect-sequence)) | implemented | planned — the field is always sent empty |
| Subscriptions established before `CONNECTED` ([§3](../connection.md#3-full-connect-sequence)) | partial — not gated on SUBACK | implemented — connect completes, and feature traffic is latched, on readiness |
| gen2 birth handshake, 60 s timeout ([§3](../connection.md#3-full-connect-sequence)) | implemented | implemented |
| Connection profile from DPS ([§4](../connection.md#4-connection-profile-selection)) | planned — blocked on the api-version | partial — local boolean placeholder, blocked on the api-version |
| Exponential backoff with jitter ([§5](../connection.md#5-reconnection)) | implemented, fixed policy | implemented, caller-replaceable policy |
| Fatal-failure classification ([§5.2](../connection.md#52-what-triggers-a-reconnect)) | planned | implemented |
| Failure taxonomy — MQTT reason-code fidelity ([§9](../connection.md#9-connection-failure-taxonomy)) | partial — CONNACK codes are classified; SUBACK, PUBACK and server-DISCONNECT codes are flattened | partial — codes are preserved as typed values and classified, with two rows that diverge from the contract |
| Certificate renewal over the hub ([§6](../connection.md#6-certificate-management-onboarding-and-renewal)) | implemented (classic) | partial (classic) — no busy rejection for a duplicate in-flight request; explicit unsupported error on gen2 |
| Device update ([§7](../connection.md#7-device-update-onboarding-and-renewal)) | planned — engine internals implemented and reused | none |

---

## Known gaps, in detail

### `CONNECTED` is announced before subscriptions are live (C)

[§3](../connection.md#3-full-connect-sequence) requires that `CONNECTED` is announced only after the
birth handshake (gen2) and after persistent subscriptions have been re-issued, so a feature client
never observes `CONNECTED` while its topic filters are missing.

The .NET client honours this: the connect call does not complete, and feature traffic is latched,
until the subscriptions (classic) or the presence flow (gen2) have completed.

The C client does **not** yet. It transitions to `CONNECTED` first and only then issues the
subscribes, discarding the result, so a request published from inside the callback can reach the wire
ahead of its own SUBSCRIBE. Treat the guarantee as the *intended* contract; see
[connection-c.md](connection-c.md) for the tracking detail and the fix phase. The gen2 birth
handshake *is* correctly gated in both clients — it waits for its own SUBACK before publishing birth
— which is the shape the feature subscriptions are being moved to.

### Duplicate in-flight CSR request id is not rejected (.NET)

[§6](../connection.md#6-certificate-management-onboarding-and-renewal) requires a second request
carrying an already in-flight request id to fail fast with a busy result.

The C client enforces it: it keeps a single CSR slot and returns a busy error.

The .NET client does not. It keeps a map keyed by request id, ignores the insert result and publishes
regardless, so a duplicate in-flight request id is sent and the caller gets an operation that never
completes.

### CSR carried in the registration (.NET)

[§3](../connection.md#3-full-connect-sequence) has the CSR ride the DPS registration.

The C client sends it. The .NET client always registers with an empty CSR field, so it can only
obtain an operational certificate through the post-connect renewal path of
[§6](../connection.md#6-certificate-management-onboarding-and-renewal).

### Connection profile is not yet readable from DPS (both)

[§4](../connection.md#4-connection-profile-selection) is blocked on the DPS api-version in both
clients: `connectionProfile` is new in `2026-11-02-preview` and both still request `2019-03-31`, so
the field never arrives. Until the api-version is raised, the .NET client carries a locally-set
boolean placeholder in the registration result and the C client has no profile at all.

### Fatal-failure classification (C)

[§5.2](../connection.md#52-what-triggers-a-reconnect) requires failures that cannot succeed on retry
— protocol errors, malformed packets, authorization failures, session-taken-over, invalid topic
filters, server-moved — to go straight to `FAULTED` rather than being retried.

The .NET client classifies these. The C client does not yet: outside the cases listed in
[connection-c.md](connection-c.md) it retries whenever a retry policy is configured.

### Failure taxonomy — where each client diverges from the contract

[§9](../connection.md#9-connection-failure-taxonomy) classifies every failure as terminal, retryable,
contained or benign. Both clients diverge from it, in opposite directions.

**C** keeps the wire code only for CONNACK. Refused SUBACKs, failed PUBACKs and server-initiated
DISCONNECTs all collapse to one generic transport result, so a deterministic refusal is retried and
`0x8E Session taken over` is indistinguishable from a routine drop. The full list is
[connection-c.md §9.5](connection-c.md).

**.NET** preserves every code as a typed value and classifies CONNACK and DISCONNECT explicitly.
Two rows disagree with the contract:

- `0x83 Implementation specific error` on CONNACK is treated as fatal. The contract classes it
  retryable, because the code is server-defined and opaque — the server has declined to say whether
  the condition is permanent, and assuming it is abandons a device that could have reconnected.
- Every socket-level failure is treated as fatal. The contract classes name-resolution failures,
  refused connections, unreachable networks and resets as retryable. The code carries a comment
  acknowledging this and calling for the distinction to be drawn.

