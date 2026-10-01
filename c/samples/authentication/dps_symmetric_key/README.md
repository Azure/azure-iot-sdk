<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# SAS to DPS and to the hub

> **Proposed API, not implemented.** This sample is not built.

Registers with DPS with a SAS token, then connects to the assigned hub with a SAS token signed with
the same key, and sends one telemetry message on whichever hub generation DPS assigned.

## Sample features

- `az_iot_sas_signer_symmetric_key` is set as both `sas.onboarding` and `sas.operational`. A PEM provider with only a CA supplies server trust.
- The SDK builds the tokens and renews the hub token before `sas.token_lifetime_seconds` runs out.
- An mqttv5 hub that refuses SAS fails the connect with `AZ_IOT_ERR_IDENTITY_REJECTED`.

## Service requirements

A DPS symmetric-key enrollment (individual or group) linked to an IoT Hub.

## Environment variables

| Variable | Meaning |
| --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | DPS ID scope |
| `AZ_IOT_DPS_REGISTRATION_ID` | Registration ID; also the device ID |
| `AZ_IOT_DPS_SYMMETRIC_KEY` | Individual enrollment key (base64). Set this or the next one. |
| `AZ_IOT_DPS_ENROLLMENT_GROUP_KEY` | Enrollment-group key (base64); the device key is derived from it and the registration ID |
| `AZ_IOT_TRUSTED_CA` | Trusted CA PEM path (optional) |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | Provisioning endpoint (optional) |
