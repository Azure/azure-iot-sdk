<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# SAS key to DPS, DPS-issued certificate to the hub

Registers with DPS with a SAS token and a CSR, then connects to the assigned hub, on either
generation, with the X.509 certificate DPS issued. See [`main.c`](main.c).

## Sample features

- `dps_auth` has a key; the provider has no bootstrap certificate, so DPS uses SAS. `hub_auth` is
  zeroed, so the hub uses the issued certificate only.
- The managed provider owns the operational key, builds the CSR and stores the issued chain. It
  has no bootstrap certificate.
- Renewal: `az_iot_connection_client_send_csr()` on an mqttv3 hub, or re-provisioning with the
  key on either generation.

## Service requirements

A DPS symmetric-key enrollment with certificate issuance (certificate management, preview).

## Configure

The sample reads these environment variables:

| Variable | Required | Meaning |
| --- | --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | yes | DPS ID scope. |
| `AZ_IOT_DPS_REGISTRATION_ID` | yes | Registration ID; also the device ID. |
| `AZ_IOT_DPS_SYMMETRIC_KEY` | this or the next | Individual enrollment key (base64). |
| `AZ_IOT_DPS_ENROLLMENT_GROUP_KEY` | this or the previous | Enrollment-group key (base64). |
| `AZ_IOT_TRUSTED_CA` | no | CA bundle (PEM file) that validates the DPS and IoT Hub server certificates. Default: the system trust store. |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | Provisioning endpoint. Default `global.azure-devices-provisioning.net`. |
| `AZ_IOT_OPERATIONAL_KEY` | no | Operational private key file; loaded if present, else generated (EC P-256). Default `operational_key.pem`. |
| `AZ_IOT_OPERATIONAL_CERT` | no | Issued operational chain file. Default `operational_cert.pem`. |

The operational key is written **unencrypted**, with permissions from the process umask when
the file is created (world-readable under the common `022`). Set `umask 077` before the first
run; an existing file keeps its mode, so restrict it explicitly, e.g. `chmod 600 operational_key.pem`.

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
cmake --build --preset linux-gcc-debug --target az_iot_sample_auth_dps_sas_key_issued_cert
./build/linux-gcc-debug/samples/authentication/az_iot_sample_auth_dps_sas_key_issued_cert
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_auth_dps_sas_key_issued_cert
.\build\windows-msvc-debug\samples\authentication\Debug\az_iot_sample_auth_dps_sas_key_issued_cert.exe
```

The target exists when the Paho adapter is enabled and the OpenSSL crypto backend and the managed
certificate provider are built (`AZ_IOT_WITH_PAHO`, `AZ_IOT_WITH_CRYPTO_OPENSSL`,
`AZ_IOT_WITH_CERT_PROVIDER_MANAGED`, OpenSSL 3.0+).

## How it ends

Exit code 0 when the telemetry message was sent over the hub connection. Otherwise 1: a
configuration or provider error, a failure retrying cannot fix, no hub connection within 60 s, or
no send completion within 30 s.

## Expected output

SDK log lines and state lines omitted:

```
[dps_sas_key_issued_cert] certificate issued (<n> in chain)
[dps_sas_key_issued_cert] connected with the newly issued certificate
[dps_sas_key_issued_cert] telemetry: AZ_IOT_OK
```

The chain length depends on the credential policy. `connected with the persisted certificate`
means the hub accepted the chain saved by a previous run.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `[dps_sas_key_issued_cert] set AZ_IOT_DPS_ID_SCOPE, ...`, exit 1 | A required variable is missing, or both or neither of the two keys are set. |
| `managed provider init failed` | The operational key could not be loaded, generated or written at `AZ_IOT_OPERATIONAL_KEY`. |
| `not connected: AZ_IOT_ERR_IDENTITY_REJECTED` before any `certificate issued` | DPS refused the token: wrong key or key kind (individual vs. group), no matching enrollment, or a wrong system clock. |
| `not connected: AZ_IOT_ERR_IDENTITY_REJECTED` or `AZ_IOT_ERR_AUTH` after `certificate issued` | The hub refused the issued certificate, e.g. the policy CA is not synced to the hub. |
| `not connected: AZ_IOT_ERR_NOT_FOUND` | DPS issued no certificate, so the hub has no credential: the enrollment has no credential policy. |
| SDK line `dps register: errorCode=<code> errorMessage=<text>`, then `not connected: timeout` | DPS refused the registration. DPS verdicts are retried until the sample's run time ends. |
| SDK warning `no Unix time yet; cannot sign a SAS token`, then `not connected: timeout` | The system clock is not set, so no DPS token can be signed. |
| Repeated `paho: connect failed: ...`, then `not connected: timeout` | DPS or the hub is unreachable (network, proxy, `AZ_IOT_DPS_GLOBAL_ENDPOINT`), or server certificate validation fails (`AZ_IOT_TRUSTED_CA`). |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| SAS to DPS only | `opts.dps_auth.sas`; `opts.hub_auth` left zeroed |
| Provider without a bootstrap certificate | `az_iot_certificate_provider_managed_init()` with only the operational paths |
| Opt in to issuance | `opts.dps.request_operational_certificate = true` and `opts.csr_payload_buffer` |
| Issuance notification | `az_iot_connection_client_set_operational_cert_callback()`, `on_operational_cert()` |
| Send on the assigned generation | `send_telemetry()` |

Design background: [certificate-management.md](../../../docs/eng/certificate-management.md).
