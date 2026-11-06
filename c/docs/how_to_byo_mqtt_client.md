<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Bring-Your-Own MQTT client adapter

This guide is for a customer (or platform team) who wants to plug a non-default MQTT client library into `azure-iot-sdk`. The SDK ships with an Eclipse Paho-C adapter, but the MQTT layer is fully pluggable: anything that can implement the `az_iot_mqtt_iface` vtable can be dropped in.

If you can produce a static or shared library that exposes an `az_iot_mqtt_adapter` (a `get_workspace_size()` and an `init()` that builds a client of the MQTT version asked in application memory), you are done. The conformance test suite ([tests/conformance/](../tests/conformance/)) verifies that your adapter is plug-compatible.

## Prerequisites

- Your MQTT client must be able to speak **MQTT v3.1.1**.
- C99 source-level compatibility for the adapter glue (the underlying client may be C++/Rust/etc., as long as it exposes a C ABI).

The SDK internally determines which MQTT version is required for each Azure service (DPS and the IoT Hub use v3.1.1) and asks your adapter's `init()` for a client of that version. Refuse a version you do not support with `AZ_IOT_ERR_NOT_SUPPORTED`.

## Files you need to write

For an adapter named `mymqtt`:

```
adapters/mymqtt/
├── CMakeLists.txt              # builds your adapter as az_iot_adapter_mymqtt
└── az_iot_mqtt_mymqtt.c    # implements az_iot_mqtt_iface (and pulls in your client lib)

inc/azure/iot/adapters/
└── az_iot_adapter_mymqtt.h # public az_iot_mymqtt_get_adapter()
```

You can take [adapters/paho/](../adapters/paho/) (a client with its own I/O thread) or
[adapters/az_mqtt/](../adapters/az_mqtt/) (a single-threaded client) as a working reference.

## Step 1 — Implement the iface vtable

The contract is in [inc/azure/iot/az_iot_mqtt_iface.h](../inc/azure/iot/az_iot_mqtt_iface.h). You must provide non-NULL function pointers for every slot:

| Slot              | Required behavior                                                                                    |
|-------------------|------------------------------------------------------------------------------------------------------|
| `connect`         | Initiate CONNECT to `opts->host:opts->port` with `client_id`. Non-blocking. Result via inbound `EVT_CONNECTED`. Honor `clean_start` and the LWT fields from `connect_options`. |
| `disconnect`      | Initiate DISCONNECT. Non-blocking. Eventually emits `EVT_DISCONNECTED` with status `AZ_IOT_OK` (only this completion uses `AZ_IOT_OK`; a lost connection reports an error). |
| `subscribe`       | Send SUBSCRIBE. Return `out_packet_id` synchronously; emit `EVT_SUBSCRIBE_ACK` when SUBACK arrives.   |
| `unsubscribe`     | Send UNSUBSCRIBE. Same pattern as `subscribe`. Emit `EVT_UNSUBSCRIBE_ACK`.                            |
| `publish`         | Send PUBLISH. For QoS>0, emit `EVT_PUBLISH_ACK` when PUBACK arrives. |
| `process_loop`    | Drive any pending I/O and dispatch queued inbound events on the calling thread.                       |
| `set_inbound_cb`  | Register the callback the SDK uses to receive `az_iot_mqtt_event` notifications.                |
| `destroy`         | Tear down the client. The SDK may call this even on a never-connected client.                        |

**On CONNACK** — populate `session_present` in the `az_iot_mqtt_event` delivered with `EVT_CONNECTED`: a session connected with Clean Session 0 has no other way to learn whether the broker resumed it.

### Session terms: Clean Session and Will

The SDK decides these per session role and hands them to `connect`. An adapter that drops any of them still connects, publishes and subscribes normally, so nothing except the wire shows the difference — which is why the conformance suite decodes the CONNECT and the DISCONNECT and asserts on them directly.

- `username`, `password` — valid only while `connect()` runs. Copy or serialize them before
  returning: the SDK wipes the password (a SAS token) as soon as `connect()` returns and reuses
  its buffer. Conformance: `connect_does_not_read_credentials_after_returning`.
- `clean_start` — map it onto Clean Session; do **not** hardcode either value.
- `lwt` — set the Will topic, payload, QoS and retain flag on the CONNECT when `lwt.topic` is non-NULL.
**On a rejected CONNACK** — set `status` from `az_iot_mqtt_connack_result(version, connack_code)` rather than reporting a blanket `AZ_IOT_ERR_MQTT`. Pass the code exactly as it came off the wire (a v3.1.1 return code); pass a negative value for failures your client raised itself, such as a refused socket or a TLS handshake error. The helper decides whether the broker refused the *identity* (`AZ_IOT_ERR_IDENTITY_REJECTED`) or merely failed to carry the *connection* (`AZ_IOT_ERR_MQTT`). The SDK retries an identity refusal on `identity_recovery` (and, in `AZ_IOT_IDENTITY_RECOVERY_REPROVISION` mode, re-provisions through DPS), and a connection failure on `reconnection_policy`. An adapter that flattens the two leaves the device on the wrong retry schedule, and unable to follow a DPS hub reassignment in `REPROVISION` mode.

**On a refused SUBACK** — set `status` from `az_iot_mqtt_suback_result(version, suback_code)`, for the same reason. Pass the code exactly as it came off the wire: a v3.1.1 SUBACK return code (`0x00`–`0x02` granted QoS, `0x80` Failure); pass a negative value for failures your client raised itself. The helper decides whether the broker refused the *filter* in a way a retry cannot change (`AZ_IOT_ERR_SUBSCRIPTION_REFUSED` — `0x80`) or the request failed some other way (`AZ_IOT_ERR_MQTT`). That distinction is what lets connection policy separate a filter that will be refused identically next time from one that failed because the service was briefly unwell; an adapter that flattens them denies it the choice. **A granted QoS lower than the one requested is a success, not a refusal** — never report it as an error.

**On a PUBACK** — set `status` from `az_iot_mqtt_puback_result(version, puback_code)`. Pass `0` for a v3.1.1 PUBACK (it carries no code), and a non-zero value for a publish your client failed itself; the helper maps `0` to `AZ_IOT_OK` and anything else to `AZ_IOT_ERR_MQTT`. Feature clients pass this status unchanged to the application's publish callback.

**On any acknowledgement that carries a code** — also set `protocol_code` on the `az_iot_mqtt_event` to that verbatim wire value, and leave it 0 only when there is none. A non-zero value is always the wire code, including the granted QoS on a SUBACK that succeeded. `status` is what the SDK branches on; `protocol_code` is what a log line and a support engineer read, and it is the only way a code newer than this SDK survives the trip into C. Do not substitute your own numbering for it.

**On an UNSUBACK** — raise `AZ_IOT_MQTT_EVT_UNSUBSCRIBE_ACK` with the packet id `unsubscribe()` returned. Conformance: `publish_subscribe_roundtrip`.

MQTT 3.1.1 packets carry no Reason String: leave `reason_string` NULL.

### Resumed sessions: no resend

Each session gets a fresh client (`create()` or `init()`), and the SDK destroys it when the session
ends, even when the next connect resumes the broker session. An adapter therefore need not resend
unacknowledged PUBLISH or PUBREL packets on a resumed session, and the SDK never asks it to. At the
session end the SDK completes the publishes still awaiting a PUBACK with
`AZ_IOT_ERR_NOT_CONNECTED`, and the layer above (the application, or the SDK for its own messages)
sends them again with new packet identifiers. This deliberately deviates from MQTT section 4.4,
`MQTT-4.4.0-1`: see [Connecting](connecting.md#resumed-sessions-and-retries).

- `az_iot_mqtt_limits.message_store_size` is memory for the copies your MQTT library needs while a
  session outlives the connection. az_mqtt requires it for QoS 1 on such a session: with 0, its
  `validate_limits_for_session_usage()` makes the connection client's `init()` fail with
  `AZ_IOT_ERR_INVALID_ARG`, and its connect refuses such a session with
  `AZ_IOT_ERR_NOT_SUPPORTED`. When it is full, return `AZ_IOT_ERR_BUSY` from `publish`.
- The SDK never passes a QoS 2 PUBLISH to `publish`.

### The single-thread contract (important)

`azure-iot-sdk` is a **single-threaded pump**. All inbound callbacks (the one registered via `set_inbound_cb`) **must fire on the thread that calls `process_loop()`**, not on whatever I/O thread your underlying MQTT client uses internally.

If your library has its own I/O thread (Paho does), the recommended pattern is:
1. Capture the event in a tiny thread-safe FIFO from the I/O thread.
2. Drain the FIFO and invoke `set_inbound_cb`'s callback inside `process_loop`.

The Paho adapter implements exactly this in [adapters/paho/az_iot_mqtt_paho.c](../adapters/paho/az_iot_mqtt_paho.c) (search for `q_push` / `q_pop`).

The application makes every SDK call from one thread or serializes them ([architecture](architecture.md)), so the SDK calls your adapter from one thread at a time. Lock only the state your own I/O threads share with it, as the FIFO above. `tls.sign` follows the same rule (see [TLS](#tls-what-the-adapter-must-do)): call it only from inside `connect()` or `process_loop()`.

### Concrete client struct

The vtable expects `az_iot_mqtt_client*` to point at a struct whose **first member** is `const az_iot_mqtt_iface* iface`. Embed your adapter state behind it:

```c
typedef struct mymqtt_client_tag {
    az_iot_mqtt_client base;          /* MUST be first */
    az_iot_mqtt_iface  iface_storage;
    az_iot_mqtt_version version;
    /* ... your state ... */
} mymqtt_client_t;
```

### TLS: what the adapter must do

Two obligations, and only one of them is a choice.

**Server certificate validation is not optional.** Whenever your adapter establishes a TLS session it must validate the server's certificate chain **and** its hostname. `az_iot_mqtt_tls_options` carries no flag that asks for an unverified session, and adding one to your own adapter would defeat the point: an unverified session authenticates nothing.

**`tls.use_tls` only SELECTS TLS.** Turn TLS on when any TLS material is present — `client_cert_path`, `client_cert_pem`, `trusted_ca_path`, `trusted_ca_pem`, a key reference — **or** when `use_tls` is set. `use_tls` exists for the connection that carries none of the others, such as server-authentication-only. Keying off the certificate alone silently downgrades those to plaintext.

> **Migration note.** This slot was previously `bool verify_server`, which could switch validation off, and was `false` in a zero-initialized struct — so a caller who simply forgot it got an unverified connection. It is now `bool use_tls` at the same offset and type (no ABI change), and validation is unconditional.
>
> An adapter still reading `opts.tls.verify_server` **will not compile**, and that is deliberate: a source alias would let such an adapter keep gating verification on a field that no longer means verification, so a caller who set only a certificate would get an *unverified* TLS session. The compile error puts the decision in front of you. The fix is to verify unconditionally and use `use_tls` solely as one of the TLS triggers.

**Copy TLS material during `connect()`.** Like `username` and `password`, the `tls` strings are valid only while `connect()` runs: the SDK then releases them to the certificate provider, which may wipe `client_key_pem` and `client_key_password`. `tls.sign` and `tls.sign_ctx` are the exception: they stay valid until the adapter client is destroyed, so the handshake may call `sign()` after `connect()` returns. Call `sign()` only from inside `connect()` or `process_loop()`, on the thread that drives the client; a client whose handshake runs on its own thread must not accept a `sign()`-only credential. Conformance: `mutual_tls_does_not_read_paths_after_returning`, and `mutual_tls_does_not_read_pem_after_returning` for `AZ_IOT_CONFORMANCE_CAP_TLS_PEM` (4.5).

**Non-extractable keys.** If `tls.client_key_uri`, `tls.client_key_psa_id` or `tls.sign` is set, the caller is asking you to authenticate with a private key that cannot be read. Honour it, or fail the connect with `AZ_IOT_ERR_NOT_SUPPORTED`. What you must never do is connect *without* the credential you were asked to use — the conformance suite checks this whether or not you claim the feature. See 4.4.

## Step 2 — Implement the adapter

An adapter is a constant struct of two functions and an optional third (see `az_iot_mqtt_adapter` in [az_iot_mqtt_iface.h](../inc/azure/iot/az_iot_mqtt_iface.h)). The application lends the connection client one workspace, and the connection client builds each session's client in it with `init()`; it runs one session, so one client, at a time:

```c
/* get_workspace_size(): bytes for the largest client this adapter builds with limits, over the
 * MQTT versions it supports; 0 when limits are invalid. */
static size_t mymqtt_get_workspace_size(const az_iot_mqtt_limits* limits)
{
    if (limits == NULL || limits->send_buffer_size == 0 /* ... */)
    {
        return 0;
    }
    return sizeof(mymqtt_client) + 15u /* any alignment */ + limits->send_buffer_size
        + limits->receive_buffer_size /* + in-flight entries, message store, connect strings */;
}

/* init(): build a client of mqtt_version in workspace, which stays valid until the client is
 * destroyed. No allocation is needed: carve every buffer out of the workspace. */
static az_iot_result mymqtt_init(
    az_iot_mqtt_version mqtt_version,
    const az_iot_mqtt_limits* limits,
    uint8_t* workspace,
    size_t workspace_size,
    az_iot_mqtt_client** out_client)
{
    if (mqtt_version != AZ_IOT_MQTT_VERSION_3_1_1)
    {
        return AZ_IOT_ERR_NOT_SUPPORTED;
    }
    if (workspace_size < mymqtt_get_workspace_size(limits))
    {
        return AZ_IOT_ERR_NOT_ENOUGH_SPACE;
    }
    /* align, zero, fill iface, place buffers; *out_client = &client->base */
    return AZ_IOT_OK;
}

/* validate_limits_for_session_usage() (optional, may be NULL): refuse limits that cannot serve the
 * sessions the connection client will run, so its init() fails instead of every connect. */
static az_iot_result mymqtt_validate_limits_for_session_usage(
    const az_iot_mqtt_limits* limits,
    const az_iot_mqtt_session_usage* usage)
{
    if (limits == NULL || usage == NULL)
    {
        return AZ_IOT_ERR_INVALID_ARG;
    }
    if (usage->session_outlives_connection && limits->message_store_size == 0)
    {
        /* log why */
        return AZ_IOT_ERR_INVALID_ARG;
    }
    return AZ_IOT_OK;
}

static const az_iot_mqtt_adapter s_mymqtt_adapter = {
    .get_workspace_size = mymqtt_get_workspace_size,
    .init = mymqtt_init,
    .validate_limits_for_session_usage = mymqtt_validate_limits_for_session_usage,
};

const az_iot_mqtt_adapter* az_iot_mymqtt_get_adapter(void) { return &s_mymqtt_adapter; }
```

The client's `destroy()` releases what the client holds and wipes what it copied (credentials included), but frees nothing: the workspace is the application's again once it returns. `az_iot_mqtt_limits` carries the sizes the application chose (send and receive buffers, requests in flight, message store, connect strings); honour the ones that apply to your client and check the others are valid. A client library that allocates its own buffers (as Paho does) may keep only its own state in the workspace.

## Step 3 — Wire it into CMake

```cmake
add_library(az_iot_adapter_mymqtt STATIC
    az_iot_mqtt_mymqtt.c
)
target_include_directories(az_iot_adapter_mymqtt PUBLIC
    ${PROJECT_SOURCE_DIR}/inc
)
target_link_libraries(az_iot_adapter_mymqtt
    PUBLIC  az_iot_core
    PRIVATE <your-mqtt-library>
)
```

## Step 4 — Run the conformance suite

This is how you verify your adapter is plug-compatible. The suite lives in [tests/conformance/](../tests/conformance/) and operates strictly through the public iface vtable — it does not depend on Paho or any specific client.

### 4.1 — Add a harness exe

Add a tiny harness that hands your `az_iot_mqtt_adapter` (its `get_workspace_size()` and `init()`, see [az_iot_mqtt_iface.h](../inc/azure/iot/az_iot_mqtt_iface.h)) to `az_iot_conformance_run()`. The suite builds each client of the suite's MQTT version with `init()`, in a heap workspace it frees after `destroy()`:

```c
/* tests/conformance/mymqtt_v3_main.c */
#include "az_iot_conformance.h"
#include "azure/iot/adapters/az_iot_adapter_mymqtt.h"

int main(void) {
    return az_iot_conformance_run(AZ_IOT_CONFORMANCE_SUITE_V3_1_1, az_iot_mymqtt_get_adapter());
}
```

Wire it into `tests/CMakeLists.txt` next to the existing Paho harnesses:

```cmake
add_executable(az_iot_conformance_mymqtt_v3 conformance/mymqtt_v3_main.c)
target_link_libraries(az_iot_conformance_mymqtt_v3 PRIVATE
    az_iot_conformance az_iot_adapter_mymqtt)
if(AZ_IOT_BUILD_CONFORMANCE_TESTS)
    add_test(NAME az_iot_conformance_mymqtt_v3 COMMAND az_iot_conformance_mymqtt_v3)
endif()
```

### 4.2 — Provide a broker

The suite needs a real MQTT broker to talk to. Locally, `eclipse-mosquitto:2` works out of the box:

```sh
docker run -d --name mqtt-mosq -p 1883:1883 eclipse-mosquitto:2 \
    sh -c "echo 'listener 1883'>/m.conf; \
           echo 'allow_anonymous true'>>/m.conf; \
           mosquitto -c /m.conf"
```

Configure the suite to point at it:

```sh
export AZ_IOT_MQTT_BROKER_HOST=localhost
export AZ_IOT_MQTT_BROKER_PORT=1883
```

If `AZ_IOT_MQTT_BROKER_HOST` is unset or empty, the harness **fails**. Whether the suite runs at all is a build-time decision (`AZ_IOT_BUILD_CONFORMANCE_TESTS`): if you have no broker, configure without it and the tests are not registered. The harness used to report itself as `Skipped` instead, which meant a suite could stop running without anyone noticing.

### 4.3 — Run

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug --output-on-failure -R conformance
```

Expected output for a fully-working adapter:

```
Test #N: az_iot_conformance_mymqtt_v3 ........   Passed
```

### 4.4 — What the suite actually exercises

The cases are in [tests/conformance/az_iot_conformance.c](../tests/conformance/az_iot_conformance.c). They cover:

- **Lifecycle:** connect/disconnect, reuse after disconnect, disconnect without connect, keep-alive,
  and client-initiated disconnects.
- **Messaging:** publish/subscribe round trips, refused SUBSCRIBE, truncated PUBLISH and unknown
  packet ids.
- **Network faults** (through a test proxy): refused, unresolvable and black-holed endpoints,
  network drops, fragmentation, latency, bandwidth limits and stalled links.
- **Session terms:** clean-session flag, Will, the DISCONNECT, session-present.
- **TLS** (with `-DAZ_IOT_BUILD_CONFORMANCE_TESTS_TLS=ON`): server certificate validation (trusted,
  untrusted, expired, hostname mismatch) and mutual TLS.
- **Egress:** a WebSocket request is never downgraded, and an unreachable proxy is never bypassed.

Re-run the suite after each SDK upgrade to catch regressions in your adapter.

The suite also holds every adapter to the safety half of the optional features, whatever it declares:

- **`a_key_reference_is_never_silently_ignored`** — given a key URI naming a provider that cannot exist, your `connect` must fail. If you have not declared key custody it must fail with `AZ_IOT_ERR_NOT_SUPPORTED`, so the caller can tell "I cannot do this" from "I tried and it failed".
- **`a_sign_hook_is_never_silently_ignored`** — the same for `tls.sign`.
- **`a_psa_key_id_is_never_silently_ignored`** — the same for `tls.client_key_psa_id`: the connect must not succeed with an id that names no usable key.

### 4.5 — Declaring optional capabilities

If your adapter implements an optional feature, say so, or the suite can only check that you refuse it cleanly.

Non-extractable key custody has **three independent routes**, and `az_iot_mqtt_tls_options` says you may implement any of them, or none. They are separate capabilities, so declare only what you implement:

| capability | route | material to supply |
|---|---|---|
| `AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_URI` | `client_key_uri` + `crypto_engine_id` — your stack has an engine/provider abstraction | `key_uri`, `crypto_engine_id`, `client_cert_path` |
| `AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_SIGN` | `sign` + `sign_ctx` — no such abstraction; you drive the handshake signature through a callback | `sign`, `sign_ctx` (may be NULL), `client_cert_path` |
| `AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_PSA` | `client_key_psa_id` — your stack is mbedTLS and signs with a key in the PSA Crypto key store; proved by a handshake and a reconnect | `client_key_psa_id` (usable for the whole run), `client_cert_path` |

```c
az_iot_conformance_options opts = { 0 };

/* Engine/provider route. */
opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_URI;
opts.key_uri          = "pkcs11:object=device-key;type=private";
opts.crypto_engine_id = "pkcs11";

/* Callback route, for a stack with no engine/provider abstraction. */
opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_SIGN;
opts.sign     = my_sign_with_hsm;
opts.sign_ctx = my_hsm_handle;

/* PSA route, for an mbedTLS stack: a key already in the PSA key store. */
opts.capabilities |= (uint32_t)AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_PSA;
opts.client_key_psa_id = my_psa_key_id;

/* Shared: a certificate carrying the PUBLIC key behind each route. */
opts.client_cert_path = "/path/to/device-cert.pem";

return az_iot_conformance_run_with_options(
    AZ_IOT_CONFORMANCE_SUITE_V3_1_1, az_iot_mymqtt_get_adapter(), &opts);
```

If your adapter loads TLS credentials from in-memory PEM (`trusted_ca_pem`, `client_cert_pem`, `client_key_pem`), declare `AZ_IOT_CONFORMANCE_CAP_TLS_PEM`. It needs no material: the suite issues a client certificate, connects with PEM only, and wipes the buffers as soon as `connect()` returns; the handshake must still succeed. It needs a TLS build (`-DAZ_IOT_BUILD_CONFORMANCE_TESTS_TLS=ON`).

Each declared route is proved by its own real TLS handshake, signed with a key you cannot read: `key_custody_completes_a_tls_handshake` for the URI route, `key_custody_sign_hook_completes_a_tls_handshake` for the callback route. Declaring one route does not oblige you to the other — the bundled Paho adapter declares only `_URI`, because Paho exposes no TLS key callback.

**A capability you declare but never exercise fails the run**, unless you opt out explicitly (option 3 below). A declaration is your claim, and a green suite has to mean the claim was checked — so by default the suite will not pass a run that skipped it. You will see:

```
conformance: FAILED: AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_URI is declared but its contract
was NOT exercised: no key was supplied. ...
```

Three ways out, in order of preference:

1. Supply that route's material, and build with `-DAZ_IOT_BUILD_CONFORMANCE_TESTS_TLS=ON`. The contract gets checked.
2. Do not declare the capability in a build that cannot check it. You are then held to the baseline, which still requires you to refuse a custody request cleanly rather than connect without the credential you were asked to use — separately for each route.
3. Set `AZ_IOT_CONFORMANCE_ALLOW_UNPROVEN=1` when no token exists on the machine. This downgrades the failure to a notice. Only the exact value `1` does so, it must be set deliberately per run, and such a run proves nothing about that capability — do not report it as conformant for the feature.

Material is all-or-none per route, and material supplied for a route you did not declare fails the run: being ignored in silence is indistinguishable from a machine with no token at all.

## Step 5 — Use your adapter at runtime

Once the conformance suite is green, give the adapter, the sizes and a workspace to the connection client:

```c
static uint8_t mqtt_workspace[MYMQTT_WORKSPACE_SIZE]; /* >= get_workspace_size(&limits) */

az_iot_connection_client_options opts = az_iot_connection_client_options_default();
/* ... workspace, identity, auth ... */
opts.mqtt_adapter = az_iot_mymqtt_get_adapter();
opts.mqtt_limits = (az_iot_mqtt_limits){ .send_buffer_size = 8192, .receive_buffer_size = 8192,
                                         .max_in_flight_messages = 8,
                                         .connect_strings_size = 4096 };
opts.mqtt_workspace = mqtt_workspace;
opts.mqtt_workspace_size = sizeof(mqtt_workspace);
if (az_iot_connection_client_init(&client, &opts) != AZ_IOT_OK)
{
    /* handle the error */
}
```

`mqtt_adapter`, `mqtt_limits` and `mqtt_workspace` are required: `init()` fails with `AZ_IOT_ERR_INVALID_ARG` without them, and with `AZ_IOT_ERR_NOT_ENOUGH_SPACE` when the workspace is too small. At session-open time the connection client asks the adapter for a client of the MQTT version the target Azure service requires. The workspace is the connection client's until `az_iot_connection_client_deinit()` returns; deinit wipes it.

## Common pitfalls

- **Calling the user callback on the wrong thread.** The SDK's whole point is to keep the user on a single thread. If you hand callbacks straight from your client's I/O thread, all higher-level state machines in the SDK become racy. Use a FIFO + drain in `process_loop`.
- **Returning success synchronously when the operation has not actually completed.** `connect`, `subscribe`, `publish` all return immediately; success/failure of the broker round-trip arrives later through the inbound callback. Returning `az_iot_OK` from `connect` only means "I accepted your CONNECT request and started working on it".
- **Forgetting to populate every vtable slot.** The conformance suite asserts every slot is non-NULL.
- **Reusing a single underlying client across DPS → Hub transitions.** Each session asks `init()` for a fresh client in the workspace; do not cache.
- **Gating server certificate validation on a caller flag.** There is no such flag, and `use_tls` is not one — it only selects TLS. Validate the chain and the hostname every time.
- **Ignoring a key reference you cannot honour.** Connecting anyway means the session carries none of the credential the caller asked to authenticate with. Fail with `AZ_IOT_ERR_NOT_SUPPORTED` instead.
- **Allocating or keeping pointers past `destroy()`.** The workspace may be reused for the next session's client as soon as `destroy()` returns; anything the client allocated must be released by then.

## Reference

- Iface header: [inc/azure/iot/az_iot_mqtt_iface.h](../inc/azure/iot/az_iot_mqtt_iface.h)
- Reference adapter: [adapters/paho/](../adapters/paho/)
- Conformance suite: [tests/conformance/](../tests/conformance/)
- Architecture overview: [architecture.md](architecture.md)
