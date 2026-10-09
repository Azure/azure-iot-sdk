<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Certificate renewal over IoT Hub

Connects to the IoT Hub DPS assigns, requests a renewed operational certificate **from the hub**
with a certificate signing request (CSR), saves the new chain, and reconnects with it. Renewal over
the hub is an **mqttv3** feature.

## Sample features

- Hub generation: renewal needs an mqttv3 hub. On an mqttv5 hub the sample prints `runtime renewal is not available on this hub generation` and exits 1.
- Uses the OpenSSL-backed managed certificate provider for the operational key, the CSR and the files.
- Connects with the operational certificate when the files hold one (for example from a previous [`dps_csr_managed`](../dps_csr_managed/README.md) run with the same files), otherwise with the bootstrap certificate.
- `az_iot_connection_client_send_csr()` is two-phase: the hub answers `202 Accepted`, then `200` with the issued chain.
- The SDK does not store the renewed chain or reconnect: the sample saves it through the provider, then calls `close()` and `open()`, because a new client certificate needs a new TLS handshake.
- Platforms: Linux and Windows.

## Service requirements

The same setup as [`dps_csr_managed`](../dps_csr_managed/README.md#prerequisites): DPS and an
**mqttv3** IoT Hub with certificate management, and an X.509 enrollment group with a credential
policy.

## Configure

The sample reads these environment variables:

| Variable | Required | Meaning |
| --- | --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | yes | DPS ID scope. |
| `AZ_IOT_DPS_REGISTRATION_ID` | yes | Registration ID; must equal the device certificate's common name. |
| `AZ_IOT_CLIENT_CERT` | yes | Bootstrap certificate chain (PEM file, leaf first). |
| `AZ_IOT_CLIENT_KEY` | yes | Bootstrap private key (PEM file). |
| `AZ_IOT_TRUSTED_CA` | yes | CA bundle (PEM file) that validates the DPS and IoT Hub server certificates, e.g. `/etc/ssl/certs/ca-certificates.crt`. |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | Provisioning endpoint. Default `global.azure-devices-provisioning.net`. |
| `AZ_IOT_OPERATIONAL_KEY` | no | Operational private key file; loaded if present, else generated. Default `operational_key.pem`. |
| `AZ_IOT_OPERATIONAL_CERT` | no | Issued operational chain file. Default `operational_cert.pem`. |

```sh
export AZ_IOT_DPS_ID_SCOPE='<id-scope>'
export AZ_IOT_DPS_REGISTRATION_ID='<registration-id>'
export AZ_IOT_CLIENT_CERT="$PWD/device-cert.pem"
export AZ_IOT_CLIENT_KEY="$PWD/device-key.pem"
export AZ_IOT_TRUSTED_CA='/etc/ssl/certs/ca-certificates.crt'
```

PowerShell:

```powershell
$env:AZ_IOT_DPS_ID_SCOPE        = '<id-scope>'
$env:AZ_IOT_DPS_REGISTRATION_ID = '<registration-id>'
$env:AZ_IOT_CLIENT_CERT         = "$PWD\device-cert.pem"
$env:AZ_IOT_CLIENT_KEY          = "$PWD\device-key.pem"
$env:AZ_IOT_TRUSTED_CA          = "$PWD\ca.pem"
```

## Build and run

Build prerequisites (toolchain, CMake, OpenSSL) are listed in the
[samples overview](../../README.md#prerequisites). Run from the repository's `c/` directory.

Linux:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_auth_hub_renew
./build/linux-gcc-debug/samples/authentication/az_iot_sample_auth_hub_renew
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_auth_hub_renew
.\build\windows-msvc-debug\samples\authentication\Debug\az_iot_sample_auth_hub_renew.exe
```

The target exists only when OpenSSL 3.0+ is found and the Paho adapter is enabled
(`AZ_IOT_WITH_CERT_PROVIDER_MANAGED`, `AZ_IOT_WITH_PAHO`); configure then prints
`building managed certificate provider`.

## How it ends

Exit code 0 when the renewed certificate was issued and the client reconnected with it; otherwise 1.

## Expected output

```
[hub_renew] hub accepted CSR; signing in progress
[hub_renew] renewed chain issued: 2 cert(s)
[hub_renew] renewal complete; reconnecting to apply the new certificate
[hub_renew] reconnected with the renewed operational certificate
```

A refused renewal prints `[hub_renew] CSR failed: status=<code> service_code=<code> retry_after=<n>s`.
The chain length depends on the credential policy.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `Required env var <NAME> not set.`, exit 1 | A required variable is missing. |
| SDK line `dps register: errorCode=<code> errorMessage=<text>` | DPS refused the registration: no matching enrollment, the certificate's common name differs from `AZ_IOT_DPS_REGISTRATION_ID`, or the enrollment is disabled. A refused request is retried until the sample's run time ends; a `failed` or `disabled` registration faults at once with `AZ_IOT_ERR_DPS_REGISTRATION_FAILED`. |
| Repeated `paho: connect failed: ... TCP/TLS connect failure` | DPS or the hub is unreachable (network, proxy, `AZ_IOT_DPS_GLOBAL_ENDPOINT`), or server certificate validation fails (`AZ_IOT_TRUSTED_CA`). |
| `Unsupported hub generation "<value>". Upgrade the SDK.` | DPS assigned a connection profile this SDK version does not know. |
| `CSR failed: ... service_code=409005` | Another renewal is active on the hub; retry later, or supersede it with `replace`. |
| `CSR failed: ... service_code=400040` | The hub could not decode or verify the CSR. |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| Provider setup | `az_iot_certificate_provider_managed_init()` |
| Request the renewal | `az_iot_connection_client_send_csr()` |
| Two-phase result; save the chain | `on_csr_event()` → `store_issued_certificate()` |
| Apply the new certificate | `az_iot_connection_client_close()` then `az_iot_connection_client_open()` |

Design background: [certificate-management.md](../../../docs/eng/certificate-management.md#hub-side-renewal-mqttv3).
