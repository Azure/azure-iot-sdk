<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# SAS to DPS, DPS-issued certificate to the hub

> **Proposed API, not implemented.** This sample is not built.

Registers with DPS with a SAS token and a CSR, then connects to the assigned hub with the
certificate DPS issued, and sends one telemetry message on whichever hub generation DPS assigned.

## Sample features

- `az_iot_sas_signer_symmetric_key` is set as `sas.onboarding` only; the hub uses the issued certificate from `certificate_provider`.
- The managed provider runs with no bootstrap certificate.
- Renewal: `az_iot_connection_client_send_csr()` on an mqttv3 hub, or re-provisioning with the symmetric key.

## Service requirements

A DPS symmetric-key enrollment with certificate issuance (certificate management, preview).

## Environment variables

| Variable | Meaning |
| --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | DPS ID scope |
| `AZ_IOT_DPS_REGISTRATION_ID` | Registration ID; also the device ID |
| `AZ_IOT_DPS_SYMMETRIC_KEY` | Individual enrollment key (base64). Set this or the next one. |
| `AZ_IOT_DPS_ENROLLMENT_GROUP_KEY` | Enrollment-group key (base64); the device key is derived from it and the registration ID |
| `AZ_IOT_TRUSTED_CA` | Trusted CA PEM path (optional) |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | Provisioning endpoint (optional) |
| `AZ_IOT_OPERATIONAL_KEY` | Operational key PEM (default `operational_key.pem`) |
| `AZ_IOT_OPERATIONAL_CERT` | Issued chain PEM (default `operational_cert.pem`) |
