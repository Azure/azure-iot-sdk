<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# DPS TPM attestation

Registers with DPS using TPM attestation, then sends telemetry to the assigned mqttv3 hub with SAS
tokens the TPM signs. See [`main.c`](main.c).

Two TPM backends implement `az_iot_dps_tpm_ops`:

- `az_iot_sample_auth_dps_tpm` simulates the TPM in software
  ([`sample_tpm_sim.c`](../../common/sample_tpm_sim.c), OpenSSL), so it runs without a TPM. The
  simulator keeps every key in process memory and in plain PEM files: use it only to try the flow.
- `az_iot_sample_auth_dps_tpm_tss` uses a TPM through the
  [tpm2-tss](https://github.com/tpm2-software/tpm2-tss) ESAPI
  ([`sample_tpm_tss.c`](../../common/sample_tpm_tss.c)): a hardware TPM, or a simulator such as
  [swtpm](https://github.com/stefanberger/swtpm). The DPS key never leaves the TPM.

## Sample features

- DPS takes TPM attestation over HTTPS or AMQP, not MQTT. This sample uses HTTPS: it registers with
  [`az_iot_dps_tpm_client.h`](../../../inc/azure/iot/az_iot_dps_tpm_client.h) over libcurl, in
  `register_with_dps()`:
  1. A register request with the endorsement key (EK) and storage root key (SRK).
  2. DPS answers 401 with a key encrypted to the TPM. The TPM imports it: TPM2_ActivateCredential
     with the EK, TPM2_Import and TPM2_Load under the SRK, then TPM2_EvictControl to persist it
     (the simulator does the same in software).
  3. The same register request, with a SAS token the TPM signs (TPM2_HMAC).
  4. Polling until DPS assigns the device. The assignment carries the hub key, which the TPM
     imports in the same way.
- The sample then connects `az_iot_connection_client` directly to the assigned hub. Hub SAS tokens
  come from `on_sas_token_required`, signed by the TPM in `sign_token_with_tpm()` as in
  [`user_provided_sas_token`](../user_provided_sas_token/README.md).
- Re-registration when the hub refuses the identity, which recovers a device that DPS has
  re-assigned, or that was deprovisioned (deleted from the hub and its enrollment from DPS) and
  then enrolled again. DPS refuses to register a device deleted from the hub while its enrollment
  remains (errorCode 409000), and registering does not enable a disabled device. The connection client cannot re-provision a TPM device, as
  its DPS session is MQTT. The sample retries the hub briefly (`identity_recovery.policy`: a newly
  assigned hub may refuse the device for a few seconds), and on the `FAULTED` event with
  `AZ_IOT_CONN_FAILURE_IDENTITY` closes and deinitializes the client, registers again (which
  imports a new hub key into the TPM) and connects to the new assignment, in
  `connect_and_send_telemetry()` and `main()`. It registers at most `SAMPLE_MAX_REGISTRATIONS`
  times per run.

## Service requirements

A DPS individual enrollment with TPM attestation for the TPM's EK, linked to an mqttv3
IoT Hub. Run the sample with `--enrollment` to print the registration ID and EK, and an
`az iot dps enrollment create` command that creates the enrollment.

## Configure

The sample reads these environment variables:

| Variable | Required | Meaning |
| --- | --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | yes, except with `--enrollment` | DPS ID scope. |
| `AZ_IOT_TPM_SIM_STATE` | no | Simulator only. Directory holding the simulated TPM's keys (`ek.pem`, `srk.pem`), created on the first run. Default: the current directory. |
| `AZ_IOT_TPM_TCTI` | no | tpm2-tss only. [TCTI](https://tpm2-tss.readthedocs.io/en/latest/group__tss2__tctildr.html) configuration, e.g. `device:/dev/tpmrm0` or `swtpm:host=127.0.0.1,port=2321`. Default: the tpm2-tss default. |
| `AZ_IOT_TPM_HMAC_KEY_HANDLE` | no | tpm2-tss only. Owner persistent handle (hex) where the DPS key is kept. Default `81000100`. |
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

### With a TPM (tpm2-tss)

`az_iot_sample_auth_dps_tpm_tss` also needs the tpm2-tss ESAPI, MU and TCTI loader libraries
(`libtss2-dev` on Debian and Ubuntu; the target is skipped without them), and access to the TPM
(e.g. membership of the `tss` group for `/dev/tpmrm0`). It uses the persistent EK (`0x81010001`)
and SRK (`0x81000001`) when they exist, and otherwise creates them from the standard templates
(TCG EK profile template L-1; RSA 2048 SRK), so `--enrollment` prints the same EK as
`tpm2_createek -G rsa`. The DPS key is kept at `AZ_IOT_TPM_HMAC_KEY_HANDLE`, replacing any key
there.

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_auth_dps_tpm_tss
export AZ_IOT_TPM_TCTI='device:/dev/tpmrm0'
./build/linux-gcc-debug/samples/authentication/az_iot_sample_auth_dps_tpm_tss --enrollment
# create the enrollment with the printed command, then:
export AZ_IOT_DPS_ID_SCOPE='<id-scope>'
./build/linux-gcc-debug/samples/authentication/az_iot_sample_auth_dps_tpm_tss
```

To try it on swtpm instead of a TPM:

```sh
mkdir -p swtpm-state
swtpm socket --tpm2 --tpmstate dir=swtpm-state --flags not-need-init,startup-clear \
  --server type=tcp,port=2321 --ctrl type=tcp,port=2322 --daemon
export AZ_IOT_TPM_TCTI='swtpm:host=127.0.0.1,port=2321'
```

## How it ends

Exit code 0 once the telemetry message is acknowledged by the hub; 1 if registration, the hub
connection or the send fails, or the hub is not mqttv3.
