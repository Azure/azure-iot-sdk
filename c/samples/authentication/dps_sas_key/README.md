<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# SAS key to DPS and to the hub

> **Proposed API, not implemented.** This sample is not built.

Registers with DPS and connects to the assigned hub with SAS tokens the SDK signs from a symmetric
key, then sends one telemetry message on whichever hub generation DPS assigned. See
[`main.c`](main.c).

## Sample features

- `dps_auth` and `hub_auth` are both `AZ_IOT_AUTH_SAS_KEY` with the same key: the hub identity
  DPS creates has that key.
- An enrollment-group key is accepted; the SDK derives the device key from it.
- `crypto = az_iot_crypto_openssl()` provides HMAC-SHA256. No `certificate_provider`.
- The hub token is renewed before it expires (default lifetime one hour).
- An mqttv5 hub refuses SAS today: the connect fails with `AZ_IOT_ERR_IDENTITY_REJECTED`.

## Service requirements

A DPS symmetric-key enrollment (individual or group) linked to an IoT Hub.

## Environment variables

| Variable | Meaning |
| --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | DPS ID scope |
| `AZ_IOT_DPS_REGISTRATION_ID` | Registration ID; also the device ID |
| `AZ_IOT_DPS_SYMMETRIC_KEY` | Individual enrollment key (base64). Set this or the next one. |
| `AZ_IOT_DPS_ENROLLMENT_GROUP_KEY` | Enrollment-group key (base64) |
| `AZ_IOT_TRUSTED_CA` | Trusted CA PEM path (optional; default trust store otherwise) |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | Provisioning endpoint (optional) |
