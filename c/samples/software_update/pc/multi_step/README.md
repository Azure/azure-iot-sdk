<!--
Copyright (c) Microsoft. All rights reserved.
Licensed under the MIT license. See LICENSE file in the project root for full license information.
-->

# Software Updates Sample - Multi-Step Update and Step Results on PC

This sample runs a deployment whose manifest has several steps, and shows what the device
reports for each one. Download and the hash read-back are simulated (zero-filled payloads, as in
[simulated_regular](../simulated_regular/README.md)). Install, apply, backup and restore only log.

## Sample features
- Target platforms: **Linux** and **Windows**.
- One hook per phase (`backup_fn`, `install_fn`, `apply_fn`, `restore_fn`), called per step,
  with a backup record per step.
- A failing step stops the workflow, and every step already backed up is restored in reverse
  order. `SAMPLE_RESTORE_FAILS=1` makes the restores fail too.
- Device failure codes from 0x100 up (`SAMPLE_ERR_*`). The SDK puts them in the low bits of
  the failing step's extended result code, with the phase in bits 31..28.
- `az_iot_su_client_set_step_result_details()` sets each step's human-readable
  `resultDetails`. The default limit is 128 bytes per step (`limits.result_details_size`).
- `AZ_IOT_SU_EVENT_REPORT_ACCEPTED` hands the final report to the observer, which prints
  every step's outcome, codes and details.
- The regular route: checks at startup, again after each applied update, and every
  `AZ_IOT_SU_POLL_INTERVAL_S` (default 4 hours).
  Each check waits at most half the interval (60 s cap).
- Requires Azure Device Provisioning service, an Azure IoT Hub and Azure Device Update.

The update to deploy needs at least two inline steps, with compatibility
`manufacturer=Contoso`, `model=SU-MultiStep` and zero-filled files. The SDK's default build
handles at most 2 steps and 2 files per update.

## Configure

| Variable | Required | Meaning |
|---|---|---|
| `AZ_IOT_DPS_ID_SCOPE`, `AZ_IOT_DPS_REGISTRATION_ID` | yes | The device's DPS enrollment |
| `AZ_IOT_CLIENT_CERT`, `AZ_IOT_CLIENT_KEY`, `AZ_IOT_TRUSTED_CA` | yes | X.509 credential and trust bundle |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | DPS endpoint; default global |
| `AZ_IOT_SU_MANUFACTURER`, `AZ_IOT_SU_MODEL` | no | Compatibility properties; default `Contoso`/`SU-MultiStep` |
| `AZ_IOT_SU_INSTALLED_PROVIDER` / `_NAME` / `_VERSION` | no | Installed update id; default `Contoso/SU-MultiStep/1.0.0` |
| `SAMPLE_FAIL_STEP=<n>` | no | 1-based step that fails; `0` (default) none |
| `SAMPLE_FAIL_PHASE=<p>` | no | `backup`, `install` (default) or `apply` |
| `SAMPLE_RESTORE_FAILS=1` | no | `restore_fn` fails as well |
| `AZ_IOT_SU_EXIT_AFTER_WORKFLOW=1` | no | Exit once the final report is accepted: 0 when it succeeded, 2 otherwise |
| `AZ_IOT_SU_POLL_INTERVAL_S` | no | Seconds between checks; default 14400 |
| `AZ_IOT_SU_LOG_LEVEL` | no | `trace`, `debug`, `info` (default), `warn`, `error`, `off` |

## Build and run

```bash
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_software_update_multi_step
SAMPLE_FAIL_STEP=2 ./build/linux-gcc-debug/samples/software_update/az_iot_sample_multi_step
```

With az_mqtt (`-DAZ_IOT_WITH_AZ_MQTT=ON`), target `az_iot_sample_software_update_multi_step_az_mqtt` builds the same sample in
`az_mqtt/` next to that binary ([adapters](../../../README.md#build)).

## Expected output (abridged)

Success, two steps:

```text
Step 0 (microsoft/script:1): backup
Step 0: install
Step 0: apply
Step 1 (microsoft/script:1): backup
Step 1: install
Step 1: apply
Final report for workflow <id>: SUCCEEDED, resultCode 700, extendedResultCodes 0
  step_0: SUCCEEDED, resultCode 700, facility 0x0, sub-code 0x0, details "installed and applied"
  step_1: SUCCEEDED, resultCode 700, facility 0x0, sub-code 0x0, details "installed and applied"
```

`SAMPLE_FAIL_STEP=2`: step 1's install fails, then steps 1 and 0 are restored:

```text
Step 1: install
Step 1: restore from backup
Step 0: restore from backup
Final report for workflow <id>: FAILED, resultCode -5, extendedResultCodes 50000102
  step_0: SUCCEEDED, resultCode 700, facility 0x0, sub-code 0x0, details "installed and applied"
  step_1: FAILED, resultCode -5, facility 0x5, sub-code 0x102, details "install: payload rejected by the target component"
```

A failed workflow is offered again with the same id; the client ignores it as a duplicate
(`AZ_IOT_SU_EVENT_UPDATE_DUPLICATE`), so this process does not retry it. Another attempt needs a
new deployment, which has a new workflow id.

## Where to look in `main.c`

| What | Where |
|---|---|
| Per-step hooks and the backup record | `step_backup()`, `step_install()`, `step_apply()`, `step_restore()` |
| Step result details | `set_details()` |
| Device failure codes | `SAMPLE_ERR_*` |
| The final report, step by step | `print_report()`, called from `on_su_event()` |
| Limits and workspace | `su_opts.limits`, `g_su_workspace` |
