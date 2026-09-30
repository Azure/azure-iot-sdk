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

`az_iot_reconnection_policy_get_default()`, `_get_retry_disabled()` and `_get_fixed_interval()`
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
  accepts). The device sends a CSR with its registration. The issued chain is passed to the
  provider and to the callback set with `az_iot_connection_client_set_operational_cert_callback()`.
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
