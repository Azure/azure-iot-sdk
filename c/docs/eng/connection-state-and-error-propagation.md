# Connection State & Error Propagation

> Engineering design for the azure-iot-sdk connection client's **state observer
> registry**, **lifecycle/reuse contract**, and **state & status notification**
> model. This is the authoritative home for these decisions; consumers such as
> the ADU feature client ([adu-client-design.md](adu-client-design.md))
> depend on it.

The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHALL**, **SHALL NOT**,
**SHOULD**, **SHOULD NOT**, **RECOMMENDED**, **MAY**, and **OPTIONAL** in this
document are to be interpreted as described in
[RFC 2119](https://datatracker.ietf.org/doc/html/rfc2119).

---

## 0. Status, and what is still open

This document is a design, and most of it has now shipped. Recording the delta here keeps the rest
readable as design rather than as a claim about the code.

**Shipped**

- **The observer registry of §2.** `az_iot_connection_client_add_state_observer()` /
  `_remove_state_observer()` replaced the single callback slot, with separate pools for feature
  clients and the application (`AZ_IOT_MAX_APP_STATE_OBSERVERS`, default 4). Adding from inside an
  observer answers `AZ_IOT_ERR_BUSY`; removing from inside one is supported, and so is `close()`.
- **Scope.** Open decision 1 below was answered yes. Every event carries
  `az_iot_connection_scope` (`DPS`, `HUB`) beside `state`, the client keeps one state per scope,
  and `az_iot_connection_client_get_state(client, scope)` is the getter. There is no unscoped
  state to ask for.
- **The diagnostics of §4.5.** `az_iot_connection_error_detail` carries `source`, `code` and a
  service-supplied `message`, hung off the event and valid for the callback only.
- **`is_retriable`**, computed by an exhaustive `reason_is_retriable()` — `-Werror=switch-enum`
  makes a new result code a compile error there, so classifying one is a decision someone has to
  take rather than one that defaults silently.
- **Per-scope retry.** Each lifecycle keeps its own ladder and its own attempt budget, so one
  cannot spend the other's.
- **`FAULTED` is settled, not terminal**: `close()` is a legal exit from it and returns the client
  to `IDLE`, which is §3.1's reuse contract extended to the fault path.

**Not shipped**

- `DEINITIALIZING` (§4.1) and the deinit guard of §3.2.
- `az_iot_conn_reason` (§4.4). Open decision 2 was answered by omission: `reason` plus
  `error->source` / `error->code` carry the information, and no second taxonomy was added.
  `is_retriable` — the other half of that decision — *was* taken. §4.4 and §5's
  `connection_reason` row describe a shape that does not exist; they are kept as the record of
  what was considered.

**Still open**

1. **Should the SDK act on its own `is_retriable`?** It is computed and reported, but the retry
   decision is still `reconnect_enabled()`, so a failure the client itself classifies
   non-retriable — a deterministic CONNACK refusal, a refused provisioning filter — is retried to
   exhaustion anyway. Two places already diverge in opposite directions: the hub subscription gate
   *does* treat a refusal as terminal, the presence and provisioning paths do not. See
   [connection-c.md §9.6](connection-c.md#96-known-gaps) items 1 and 2.
2. **What `0` means in `error->code`.** The field documents it as "none supplied", which is
   ambiguous against a genuine `0x00`. `source` disambiguates it today by convention rather than
   by construction.

The failure classification this document's `reason` field carries is specified in
[connection.md §9](../connection.md#9-connection-failure-taxonomy), with the C realization in
[connection-c.md §9](connection-c.md#9-connection-failure-realization-c-partly-implemented).
---

## 1. Motivation

The connection client USED TO expose a **single** state callback
(`az_iot_connection_client_set_state_callback` -> `state_cb` / `state_cb_ctx`)
reserved for the application, leaving feature clients (twin, telemetry, c2d,
direct method, file upload, and the ADU client) with **no** way to learn about
connection transitions. That caused three problems:

1. **No reconnect awareness for feature clients.** A feature client that needs to
   re-report or re-arm state on a fresh session cannot, because it never hears
   about `CONNECTED`/`RECONNECTING`.
2. **Use-after-free on teardown.** Feature clients hold a raw `conn` pointer and
   register inbound handlers into the connection client's dispatch table. If the
   application destroys the connection client while a feature client still holds
   it, the next feature-client call dereferences freed memory.
3. **Lossy diagnostics.** The single `az_iot_result reason` collapses MQTT,
   TLS, and socket failures into one SDK-level code, discarding the raw
   information an application needs for telemetry or recovery decisions.

This design replaces the single callback with a **shared observer registry**,
adds a **lifecycle/reuse contract** with a teardown notification, and introduces
a **rich status struct**.

> **STATUS: the event argument, the observer registry (section 2) and SCOPED
> state (section 2.6) are implemented. The rich status fields (section 4.3) are
> not.**
>
> Client-separation phase P1d changed `az_iot_connection_state_callback` to take
> one SDK-produced, size-stamped `az_iot_connection_state_event` carrying the
> resolved connection profile
> ([client-separation.md section 9](client-separation.md#the-profile-can-change-while-the-device-is-running)).
> The registry reuses that signature unchanged, which is why it did not have to
> introduce a second callback type. The remaining work -- `source`,
> `protocol_code`, `transport_code`, `message` -- must extend that same event
> rather than introduce a second status parameter.
>
> Where the shipped registry differs from this document, see section 2.5. The
> scope dimension, which this document predates, is described in section 2.6.

---

## 2. Connection State Observer Registry

Replaces the single `set_state_callback` with **one shared observer registry**
used by both the application and the feature clients.

### 2.1 Registry shape

The connection client holds **one** array of entries
`{ cb, user_ctx, is_feature_client }`. Public registration sets
`is_feature_client = false`; an internal (internal-header) helper sets `true`.
The application **MUST NOT** be able to register as a feature client — the flag
is set by the registration helper, never passed by the caller.

```c
typedef void (*az_iot_connection_state_observer_callback)(
  const az_iot_connection_state_event* event,  /* never NULL; see §4 */
  void* user_ctx);

/* Public — application */
az_iot_result az_iot_connection_client_add_state_observer(
    az_iot_connection_client*, az_iot_connection_state_observer_callback, void* user_ctx);
az_iot_result az_iot_connection_client_remove_state_observer(
    az_iot_connection_client*, az_iot_connection_state_observer_callback, void* user_ctx);

/* Internal header — feature clients */
az_iot_result az_iot_connection_client__add_state_observer(
    az_iot_connection_client*, az_iot_connection_state_observer_callback, void* user_ctx);
```

### 2.2 Dispatch ordering

Each transition dispatches in **two passes**: (1) all feature-client entries in
registration order, then (2) all application entries in registration order.
Feature clients are **always notified before** the application, so by the time
the application observer runs, feature clients have already reacted (re-subscribed,
flagged re-report, or detached).

### 2.3 Capacity (compile-time configurable)

Total default **10** = **6 feature-client** + **4 application** slots (the 6
feature clients: twin, telemetry, c2d, direct method, file upload, adu):

```c
#ifndef AZ_IOT_CONN_MAX_FEATURE_OBSERVERS
#define AZ_IOT_CONN_MAX_FEATURE_OBSERVERS 6
#endif
#ifndef AZ_IOT_CONN_MAX_APP_OBSERVERS
#define AZ_IOT_CONN_MAX_APP_OBSERVERS 4
#endif
```

`add_*_observer` returns `AZ_IOT_ERR_NOT_SUPPORTED` when the relevant pool is
full.

### 2.4 Removal & reentrancy

- Feature-client `deinit` **MUST** self-remove its entry.
- An observer callback **MUST NOT** call `add`, or any client `init`/`deinit`
  that would add, during a dispatch. Enforcement:
  - the client carries a `dispatching` guard flag;
  - `add` returns `AZ_IOT_ERR_BUSY` when called during dispatch;
  - debug builds assert.
- An observer callback **MAY** call `remove`, and a feature-client `deinit` from
  inside a dispatch MUST be able to: the entry holds a raw pointer to storage
  the deinit is about to release, and the caller has no later point at which to
  retry. See deviation 5 in §2.5.
- Because ADDITION during dispatch is forbidden, and removal only ever clears a
  slot in place, **no registry snapshot is required**: the dispatch loop
  re-reads each slot and skips a NULL callback, and nothing is compacted. The
  `DEINITIALIZING` notice (§3.3) performs **no** list mutation either — feature
  clients only poison their own local pointers.

### 2.5 What shipped, and where it differs from §2.1–§2.4

The registry is implemented. Five deviations from the design above, each
deliberate:

1. **Two arrays, not one array with an `is_feature_client` flag.** §2.1 proposed
   a single array carrying the flag. Separate `feature_state_observers[]` and
   `app_state_observers[]` give the same guarantee structurally: an application
   cannot land in the feature-client pool because it calls a different function,
   not because a flag was set correctly. It also makes §2.3's "neither pool can
   starve the other" true by construction rather than by bookkeeping, and it
   makes the two-pass dispatch of §2.2 the natural loop rather than a filter.
2. **Names.** The capacity macros are `AZ_IOT_MAX_FEATURE_STATE_OBSERVERS` (6)
   and `AZ_IOT_MAX_APP_STATE_OBSERVERS` (4) — the counts §2.3 specifies, under
   names matching the `AZ_IOT_MAX_*` family already in the public header.
3. **A full pool answers `AZ_IOT_ERR_NOT_ENOUGH_SPACE`, not
   `AZ_IOT_ERR_NOT_SUPPORTED`.** §2.3 said the latter. The pool being full is a
   capacity condition, and `NOT_ENOUGH_SPACE` is what every other bounded pool
   in this client already returns; `NOT_SUPPORTED` would read as "this build
   has no observer registry".
4. **No debug-build assert on reentrant mutation** (§2.4's third bullet). The
   `AZ_IOT_ERR_BUSY` return is the contract and is covered by a test; an assert
   would add a second, divergent failure mode for the same mistake, and this
   client does not assert anywhere else.
5. **REMOVAL during dispatch is permitted; only addition is refused.** §2.4
   originally forbade both. Refusing removal is not a safe default: a feature
   client destroyed from inside an observer -- a natural reaction to FAULTED --
   runs its deinit within the dispatch, and had no way to give its seat back,
   so the entry became a call into freed storage and the seat leaked. Removal
   is safe against the walk because it clears a slot in place and the loop
   re-reads each slot, skipping NULL. Addition stays refused: a subscriber
   added mid-pass would be handed a transition it was not watching for.

Two further points the design did not state, both now pinned by tests:

- **Registration is idempotent on the `(cb, user_ctx)` PAIR**, not on `cb`
  alone. One callback shared by two owners is two subscriptions and is
  delivered twice; registering the same pair again consumes no second slot and
  causes no second delivery.
- **Removal matches the same pair**, so withdrawing one owner's subscription
  leaves another owner sharing that callback registered. It answers
  `AZ_IOT_ERR_NOT_FOUND` when the pair is not registered.

`az_iot_connection_client_set_state_callback()` is **removed**, not deprecated:
the libraries are unreleased, and keeping a single-slot setter beside a registry
would leave two ways to subscribe with different semantics.

---

### 2.6 Scope: state is `(scope, state)`, not `state`

This document predates the scope dimension. Every state in it is now half of a
pair.

A device that provisions through DPS runs **two independent lifecycles**: the
provisioning session, and the hub session. They fail, retry and settle
separately — a provisioning session dropping must not disturb a healthy hub
connection, and a hub drop must not invalidate a provisioning session a feature
client is using.

```c
typedef enum az_iot_connection_scope
{
  AZ_IOT_CONN_SCOPE_DPS = 0,
  AZ_IOT_CONN_SCOPE_HUB = 1
} az_iot_connection_scope;
```

`scope` sits beside `state` in `az_iot_connection_state_event`, and
`az_iot_connection_client_get_state(client, scope)` is the poll-side
equivalent. **There is no unscoped state to ask for.**

Why it is not optional:

- **`CONNECTED` was ambiguous.** For hub messaging it means the hub is usable;
  for the device-update client, riding the provisioning session, it does not.
- **A whole phase was invisible.** `set_state_to()` suppresses a transition
  whose value is unchanged. With one shared value, the hub's `CONNECTING`
  immediately after the DPS one was dropped as a no-op, so a DPS + hub run
  reported exactly one `CONNECTING` and one `CONNECTED` — an application could
  not tell provisioning from hub connect, nor either from a retry loop. **The
  suppression is now per scope.**
- **It replaces a special case with a rule.** A provisioning session opened for
  a feature client used to have to be hidden from the public state, because
  announcing it would report a lifecycle the application never asked for. With
  scopes it is simply `DPS:*`, and `HUB:*` is untouched.

Rules that follow, each pinned by a test:

1. **IDLE and FAULTED are per scope, and still distinct.** A clean peer
   disconnect with retries disabled is `HUB:IDLE` — reopenable. Only *failures*
   reach `FAULTED`. Conflating them per scope is the same mistake as conflating
   them globally.
2. **A session teardown settles its scope.** When nothing still holds the
   provisioning session it is released at registration, so `DPS` emits
   `DISCONNECTING` then `IDLE` even though the hub connect is about to start. Leaving it pinned at `CONNECTING`
   would make the *next* re-provisioning run invisible, by the same suppression
   rule above.
3. **A failure is reported against the scope that failed**, not the scope the
   recovery attempt uses: a hub CONNACK that rejects the identity is a HUB
   session going down, even though the retry is a DPS registration.
4. **`close()` settles both.** It is a statement about the client, not about one
   lifecycle.
5. **Under `dps.provision_only` (§2.8) `HUB` never leaves `IDLE`.** That is
   the answer, not an error — nothing should wait on a `HUB:CONNECTED` that
   cannot come.
6. **`DPS:CONNECTED` is real, and it is the SUBACK.** The provisioning session
   reaches `CONNECTED` when its subscription is confirmed — the same fact that
   makes it usable to a feature client. An ordinary DPS device therefore reports
   `DPS:CONNECTING → CONNECTED → DISCONNECTING → IDLE` around its registration.
   **`profile` is NULL on `DPS:CONNECTED`**: the hub generation comes from the
   *assignment*, which does not exist yet at the SUBACK, and on a
   `provision_only` device never will. It is carried on `HUB:CONNECTED` only.

One consequence worth stating because it bit during implementation: with two
scopes, a state-only test for "a retry is pending" is wrong. A hub failure whose
recovery is a re-registration leaves `HUB` in `RECONNECTING` while the attempt
runs on `DPS`, so the pending-retry deadline — not the state — is the token, and
firing consumes it.

### 2.7 A third, internal retry ladder: the user-held provisioning session

§2.6 says the retry ladders are per scope, and `max_attempts` is a budget per
ladder. There is one ladder beyond those two. It is internal — it adds no
option, no enum value and no public field — but it is worth stating, because
"per scope" is otherwise a complete description and this is a deviation from it.

**What it paces.** A provisioning session can be held by a feature client past
registration (the session is refcounted). When such a session *fails*, the
connection client deliberately does **not** put it through `schedule_reconnect()`:
that begins with `teardown_active()`, so a side channel dropping would destroy a
healthy hub connection. The DPS scope settles at `IDLE` instead.

But `IDLE` is exactly what `dps_session_ensure()` lets through. With no deadline,
the next pump tick opened another session — a full TLS handshake, CONNECT and
SUBSCRIBE per tick, against a service that is already failing, for as long as the
application kept pumping. The ladder is what stops that.

**Why not reuse `retry_attempt[DPS]`.** That is the *registration* ladder. Spending
it on a user session's outage would leave a later re-provisioning with no budget
— the same argument that made `max_attempts` per-ladder in the first place.

**Where the numbers come from.** `opts.reconnection_policy`, via
`az_iot_reconnect_delay_ms()` on its own attempt counter. So the pacing, the
jitter and the bound are the ones the application already configured; a feature
client does not get a second retry vocabulary to learn.

**Retries disabled is an explicit refusal, not a zero delay.**
`az_iot_reconnect_delay_ms()` returns 0 ms when `initial_delay_ms == 0`, which
would pace nothing — "no retries" would be the one setting that reproduced the
hot loop. So the core latches instead, and `dps_session_ensure()` answers
`AZ_IOT_ERR_NOT_SUPPORTED`: a settled answer a holder can report, rather than a
session attempt per tick. `max_attempts` being spent latches the same way.

**Exits from the latch**, all of which mean the demand or the evidence changed:
a session that comes up, a successful registration, `open()`, `close()` — which
clears it *before* its idempotent early return, because on a DPS-only device both
scopes sit at `IDLE` and that early return is the case that most needs clearing —
and the last user releasing its ref, since a later holder is new demand.

**Two paths that are easy to miss.**

* A **synchronous** `dps_start()` failure — the adapter cannot be built, or `connect()` refuses
  inline — never reaches `dps_finalize()`, so the deferred path does not pace it. It is paced at
  the call site instead. Without that, the caller got the error and asked again on the next pump
  tick: the same hot loop, on the path least likely to fix itself.
* A `close()` from inside the synchronous `DPS:CONNECTING` announcement is a **cancellation**, and
  it reports the same result code as a genuine start failure. Pacing it would recreate the
  deadline — or the latch, with retries disabled — immediately after `close()` reset it, so the
  documented escape would not work. The two are told apart by a flag `dps_start()` sets;
  `user_close` does **not** work here, because `close()` with no hub adapter (exactly this case)
  clears it before returning.
* The deferred failure is only charged **while a user ref is still held**. Release is reachable
  from inside a message callback, so a finalize can land in the same pump iteration with
  `dps_user_count` already 0 — after the collect that would otherwise have torn the session down.
  Recording a backoff there would hand the next holder a latch it did not earn.

**What the core does not decide.** Whether the *operation* is still worth
re-issuing is the feature client's judgement, not the connection client's: the
core has no idea what the request meant. The core paces the transport; the
feature client bounds the operation and reports it.

### 2.8 `dps.provision_only`: a device with no IoT Hub

Some devices legitimately have no hub. Device Update v2 devices are shaped that
way: their device operations all run **pre-registration** over the provisioning
session, and the device is never assigned a hub.

`opts.dps.provision_only` declares that. The client brings up a provisioning
session, keeps it up, and never registers and never connects to a hub.

**Settled state is `DPS:CONNECTED` + `HUB:IDLE`.** `HUB` is never announced at
all, so nothing waits on a `HUB:CONNECTED` that cannot come. No new enum value
was needed — that is what the scope dimension bought: before it, "provisioned
but hubless" had no way to be expressed except by overloading `CONNECTED` or
adding a state.

**Why registration is skipped rather than attempted.** An enrollment with no
linked hub answers the registration with `errorCode 401001`, *"IoTHub not
found"*. That is a failure, routed through the reconnection policy like any
other — so a device that will never have a hub would retry forever, or fault,
and tear down the very session its feature clients were using on each attempt.
There is nothing for it to succeed at.

**Why it is declared and not inferred.** "The registration failed with 401001"
is exactly what a **misconfigured** enrollment looks like too — one that should
have had a hub and does not. Treating that as success would remove the
operator's only signal for a real misconfiguration.

**Rejected combination**, at `open()` with `AZ_IOT_ERR_INVALID_ARG` rather than
silently ignoring one half:

- `dps.request_operational_certificate` — the certificate is issued *by* a
  registration, which this device never performs.

**How the session is kept.** The client takes a **standing ref** for its whole
`open()`..`close()` life, distinct from the registration ref (a task) and from
the feature clients' user refs. Without it the pump would collect the session
whenever no feature client happened to be holding one. The ref also makes the
`DPS` scope "the application's" for the purposes of `open()`'s
already-open check, and it is what `do_work()` reads to re-establish a session
that dropped — **paced by the same ladder as a user-held session** (§2.7), so a
refusing service is not hammered. `close()` ends the demand, and does so
*before* its idempotent early return, because on this device both scopes sit at
`IDLE` while a backoff is pending — which is precisely that early return.

### 2.9 Diagnostic detail: `is_retriable` and `error`

A failure event carries two things beyond `reason`.

**`is_retriable`** — would another attempt at this *cause* plausibly succeed? It says nothing
about whether the SDK will try. That matters because an application that set
`az_iot_reconnection_policy_get_retry_disabled()` **is** the retry policy: it sees `FAULTED` and
has to decide, and `reason` alone is too coarse (`AZ_IOT_ERR_MQTT` covers both a dropped socket
and a rejected identity). Computed from `reason` by an exhaustive switch, so adding a result code
is a compile error until someone classifies it. It errs toward retriable: a wrong "do not retry"
strands a device that would have recovered, a wrong "retriable" costs one attempt.

The application does **not** need a retry counter from the SDK. It configured the policy and can
read it back, and with the policy in hand `is_retriable` separates every case — with retries
disabled, `FAULTED` + retriable means the SDK never tried; with `max_attempts = N`, it means N
were spent.

**`error`** — `{source, code, message}`, or NULL. `source` names the **codebook that decodes
`code`**, not the connection: which connection is already `scope`.

| `source` | `code` is | `message` |
|---|---|---|
| `_TRANSPORT` | the adapter's own code: TLS, socket, DNS. Not comparable across adapters | empty |
| `_MQTT` | a code off the wire: CONNACK, SUBACK, or a server-sent v5 DISCONNECT reason | empty |
| `_DPS` | the provisioning service's `extended_error_code`, e.g. `401001` | the service's text |

There is deliberately no `_HUB`: a hub CONNACK and a DPS CONNACK are both `_MQTT`, and a value
matching two sources would make the application guess which the SDK picked. The asymmetry in the
table is a property of the services — DPS returns a structured error document, IoT Hub does not.

**Lifetime.** `message` points into the adapter's inbound buffer and dies with the callback, like
the event itself. Copy anything that must be retained.

**Staging.** A failure is usually recorded in an adapter callback and reported later from the pump,
so the detail is staged on the client and attached when the transition runs. It is scoped, so a
DPS verdict cannot attach to a hub event; it rides **every** event of one failure's sequence
(`DISCONNECTING` → `IDLE` → `RECONNECTING`/`FAULTED`), because they all report the same failure and
consuming it on the first would leave the terminal event — the one applications act on — empty;
and it is discarded when the scope next reaches `CONNECTING` or `CONNECTED`, which is when the old
evidence stops describing anything current.

**A success stages nothing.** A SUBACK that succeeded still carries a `protocol_code` — the granted
QoS — and staging it would hand a later failure a code describing something that worked.

---

## 3. Lifecycle & Reuse Contract

### 3.1 Supported reuse: `close` → `open`

`close()` returns the client to `IDLE` and is idempotent; `open()` accepts a
client in `IDLE`. `close` → `open` **is the supported reuse path** and **MUST**
remain supported. Feature clients, persistent subscriptions, and registered
observers survive across a `close`/`open` cycle.

### 3.2 Unsupported: `deinit` → `init` (guarded)

Re-initializing a `deinit`'d struct **is NOT supported** and **MUST** be
rejected via a poison magic field:

```c
az_iot_result az_iot_connection_client_init(...) {
    if (client->magic == AZ_IOT_CONN_MAGIC_ALIVE) return AZ_IOT_ERR_ALREADY_INITIALIZED;
    if (client->magic == AZ_IOT_CONN_MAGIC_DEAD)  return AZ_IOT_ERR_NOT_SUPPORTED; /* no reuse */
    ...
    client->magic = AZ_IOT_CONN_MAGIC_ALIVE;
}
void az_iot_connection_client_deinit(...) {
    ...
    client->magic = AZ_IOT_CONN_MAGIC_DEAD;   /* poison: blocks re-init */
}
```

The same poison pattern applies to **every feature client** struct. Documented
caveat: structs are caller-allocated, so a fresh struct holding garbage *could*
coincidentally match a magic value. Callers **MUST** pass a zeroed or freshly
declared struct to `init`.

### 3.3 Teardown notification & detach safety

- `deinit` emits a terminal `AZ_IOT_CONN_STATE_DEINITIALIZING` notification as
  its **first** action, before freeing anything.
- A feature client's `DEINITIALIZING` handler **MUST** only null its `conn`
  pointer and set `detached = true`. It **MUST NOT** call back into the
  connection client or other feature clients.
- After detach, every feature-client entry point returns `AZ_IOT_ERR_DETACHED`
  (new error code) instead of dereferencing a dead connection client.
- Correct teardown order remains **feature clients first, then connection
  client**. The `DEINITIALIZING` + `detached` mechanism is a **safety net** for
  misordering, not a license to ignore order.

---

## 4. Connection State & Status Notification

### 4.1 State enum (single enum)

Lifecycle and connection state share one enum (`az_iot_connection_state`),
with a terminal lifecycle value:

- `IDLE`, `CONNECTING`, `CONNECTED`, `RECONNECTING`, `DISCONNECTING`, `FAULTED`,
  and terminal `DEINITIALIZING`.

Each value is reported **per scope** (section 2.6): the enum says what happened,
`scope` says to which lifecycle. `DEINITIALIZING` is the exception — it is a
client-level event, emitted once, not once per scope.

### 4.2 Observer signature

P1d replaced the loose `(state, reason)` arguments with one const event pointer
(never NULL). The event, its `profile`, and any future string it references are
**valid only for the duration of the callback**; observers **MUST** copy anything
they need to retain.

### 4.3 Event and future status fields

The first four fields are implemented. Rich classification and raw diagnostics
append to the same size-stamped event; they do not introduce another callback
parameter or a nested status object:

```c
typedef struct az_iot_connection_state_event
{
  uint32_t                    _internal_size;
  /* WHICH lifecycle this event is about. Shipped; see section 2.6. `state` is
   * meaningless without it. */
  az_iot_connection_scope     scope;
  az_iot_connection_state    state;

    /* SDK-level result of the operation that produced this status. This is the
     * normalized azure-iot-sdk return code (AZ_IOT_OK on success, or an
     * AZ_IOT_ERR_* value). Always populated. */
  az_iot_result               reason;

  /* Non-NULL exactly for CONNECTED; implemented by P1d. */
  const az_iot_hub_profile*   profile;

    /* WHY the transition/fault happened, as a stable high-level category
     * (see §4.4 taxonomy). Drives application decisions without requiring it to
   * decode raw protocol/transport codes. Future field. */
  az_iot_conn_reason          connection_reason;

    /* Client-computed hint: will the SDK keep trying on its own (true) or has it
     * given up / is this terminal (false)? Convenience derived from `reason`. */
    bool                         is_retriable;

    /* Which layer the fault originated in (CLIENT / TRANSPORT / TLS / SOCKET /
     * OTHER / NONE). Tells the application where to look; NONE on success. */
    az_iot_error_source          source;

    /* Raw messaging-protocol reason code, verbatim from the transport protocol
     * (e.g. an MQTT CONNACK or DISCONNECT reason code). 0 when not applicable.
     * For diagnostics/telemetry only — prefer `reason`/`is_retriable` for logic. */
    int32_t                      protocol_code;

    /* Raw lower-transport code from the TLS/socket layer (e.g. a TLS alert or a
     * socket errno). 0 when not applicable. For diagnostics/telemetry only. */
    int32_t                      transport_code;

    /* Optional human-readable detail string. May be NULL. VALID ONLY for the
     * duration of the callback — copy it if you need to retain it. */
    const char*                  message;
  } az_iot_connection_state_event;
```

  `connection_reason` is deliberately not named `reason`: P1d already defines
  `reason` as the normalized `az_iot_result`, and two fields with the same name
  meaning different things is a trap regardless of layout.

  **There is no prefix-compatibility constraint on this struct yet.** These
  libraries are unreleased (`git tag` is empty) and every consumer of the event
  is in this repository, so members are ordered for sense, not appended for
  compatibility — `scope` sits beside `state` because the two are only
  meaningful together (section 2.6). `_internal_size` is carried so that growth
  becomes safe *after* the first release; it does not oblige append-only
  ordering before it. Once a release exists, that flips and this paragraph
  should be rewritten to say so.

- **`is_retriable`** — included. Derivable from `reason`, but it directly answers
  "is the SDK going to keep trying?" without forcing the app to memorize the
  taxonomy. Computed by the client.
- **`source`** — single enum (not flags). It identifies the originating layer of
  a fault and is intentionally **not** connection-specific (the same layering
  applies to publish/subscribe failures), so it is named generically:
  ```c
  typedef enum {
      AZ_IOT_ERROR_SOURCE_NONE = 0,  /* success / no error */
      AZ_IOT_ERROR_SOURCE_CLIENT,    /* SDK logic: user_close, guards, deinit */
      AZ_IOT_ERROR_SOURCE_TRANSPORT, /* messaging protocol (MQTT today; others later) */
      AZ_IOT_ERROR_SOURCE_TLS,       /* TLS handshake / cert validation */
      AZ_IOT_ERROR_SOURCE_SOCKET,    /* TCP / DNS / errno transport */
      AZ_IOT_ERROR_SOURCE_OTHER      /* BYO / third-party adapter, uncategorized */
  } az_iot_error_source;
  ```
  `TRANSPORT` (rather than `MQTT`) keeps the layer name protocol-agnostic for
  future non-MQTT transports. `OTHER` is needed because the SDK supports
  bring-your-own MQTT clients
  ([how_to_byo_mqtt_client.md](../how_to_byo_mqtt_client.md)); a third-party
  transport can raise failures that map to none of TRANSPORT/TLS/SOCKET. `TLS` is
  split from `SOCKET` because cert/handshake failures are terminal while a
  DNS/connect blip is retriable — the distinction drives `is_retriable`.
  Flags-style is deferred until a real multi-layer-attribution case exists.

### 4.4 Reason taxonomy

`az_iot_conn_reason` distinguishes retriable vs. terminal causes:

| Reason | Typical `source` | `is_retriable` | Meaning |
|---|---|---|---|
| `AZ_IOT_CONN_REASON_NONE` | `NONE` | n/a | Success / no error context. |
| `AZ_IOT_CONN_REASON_USER_CLOSE` | `CLIENT` | false | Application called `close()`. Expected. |
| `AZ_IOT_CONN_REASON_AUTH_FAILED` | `TRANSPORT` | false | CONNACK refused credentials. Terminal. |
| `AZ_IOT_CONN_REASON_NETWORK` | `SOCKET` | true | TCP/DNS failure. Transient. |
| `AZ_IOT_CONN_REASON_TLS` | `TLS` | false | Handshake / cert validation failure. Terminal. |
| `AZ_IOT_CONN_REASON_TIMEOUT` | `SOCKET`/`TRANSPORT` | true | Connect or keep-alive timeout. Transient. |
| `AZ_IOT_CONN_REASON_SERVER_CLOSED` | `TRANSPORT` | true | Broker-initiated DISCONNECT. Transient. |
| `AZ_IOT_CONN_REASON_PROTOCOL` | `TRANSPORT` | false | Protocol violation. Terminal. |
| `AZ_IOT_CONN_REASON_DEINITIALIZED` | `CLIENT` | false | Client is being torn down (`DEINITIALIZING`). |

> The `is_retriable` column is the SDK's default classification; the connection
> client computes it from the same logic it uses to decide whether to schedule a
> reconnect.

### 4.5 Down-stack diagnostics plumbing (in scope)

`protocol_code` / `transport_code` are part of this design **up front**, not shipped
zero-filled and wired later. The `az_iot_mqtt_iface` connect-failure / disconnect
path **MUST** surface raw reason codes upward so these fields can be populated.
They are `0` when not applicable, so observers can ignore them.

---

## 5. Consumer Ergonomics

The struct is designed so an app answers four questions in order, each with a
single field, and can stop as soon as it has what it needs:

| Question the app asks | Field to read | Notes |
|---|---|---|
| "Which connection is this about?" | `scope` | **Read first.** `state` is meaningless without it: `CONNECTED` on `SCOPE_DPS` does not mean the hub is usable. See section 2.6. |
| "Am I connected now?" | `state` | `CONNECTED` on `SCOPE_HUB` = usable hub session; everything else is not-ready. |
| "Is this about connection at all, or lifecycle?" | `state` | `DEINITIALIZING` is the only non-connection value. |
| "Is this terminal or will the SDK recover?" | `is_retriable` | No taxonomy knowledge needed. |
| "Do I need to act / surface an error?" | `connection_reason` + `source` | `connection_reason==USER_CLOSE` ⇒ expected; `source` tells which layer. |
| "I want the raw code for logs/telemetry" | `protocol_code` / `transport_code` / `message` | All optional; `0`/NULL when n/a. |

An app that only cares about "connected or not" reads `state` and ignores the
rest. An app that wants robust diagnostics has every layer's raw code without the
SDK collapsing them.

### Example 1 — minimal app: only cares about session availability

```c
static void on_conn(const az_iot_connection_state_event* event, void* ctx)
{
  ((app_t*)ctx)->online = (event->state == AZ_IOT_CONN_STATE_CONNECTED);
}
```

### Example 2 — reconnection-aware app: re-report on fresh session, log drops

```c
static void on_conn(const az_iot_connection_state_event* event, void* ctx)
{
    app_t* app = ctx;
  switch (event->state)
    {
        case AZ_IOT_CONN_STATE_CONNECTED:
            app->online = true;
            /* ADU re-reports cached device properties on the next do_work(). */
            break;

        case AZ_IOT_CONN_STATE_RECONNECTING:
            app->online = false;
            /* is_retriable is implied here, but reason/message tell the user WHY. */
            AZ_IOT_LOG_INFOF("link dropped: %s (retrying)",
                             event->message ? event->message : "network");
            break;

        default:
            app->online = false;
            break;
    }
}
```

### Example 3 — diagnostics-heavy app: decide whether to alert vs. wait

```c
static void on_conn(const az_iot_connection_state_event* event, void* ctx)
{
    app_t* app = ctx;
  if (event->state == AZ_IOT_CONN_STATE_CONNECTED) { app->online = true; return; }

    app->online = false;

    /* Expected, app-initiated close — not an error. */
    if (event->connection_reason == AZ_IOT_CONN_REASON_USER_CLOSE) return;

    if (event->is_retriable)
    {
        /* SDK will keep trying; just record for telemetry. */
        AZ_IOT_LOG_WARNF("transient (%s): proto=%d transport=%d",
                         az_iot_error_source_to_string(event->source),
                         event->protocol_code, event->transport_code);
    }
    else
    {
        /* Terminal — SDK gave up. App must act (re-provision, rotate creds, alert). */
        switch (event->source)
        {
            case AZ_IOT_ERROR_SOURCE_TLS:       raise_cert_alert(app);        break; /* bad/expired cert */
            case AZ_IOT_ERROR_SOURCE_TRANSPORT: raise_auth_alert(app);        break; /* CONNACK refused  */
            default:                            raise_generic_alert(app);     break;
        }
    }
}
```

### Example 4 — teardown: only poison local state; never call back in

```c
static void on_conn(const az_iot_connection_state_event* event, void* ctx)
{
  if (event->state == AZ_IOT_CONN_STATE_DEINITIALIZING)
        ((app_t*)ctx)->conn_alive = false;   /* feature clients self-detach; see §3.3 */
}
```

---

## 6. Open Items / Future Work

- **Reconnection & retry policy** (backoff curve, jitter, max attempts, per-reason
  caps) is intentionally **not** specified here yet; it will be added to this doc
  (or a sibling) once the state/status surface above is implemented.
- **Flags-style `source`** is deferred until a real case requires recording more
  than one originating layer per status.

---

## 7. References

- [connection.md](../connection.md) — the language-neutral connection lifecycle contract, and the failure taxonomy this document's `reason` field reports
- [connection-c.md](connection-c.md) — the C realization of that contract
- [azure-iot-sdk SDK design](../design.md) — overall architecture
- [how_to_byo_mqtt_client.md](../how_to_byo_mqtt_client.md) — bring-your-own MQTT client model
- [adu-client-design.md](adu-client-design.md) — first consumer of this foundation
