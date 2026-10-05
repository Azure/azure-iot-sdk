# Connection state and error propagation

How the connection client reports its state: the observer registry, the per-scope state model, the
state event and its diagnostic detail, and the reuse and teardown contract. Types and functions are
in [az_iot_connection_client.h](../../inc/azure/iot/az_iot_connection_client.h). Retry behaviour is
in [connection-c.md](connection-c.md) §5.

---

## 1. Observer registry

Any number of parties can watch one connection: the application and every attached feature client.

```c
az_iot_result az_iot_connection_client_add_state_observer(
    az_iot_connection_client* client, az_iot_connection_state_callback cb, void* user_ctx);
az_iot_result az_iot_connection_client_remove_state_observer(
    az_iot_connection_client* client, az_iot_connection_state_callback cb, void* user_ctx);
```

Feature clients register through an internal entry point into a separate pool.

| Rule | Detail |
| --- | --- |
| Two pools | `AZ_IOT_MAX_FEATURE_STATE_OBSERVERS` (6) for feature clients, `AZ_IOT_MAX_APP_STATE_OBSERVERS` (4) for the application. Neither can starve the other, and the application cannot land in the feature pool. A full pool answers `AZ_IOT_ERR_NOT_ENOUGH_SPACE`. |
| Dispatch order | Two passes per transition: every feature-client observer in registration order, then every application observer. By the time the application runs, feature clients have already reacted. |
| Identity | Registration is idempotent on the `(cb, user_ctx)` pair. Removal matches the same pair and answers `AZ_IOT_ERR_NOT_FOUND` when it is not registered. |
| Adding during dispatch | Refused with `AZ_IOT_ERR_BUSY`: a subscriber added mid-pass would be handed a transition it was not watching for. |
| Removing during dispatch | Allowed. A feature client torn down in reaction to a transition must be able to give its seat back. Removal clears the slot in place and the dispatch loop skips empty slots, so no snapshot is needed. |
| `close()` during dispatch | Allowed. |
| Feature-client teardown | A feature client's `deinit()` removes its own entry. |

---

## 2. Scope: state is `(scope, state)`

A device that provisions through DPS runs two independent lifecycles: the provisioning session and
the hub session. They fail, retry and settle separately.

```c
typedef enum az_iot_connection_scope
{
  AZ_IOT_CONN_SCOPE_DPS = 0,
  AZ_IOT_CONN_SCOPE_HUB = 1
} az_iot_connection_scope;
```

Every event carries `scope`, and `az_iot_connection_client_get_state(client, scope)` is the poll-side
equivalent. There is no unscoped state.

Rules:

1. **`IDLE` and `FAULTED` are per scope and distinct.** A clean disconnect with retries disabled is
   `HUB:IDLE`, which is reopenable. Only failures reach `FAULTED`.
2. **A session teardown settles its scope.** When nothing holds the provisioning session after
   registration, `DPS` reports `DISCONNECTING` then `IDLE`, even though the hub connect is about to
   start.
3. **A failure is reported against the scope that failed**, not the scope the recovery uses: a hub
   CONNACK that rejects the identity is a `HUB` failure, even though the retry is a DPS
   registration.
4. **`close()` settles both scopes.**
5. **Under `dps.provision_only` (§4) `HUB` never leaves `IDLE`.**
6. **`DPS:CONNECTED` is the provisioning SUBACK.** A normal DPS device reports
   `DPS:SETTING_UP → CONNECTING → CONNECTED → DISCONNECTING → IDLE` around its registration. `profile` is NULL on
   `DPS:CONNECTED`; the hub generation comes from the assignment and is carried on `HUB:CONNECTED`.
7. **Duplicate transitions are suppressed per scope.**
8. **Every attempt starts in `SETTING_UP`**, before any step that can fail on the device, so a
   failed attempt is always a transition and is never suppressed. A failure there goes straight to
   `RETRY_PENDING`, `FAULTED` or `IDLE` (from `open()`, or a session a feature client asked for),
   with no `DISCONNECTING` for a session that never existed. A registration on a provisioning session that is already up reports
   `SETTING_UP → CONNECTED`.
   `open()`'s configuration checks run before any attempt: a refusal is returned and raises no
   event.

With two scopes, "a retry is pending" cannot be read from state alone: a hub failure whose recovery
is a re-registration leaves `HUB` in `RETRY_PENDING` while the attempt runs on `DPS`.

---

## 3. Shared provisioning sessions

A feature client (for example software updates) can hold the provisioning session open past
registration; the session is reference-counted. When such a session fails, the connection client
does **not** run its normal reconnect path, because that tears down the active hub connection.
`DPS` settles at `IDLE` instead, and the next session attempt is paced by its own retry counter:

- The delays come from `opts.reconnection_policy`, on a counter separate from the registration
  ladder, so a user-held session's outage cannot spend a later re-provisioning's budget.
- With retries disabled, or `max_attempts` spent, the client refuses further session attempts with
  `AZ_IOT_ERR_NOT_SUPPORTED` instead of retrying on every pump tick.
- The refusal clears when a session comes up, a registration succeeds, on `open()` or `close()`,
  or when the last holder releases its reference.
- A synchronous session-start failure is paced at the call site. A `close()` from inside the
  `DPS:CONNECTING` announcement is a cancellation and is not paced.

The core paces the transport. Whether an operation is still worth re-issuing is the feature
client's decision.

---

## 4. `dps.provision_only`

For a device that never connects to an IoT Hub: the client brings up a provisioning session, keeps
it up, and never registers or connects to a hub. Feature clients that work over the provisioning
session (software updates) use it.

- Settled state is `DPS:CONNECTED` + `HUB:IDLE`. `HUB` is never announced.
- Registration is skipped, not attempted: an enrollment with no linked hub fails registration, and
  retrying it would tear down the session in use.
- It is declared, not inferred, because a failed registration is also what a misconfigured
  enrollment looks like.
- `open()` refuses it with `AZ_IOT_ERR_INVALID_ARG` without `dps.id_scope` and
  `dps.registration_id`, or together with `dps.request_operational_certificate`.
- The client holds a standing reference to the session from `open()` to `close()`. A dropped
  session is re-established, paced as in §3.

---

## 5. The state event

```c
typedef struct az_iot_connection_state_event
{
  az_iot_connection_scope scope;
  az_iot_connection_state state;
  az_iot_result reason;
  const az_iot_hub_profile* profile;
  bool is_retriable;
  const az_iot_connection_error_detail* error;
} az_iot_connection_state_event;
```

- The event and everything it points to are valid only for the duration of the callback.
- `profile` is set on `HUB:CONNECTED`, and on a failure with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH`
  or `_UNSUPPORTED`, so the application can rebuild its feature clients.
- `deinit()` raises no event.

### 5.1 `is_retriable`

Would another attempt at this cause plausibly succeed? It does not say whether the SDK will try.
An application that disabled retries is its own retry policy, and `reason` alone is too coarse
(`AZ_IOT_ERR_MQTT` covers both a dropped socket and a refused broker).

It is computed from `reason` by an exhaustive switch, so a new result code does not compile until it
is classified. It errs toward retriable. With retries disabled, `FAULTED` + retriable means the SDK
never tried; with `max_attempts = N`, it means N retries were spent.

### 5.2 `error`

`{ source, code, message }`, or NULL. `source` names the codebook that decodes `code`:

| `source` | `code` | `message` |
| --- | --- | --- |
| `AZ_IOT_CONN_ERR_SRC_TRANSPORT` | The adapter's own code (TLS, socket, DNS). Not comparable across adapters. | empty |
| `AZ_IOT_CONN_ERR_SRC_MQTT` | A wire code: CONNACK, SUBACK, or a server-sent MQTT 5 DISCONNECT reason. | empty |
| `AZ_IOT_CONN_ERR_SRC_DPS` | The provisioning service's `extended_error_code`, for example `401001`. | the service's text, up to `AZ_IOT_CONN_ERROR_MESSAGE_MAX` bytes |
| `AZ_IOT_CONN_ERR_SRC_LOCAL` | The `az_iot_result` of a step that failed on the device: configuration, credential, feature-client bind, MQTT adapter API. | the step, for example `SAS token signing failed` |

There is no hub source: a hub CONNACK and a DPS CONNACK are both `_MQTT`.

A failure is usually recorded in an adapter callback and reported later from the pump, so the detail
is staged per scope and attached when the transition runs. It rides every event of one failure's
sequence (`DISCONNECTING` → `IDLE` → `RETRY_PENDING`/`FAULTED`) and is discarded when the scope next
reaches `SETTING_UP`, `CONNECTING` or `CONNECTED`. A success stages nothing. A DPS registration that
fails on the SUBACK path keeps its detail across the deferred `DPS:CONNECTED` that precedes its
settle.

`code` 0 means "none supplied"; `source` disambiguates it.

---

## 6. Reuse and teardown

- **`close()` → `open()` is the supported reuse path.** Feature clients, persistent subscriptions
  and observers survive it. `close()` is idempotent.
- **`FAULTED` is settled, not terminal.** `close()` is legal from it and returns the client to
  `IDLE`; `open()` then starts a fresh attempt.
- **`open()` is legal only from `IDLE`**; otherwise it answers `AZ_IOT_ERR_ALREADY_INITIALIZED`.
- **Teardown order:** `deinit()` every feature client first, then the connection client.

---

## 7. Examples

Minimal: act only on hub availability.

```c
static void on_state(const az_iot_connection_state_event* e, void* ctx)
{
  if (e->scope != AZ_IOT_CONN_SCOPE_HUB)
  {
    return;
  }
  app_set_online(ctx, e->state == AZ_IOT_CONN_STATE_CONNECTED);
}
```

With retries disabled: decide whether to retry, and log the evidence.

```c
static void on_state(const az_iot_connection_state_event* e, void* ctx)
{
  if (e->state != AZ_IOT_CONN_STATE_FAULTED)
  {
    return;
  }
  if (e->error != NULL)
  {
    app_log_fault(e->scope, e->reason, e->error->source, e->error->code, e->error->message);
  }
  app_schedule_reopen(ctx, e->is_retriable); /* close() then open() later, or alert */
}
```

After a generation change: rebuild feature clients.

```c
if (e->state == AZ_IOT_CONN_STATE_FAULTED
    && e->reason == AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH && e->profile != NULL)
{
  app_request_rebuild(ctx, e->profile->connection_profile); /* then close() and open() */
}
```
