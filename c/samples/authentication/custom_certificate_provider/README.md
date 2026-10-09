<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# DPS-issued certificate, CSR built by the application

The same flow as [`dps_csr_managed`](../dps_csr_managed/README.md): authenticate to DPS with the
bootstrap certificate, send a CSR with the registration, save the issued operational chain, and
connect to the assigned hub with it. The difference is who owns the crypto: here the certificate
provider is sample application code in [`samples/common`](../../common/sample_cert_provider.c),
showing how an application implements the provider interface. It is sample code, not a production
component.

## Sample features

- Hub generations: mqttv3 and mqttv5, whichever DPS assigns.
- `sample_cert_provider` implements the full `az_iot_certificate_provider` interface and builds a real PKCS#10 CSR with platform crypto: OpenSSL 3.0+ on Linux (`sample_csr_openssl.c`), CNG on Windows (`sample_csr_cng.c`).
- Opts in to issuance with `dps.request_operational_certificate`.
- Platforms: Linux and Windows.

## Service requirements

The same setup as [`dps_csr_managed`](../dps_csr_managed/README.md#prerequisites): DPS with
certificate management, a linked IoT Hub, and an X.509 enrollment group with a credential policy.
Without a credential policy DPS issues no certificate and the sample connects with the bootstrap
identity.

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
cmake --build --preset linux-gcc-debug --target az_iot_sample_auth_custom_certificate_provider
./build/linux-gcc-debug/samples/authentication/az_iot_sample_auth_custom_certificate_provider
```

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_auth_custom_certificate_provider
.\build\windows-msvc-debug\samples\authentication\Debug\az_iot_sample_auth_custom_certificate_provider.exe
```

The target exists when the Paho adapter is enabled and, outside Windows, OpenSSL 3.0+ is found.

## How it ends

Exit code 0 when the hub connection was reached; the last line says with which identity. Otherwise 1.

## Expected output

```
[custom_cert] operational certificate issued: 2 cert(s) in chain
[custom_cert] connected with operational identity
```

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `Required env var <NAME> not set.`, exit 1 | A required variable is missing. |
| SDK line `dps register: errorCode=<code> errorMessage=<text>` | DPS refused the registration: no matching enrollment, the certificate's common name differs from `AZ_IOT_DPS_REGISTRATION_ID`, or the enrollment is disabled. DPS verdicts are retried until the sample's run time ends. |
| Repeated `paho: connect failed: ... TCP/TLS connect failure` | DPS or the hub is unreachable (network, proxy, `AZ_IOT_DPS_GLOBAL_ENDPOINT`), or server certificate validation fails (`AZ_IOT_TRUSTED_CA`). |
| `Unsupported hub generation "<value>". Upgrade the SDK.` | DPS assigned a connection profile this SDK version does not know. |
| `connected with bootstrap identity` | DPS issued no certificate: the enrollment group has no credential policy. |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| The provider | [`sample_cert_provider.c`](../../common/sample_cert_provider.c), `sample_cert_provider_init()` |
| Platform CSR code | [`sample_csr_openssl.c`](../../common/sample_csr_openssl.c), [`sample_csr_cng.c`](../../common/sample_csr_cng.c) |
| Opt in to issuance | `copts.dps.request_operational_certificate = true` and `copts.csr_payload_buffer` |
| Issuance notification | `az_iot_connection_client_set_operational_cert_callback()`, `on_operational_cert()` |

Design background: [certificate-management.md](../../../docs/eng/certificate-management.md).
