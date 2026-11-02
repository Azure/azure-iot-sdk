<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# DPS TPM attestation

Registers with DPS using TPM attestation, then sends telemetry to the assigned mqttv3 hub with SAS
tokens the TPM signs. See [`main.c`](main.c).

The TPM is simulated in software ([`sample_tpm_sim.c`](../../common/sample_tpm_sim.c), OpenSSL),
so the sample runs without a TPM. The simulator keeps every key in process memory and in plain
PEM files: use it only to try the flow. With a real TPM, implement `az_iot_dps_tpm_ops` with your
TPM software stack instead.

## Sample features

- DPS takes TPM attestation over HTTPS or AMQP, not MQTT. This sample uses HTTPS: it registers with
  [`az_iot_dps_tpm_client.h`](../../../inc/azure/iot/az_iot_dps_tpm_client.h) over libcurl, in
  `register_with_dps()`:
  1. A register request with the endorsement key (EK) and storage root key (SRK).
  2. DPS answers 401 with a key encrypted to the TPM. The simulated TPM imports it, as
     TPM2_ActivateCredential and TPM2_Import would.
  3. The same register request, with a SAS token the TPM signs (TPM2_HMAC).
  4. Polling until DPS assigns the device. The assignment carries the hub key, which the TPM
     imports in the same way.
- The sample then connects `az_iot_connection_client` directly to the assigned hub. Hub SAS tokens
  come from `on_sas_token_required`, signed by the TPM in `sign_token_with_tpm()` as in
  [`user_provided_sas_token`](../user_provided_sas_token/README.md).
- mqttv3 hubs only: mqttv5 hubs do not accept SAS.

## Service requirements

A DPS individual enrollment with TPM attestation for the simulated TPM's EK, linked to an mqttv3
IoT Hub. Run the sample with `--enrollment` to print the registration ID and EK, and an
`az iot dps enrollment create` command that creates the enrollment.

## Configure

The sample reads these environment variables:

| Variable | Required | Meaning |
| --- | --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | yes, except with `--enrollment` | DPS ID scope. |
| `AZ_IOT_TPM_SIM_STATE` | no | Directory holding the simulated TPM's keys (`ek.pem`, `srk.pem`), created on the first run. Default: the current directory. |
| `AZ_IOT_DPS_REGISTRATION_ID` | no | Registration ID. Default: derived from the EK (`az_iot_dps_tpm_registration_id_from_ek()`). |
| `AZ_IOT_TRUSTED_CA` | no | CA bundle (PEM file) that validates the DPS and IoT Hub server certificates. Default: the system trust store. |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | Provisioning endpoint. Default `global.azure-devices-provisioning.net`. |

## Build and run

Build prerequisites (toolchain, CMake, OpenSSL) are listed in the
[samples overview](../../README.md#prerequisites); this sample also needs libcurl
(`libcurl4-openssl-dev` on Debian and Ubuntu). Run from the repository's `c/` directory.

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_auth_dps_tpm
mkdir -p tpm-state
export AZ_IOT_TPM_SIM_STATE=tpm-state
./build/linux-gcc-debug/samples/authentication/az_iot_sample_auth_dps_tpm --enrollment
# create the enrollment with the printed command, then:
export AZ_IOT_DPS_ID_SCOPE='<id-scope>'
./build/linux-gcc-debug/samples/authentication/az_iot_sample_auth_dps_tpm
```

With az_mqtt (`-DAZ_IOT_WITH_AZ_MQTT=ON`), target `az_iot_sample_auth_dps_tpm_az_mqtt` builds the same sample in
`az_mqtt/` next to that binary ([adapters](../../README.md#build)).

The target exists when the OpenSSL crypto backend and libcurl are found.

## How it ends

Exit code 0 once the telemetry message is acknowledged by the hub; 1 if registration, the hub
connection or the send fails, or the hub is not mqttv3.
