<!--
Copyright (c) Microsoft. All rights reserved.
Licensed under the MIT license. See LICENSE file in the project root for full license information.
-->

# Software Updates Sample - Reboot and Resume on PC

Some updates take effect only after the device restarts. This sample shows how a workflow
survives that restart. `install_fn` or `apply_fn` returns `AZ_IOT_SU_RESULT_REBOOT_REQUIRED`,
and the client saves the workflow through `persist_state_fn`. After the restart,
`az_iot_su_client_resume()` reads it back through `load_state_fn`, and the workflow continues.
The "reboot" is the process exiting; running it again is the device starting. Download and
the hash read-back are simulated (zero-filled payloads); install and apply only log.

## Sample features
- Target platforms: **Linux** and **Windows**.
- `persist_state_fn` writes a file atomically (temporary file, then rename; `MoveFileExA` on
  Windows) and erases it on a zero-length write. `load_state_fn` reads it back.
- `SAMPLE_REBOOT_AT=install:<n>` or `apply:<n>` picks where a reboot is needed (1-based
  step).
  - After an install reboot, the step resumes at Apply.
  - After an apply reboot, the workflow resumes at the next step.
  - After the last apply, the workflow has succeeded. A new update is deferred
    (`AZ_IOT_SU_EVENT_UPDATE_DEFERRED`) until the reboot.
- `resume()` runs before the first update check, and no check is made while a resumed
  workflow runs.
- `SAMPLE_PERSIST_FAIL_COUNT=<n>` fails the first `n` writes. The client holds the workflow and
  retries (`AZ_IOT_SU_EVENT_PERSIST_FAILED`, then `_RECOVERED`), and the sample does not
  "reboot" while a write is failing. After the last apply, a final report accepted before the
  retry also allows the "reboot", once any record left (stale by then) is erased; so does one the
  client drops.
- Step result details set before the reboot are reported in the final report after it.
- A failed workflow cancels the pending "reboot".
- After a succeeded update, later checks advertise its id as installed
  (`az_iot_su_client_update_device_properties()`), once per id: a repeated report of the same
  update does not refresh them again.
- Each update check waits at most half of `AZ_IOT_SU_POLL_INTERVAL_S` (60 s cap).
- Update checks pause from a workflow's start, or from a resumed unsent final report
  (`az_iot_su_client_is_terminal_report_owed()`), until that report settles, so a check never
  advertises the previous installed id. A report accepted, or dropped by the client
  (`AZ_IOT_SU_EVENT_OPERATION_ABANDONED` with `report_terminal`), settles it. After an accepted
  succeeded report, checks advertise its installed id. After a dropped one, checks stop: advertising
  the id (`az_iot_su_client_update_device_properties()`) would send the dropped report again. A
  report abandoned for a lost session stays owed and is sent again that way.
- A connection that faults for good ends the process (exit 1), even during an update; a saved
  record, if any, resumes it on the next run.
- Requires Azure Device Provisioning service, an Azure IoT Hub and Azure Device Update.

## Configure

| Variable | Required | Meaning |
|---|---|---|
| `AZ_IOT_DPS_ID_SCOPE`, `AZ_IOT_DPS_REGISTRATION_ID` | yes | The device's DPS enrollment |
| `AZ_IOT_CLIENT_CERT`, `AZ_IOT_CLIENT_KEY`, `AZ_IOT_TRUSTED_CA` | yes | X.509 credential and trust bundle |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | DPS endpoint; default global |
| `AZ_IOT_SU_MANUFACTURER`, `AZ_IOT_SU_MODEL` | no | Compatibility; default `Contoso`/`SU-Reboot` |
| `AZ_IOT_SU_INSTALLED_PROVIDER` / `_NAME` / `_VERSION` | no | Installed update id at startup; default `Contoso/SU-Reboot/1.0.0`. A real device reads it from its image; set it to the applied id when restarting after a completed update |
| `SAMPLE_REBOOT_AT` | no | `install:<n>` (default `install:1`) or `apply:<n>` |
| `SAMPLE_STATE_FILE` | no | Saved workflow; default `./su_reboot_state.bin` |
| `SAMPLE_PERSIST_FAIL_COUNT` | no | Number of state writes to fail first |
| `AZ_IOT_SU_EXIT_AFTER_WORKFLOW=1` | no | Exit once the final report is accepted (0 when it succeeded, 2 otherwise) or dropped (2) |
| `AZ_IOT_SU_POLL_INTERVAL_S` | no | Seconds between checks; default 14400 |
| `AZ_IOT_SU_LOG_LEVEL` | no | `trace`, `debug`, `info` (default), `warn`, `error`, `off` |

## Build and run

```bash
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_software_update_reboot_resume
B=./build/linux-gcc-debug/samples/software_update/az_iot_sample_reboot_resume
SAMPLE_REBOOT_AT=install:1 $B   # installs step 0, saves the workflow, exits ("reboot")
$B                              # resumes at Apply, finishes, reports
```

With az_mqtt (`-DAZ_IOT_WITH_AZ_MQTT=ON`), target `az_iot_sample_software_update_reboot_resume_az_mqtt` builds the same sample in
`az_mqtt/` next to that binary ([adapters](../../../README.md#build)).

Size the storage behind `persist_state_fn` for `AZ_IOT_SU_STATE_BLOB_MAX_SIZE(max_update_size,
result_details_size)` bytes.

## Where to look in `main.c`

| What | Where |
|---|---|
| Atomic save, erase and load | `reboot_persist()`, `reboot_load()` |
| Asking for a reboot | `reboot_install()`, `reboot_apply()` |
| Resume before the first check | `az_iot_su_client_resume()` in `main()` |
| Not rebooting while a write fails | `PERSIST_FAILED` / `_RECOVERED` in `on_su_event()` and the main loop |
| Deferred update after the last apply | `UPDATE_DEFERRED` in `on_su_event()` |
