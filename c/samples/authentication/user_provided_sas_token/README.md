<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# SAS tokens from the application

The SDK notifies the application when a SAS token is needed: before each connect that needs one,
and at `sas.renewal_percent` of its lifetime. The SDK never sees a key. See [`main.c`](main.c).

## Sample features

- `dps_auth` and `hub_auth` set only `on_sas_token_required`. Certificates and keys can be set
  too; the callback is then the last fallback.
- The callback receives the role, hub generation, resource URI and key name the token must carry,
  and whether it renews a connected session. It must not block.
- This sample records the request in the callback and supplies the token with
  `az_iot_connection_client_update_sas_token()` from the `do_work()` thread. A connect attempt
  waits at most `connect_timeout_seconds`; it is then retried, and the callback notified again.
- The SDK formats the token ([`az_iot_sas_token.h`](../../../inc/azure/iot/az_iot_sas_token.h)):
  `az_iot_sas_token_string_to_sign()` gives what to sign, `az_iot_sas_token_from_signature()`
  builds the token from its HMAC-SHA256. Only the HMAC needs the key: `key_store_hmac()` uses
  OpenSSL so the sample runs; replace it with a TPM, HSM or secure element, or `sign_token()`
  with a call to a token service.
- No crypto backend and no clock are needed in the SDK.

## Service requirements

A DPS symmetric-key enrollment linked to an IoT Hub. For an enrollment group, set the device key
derived from the group key (`az_iot_sas_derive_device_key()`).

## Environment variables

| Variable | Meaning |
| --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | DPS ID scope |
| `AZ_IOT_DPS_REGISTRATION_ID` | Registration ID; also the device ID |
| `AZ_IOT_DPS_SYMMETRIC_KEY` | Enrollment key (base64), held by the stand-in key store |
| `AZ_IOT_TRUSTED_CA` | Trusted CA PEM path (optional) |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | DPS global endpoint (optional) |
