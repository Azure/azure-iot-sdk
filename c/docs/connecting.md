<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Connecting a device

How the connection client provisions a device, connects it, and keeps it connected. The API is in
[`az_iot_connection_client.h`](../inc/azure/iot/az_iot_connection_client.h), which documents every
option in full.

## Minimal setup

```c
static uint8_t workspace[AZ_IOT_CONNECTION_CLIENT_DEFAULT_WORKSPACE_SIZE];
az_iot_connection_client_options opts = az_iot_connection_client_options_default();
opts.workspace = workspace; /* required; see client-configuration.md */
opts.workspace_size = sizeof(workspace);
opts.dps.id_scope = "<id-scope>";
opts.dps.registration_id = "<registration-id>";
opts.certificate_provider = &certs.base; /* e.g. az_iot_certificate_provider_pem */

az_iot_mqtt_factory* v3 = az_iot_paho_factory_create_v3_1_1();
az_iot_mqtt_factory* v5 = az_iot_paho_factory_create_v5();
if (az_iot_connection_client_init(&client, &opts) != AZ_IOT_OK)
{
  az_iot_paho_factory_destroy(v3);
  az_iot_paho_factory_destroy(v5);
  return 1;
}
if (az_iot_connection_client_add_state_observer(&client, on_state, ctx) != AZ_IOT_OK
    || az_iot_connection_client_register_mqtt_factory(&client, v3) != AZ_IOT_OK
    || az_iot_connection_client_register_mqtt_factory(&client, v5) != AZ_IOT_OK)
{
  az_iot_connection_client_deinit(&client);
  az_iot_paho_factory_destroy(v3); /* NULL: no-op */
  az_iot_paho_factory_destroy(v5);
  return 1;
}

/* Create feature clients, then: */
if (az_iot_connection_client_open(&client) != AZ_IOT_OK)
{
  /* deinit feature clients, then: */
  az_iot_connection_client_deinit(&client);
  az_iot_paho_factory_destroy(v3);
  az_iot_paho_factory_destroy(v5);
  return 1;
}
for (;;)
{
  (void)az_iot_connection_client_do_work(&client, 100);
}
```

Start from `az_iot_connection_client_options_default()`. A zero-initialized options struct has
reconnection disabled.

## MQTT factory ownership

- The application owns every factory it creates with `az_iot_paho_factory_create_*()`.
- `az_iot_connection_client_register_mqtt_factory()` lends the factory to the connection client.
  Keep it alive until `az_iot_connection_client_deinit()` returns, then destroy it with
  `az_iot_paho_factory_destroy()`. Destroying it earlier is a use-after-free.
- A factory that was never registered, or whose registration failed, can be destroyed at any time.
- Registering the same factory twice returns `AZ_IOT_OK` and keeps one registration.

The same rules apply to the az_mqtt adapter factories, and match the certificate providers:
the application creates them, and destroys them after the connection client.
Each sample that connects is also built on az_mqtt, from the same `main.c`
([samples](../samples/README.md#build)).

Teardown:

```c
/* deinit feature clients, then: */
az_iot_connection_client_deinit(&client);
az_iot_paho_factory_destroy(v3);
az_iot_paho_factory_destroy(v5);
```

To skip DPS and connect straight to a hub, set `iothub_hostname` and `device_id` instead of the `dps` fields,
and set `connection_profile` to `AZ_IOT_CONNECTION_PROFILE_MQTT_V5` for an mqttv5 hub (the default
is mqttv3). To connect to a saved hub and fall back to DPS, set both; see
[Saved hub with DPS fallback](#saved-hub-with-dps-fallback).

[`samples/unified/telemetry`](../samples/unified/telemetry/main.c) is a complete example, but not
production-ready code. It reads its key from a file; for production, see
[Production keys](#production-keys).

## Connection states

Values of `az_iot_connection_state` (`AZ_IOT_CONN_STATE_*`):

| State | Meaning |
| --- | --- |
| `IDLE` | Not connected. `open()` is legal only here. |
| `SETTING_UP` | A connection or registration attempt's local steps: feature-client binds, adapter creation, credential load or SAS signing, registration body. Entered by every attempt, including each retry. |
| `CONNECTING` | The network connect is starting: announced just before it is issued, then held while the handshake is in flight. |
| `CONNECTED` | Ready. Every required subscription is in place. |
| `RETRY_PENDING` | A retry is scheduled; nothing is in flight. The next attempt starts in `SETTING_UP`. |
| `DISCONNECTING` | A session is closing: `close()` was called, or the provisioning session ends after registration. |
| `FAULTED` | Stopped after a failure. The SDK does not retry from here. Call `close()` to return to `IDLE`, then `open()` again. |

State is tracked per scope: `AZ_IOT_CONN_SCOPE_DPS` for the provisioning session and
`AZ_IOT_CONN_SCOPE_HUB` for the hub session. Read it with
`az_iot_connection_client_get_state(client, scope)`, or watch it with
`az_iot_connection_client_add_state_observer()`.

Each state event carries `scope`, `state`, `reason` (an `az_iot_result`), `is_retriable`, and
optional `error` detail (source, code, message). The event is valid only during the callback.

Every attempt moves its scope: `SETTING_UP`, then `CONNECTING` and `CONNECTED`, or a failure state.
A DPS registration on a provisioning session that is already up goes `SETTING_UP` → `CONNECTED`,
with no `CONNECTING`, since nothing new is connected.
So each failed attempt produces an event, including under a policy that retries forever. A step that
fails on the device carries `error->source == AZ_IOT_CONN_ERR_SRC_LOCAL`, the step's `az_iot_result`
as `code`, and the step as `message`, for example `certificate provider load() failed`. A refused
MQTT 5 CONNACK or a server DISCONNECT with a nonzero reason code carries
`error->source == AZ_IOT_CONN_ERR_SRC_MQTT`, the reason code as `code`, and the server's Reason
String, if it sent one, as `message` (at most `AZ_IOT_CONN_ERROR_MESSAGE_MAX` bytes). The Paho
adapter cannot report a refused CONNACK's Reason String.
`open()` first validates the configuration (credential shape, CSR support, registration payload
and its buffer); a refusal there starts no attempt, raises no event, and is reported only by
`open()`'s return value.

## Opening and closing

- `open()` starts a connection from `IDLE`. `close()` returns the client to `IDLE` from any state;
  from a live session the disconnect completes in `do_work()`.
- `close()` then `open()` reconnects after `FAULTED`. Feature clients and observers survive it.
- `open()`, `close()` and `deinit()` cannot be called from a callback the client runs: state
  observers, publish completion callbacks, the SAS token callback, the DPS operational-certificate,
  registration-payload and assignment callbacks, CSR callbacks, subscription-failure callbacks,
  and feature clients' message and response handlers. There they return `AZ_IOT_ERR_BUSY` and
  change nothing. Record the decision in the callback and call them after `do_work()` returns.
  Feature clients' own `deinit()` stays callable from callbacks.
- The client reconnects by itself under `reconnection_policy`. To decide yourself, use
  `az_iot_connection_client_get_disabled_retry_policy()`: a failure then ends in `FAULTED`.

```c
static bool g_restart;

static void on_state(const az_iot_connection_state_event* event, void* user_ctx)
{
  (void)user_ctx;
  if (event->scope == AZ_IOT_CONN_SCOPE_HUB && event->state == AZ_IOT_CONN_STATE_FAULTED)
  {
    g_restart = true; /* close() here would return AZ_IOT_ERR_BUSY. */
  }
}

/* Application loop. */
for (;;)
{
  (void)az_iot_connection_client_do_work(&client, 100);
  if (g_restart)
  {
    g_restart = false;
    (void)az_iot_connection_client_close(&client);
    if (az_iot_connection_client_open(&client) != AZ_IOT_OK)
    {
      break;
    }
  }
}
```

## Provisioning and the hub profile

On `open()`, the client registers with DPS over MQTT 3.1.1, receives the assigned hub, device id
and connection profile, and then connects to the hub. It picks MQTT 3.1.1 or 5 from the profile.
See [Architecture](architecture.md#hub-generations-mqttv3-and-mqttv5).

- `az_iot_connection_client_get_hub_profile()` reports the assigned profile once the hub is
  `CONNECTED`.
- `az_iot_connection_client_get_iothub_address()` and `az_iot_connection_client_get_device_id()`
  return the assigned hub and device id once the assignment is adopted (`NULL` before). The device
  id can differ from the registration id (e.g. custom allocation); save these two values, not the
  registration id, to connect directly later.
- Feature clients may be created before or after `open()`. Creating one for the wrong generation
  fails with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH`.
- A custom registration payload (one JSON object) can be sent with `dps.registration_payload`,
  built into `dps.registration_body_buffer`. The service's reply is delivered through
  `az_iot_connection_client_set_registration_payload_callback()`.

The assignment is cached. Reconnects go back to the same hub, and the client re-provisions only
when:

- the application calls `az_iot_connection_client_request_reprovision()`,
- the hub rejects the device identity at CONNACK and `identity_recovery.mode` is
  `AZ_IOT_IDENTITY_RECOVERY_REPROVISION`,
- `dps.max_hub_connect_attempts_before_reprovision` consecutive hub connects fail (default 50),
- a registration fails or returns no assignment, or
- the client rejected the assignment (`AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED` or
  `_MISMATCH`). The next `open()` registers again instead of reusing the cached hub.

### Saved hub with DPS fallback

The cache lives only as long as the client. To keep using an assignment across a restart, the
application saves it and supplies it with the `dps` fields:

```c
opts.iothub_hostname = saved_hub;        /* az_iot_dps_assignment::hub_hostname */
opts.device_id = saved_device_id;        /* az_iot_dps_assignment::device_id */
opts.connection_profile = saved_profile; /* az_iot_dps_assignment::profile */
opts.dps.id_scope = "<id-scope>";
opts.dps.registration_id = "<registration-id>";
```

- Save the values from the callback set with
  `az_iot_connection_client_set_dps_assignment_callback()`. It fires each time an assignment is
  adopted (first registration and every re-provision), before the hub connect. A rejected
  assignment is not reported, and its issued certificate is not stored.
- `open()` connects to the saved hub; DPS is not contacted while it accepts the device.
- It goes to DPS under the same conditions as a cached assignment (above). With
  `options_default()`, `identity_recovery.mode` is `RETRY_HUB`, so a hub that refuses the identity
  is retried; set `AZ_IOT_IDENTITY_RECOVERY_REPROVISION` to register again instead.
- A registration replaces the hub, device id and profile; the callback reports the new values.
- `connection_profile` is declared, as for a direct connect: a feature client for the other
  generation fails with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH`.
- An empty `iothub_hostname`, or one without `device_id`, fails `open()` with `AZ_IOT_ERR_INVALID_ARG`.
- Certificates issued earlier are the provider's to keep: when `load(AZ_IOT_CRED_OPERATIONAL)`
  returns the stored chain, the saved hub is connected with it and no certificate is requested.
  DPS still authenticates with the bootstrap credential. With
  `dps.request_operational_certificate`, the issued chain is already stored in the provider when
  the callback runs (the managed provider writes it to `operational_cert_pem_path`).

Sample: [`samples/authentication/dps_persist_assignment`](../samples/authentication/dps_persist_assignment/README.md).

## Reconnection

`reconnection_policy` controls automatic retries:

| Field | Default | Meaning |
| --- | --- | --- |
| `initial_delay_ms` | 1000 | First delay. `0` disables automatic reconnect. |
| `max_delay_ms` | 60000 | Cap on the exponential backoff. |
| `max_attempts` | 0 | Automatic retries after a failure before `FAULTED`; the count resets on success. `0` retries forever. |
| `jitter_pct` | 20 | Random variation, percent. |
| `jitter_mode` | `AZ_IOT_RETRY_JITTER_SYMMETRIC` | `_SYMMETRIC`: ± `jitter_pct`. `_ADDITIVE`: + 0..`jitter_pct`, never shorter than the backoff. |

`az_iot_connection_client_get_default_retry_policy()`, `_get_disabled_retry_policy()` and `_get_fixed_interval_retry_policy()`
build the common shapes.

- DPS and the hub each have their own retry count, so failures on one do not use up the other's
  attempts.
- A `Retry-After` from DPS is honoured even when it exceeds `max_delay_ms`.
- `close()` never triggers a reconnect.
- A failure inside `open()` itself is returned from `open()` and is not retried.

### Identity recovery

When the hub refuses the identity (a CONNACK with `AZ_IOT_ERR_IDENTITY_REJECTED`, or an mqttv5
`Not authorized` DISCONNECT with `AZ_IOT_ERR_AUTH`), the cause is unknown: the device may be
disabled, its certificate revoked, or its assignment moved. `identity_recovery` controls what
happens next. For a CONNACK refusal with more than one credential source, this applies once every
source of a pass is refused (see Fallback); an mqttv5 DISCONNECT refusal applies at once.

| Field | `options_default()` | Zeroed | Meaning |
| --- | --- | --- | --- |
| `mode` | `RETRY_HUB` | `REPROVISION` | What the next attempt does; see below. |
| `policy` | 5 min initial, 1 h cap, ±25%, no limit | uses `reconnection_policy` | Retry schedule after a refusal. |
| `after_assignment_policy` | 5 s initial, 15 s cap, ±20%, 4 attempts | none: `policy` from the first refusal | Retry schedule after a refusal from a hub DPS assigned in this episode, until `HUB:CONNECTED`; then `policy`. |
| `max_duration_seconds` | 0 | 0 | Stop this long after the first refusal; `0` = no limit. No attempt in the episode (hub connect, DPS registration or poll) starts at or after it, until `HUB:CONNECTED`. Recovery stops as soon as the next attempt could not start in time, so the fault may come before the duration ends. Not kept across restarts. |

| `mode` | Next attempt after a refusal |
| --- | --- |
| `AZ_IOT_IDENTITY_RECOVERY_RETRY_HUB` | The cached hub. No DPS registration, no certificate request. |
| `AZ_IOT_IDENTITY_RECOVERY_REPROVISION` | A DPS registration after a CONNACK refusal (the earlier behaviour); the hub after an mqttv5 DISCONNECT refusal. With `dps.request_operational_certificate`, every registration requests a new certificate. |
| `AZ_IOT_IDENTITY_RECOVERY_NONE` | None: the refusal faults. |

- The schedule is separate from the reconnection ladders, and DPS registrations do not reset it.
  Only `HUB:CONNECTED`, `open()` and `close()` do. So repeated DPS-accept / hub-reject cycles
  still back off and stop at the policy's `max_attempts`.
- A hub DPS has just assigned may refuse the device until the new registration reaches it. So
  after a DPS assignment, refusals are first retried on `after_assignment_policy` (by default
  4 attempts within about a minute), then on `policy`. Its attempts are spent once per episode:
  DPS registrations do not restore them. `mode` still decides where each attempt goes.
- When a limit is reached the client goes to `FAULTED` with the refusal as `reason`, on the HUB scope
  and, if a re-registration was pending, on the DPS scope as well.
- With `reconnection_policy` disabled, the first mqttv5 DISCONNECT refusal faults, as does the
  CONNACK refusal that ends a credential pass.
- `az_iot_connection_client_request_reprovision()` makes the next attempt a DPS registration.
  A pending hub retry runs on the next `do_work()`; a pending DPS retry keeps its schedule.

`RETRY_PENDING` and `FAULTED` events carry `recovery`: the classification
(`AZ_IOT_CONN_FAILURE_TRANSIENT`, `_IDENTITY`, `_TERMINAL`), the endpoint, the attempt count, the
delay to the next attempt and whether it goes to DPS. `error` carries the raw reason code.

What survives a reconnect:

| Kept | Lost |
| --- | --- |
| The hub assignment and connection profile | In-flight QoS 1 publishes that were not yet acknowledged |
| Subscriptions registered by feature clients | Pending twin requests and direct-method responses |
| The operational certificate | Desired-property updates sent while disconnected (mqttv3). Call the twin client's `get()` to resynchronize. |
| Software update progress and unsent reports | |

Re-send anything in the "Lost" column once the connection is back.

## Network: WebSockets and proxies

| Option | Effect |
| --- | --- |
| `transport` | `AZ_IOT_MQTT_TRANSPORT_TCP` (default, port 8883) or `AZ_IOT_MQTT_TRANSPORT_WEBSOCKET` (port 443). |
| `proxy` | HTTP proxy host, port and optional Basic credentials. The connection is tunnelled with HTTP `CONNECT`, for either transport. |
| `port` | `0` picks the transport's default. |

The settings apply to DPS and the hub alike. TLS runs end to end inside the proxy tunnel. An
adapter that cannot honour a setting refuses it with `AZ_IOT_ERR_NOT_SUPPORTED`; it does not
silently connect another way.

With the Paho adapter, leaving `proxy` unset lets Paho use the lowercase `https_proxy` /
`http_proxy` environment variables only when `PAHO_C_CLIENT_USE_HTTP_PROXY=TRUE` is set; it then
honours `no_proxy`, and `https_proxy` must start with `http://` or `https://`. Set `proxy` explicitly
to avoid depending on the environment. Other Paho limitations: [Dependencies](dependencies.md#eclipse-paho-mqtt-c).

Examples: [`samples/unified/websockets`](../samples/unified/websockets/main.c),
[`samples/unified/proxy`](../samples/unified/proxy/main.c).

## MQTT session options

| Option | Effect |
| --- | --- |
| `session_continuity` | `DEFAULT` / `RESUME` / `CLEAN`. The hub session resumes by default. DPS sessions are always clean. |
| `session_expiry_seconds` | mqttv5 only. How long the broker keeps the session after a disconnect. Default 1 hour. |
| `lwt` | Optional Last Will. The SDK sets none of its own. |
| `keep_alive_seconds`, `connect_timeout_seconds` | Default 30 s each. |
| `twin_push` | mqttv5 only. Fixed for the client's lifetime. `push_desired` (default `false`): the service pushes the desired snapshot on connect and each desired patch while connected. When `false`, no desired patches are sent; the twin client fetches the snapshot on connect when behind, and later changes are seen only on reconnect or after `az_iot_mqttv5_twin_client_get()`. `push_reported` (default `false`): the service pushes the reported section on connect. With either set, a twin-push is expected after each connect for the requested sections the birth-ack shows out of sync (none when both are in sync); if none arrives within 5 to 15 minutes (longer after consecutive misses), the twin client ends the session with a DISCONNECT and the connection reconnects, or faults with `AZ_IOT_ERR_TIMEOUT` without a reconnection policy. `az_iot_connection_client_do_work()` checks it. |

A device that does not use twin initializes no twin client and leaves both `twin_push` bits `false`.
On an mqttv5 hub its birth then requests no twin session and the service sends it no twin traffic;
`ih/{device_id}/dev/#` covers the twin topic, but nothing is published there. On an mqttv3 hub, only
an initialized twin client subscribes to twin topics. A `twin_push` bit set without a twin client
makes the service push sections that the device drops.

## Certificates

Every connection uses TLS. `certificate_provider` supplies the trusted CA and the X.509 device
credential. `open()` fails with `AZ_IOT_ERR_CREDENTIAL_INCOMPLETE` without one, unless every role
the client uses has a SAS key (see [Authentication](#authentication)); a SAS role then uses
server-authenticated TLS. `dps.request_operational_certificate` always needs a provider.

Operational certificates:

- **Issued at provisioning.** Set `dps.request_operational_certificate` and provide
  `csr_payload_buffer` (`AZ_IOT_CSR_PAYLOAD_BUFFER_MIN` bytes is enough for any CSR the service
  accepts). The provider must implement `get_csr()`, `release_csr()` and
  `store_issued_certificate()`. The device sends the provider's CSR with its registration. The
  issued chain is stored with the provider's `store_issued_certificate()`, then passed with the
  store result to the callback set with `az_iot_connection_client_set_operational_cert_callback()`,
  if set. If storage fails, the registration fails.
- **Renewed over the hub** (mqttv3 only). Call `az_iot_connection_client_send_csr()` while
  `CONNECTED`; the CSR comes from the provider (same three hooks). One renewal can be in flight
  at a time. The renewed chain is stored with the provider's `store_issued_certificate()` before
  the callback, which reports the result in `store_status`. The session is kept either way. If
  the store succeeded, `close()` then `open()` connects with the renewed chain; if it failed, the
  provider keeps the previous credential and the next connect uses it. See
  [`samples/authentication/hub_renew`](../samples/authentication/hub_renew/main.c).
- A DPS-issued certificate is used from the hub connect that follows the registration. The live
  session is never interrupted.
- CSR profile. The service requires a PKCS#10 CSR, base64 DER without PEM framing, with the
  common name set to the DPS registration ID. It does not specify a key algorithm. Certificate
  authorities created in Azure Device Registry sign the certificates they issue with ECDSA
  P-384/SHA-384, whatever key the CSR carries. The managed provider generates an EC P-256 (the
  default), EC P-384 or RSA 2048 key (`key_type`). For an EC key it signs the CSR with
  SHA-256 up to 256 bits, SHA-384 up to 384 bits (P-384) and SHA-512 above; for RSA, SHA-256. The
  digest follows the key in use, which may have been loaded from disk.
- At connect time the operational credential is preferred, with the bootstrap credential as a
  fallback.

### Production keys

Most X.509 samples read the device's private key from a PEM file, so they run anywhere; the
`hsm_pkcs11` sample keeps it in a token, `hsm_sign_callback` shows the `sign()` route with a
stand-in signer, and the SAS-only samples use none. A key in a file
can be copied, and the device's identity with it. In production, keep each device's X.509 private
key in hardware -- an HSM, a TPM or a secure element -- which signs with it but never releases it.
The certificate can stay a file: it is public. A password-protected PEM key is not a substitute: the
device must keep the password where it can read it, next to the key, so a copy of the device's
storage still yields the key.

| The key | The provider returns | Adapter | Sample |
| --- | --- | --- | --- |
| Can be named (PKCS#11 URI, TPM 2.0 object) and has an OpenSSL 3.x provider | `client_key_uri` + `crypto_engine_id` | Paho (`AZ_IOT_PAHO_KEY_CUSTODY`); az_mqtt with OpenSSL TLS | [`hsm_pkcs11`](../samples/authentication/hsm_pkcs11/README.md) |
| Is a PSA Crypto key (e.g. the ESP32 Digital Signature peripheral) | `client_key_psa_id` | az_mqtt with mbedTLS TLS (ESP-IDF); others refuse it with `AZ_IOT_ERR_NOT_SUPPORTED` | [ESP32 software updates](../samples/software_update/esp32/README.md#device-identity) |
| Can only be asked to sign | A `sign()` hook | Your own ([Bring your own MQTT client](how_to_byo_mqtt_client.md)); Paho and az_mqtt refuse it with `AZ_IOT_ERR_NOT_SUPPORTED` | [`hsm_sign_callback`](../samples/authentication/hsm_sign_callback/README.md) |

- **Bootstrap keys** authenticate to DPS; keep them in hardware too.
- **Operational keys.** `az_iot_certificate_provider_managed` generates the operational key in
  software and stores it unencrypted in an owner-only file. To keep it in hardware, implement a
  provider that creates the key there and signs the CSR with it in `get_csr()`. For TLS, `load()`
  returns a reference to the key, or the provider implements `sign()` and `load()` returns the
  certificate with no key, as in the table above. Start from
  [`custom_provider_template`](../samples/authentication/custom_provider_template/README.md).
- **SAS keys** are secrets too; keep them in hardware with a user-provided token.
  `on_sas_token_required` only notifies and must not block: sign the
  `az_iot_sas_token_string_to_sign()` output in hardware outside the callback, build the token
  with `az_iot_sas_token_from_signature()`, and supply it with
  `az_iot_connection_client_update_sas_token()` from the `do_work()` thread. See
  [Authentication](#authentication) and
  [`user_provided_sas_token`](../samples/authentication/user_provided_sas_token/README.md).
- **If a key must be a file**, give each device its own key, and let only the account the device
  software runs as read or change it (e.g. `chmod 600`; the directory not writable by others).

## Authentication

> **Implemented:** X.509 from `certificate_provider` (several certificates per role), then SAS
> tokens signed with the primary and secondary keys, then tokens from `on_sas_token_required`,
> with fallback on rejection; `trusted_ca`; `unix_time`; `token_lifetime_seconds`; renewal of the
> hub's token (`renewal_percent`); `auth_source` and `x509_index` in state events.

Each role -- DPS and hub -- is configured with any of these credential sources, tried in this
order, skipping any not set:

| Source | Configure | Notes |
| --- | --- | --- |
| X.509 certificates | `certificate_provider` | As today. The provider may offer more than one per role (index 0, 1, ...). |
| Primary, secondary key | `dps_auth` / `hub_auth`: `sas.primary_key_base64` (+ `secondary_key_base64`, `is_enrollment_group_key`) | The SDK signs tokens with the backend in `crypto`, and needs a Unix time: `time()`, or `unix_time.get_time`. |
| User-provided token | `sas.on_sas_token_required` | The application supplies tokens with `az_iot_connection_client_update_sas_token()`; the SDK never sees its key. |

Setting only X.509, or only SAS, uses that alone. A zeroed `az_iot_auth` means no SAS.

```c
copts.dps_auth.sas.primary_key_base64 = primary;
copts.dps_auth.sas.secondary_key_base64 = secondary;  /* optional */
copts.hub_auth = copts.dps_auth;          /* same keys for the hub; leave zeroed for X.509 only */
copts.crypto = az_iot_crypto_openssl();   /* HMAC-SHA256 for the tokens */
static uint8_t sas_buf[AZ_IOT_SAS_BUFFER_SIZE(2, AZ_IOT_SAS_TOKEN_SIZE(256))]; /* IDs <= 256 */
copts.sas_buffer.buffer = sas_buf;        /* keys + token, app memory */
copts.sas_buffer.size = sizeof(sas_buf);
copts.trusted_ca.path = "ca.pem";         /* server trust, any credential */
```

- **Fallback.** When the service rejects a credential -- a CONNACK refusal, or for DPS
  registration error `401000` -- the next source is tried at once, without a
  `reconnection_policy` delay, even with the policy disabled. The `RETRY_PENDING` event carries the
  rejected credential in `auth_source`, classification `AZ_IOT_CONN_FAILURE_IDENTITY`, attempt 0
  and no delay. Other failures retry the same source under the policy; with the policy disabled, they fault, including one starting a fallback attempt. A fallback to a certificate that is gone by then ends the pass as a local failure, so rejected keys are not retried unpaced. A pass tries each source
  once, from the one it began with, wrapping; it counts as one policy attempt. The source a
  fallback selected is kept until rejected; otherwise each attempt starts at the first available
  source, so a certificate that becomes available (such as one DPS issued) is used next. `open()`
  starts again at the first. `identity_recovery` applies only after a pass in which all of the hub's credentials are rejected. Provisioning sessions without a registration (`provision_only`, or held by a feature client) fall back the same way, but report no `RETRY_PENDING`: `DISCONNECTING` and `IDLE` carry the rejected `auth_source` and reason, then `CONNECTING` the next source. A fully rejected pass is paced by the policy.
- **Cost.** Only devices configured with more than one source pay for fallback: one extra
  connect per rejected source, once per credential change (the working source is kept).
- **Memory.** All SAS state -- decoded keys, signing scratch, the token -- lives in
  `sas_buffer`, which the app provides only when it uses SAS keys or `on_sas_token_required`. Size it with
  `AZ_IOT_SAS_BUFFER_SIZE(distinct keys, token area)`; a key set identically for DPS and the hub
  counts once. The token is wiped once the transport has taken it; the whole buffer at
  `deinit()`.
- **Keys are fixed at `init()`.** They are copied and decoded there; to change them,
  re-initialize the client and its feature clients. Use `on_sas_token_required` to rotate without
  re-initializing.
- **Token callback.** `on_sas_token_required` only notifies: it receives the role, hub
  generation, resource URI, key name and `is_renewal`, and must not block. It is called from
  `do_work()`, never from `open()`; the attempt waits in `SETTING_UP` (no `auth_source` yet) until
  a token is supplied with `az_iot_connection_client_update_sas_token()`, from the callback or
  later, within `connect_timeout_seconds`; otherwise it fails with `AZ_IOT_ERR_TIMEOUT` and is
  retried under the policy, notifying again. To stop waiting, call `close()`. A renewal (hub, or a
  provisioning session kept open) is notified while the session stays up, with no timeout.
- **Supplying a token.** `update_sas_token()` checks the token's `sr` (decoded) and `skn` against
  the role's current identity: `AZ_IOT_ERR_INVALID_ARG` on a mismatch, `AZ_IOT_ERR_NOT_FOUND` for
  a hub not yet assigned by DPS. A waiting attempt or renewal takes it. Otherwise, a session
  connected with a user-provided token (the hub, or a provisioning session that is not
  registering) renews with it at once (disconnect and reconnect, as below);
  anything else keeps it for the role's next attempt that uses a user-provided token, until the
  role settles in `IDLE` or `FAULTED`, `close()`, or the single token area is needed first (a
  key-signed token, or the other role's token request). A token supplied after its attempt timed out
  is kept for the retry, unless the role settles in `IDLE` first: a provisioning session without a
  registration (`provision_only`, or held by a feature client) does, and asks again. Call it from the `do_work()` thread or an SDK callback; it returns
  `AZ_IOT_ERR_BUSY` while a token is being handed to a CONNECT, or from the other role's callback.
  During a callback the resource URI shares the token area, so the token has less room then.
  `lifetime_seconds` counts from the call; a token that expired before use is asked for again,
  and a session whose token expires before its replacement arrives is ended, the reconnect
  waiting for the token. A new DPS assignment drops a hub token asked for or held before it; the
  next attempt asks again.
- **Building a token.** [`az_iot_sas_token.h`](../inc/azure/iot/az_iot_sas_token.h) formats it
  from the request's `resource_uri` and `key_name` and an expiry: `az_iot_sas_token_string_to_sign()`
  gives what to sign, and `az_iot_sas_token_from_signature()` builds the token from its
  HMAC-SHA256, so a key that never leaves a TPM, HSM or secure element can sign it.
  `az_iot_sas_token_sign()` does both with a key in memory, through a crypto backend;
  `az_iot_sas_derive_device_key()` derives a device key from an enrollment-group key.
- **Renewal.** In `hub_auth.sas` and `dps_auth.sas`: at `renewal_percent` (default 80; 1-99) of
  `token_lifetime_seconds` (key-signed, default one hour) or of the supplied `lifetime_seconds`,
  by the monotonic clock or Unix time, whichever comes first (the former may stop in suspend).
  MQTT 3.1.1 cannot re-authenticate a live session, so the SDK
  ends it and reconnects at once, with or without a `reconnection_policy`: `RETRY_PENDING`,
  `SETTING_UP`, `CONNECTING`, `CONNECTED`, each with `is_credential_renewal` and reason
  `AZ_IOT_OK`. Before its DISCONNECT, the session's deliveries settle for up to 5 s (not past the
  token's expiry by either clock): publishes awaiting a PUBACK, and SDK requests awaiting a
  response. Meanwhile a new QoS 1 publish returns `AZ_IOT_ERR_BUSY` (QoS 0 is sent), and once the
  DISCONNECT is sent any publish returns `AZ_IOT_ERR_NOT_CONNECTED`. Publishes still awaiting a
  PUBACK then complete with `AZ_IOT_ERR_NOT_CONNECTED`. A failed
  reconnect is an ordinary failure (fallback, policy). A provisioning session kept open without a
  registration (`provision_only`, or held by a feature client) is renewed the same way, reporting
  `DISCONNECTING`, `SETTING_UP`, `CONNECTING`, `CONNECTED`, each flagged; its feature clients see
  `DISCONNECTING` as for any loss of the session. A registration in progress is not interrupted.
- **Multiple certificates.** `load()` takes an index: certificate 0, 1, ... of the role, until
  `AZ_IOT_ERR_NOT_FOUND`, and never beyond `AZ_IOT_MAX_CERTS_PER_ROLE` (default 4). Each is a
  source in the pass, in index order, before the keys: a rejected certificate falls back to the
  next index at once. The index that connected is kept, like any source, and reported in
  `x509_index`. The hub uses the operational certificates when index 0 of that role exists,
  else the bootstrap ones, checked on every attempt: an operational certificate that appears
  (e.g. issued by DPS) replaces a kept bootstrap one. Without a certificate at index 0, none past
  it is asked for or reused: a kept later index is dropped, for DPS and the hub. A provider with one certificate per role returns `AZ_IOT_ERR_NOT_FOUND`
  for index > 0. Uses: a self-signed identity's primary and secondary certificates, or the
  previous issued certificate kept as a rollback.
- **DPS-issued certificate.** With `dps.request_operational_certificate`, the hub tries the issued
  certificate first; DPS can still use SAS. See
  [`dps_sas_key_issued_cert`](../samples/authentication/dps_sas_key_issued_cert/README.md).
- **mqttv5 hubs** do not accept SAS yet; the design allows for it. The token request carries the
  hub generation, and renewal can use MQTT 5 in-session re-authentication instead of a reconnect.
  Until then, a rejection is `AZ_IOT_ERR_IDENTITY_REJECTED`.
- **Server trust.** `trusted_ca` applies to every connection. Without it, the X.509 provider's CA
  is used if there is one, otherwise the adapter's default store.
- **TPM attestation.** DPS takes it over HTTPS or AMQP, not MQTT, so it is a separate step before the
  connection client: [`az_iot_dps_tpm_client.h`](../inc/azure/iot/az_iot_dps_tpm_client.h) builds
  the registration requests and parses the responses; the application sends them over HTTPS and
  runs the TPM (import the DPS key, HMAC-SHA256). For an mqttv3 hub, then connect directly to the
  assigned hub (`iothub_hostname`, `device_id`, `connection_profile` from the assignment) and sign hub tokens
  with the TPM key from `on_sas_token_required`. mqttv5 hubs do not accept SAS and need an X.509
  certificate, which DPS does not document issuing under TPM attestation. The connection client does not re-provision such a device; on
  `AZ_IOT_ERR_IDENTITY_REJECTED`, register again. See
  [`dps_tpm`](../samples/authentication/dps_tpm/README.md).

## Crypto backend

`crypto` supplies SHA-256 and RS256 verification to every feature that needs them; today that
is software updates. Set it once:

```c
copts.crypto = az_iot_crypto_openssl(); /* or az_iot_crypto_mbedtls() */
```

| Backend | Header | Needs |
| --- | --- | --- |
| `az_iot_crypto_openssl()` | `az_iot_crypto_openssl.h` | OpenSSL 3.0+ |
| `az_iot_crypto_mbedtls()` | `az_iot_crypto_mbedtls.h` | mbedTLS 3.6 LTS or 4.1+ (PSA Crypto), e.g. ESP-IDF |

To bring your own, fill an `az_iot_crypto` with your functions
([az_iot_crypto.h](../inc/azure/iot/az_iot_crypto.h)). `init()` rejects a backend without
SHA-256 or with another `version`; software updates also need `verify_rs256`. A client that uses
no crypto can leave `crypto` NULL.

## Common results

| Result | Meaning |
| --- | --- |
| `AZ_IOT_ERR_IDENTITY_REJECTED` | The hub refused the device identity. Retried on `identity_recovery`. |
| `AZ_IOT_ERR_AUTH` | The hub ended an mqttv5 session as `Not authorized`. Retried on `identity_recovery`. |
| `AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED` | DPS assigned a profile this SDK does not support. The next `open()` re-provisions. |
| `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH` | Feature clients do not match the assigned generation. Rebuild them, then `close()` and `open()`; the next `open()` re-provisions. |
| `AZ_IOT_ERR_SUBSCRIPTION_REFUSED` | The hub refused a subscription the session needs. Terminal, since a retry would be refused again. |
| `AZ_IOT_ERR_CREDENTIAL_INCOMPLETE` | The certificate provider returned unusable material, such as a certificate without a key. |
| `AZ_IOT_ERR_PUBLISH_REFUSED` | Publish callback: the hub refused the message and will refuse it again (mqttv5 `0x87`, `0x90`, `0x99`), or it is over the hub's Maximum Packet Size (`0x95`). Do not resend unchanged. |
| `AZ_IOT_ERR_MQTT` | Transport, TLS or broker failure. The adapter log has the detail. |
| `AZ_IOT_ERR_TIMEOUT` | A handshake step or CSR operation timed out. |

All results are listed in [`az_iot_result.h`](../inc/azure/iot/az_iot_result.h).
