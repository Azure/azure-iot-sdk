<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# SAS keys to DPS and to the hub

> **Proposed API, not implemented.** This sample is not built.

Registers with DPS and connects to the assigned hub with SAS tokens the SDK signs from symmetric
keys, then sends one telemetry message on whichever hub generation DPS assigned. See
[`main.c`](main.c).

## Sample features

- `dps_auth` and `hub_auth` are both `AZ_IOT_AUTH_SAS_TOKEN` with the same keys: the hub identity
  DPS creates has them.
- Primary key, and optionally a secondary key; the secondary is used when the service rejects the
  primary. The state callback prints which key connected.
- Enrollment-group keys are accepted; the SDK derives the device keys from them.
- `crypto = az_iot_crypto_openssl()` provides HMAC-SHA256. No `certificate_provider`.
- The hub token (default lifetime one hour) is renewed at `sas_renewal_percent` of it (default
  80%) by a reconnect, reported with `is_credential_renewal` set.
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
| `AZ_IOT_SAS_RENEWAL_PERCENT` | Renewal point, 1-99 (optional; default 80) |
| `AZ_IOT_TRUSTED_CA` | Trusted CA PEM path (optional; default trust store otherwise) |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | Provisioning endpoint (optional) |
