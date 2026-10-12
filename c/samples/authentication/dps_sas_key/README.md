<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# SAS keys to DPS and to the hub

Registers with DPS and connects to the assigned hub with SAS tokens the SDK signs from symmetric
keys, then sends one telemetry message on whichever hub generation DPS assigned. See
[`main.c`](main.c).

## Sample features

- `dps_auth` and `hub_auth` carry the same keys: the hub identity DPS creates has them. No
  `certificate_provider`, so SAS is the only source.
- Primary key, and optionally a secondary key, tried when the primary is rejected. The state
  callback prints which credential connected.
- Enrollment-group keys are accepted; the SDK derives the device keys from them.
- `crypto = az_iot_crypto_openssl()` provides HMAC-SHA256. No `certificate_provider`.
- Tokens last `sas.token_lifetime_seconds` (default one hour). At `sas.renewal_percent` of it
  (default 80) the client reconnects to the hub with a new token; the state callback prints
  `(token renewal)` for those events.
- A hub that does not accept SAS (the mqttv5 hub, until it does) fails the connect with
  `AZ_IOT_ERR_IDENTITY_REJECTED`.

## Service requirements

A DPS symmetric-key enrollment (individual or group) linked to an IoT Hub.

## Configure

The sample reads these environment variables:

| Variable | Required | Meaning |
| --- | --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | yes | DPS ID scope. |
| `AZ_IOT_DPS_REGISTRATION_ID` | yes | Registration ID; also the device ID. |
| `AZ_IOT_DPS_SYMMETRIC_KEY` | this or the next | Individual enrollment key (base64). |
| `AZ_IOT_DPS_ENROLLMENT_GROUP_KEY` | this or the previous | Enrollment-group key (base64). |
| `AZ_IOT_DPS_SECONDARY_KEY` | no | Secondary key of the same enrollment (base64). |
| `AZ_IOT_SAS_TOKEN_RENEWAL_PERCENT` | no | Renewal point, 1-99. Default 80. |
| `AZ_IOT_TRUSTED_CA` | no | CA bundle (PEM file) that validates the DPS and IoT Hub server certificates. Default: the system trust store. |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | Provisioning endpoint. Default `global.azure-devices-provisioning.net`. |

```sh
export AZ_IOT_DPS_ID_SCOPE='<id-scope>'
export AZ_IOT_DPS_REGISTRATION_ID='<registration-id>'
export AZ_IOT_DPS_SYMMETRIC_KEY='<primary-key>'
```

PowerShell:

```powershell
$env:AZ_IOT_DPS_ID_SCOPE        = '<id-scope>'
$env:AZ_IOT_DPS_REGISTRATION_ID = '<registration-id>'
$env:AZ_IOT_DPS_SYMMETRIC_KEY   = '<primary-key>'
```

## Build and run

Build prerequisites (toolchain, CMake, OpenSSL) are listed in the
[samples overview](../../README.md#prerequisites). Run from the repository's `c/` directory.

Linux:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_auth_dps_sas_key
./build/linux-gcc-debug/samples/authentication/az_iot_sample_auth_dps_sas_key
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_auth_dps_sas_key
.\build\windows-msvc-debug\samples\authentication\Debug\az_iot_sample_auth_dps_sas_key.exe
```

The target exists when the Paho adapter is enabled and the OpenSSL crypto backend is built
(`AZ_IOT_WITH_PAHO`, `AZ_IOT_WITH_CRYPTO_OPENSSL`, OpenSSL 3.0+).

## How it ends

Exit code 0 when the telemetry message was sent. Otherwise 1: a configuration error, a failure
retrying cannot fix, no hub connection within 60 s, or no send completion within 30 s.

## Expected output

SDK log lines and earlier state lines omitted:

```
[dps_sas_key] hub: Connected (AZ_IOT_OK), credential: primary key
[dps_sas_key] telemetry: AZ_IOT_OK
```

The credential reads `secondary key` when the primary was rejected. State lines of a renewal
reconnect end with `(token renewal)`.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `[dps_sas_key] set AZ_IOT_DPS_ID_SCOPE, ...`, exit 1 | A required variable is missing, or both or neither of the two keys are set. |
| `AZ_IOT_SAS_TOKEN_RENEWAL_PERCENT must be 1-99`, exit 1 | The renewal point is not a whole number from 1 to 99. |
| `not connected: AZ_IOT_ERR_IDENTITY_REJECTED` | DPS or the hub refused the token: wrong key, or wrong key kind (individual vs. group), no matching enrollment, a wrong system clock, or an mqttv5 hub, which does not accept SAS yet. |
| SDK line `dps register: errorCode=<code> errorMessage=<text>`, then `not connected: timeout` | DPS refused the registration. DPS verdicts are retried until the sample's run time ends. |
| SDK warning `no Unix time yet; cannot sign a SAS token`, then `not connected: timeout` | The system clock is not set, so no token can be signed. |
| Repeated `paho: connect failed: ...`, then `not connected: timeout` | DPS or the hub is unreachable (network, proxy, `AZ_IOT_DPS_GLOBAL_ENDPOINT`), or server certificate validation fails (`AZ_IOT_TRUSTED_CA`). |
| `not connected: AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED` | DPS assigned a connection profile this SDK version does not know. |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| Keys for both roles | `sas.sas.primary_key_base64`, `secondary_key_base64`, `is_enrollment_group_key`; `opts.dps_auth` and `opts.hub_auth` |
| Renewal point | `sas.sas.renewal_percent` |
| HMAC backend and SAS memory | `opts.crypto = az_iot_crypto_openssl()`, `opts.sas_buffer` |
| Which credential connected | `on_conn_state()`: `event->auth_source`, `event->is_credential_renewal` |
| Send on the assigned generation | `send_telemetry()` |

Design background: [connecting.md](../../../docs/connecting.md#authentication).
