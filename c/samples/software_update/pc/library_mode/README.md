<!--
Copyright (c) Microsoft. All rights reserved.
Licensed under the MIT license. See LICENSE file in the project root for full license information.
-->

# Software Updates Sample - Library Mode on PC

In library mode the software updates client keeps the service side of a deployment: update
checks, manifest signature verification, status reports and their retries. It hands each
verified update to the application, which downloads, installs and reports on its own state
machine. No platform hooks are set. Use it when an existing update agent owns the device work.
The design is in [software-updates.md §5.3](../../../../docs/eng/software-updates.md).

Download and install are simulated as in [simulated_regular](../simulated_regular/README.md):
zero-filled payloads of the declared size; install and apply only log. The hash check is real.

## Sample features
- Target platforms: **Linux** and **Windows**.
- `su_opts.library_mode = true`, no `hooks`; root keys are still required.
- `AZ_IOT_SU_EVENT_UPDATE_AVAILABLE` hands over the verified `request` and `manifest`. They stay
  valid until the final report. The observer only takes them; the main loop does one file or
  one step per pass, so `do_work()` keeps running.
- The application checks whether the update is already installed (library mode calls no
  `is_installed_fn`) and reports it `SKIPPED`.
- Each downloaded file is checked with `az_iot_su_verify_file_hash()`.
- One `IN_PROGRESS` report after the downloads, then the final report with one result per
  manifest step: `SUCCEEDED`, or `FAILED` with the failing step and later steps `SKIPPED`.
  `az_iot_su_client_report_status()` sends and retries it.
- After a success the client sends the new update id as installed on later checks. Library
  mode persists nothing, so the sample keeps the id in a file (`AZ_IOT_SU_INSTALLED_FILE`),
  written before the success report, and passes it to `az_iot_su_client_init()` on the next run.
- The regular route: checks at startup, again after each applied update, and every
  `AZ_IOT_SU_POLL_INTERVAL_S` (default 4 hours). On a device that has never registered, the
  startup check is rejected (no device record yet), so the sample checks again once the hub
  connects. A final report that lost its provisioning session is resent by a check 30 s later.
- Requires Azure Device Provisioning service, an Azure IoT Hub and Azure Device Update.

Deploy an update as for [simulated_regular](../simulated_regular/README.md), with compatibility
`manufacturer=Contoso`, `model=SU-Sim` and zero-filled files.

## Configure

| Variable | Required | Meaning |
|---|---|---|
| `AZ_IOT_DPS_ID_SCOPE`, `AZ_IOT_DPS_REGISTRATION_ID` | yes | The device's DPS enrollment |
| `AZ_IOT_CLIENT_CERT`, `AZ_IOT_CLIENT_KEY`, `AZ_IOT_TRUSTED_CA` | yes | X.509 credential and trust bundle |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | DPS endpoint; default global |
| `AZ_IOT_SU_MANUFACTURER`, `AZ_IOT_SU_MODEL` | no | Compatibility properties; default `Contoso`/`SU-Sim` |
| `AZ_IOT_SU_INSTALLED_PROVIDER` / `_NAME` / `_VERSION` | no | Installed update id when `AZ_IOT_SU_INSTALLED_FILE` does not exist; default `Contoso/SU-Sim/1.0.0` |
| `AZ_IOT_SU_INSTALLED_FILE` | no | Where the installed update id is kept; default `./su_library_installed.txt`. The sample stops if it exists but cannot be read or is malformed |
| `SU_SIM_FAIL_STEP=<n>` | no | 1-based step whose install fails; `0` (default) none |
| `SU_SIM_HASH_MISMATCH=1` | no | Corrupt the payload, so the hash check fails |
| `SU_SIM_DELAY_MS` | no | Per-download delay |
| `AZ_IOT_SU_POLL_INTERVAL_S` | no | Seconds between checks; default 14400 |
| `AZ_IOT_SU_LOG_LEVEL` | no | `trace`, `debug`, `info` (default), `warn`, `error`, `off` |

## Build and run

```bash
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_software_update_library_mode
./build/linux-gcc-debug/samples/software_update/az_iot_sample_library_mode
```

With az_mqtt (`-DAZ_IOT_WITH_AZ_MQTT=ON`), target `az_iot_sample_software_update_library_mode_az_mqtt` builds the same sample in
`az_mqtt/` next to that binary ([adapters](../../../README.md#build)).

## Expected output (abridged)

Success:

```text
Update workflow: VerifyingManifest -> Delegated
Verified update Contoso/SU-Sim/1.0.1 handed over: 1 file(s), 1 step(s).
Update applied. Installed: Contoso/SU-Sim/1.0.1 (saved to ./su_library_installed.txt)
Update workflow: Delegated -> Idle
Status report accepted (final).
Workflow completed; final report accepted.
```

`SU_SIM_HASH_MISMATCH=1`: the final report is `FAILED`, resultCode -3, extendedResultCodes
30000001, and every step `SKIPPED`. A failed workflow is offered again under the same id and
retried after a wait, as in the managed client.

## Where to look in `main.c`

| What | Where |
|---|---|
| Library mode, no hooks | `su_opts.library_mode` in `main()` |
| Taking the update | `AZ_IOT_SU_EVENT_UPDATE_AVAILABLE` in `on_su_event()` |
| The application's state machine | `advance_delegated_update()` |
| Hash check | `az_iot_su_verify_file_hash()`, `read_downloaded_file()` |
| Reports | `finish_delegated_update()`, the `IN_PROGRESS` report in `advance_delegated_update()` |
| Installed update id across runs | `load_installed_update_id()`, `save_applied_update_id_as_installed()` |
