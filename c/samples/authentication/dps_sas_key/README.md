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

## Environment variables

| Variable | Meaning |
| --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | DPS ID scope |
| `AZ_IOT_DPS_REGISTRATION_ID` | Registration ID; also the device ID |
| `AZ_IOT_DPS_SYMMETRIC_KEY` | Individual enrollment key (base64). Set this or the next one. |
| `AZ_IOT_DPS_ENROLLMENT_GROUP_KEY` | Enrollment-group key (base64) |
| `AZ_IOT_DPS_SECONDARY_KEY` | Secondary key of the same enrollment (base64, optional) |
| `AZ_IOT_SAS_TOKEN_RENEWAL_PERCENT` | Renewal point, 1-99 (optional; default 80) |
| `AZ_IOT_TRUSTED_CA` | Trusted CA PEM path (optional; default trust store otherwise) |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | Provisioning endpoint (optional) |
