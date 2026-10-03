<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# SAS tokens from the application

> **Proposed API, not implemented.** This sample is not built.

The SDK asks the application for a SAS token before each connect that needs one, and again at
`sas_renewal_percent` of its validity. The SDK never sees a key. See [`main.c`](main.c).

## Sample features

- `dps_auth` and `hub_auth` are both `AZ_IOT_AUTH_SAS_TOKEN` with only `user_provided_token` set.
  Keys can be set too; the callback is then the last fallback.
- The callback receives the role, hub generation, resource URI and key name the token must carry.
  It must not block: it answers `READY` (token in the buffer), `PENDING`, or `UNAVAILABLE` with a
  `retry_after_seconds`.
- This sample answers `PENDING` and delivers the token with
  `az_iot_connection_client_complete_sas_token()` from the `do_work()` thread. A pending request
  times out after `connect_timeout_seconds`; the attempt is then retried.
- `sign_token()` signs with OpenSSL so the sample runs; replace it with a TPM, HSM or secure
  element, or a call to a token service.
- No crypto backend and no clock are needed in the SDK.

## Service requirements

A DPS symmetric-key individual enrollment linked to an IoT Hub.

## Environment variables

| Variable | Meaning |
| --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | DPS ID scope |
| `AZ_IOT_DPS_REGISTRATION_ID` | Registration ID; also the device ID |
| `AZ_IOT_DPS_SYMMETRIC_KEY` | Enrollment key (base64), held by the stand-in key store |
| `AZ_IOT_TRUSTED_CA` | Trusted CA PEM path (optional) |
