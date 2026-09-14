<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Bring-Your-Own MQTT client adapter

This guide is for a customer (or platform team) who wants to plug a non-default MQTT client library into `azure-iot-sdk`. The SDK ships with an Eclipse Paho-C adapter, but the MQTT layer is fully pluggable: anything that can implement the `az_iot_mqtt_iface` vtable can be dropped in.

If you can produce a static or shared library that exposes one factory function per MQTT version you support, you are done. The conformance test suite ([tests/conformance/](../tests/conformance/)) verifies that your adapter is plug-compatible.

## Prerequisites

- Your MQTT client must be able to speak at least one of:
  - **MQTT v3.1.1**, or
  - **MQTT v5**.
- Implementations may speak both; ship one factory per version.
- C99 source-level compatibility for the adapter glue (the underlying client may be C++/Rust/etc., as long as it exposes a C ABI).

The SDK internally determines which MQTT version is required for each Azure service (DPS uses v3.1.1, Hub-Next uses v5). Your adapter only needs to declare the version it supports — the SDK handles the rest.

## Files you need to write

For an adapter named `mymqtt`:

```
adapters/mymqtt/
├── CMakeLists.txt              # builds your adapter as az_iot_adapter_mymqtt
└── az_iot_mqtt_mymqtt.c    # implements az_iot_mqtt_iface (and pulls in your client lib)

inc/azure/iot/adapters/
└── az_iot_adapter_mymqtt.h # public factory builders (one per version)
```

You can take [adapters/paho/](../adapters/paho/) as a working reference.

## Step 1 — Implement the iface vtable

The contract is in [inc/azure/iot/az_iot_mqtt_iface.h](../inc/azure/iot/az_iot_mqtt_iface.h). You must provide non-NULL function pointers for every slot:

| Slot              | Required behavior                                                                                    |
|-------------------|------------------------------------------------------------------------------------------------------|
| `connect`         | Initiate CONNECT to `opts->host:opts->port` with `client_id`. Non-blocking. Result via inbound `EVT_CONNECTED`. For v5: honor `clean_start`, `session_expiry_seconds`, `user_properties`, and LWT fields from `connect_options`. |
| `disconnect`      | Initiate DISCONNECT. Non-blocking. Eventually emits `EVT_DISCONNECTED`.                              |
| `subscribe`       | Send SUBSCRIBE. Return `out_packet_id` synchronously; emit `EVT_SUBSCRIBE_ACK` when SUBACK arrives.   |
| `unsubscribe`     | Send UNSUBSCRIBE. Same pattern as `subscribe`. Emit `EVT_UNSUBSCRIBE_ACK`.                            |
| `publish`         | Send PUBLISH. For QoS>0, emit `EVT_PUBLISH_ACK` when PUBACK arrives. For v5: set `content_type`, `response_topic`, `correlation_data`, and `user_properties` from `az_iot_mqtt_message` on the outbound packet. |
| `process_loop`    | Drive any pending I/O and dispatch queued inbound events on the calling thread.                       |
| `set_inbound_cb`  | Register the callback the SDK uses to receive `az_iot_mqtt_event` notifications.                |
| `destroy`         | Tear down the client. The SDK may call this even on a never-connected client.                        |

### MQTT v5 property handling (critical for HUB_NEXT)

The SDK's feature clients (direct methods, twin, C2D) use MQTT v5 properties extensively when connected to Hub-Next. Your v5 adapter **must**:

**On outbound PUBLISH** — propagate these `az_iot_mqtt_message` fields as MQTT v5 properties:
- `correlation_data` / `correlation_data_len` → Correlation Data property
- `response_topic` → Response Topic property
- `content_type` → Content Type property
- `user_properties` / `user_properties_count` → User Property pairs

**On inbound MESSAGE** — extract these MQTT v5 properties from the received packet and populate the corresponding `az_iot_mqtt_message` fields before delivering `EVT_MESSAGE`:
- Correlation Data → `correlation_data` / `correlation_data_len`
- Response Topic → `response_topic`
- Content Type → `content_type`
- User Properties → `user_properties` / `user_properties_count`

If any property is absent in the packet, set the pointer to NULL and the length/count to 0.

**On CONNACK** — populate `session_present` in the `az_iot_mqtt_event` delivered with `EVT_CONNECTED`.

**On a rejected CONNACK** — set `status` from `az_iot_mqtt_connack_result(version, connack_code)` rather than reporting a blanket `AZ_IOT_ERR_MQTT`. Pass the code exactly as it came off the wire (a v3.1.1 return code, or a v5 reason code); pass a negative value for failures your client raised itself, such as a refused socket or a TLS handshake error. The helper decides whether the broker refused the *identity* (`AZ_IOT_ERR_IDENTITY_REJECTED`) or merely failed to carry the *connection* (`AZ_IOT_ERR_MQTT`), and the SDK re-provisions through DPS on the former and only on the former. An adapter that flattens the two leaves a device unable to follow a DPS hub reassignment.

**On a refused SUBACK** — set `status` from `az_iot_mqtt_suback_result(version, suback_code)`, for the same reason. Pass the code exactly as it came off the wire: a v3.1.1 SUBACK return code (`0x00`–`0x02` granted QoS, `0x80` Failure) or a v5 reason code; pass a negative value for failures your client raised itself. The helper decides whether the broker refused the *filter* in a way a retry cannot change (`AZ_IOT_ERR_SUBSCRIPTION_REFUSED` — `0x87` Not authorized, `0x8F` Topic Filter invalid) or hit something transient (`AZ_IOT_ERR_MQTT` — `0x97` Quota exceeded, `0x80` Unspecified error). That distinction is what lets connection policy separate a filter that will be refused identically next time from one that failed because the service was briefly unwell; an adapter that flattens them denies it the choice. **A granted QoS lower than the one requested is a success, not a refusal** — never report it as an error.

**On any acknowledgement that carries a code** — also set `protocol_code` on the `az_iot_mqtt_event` to that verbatim wire value, and leave it 0 only when there is none. A non-zero value is always the wire code, including the granted QoS on a SUBACK that succeeded. `status` is what the SDK branches on; `protocol_code` is what a log line and a support engineer read, and it is the only way a code newer than this SDK survives the trip into C. Do not substitute your own numbering for it.

A v3.1.1 adapter may ignore all v5-only fields (they will always be NULL/zero when passed to `publish`).

### The single-thread contract (important)

`azure-iot-sdk` is a **single-threaded pump**. All inbound callbacks (the one registered via `set_inbound_cb`) **must fire on the thread that calls `process_loop()`**, not on whatever I/O thread your underlying MQTT client uses internally.

If your library has its own I/O thread (Paho does), the recommended pattern is:
1. Capture the event in a tiny thread-safe FIFO from the I/O thread.
2. Drain the FIFO and invoke `set_inbound_cb`'s callback inside `process_loop`.

The Paho adapter implements exactly this in [adapters/paho/az_iot_mqtt_paho.c](../adapters/paho/az_iot_mqtt_paho.c) (search for `q_push` / `q_pop`).

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

**Non-extractable keys.** If `tls.client_key_uri` or `tls.sign` is set, the caller is asking you to authenticate with a private key that cannot be read. Honour it, or fail the connect with `AZ_IOT_ERR_NOT_SUPPORTED`. What you must never do is connect *without* the credential you were asked to use — the conformance suite checks this whether or not you claim the feature. See 4.4.

## Step 2 — Implement the factory

A factory is just a struct + a `create` function:

```c
typedef struct mymqtt_factory_state_tag {
    az_iot_mqtt_factory public_;
    /* ... your state ... */
} mymqtt_factory_state_t;

static az_iot_mqtt_client* mymqtt_factory_create(void* factory_ctx)
{
    mymqtt_factory_state_t* st = factory_ctx;
    /* allocate, fill iface_storage, return &client->base */
}

az_iot_mqtt_factory* az_iot_mymqtt_factory_create_v3_1_1(void)
{
    mymqtt_factory_state_t* st = calloc(1, sizeof(*st));
    st->public_.version = AZ_IOT_MQTT_VERSION_3_1_1;
    st->public_.create = mymqtt_factory_create;
    st->public_.factory_ctx = st;
    return &st->public_;
}
```

The factory struct is minimal: it advertises the MQTT **version** it supports and provides a `create` function that produces clients for that version. The SDK internally knows which version is required for each Azure service and selects the appropriate registered factory at connection time.

## Step 3 — Wire it into CMake

```cmake
add_library(az_iot_adapter_mymqtt STATIC
    az_iot_mqtt_mymqtt.c
)
target_include_directories(az_iot_adapter_mymqtt PUBLIC
    ${CMAKE_SOURCE_DIR}/inc
)
target_link_libraries(az_iot_adapter_mymqtt
    PUBLIC  az_iot_core
    PRIVATE <your-mqtt-library>
)
```

## Step 4 — Run the conformance suite

This is how you verify your adapter is plug-compatible. The suite lives in [tests/conformance/](../tests/conformance/) and operates strictly through the public iface vtable — it does not depend on Paho or any specific client.

### 4.1 — Add a harness exe

For each MQTT version your adapter supports, add a tiny harness that hands your factory to `az_iot_conformance_run()`:

```c
/* tests/conformance/mymqtt_v3_main.c */
#include "az_iot_conformance.h"
#include "azure/iot/adapters/az_iot_adapter_mymqtt.h"

int main(void) {
    az_iot_mqtt_factory* f = az_iot_mymqtt_factory_create_v3_1_1();
    int rc = az_iot_conformance_run(AZ_IOT_CONFORMANCE_SUITE_V3_1_1, f);
    az_iot_mymqtt_factory_destroy(f);
    return rc;
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

Repeat for `_v5` if applicable.

### 4.2 — Provide a broker

The suite needs a real MQTT broker to talk to. Locally, `eclipse-mosquitto:2` works out of the box:

```sh
docker run -d --name aeg-mosq -p 1883:1883 eclipse-mosquitto:2 \
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
Test #N: az_iot_conformance_mymqtt_v5 ........   Passed
```

### 4.4 — What the suite actually exercises

Today the suites cover the iface contract end-to-end:

1. **`connect_disconnect_roundtrip`** — fresh client connects, observes `EVT_CONNECTED(OK)`, disconnects cleanly.
2. **`publish_subscribe_roundtrip`** — subscribe to a unique topic with QoS 1, observe `EVT_SUBSCRIBE_ACK(OK)`, publish a known payload, receive it back via `EVT_MESSAGE`, validate topic + payload bytes.
3. **`disconnect_without_connect_is_rejected`** — calling `disconnect` on a never-connected client must not return `az_iot_OK`.

More tests will be added as the SDK grows (reconnect semantics, large payloads, retained messages, MQTTv5 properties, malformed-input handling, etc.). Re-running the suite after each SDK upgrade is the recommended way to catch regressions in your adapter.

The suite also holds every adapter to the safety half of the optional features, whatever it declares:

- **`a_key_reference_is_never_silently_ignored`** — given a key URI naming a provider that cannot exist, your `connect` must fail. If you have not declared key custody it must fail with `AZ_IOT_ERR_NOT_SUPPORTED`, so the caller can tell "I cannot do this" from "I tried and it failed".
- **`a_sign_hook_is_never_silently_ignored`** — the same for `tls.sign`.

### 4.5 — Declaring optional capabilities

If your adapter implements an optional feature, say so, or the suite can only check that you refuse it cleanly.

> **Renamed:** `AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY` is now `AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_URI` (same bit, same proof). There is no alias: change the name to the route you implement. If you implement the callback route, that build error is the point — you want `_SIGN`, which the old single capability could never prove.

Non-extractable key custody has **two independent routes**, and `az_iot_mqtt_tls_options` says you may implement either, both or neither. They are separate capabilities, so declare only what you implement:

| capability | route | material to supply |
|---|---|---|
| `AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_URI` | `client_key_uri` + `crypto_engine_id` — your stack has an engine/provider abstraction | `key_uri`, `crypto_engine_id`, `client_cert_path` |
| `AZ_IOT_CONFORMANCE_CAP_KEY_CUSTODY_SIGN` | `sign` + `sign_ctx` — no such abstraction; you drive the handshake signature through a callback | `sign`, `sign_ctx` (may be NULL), `client_cert_path` |

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

/* Shared: a certificate carrying the PUBLIC key behind each route. */
opts.client_cert_path = "/path/to/device-cert.pem";

return az_iot_conformance_run_with_options(AZ_IOT_CONFORMANCE_SUITE_V3_1_1, f, &opts);
```

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

Once the conformance suite is green, plug the factory into the connection client:

```c
az_iot_mqtt_factory* f = az_iot_mymqtt_factory_create_v3_1_1();
az_iot_connection_client_register_mqtt_factory(client, f);
/* register a v5 factory too if you support HUB_NEXT */
```

The connection client picks the right factory at session-open time based on the MQTT version required for the target Azure service.

## Common pitfalls

- **Calling the user callback on the wrong thread.** The SDK's whole point is to keep the user on a single thread. If you hand callbacks straight from your client's I/O thread, all higher-level state machines in the SDK become racy. Use a FIFO + drain in `process_loop`.
- **Returning success synchronously when the operation has not actually completed.** `connect`, `subscribe`, `publish` all return immediately; success/failure of the broker round-trip arrives later through the inbound callback. Returning `az_iot_OK` from `connect` only means "I accepted your CONNECT request and started working on it".
- **Forgetting to populate every vtable slot.** The conformance suite asserts every slot is non-NULL.
- **Reusing a single underlying client across DPS → Hub transitions.** Each session asks the factory for a fresh client; do not cache.
- **Gating server certificate validation on a caller flag.** There is no such flag, and `use_tls` is not one — it only selects TLS. Validate the chain and the hostname every time.
- **Ignoring a key reference you cannot honour.** Connecting anyway means the session carries none of the credential the caller asked to authenticate with. Fail with `AZ_IOT_ERR_NOT_SUPPORTED` instead.
- **Wrong version.** Registering only a v3.1.1 factory when you need Hub-Next (which requires v5) will fail at connection time with `AZ_IOT_ERR_NOT_SUPPORTED`.

## Feature clients and what they need from the adapter

The following feature clients depend on v5 adapter properties being correctly passed through:

| Feature Client | Outbound v5 properties used | Inbound v5 properties expected |
|----------------|----------------------------|---------------------------------|
| Direct Method  | `correlation_data`, `response_topic` | `correlation_data`, `response_topic` |
| Twin           | `correlation_data` | `correlation_data` |
| C2D            | (none — receive only) | `content_type` |
| Telemetry      | `content_type`, `user_properties` | (none — send only) |

If your v5 adapter does not propagate these properties, the feature clients will silently fail to correlate responses or deliver incomplete data to the application.

## Reference

- Iface header: [inc/azure/iot/az_iot_mqtt_iface.h](../inc/azure/iot/az_iot_mqtt_iface.h)
- Reference adapter: [adapters/paho/](../adapters/paho/)
- Conformance suite: [tests/conformance/](../tests/conformance/)
- Architecture overview: [docs/design.md](design.md)
