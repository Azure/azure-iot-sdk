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

## Configure

The sample reads these environment variables:

| Variable | Required | Meaning |
| --- | --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | yes | DPS ID scope. |
| `AZ_IOT_DPS_REGISTRATION_ID` | yes | Registration ID; also the device ID. |
| `AZ_IOT_DPS_SYMMETRIC_KEY` | yes | Enrollment key (base64, at most 64 bytes decoded), held by the stand-in key store. |
| `AZ_IOT_TRUSTED_CA` | no | CA bundle (PEM file) that validates the DPS and IoT Hub server certificates. Default: the system trust store. |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | Provisioning endpoint. Default `global.azure-devices-provisioning.net`. |

```sh
export AZ_IOT_DPS_ID_SCOPE='<id-scope>'
export AZ_IOT_DPS_REGISTRATION_ID='<registration-id>'
export AZ_IOT_DPS_SYMMETRIC_KEY='<device-key>'
```

PowerShell:

```powershell
$env:AZ_IOT_DPS_ID_SCOPE        = '<id-scope>'
$env:AZ_IOT_DPS_REGISTRATION_ID = '<registration-id>'
$env:AZ_IOT_DPS_SYMMETRIC_KEY   = '<device-key>'
```

## Build and run

Build prerequisites (toolchain, CMake, OpenSSL) are listed in the
[samples overview](../../README.md#prerequisites). Run from the repository's `c/` directory.

Linux:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_auth_user_provided_sas_token
./build/linux-gcc-debug/samples/authentication/az_iot_sample_auth_user_provided_sas_token
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_auth_user_provided_sas_token
.\build\windows-msvc-debug\samples\authentication\Debug\az_iot_sample_auth_user_provided_sas_token.exe
```

The target exists when the Paho adapter is enabled and OpenSSL is found (`AZ_IOT_WITH_PAHO`).

## How it ends

Exit code 0 when the hub connection was reached; the sample sends no telemetry. Otherwise 1: a
configuration error, a failure retrying cannot fix, or no hub connection within 60 s.

## Expected output

SDK log lines and state lines omitted:

```
[user_provided_sas_token] DPS token: AZ_IOT_OK
[user_provided_sas_token] hub token: AZ_IOT_OK
[user_provided_sas_token] connected to the hub with an application-supplied token
```

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `[user_provided_sas_token] set AZ_IOT_DPS_ID_SCOPE, ...`, exit 1 | A required variable is missing, or the key is not valid base64 or is longer than 64 bytes. |
| `[user_provided_sas_token] AZ_IOT_ERR_IDENTITY_REJECTED` | DPS or the hub refused the token: wrong key, a group key instead of the derived device key, no matching enrollment, a wrong system clock, or an mqttv5 hub, which does not accept SAS yet. |
| `[user_provided_sas_token] timeout` with no `DPS token:` line | The system clock is not set (`time()` fails or returns 0); token requests wait for it. |
| SDK line `dps register: errorCode=<code> errorMessage=<text>`, then `timeout` | DPS refused the registration. DPS verdicts are retried until the sample's run time ends. |
| Repeated `paho: connect failed: ...`, then `timeout` | DPS or the hub is unreachable (network, proxy, `AZ_IOT_DPS_GLOBAL_ENDPOINT`), or server certificate validation fails (`AZ_IOT_TRUSTED_CA`). |
| `resource URI too long for this sample` | The request does not fit the sample's buffers; the attempt times out and is retried. |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| Token callback, no keys in the SDK | `tokens.sas.on_sas_token_required = request_token`; `opts.dps_auth` and `opts.hub_auth` |
| Record the request | `request_token()` |
| Sign and supply the token | `issue_pending_tokens()` → `sign_token()` → `az_iot_connection_client_update_sas_token()` |
| Replace with your key store or token service | `key_store_hmac()`, `sign_token()` |

Design background: [connecting.md](../../../docs/connecting.md#authentication).
