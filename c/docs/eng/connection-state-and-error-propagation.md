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

## 1. Motivation

Today the connection client exposes a **single** state callback
(`az_iot_connection_client_set_state_callback` → `state_cb` / `state_cb_ctx`)
reserved for the application, and feature clients (twin, telemetry, c2d, direct
method, file upload, and the planned ADU client) have **no** way to learn about
connection transitions. This creates three problems:

1. **No reconnect awareness for feature clients.** A feature client that needs to
   re-report or re-arm state on a fresh session cannot, because it never hears
   about `CONNECTED`/`RECONNECTING`.
2. **Use-after-free on teardown.** Feature clients hold a raw `conn` pointer and
   register inbound handlers into the connection client's dispatch table. If the
   application destroys the connection client while a feature client still holds
   it, the next feature-client call dereferences freed memory.
3. **Lossy diagnostics.** The single `az_iot_result_t reason` collapses MQTT,
   TLS, and socket failures into one SDK-level code, discarding the raw
   information an application needs for telemetry or recovery decisions.

This design replaces the single callback with a **shared observer registry**,
adds a **lifecycle/reuse contract** with a teardown notification, and introduces
a **rich status struct**.

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
typedef void (*az_iot_connection_state_observer_cb)(
    az_iot_connection_state_t   state,
    const az_iot_conn_status_t* status,   /* never NULL; see §4 */
    void*                       user_ctx);

/* Public — application */
az_iot_result_t az_iot_connection_client_add_state_observer(
    az_iot_connection_client_t*, az_iot_connection_state_observer_cb, void* user_ctx);
az_iot_result_t az_iot_connection_client_remove_state_observer(
    az_iot_connection_client_t*, az_iot_connection_state_observer_cb, void* user_ctx);

/* Internal header — feature clients */
az_iot_result_t az_iot_connection_client__add_state_observer(
    az_iot_connection_client_t*, az_iot_connection_state_observer_cb, void* user_ctx);
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
- An observer callback **MUST NOT** call `add`/`remove` or any client
  `init`/`deinit` during a dispatch. Enforcement:
  - the client carries a `dispatching` guard flag;
  - `add`/`remove` return `AZ_IOT_ERR_BUSY` when called during dispatch;
  - debug builds assert.
- Because mutation-during-dispatch is forbidden, **no registry snapshot is
  required**. The sole exception (the `DEINITIALIZING` notice, §3.3) performs
  **no** list mutation — feature clients only poison their own local pointers.

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
az_iot_result_t az_iot_connection_client_init(...) {
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

Lifecycle and connection state share one enum (`az_iot_connection_state_t`),
with a terminal lifecycle value:

- `IDLE`, `CONNECTING`, `CONNECTED`, `RECONNECTING`, `DISCONNECTING`, `FAULTED`,
  and terminal `DEINITIALIZING`.

### 4.2 Observer signature

The bare `az_iot_result_t reason` is replaced by a status struct passed by const
pointer (never NULL). The struct and any string it references are **valid only
for the duration of the callback**; observers **MUST** copy anything they need to
retain.

### 4.3 Status struct

```c
typedef struct
{
    /* SDK-level result of the operation that produced this status. This is the
     * normalized azure-iot-sdk return code (AZ_IOT_OK on success, or an
     * AZ_IOT_ERR_* value). Always populated. */
    az_iot_result_t       result;

    /* WHY the transition/fault happened, as a stable high-level category
     * (see §4.4 taxonomy). Drives application decisions without requiring it to
     * decode raw protocol/transport codes. Always populated. */
    az_iot_conn_reason_t  reason;

    /* Client-computed hint: will the SDK keep trying on its own (true) or has it
     * given up / is this terminal (false)? Convenience derived from `reason`. */
    bool                  is_retriable;

    /* Which layer the fault originated in (CLIENT / TRANSPORT / TLS / SOCKET /
     * OTHER / NONE). Tells the application where to look; NONE on success. */
    az_iot_error_source_t source;

    /* Raw messaging-protocol reason code, verbatim from the transport protocol
     * (e.g. an MQTT CONNACK or DISCONNECT reason code). 0 when not applicable.
     * For diagnostics/telemetry only — prefer `reason`/`is_retriable` for logic. */
    int32_t               protocol_code;

    /* Raw lower-transport code from the TLS/socket layer (e.g. a TLS alert or a
     * socket errno). 0 when not applicable. For diagnostics/telemetry only. */
    int32_t               transport_code;

    /* Optional human-readable detail string. May be NULL. VALID ONLY for the
     * duration of the callback — copy it if you need to retain it. */
    const char*           message;
} az_iot_conn_status_t;
```

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
  } az_iot_error_source_t;
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

`az_iot_conn_reason_t` distinguishes retriable vs. terminal causes:

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
| "Am I connected now?" | `state` | `CONNECTED` = usable session; everything else is not-ready. |
| "Is this about connection at all, or lifecycle?" | `state` | `DEINITIALIZING` is the only non-connection value. |
| "Is this terminal or will the SDK recover?" | `is_retriable` | No taxonomy knowledge needed. |
| "Do I need to act / surface an error?" | `reason` + `source` | `reason==USER_CLOSE` ⇒ expected; `source` tells which layer. |
| "I want the raw code for logs/telemetry" | `protocol_code` / `transport_code` / `message` | All optional; `0`/NULL when n/a. |

An app that only cares about "connected or not" reads `state` and ignores the
rest. An app that wants robust diagnostics has every layer's raw code without the
SDK collapsing them.

### Example 1 — minimal app: only cares about session availability

```c
static void on_conn(az_iot_connection_state_t state,
                    const az_iot_conn_status_t* status, void* ctx)
{
    (void)status;
    ((app_t*)ctx)->online = (state == AZ_IOT_CONN_STATE_CONNECTED);
}
```

### Example 2 — reconnection-aware app: re-report on fresh session, log drops

```c
static void on_conn(az_iot_connection_state_t state,
                    const az_iot_conn_status_t* status, void* ctx)
{
    app_t* app = ctx;
    switch (state)
    {
        case AZ_IOT_CONN_STATE_CONNECTED:
            app->online = true;
            /* ADU re-reports cached device properties on the next do_work(). */
            break;

        case AZ_IOT_CONN_STATE_RECONNECTING:
            app->online = false;
            /* is_retriable is implied here, but reason/message tell the user WHY. */
            AZ_IOT_LOG_INFO("link dropped: %s (retrying)",
                            status->message ? status->message : "network");
            break;

        default:
            app->online = false;
            break;
    }
}
```

### Example 3 — diagnostics-heavy app: decide whether to alert vs. wait

```c
static void on_conn(az_iot_connection_state_t state,
                    const az_iot_conn_status_t* status, void* ctx)
{
    app_t* app = ctx;
    if (state == AZ_IOT_CONN_STATE_CONNECTED) { app->online = true; return; }

    app->online = false;

    /* Expected, app-initiated close — not an error. */
    if (status->reason == AZ_IOT_CONN_REASON_USER_CLOSE) return;

    if (status->is_retriable)
    {
        /* SDK will keep trying; just record for telemetry. */
        AZ_IOT_LOG_WARN("transient (%s): proto=%d transport=%d",
                        az_iot_error_source_to_string(status->source),
                        status->protocol_code, status->transport_code);
    }
    else
    {
        /* Terminal — SDK gave up. App must act (re-provision, rotate creds, alert). */
        switch (status->source)
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
static void on_conn(az_iot_connection_state_t state,
                    const az_iot_conn_status_t* status, void* ctx)
{
    (void)status;
    if (state == AZ_IOT_CONN_STATE_DEINITIALIZING)
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

- [azure-iot-sdk SDK design](../design.md) — overall architecture
- [how_to_byo_mqtt_client.md](../how_to_byo_mqtt_client.md) — bring-your-own MQTT client model
- [adu-client-design.md](adu-client-design.md) — first consumer of this foundation
