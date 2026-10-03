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
| `RECONNECTING` | Waiting out a backoff delay before the next attempt. |
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

- the hub rejects the device identity,
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

Every connection uses TLS with X.509 client authentication. `certificate_provider` supplies the
trusted CA and the device credential; `open()` fails with `AZ_IOT_ERR_CREDENTIAL_INCOMPLETE`
without one.

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

## Authentication (proposed)

> **Proposed, not implemented.** Today every connection authenticates with X.509.

Each role -- DPS (`dps_auth`) and hub (`hub_auth`) -- picks one kind, independently:

| Kind | Configure | Notes |
| --- | --- | --- |
| `AZ_IOT_AUTH_X509` (default) | `certificate_provider` | As today. A zeroed `az_iot_auth` is X.509. |
| `AZ_IOT_AUTH_SAS_TOKEN` | Any of `sas.primary_key_base64` (+ `secondary_key_base64`, `is_enrollment_group_key`) and `sas.user_provided_token` | With keys, the SDK signs tokens with the backend in `crypto`, and needs a Unix time: `time()`, or `unix_time.get_time`. The callback supplies tokens; the SDK never sees its key. |

```c
copts.dps_auth.kind = AZ_IOT_AUTH_SAS_TOKEN;
copts.dps_auth.sas.primary_key_base64 = primary;
copts.dps_auth.sas.secondary_key_base64 = secondary;  /* optional */
copts.hub_auth = copts.dps_auth;          /* or X.509: leave hub_auth zeroed */
copts.crypto = az_iot_crypto_openssl();   /* HMAC-SHA256 for the tokens */
copts.trusted_ca.path = "ca.pem";         /* server trust, any kind */
```

- **Fallback.** Primary key, then secondary key, then `user_provided_token`, skipping any not set.
  The SDK moves on only when the service rejects a credential (`AZ_IOT_ERR_IDENTITY_REJECTED`);
  other failures retry the same one. The one that connects is kept until rejected; `open()`
  starts again at the primary. The hub re-provisions only after all its credentials are rejected.
  State events report the credential in `sas_source`.
- **Keys are fixed at `init()`.** They are copied and decoded there; to change them,
  re-initialize the client and its feature clients. Use `user_provided_token` to rotate without
  re-initializing.
- **Token callback.** It must not block. It answers `READY`, `PENDING` (deliver later with
  `az_iot_connection_client_complete_sas_token()`, within `connect_timeout_seconds`), or
  `UNAVAILABLE` with `retry_after_seconds` (0: the reconnection policy decides).
- **Renewal.** At `sas_renewal_percent` of a hub token's validity (default 80; 1-99), for keys
  (`sas_token_lifetime_seconds`, default one hour) and callback tokens alike. MQTT 3.1.1 cannot
  re-authenticate a live session, so the SDK reconnects the hub; those state events carry
  `is_credential_renewal` and reason `AZ_IOT_OK`. DPS tokens are made per attempt.
- **DPS-issued certificate.** With `dps.request_operational_certificate`, `hub_auth` must stay
  X.509; DPS can still use SAS. See
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
| `AZ_IOT_ERR_IDENTITY_REJECTED` | The broker refused the device identity. The next attempt re-provisions. |
| `AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED` | DPS assigned a profile this SDK does not support. The next `open()` re-provisions. |
| `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH` | Feature clients do not match the assigned generation. Rebuild them, then `close()` and `open()`; the next `open()` re-provisions. |
| `AZ_IOT_ERR_SUBSCRIPTION_REFUSED` | The hub refused a subscription the session needs. Terminal, since a retry would be refused again. |
| `AZ_IOT_ERR_CREDENTIAL_INCOMPLETE` | The certificate provider returned unusable material, such as a certificate without a key. |
| `AZ_IOT_ERR_MQTT` | Transport, TLS or broker failure. The adapter log has the detail. |
| `AZ_IOT_ERR_TIMEOUT` | A handshake step or CSR operation timed out. |

All results are listed in [`az_iot_result.h`](../inc/azure/iot/az_iot_result.h).
