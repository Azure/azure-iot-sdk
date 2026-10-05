<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Connecting a device

How the connection client provisions a device, connects it, and keeps it connected. The API is in
[`az_iot_connection_client.h`](../inc/azure/iot/az_iot_connection_client.h), which documents every
option in full.

## Minimal setup

```c
az_iot_connection_client_options opts = az_iot_connection_client_options_default();
opts.dps.id_scope = "<id-scope>";
opts.dps.registration_id = "<registration-id>";
opts.certificate_provider = &certs.base; /* e.g. az_iot_certificate_provider_pem */

if (az_iot_connection_client_init(&client, &opts) != AZ_IOT_OK
    || az_iot_connection_client_add_state_observer(&client, on_state, ctx) != AZ_IOT_OK
    || az_iot_connection_client_register_mqtt_factory(&client, az_iot_paho_factory_create_v3_1_1())
        != AZ_IOT_OK
    || az_iot_connection_client_register_mqtt_factory(&client, az_iot_paho_factory_create_v5())
        != AZ_IOT_OK)
{
  return 1;
}

/* Create feature clients, then: */
if (az_iot_connection_client_open(&client) != AZ_IOT_OK)
{
  return 1;
}
for (;;)
{
  (void)az_iot_connection_client_do_work(&client, 100);
}
```

Start from `az_iot_connection_client_options_default()`. A zero-initialized options struct has
reconnection disabled.

To skip DPS and connect straight to a hub, set `host` and `client_id` instead of the `dps` fields,
and set `connection_profile` to `AZ_IOT_CONNECTION_PROFILE_MQTT_V5` for an mqttv5 hub (the default
is mqttv3).

[`samples/unified/telemetry`](../samples/unified/telemetry/main.c) is a complete example.

## Connection states

Values of `az_iot_connection_state` (`AZ_IOT_CONN_STATE_*`):

| State | Meaning |
| --- | --- |
| `IDLE` | Not connected. `open()` is legal only here. |
| `CONNECTING` | A connect attempt is in progress, including DPS provisioning. |
| `CONNECTED` | Ready. Every required subscription is in place. |
| `RETRY_PENDING` | Waiting out a backoff delay before the next attempt. |
| `DISCONNECTING` | A session is closing: `close()` was called, or the provisioning session ends after registration. |
| `FAULTED` | Stopped after a failure. The SDK does not retry from here. Call `close()` to return to `IDLE`, then `open()` again. |

State is tracked per scope: `AZ_IOT_CONN_SCOPE_DPS` for the provisioning session and
`AZ_IOT_CONN_SCOPE_HUB` for the hub session. Read it with
`az_iot_connection_client_get_state(client, scope)`, or watch it with
`az_iot_connection_client_add_state_observer()`.

Each state event carries `scope`, `state`, `reason` (an `az_iot_result`), `is_retriable`, and
optional `error` detail (source, wire code, service message). The event is valid only during the
callback.

## Provisioning and the hub profile

On `open()`, the client registers with DPS over MQTT 3.1.1, receives the assigned hub, device id
and connection profile, and then connects to the hub. It picks MQTT 3.1.1 or 5 from the profile.
See [Architecture](architecture.md#hub-generations-mqttv3-and-mqttv5).

- `az_iot_connection_client_get_hub_profile()` reports the assigned profile once the hub is
  `CONNECTED`.
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

## Reconnection

`reconnection_policy` controls automatic retries:

| Field | Default | Meaning |
| --- | --- | --- |
| `initial_delay_ms` | 1000 | First delay. `0` disables automatic reconnect. |
| `max_delay_ms` | 60000 | Cap on the exponential backoff. |
| `max_attempts` | 0 | Automatic retries after a failure before `FAULTED`; the count resets on success. `0` retries forever. |
| `jitter_pct` | 20 | Random variation, ± percent. |

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
happens next:

| Field | `options_default()` | Zeroed | Meaning |
| --- | --- | --- | --- |
| `mode` | `RETRY_HUB` | `REPROVISION` | What the next attempt does; see below. |
| `policy` | 5 min initial, 1 h cap, ±25%, no limit | uses `reconnection_policy` | Retry schedule after a refusal. |
| `max_duration_seconds` | 0 | 0 | Stop this long after the first refusal; `0` = no limit. No attempt in the episode (hub connect, DPS registration or poll) starts at or after it, until `HUB:CONNECTED`. Recovery stops as soon as the next attempt could not start in time, so the fault may come before the duration ends. Not kept across restarts. |

| `mode` | Next attempt after a refusal |
| --- | --- |
| `AZ_IOT_IDENTITY_RECOVERY_RETRY_HUB` | The cached hub. No DPS registration, no certificate request. |
| `AZ_IOT_IDENTITY_RECOVERY_REPROVISION` | A DPS registration after a CONNACK refusal (the earlier behaviour); the hub after an mqttv5 DISCONNECT refusal. With `dps.request_operational_certificate`, every registration requests a new certificate. |
| `AZ_IOT_IDENTITY_RECOVERY_NONE` | None: the refusal faults. |

- The schedule is separate from the reconnection ladders, and DPS registrations do not reset it.
  Only `HUB:CONNECTED`, `open()` and `close()` do. So repeated DPS-accept / hub-reject cycles
  still back off and stop at the policy's `max_attempts`.
- When a limit is reached the client goes to `FAULTED` with the refusal as `reason`, on the HUB scope
  and, if a re-registration was pending, on the DPS scope as well.
- With `reconnection_policy` disabled, the first refusal faults.
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
`http_proxy` environment variables. Set `proxy` explicitly to avoid this.

Examples: [`samples/unified/websockets`](../samples/unified/websockets/main.c),
[`samples/unified/proxy`](../samples/unified/proxy/main.c).

## MQTT session options

| Option | Effect |
| --- | --- |
| `session_continuity` | `DEFAULT` / `RESUME` / `CLEAN`. The hub session resumes by default. DPS sessions are always clean. |
| `session_expiry_seconds` | mqttv5 only. How long the broker keeps the session after a disconnect. Default 1 hour. |
| `lwt` | Optional Last Will. The SDK sets none of its own. |
| `keep_alive_seconds`, `connect_timeout_seconds` | Default 30 s each. |

## Certificates

Every connection uses TLS. `certificate_provider` supplies the trusted CA and the X.509 device
credential. `open()` fails with `AZ_IOT_ERR_CREDENTIAL_INCOMPLETE` without one, unless every role
the client uses has a SAS key (see [Authentication](#authentication)); a SAS role then uses
server-authenticated TLS. `dps.request_operational_certificate` always needs a provider.

Operational certificates:

- **Issued at provisioning.** Set `dps.request_operational_certificate` and provide
  `csr_payload_buffer` (`AZ_IOT_CSR_PAYLOAD_BUFFER_MIN` bytes is enough for any CSR the service
  accepts). The device sends a CSR with its registration. The issued chain goes to the provider's
  `store_issued_certificate()`, if it has one, and then to the callback set with
  `az_iot_connection_client_set_operational_cert_callback()`, if set. At least one must be
  present. If storage fails, the callback is skipped and the registration fails.
- **Renewed over the hub** (mqttv3 only). Call `az_iot_connection_client_send_csr()` while
  `CONNECTED`. One renewal can be in flight at a time. The SDK does not store the renewed chain:
  copy or persist it in the callback (it is valid only there), for example through the
  provider, then `close()` and `open()` to use it. See
  [`samples/authentication/hub_renew`](../samples/authentication/hub_renew/main.c).
- A DPS-issued certificate is used from the hub connect that follows the registration. The live
  session is never interrupted.
- At connect time the operational credential is preferred, with the bootstrap credential as a
  fallback.

Keys can stay in hardware (PKCS#11, TPM) with the Paho adapter. See
[`samples/authentication`](../samples/authentication/README.md).

## Authentication

> **Partly implemented.** Implemented: X.509 from `certificate_provider`, then a SAS token signed
> with the primary key; `trusted_ca`; `unix_time`; `token_lifetime_seconds`; `auth_source` in
> state events. Proposed, not implemented yet: fallback to further certificates and to the
> secondary key on rejection, `user_provided_token` (`init()` returns
> `AZ_IOT_ERR_NOT_SUPPORTED`), and planned renewal (`renewal_percent`). Until renewal lands, the
> hub ends the session when the token expires and the client reconnects with a new one.

Each role -- DPS and hub -- is configured with any of these credential sources, tried in this
order, skipping any not set:

| Source | Configure | Notes |
| --- | --- | --- |
| X.509 certificates | `certificate_provider` | As today. The provider may offer more than one per role (index 0, 1, ...). |
| Primary, secondary key | `dps_auth` / `hub_auth`: `sas.primary_key_base64` (+ `secondary_key_base64`, `is_enrollment_group_key`) | The SDK signs tokens with the backend in `crypto`, and needs a Unix time: `time()`, or `unix_time.get_time`. |
| User-provided token | `sas.user_provided_token` | The application supplies tokens; the SDK never sees its key. |

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

- **Fallback.** When the service rejects a credential (`AZ_IOT_ERR_IDENTITY_REJECTED`), the next
  source is tried at once, without a `reconnection_policy` delay. Other failures retry the same
  source under the policy. One pass over all sources counts as one policy attempt; with the
  policy disabled, `open()` still makes one full pass. The source that connects is kept until
  rejected; `open()` starts again at the first. `identity_recovery` applies only after a pass in
  which all of the hub's credentials are rejected. State events report the credential in
  `auth_source` (and `x509_index`).
- **Cost.** Only devices configured with more than one source pay for fallback: one extra
  connect per rejected source, once per credential change (the working source is kept).
- **Memory.** All SAS state -- decoded keys, signing scratch, the token -- lives in
  `sas_buffer`, which the app provides only when it uses SAS keys. Size it with
  `AZ_IOT_SAS_BUFFER_SIZE(distinct keys, token area)`; a key set identically for DPS and the hub
  counts once. The token is wiped once the transport has taken it; the whole buffer at
  `deinit()`.
- **Keys are fixed at `init()`.** They are copied and decoded there; to change them,
  re-initialize the client and its feature clients. Use `user_provided_token` to rotate without
  re-initializing.
- **Token callback.** It must not block. It answers `READY`, `PENDING` (deliver later with
  `az_iot_connection_client_complete_sas_token()`, within `connect_timeout_seconds`), or
  `UNAVAILABLE` with `retry_after_seconds` (0: the reconnection policy decides).
- **Renewal.** Per role, in `sas`: at `renewal_percent` (default 80; 1-99) of
  `token_lifetime_seconds` (key-signed, default one hour) or of the callback's `valid_seconds`.
  Applies to any session held open. MQTT 3.1.1 cannot re-authenticate a live session, so the SDK
  reconnects; those state events carry `is_credential_renewal` and reason `AZ_IOT_OK`.
- **Multiple certificates.** The client loads provider certificates at index 0, 1, ... until
  `AZ_IOT_ERR_NOT_FOUND`, and never beyond `AZ_IOT_MAX_CERTS_PER_ROLE` (default 4).
- **DPS-issued certificate.** With `dps.request_operational_certificate`, the hub tries the issued
  certificate first; DPS can still use SAS. See
  [`dps_sas_key_issued_cert`](../samples/authentication/dps_sas_key_issued_cert/README.md).
- **mqttv5 hubs** do not accept SAS yet; the design allows for it. The token request carries the
  hub generation, and renewal can use MQTT 5 in-session re-authentication instead of a reconnect.
  Until then, a rejection is `AZ_IOT_ERR_IDENTITY_REJECTED`.
- **Server trust.** `trusted_ca` applies to every connection. Without it, the X.509 provider's CA
  is used if there is one, otherwise the adapter's default store.

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
| `AZ_IOT_ERR_MQTT` | Transport, TLS or broker failure. The adapter log has the detail. |
| `AZ_IOT_ERR_TIMEOUT` | A handshake step or CSR operation timed out. |

All results are listed in [`az_iot_result.h`](../inc/azure/iot/az_iot_result.h).
