<!--
Copyright (c) Microsoft. All rights reserved.
Licensed under the MIT license. See LICENSE file in the project root for full license information.
-->

# Software Updates Sample - Real Download on PC

This sample shows the part of a deployment the application owns: the download. Each
offered file is fetched over HTTPS with libcurl, a piece per `do_work()` call, through
the same HTTP proxy as the device's MQTT sessions. The SDK then checks each file's SHA-256
against the signed manifest by reading it back. Install and apply are simulated: nothing
on the device changes.

## Sample features
- Target platforms: **Linux** (and Windows when CMake finds libcurl).
- `download_fn` returns `AZ_IOT_SU_RESULT_IN_PROGRESS` until the transfer ends, so
  `do_work()` never blocks on the network.
- `read_file_fn` streams the downloaded file back for the SDK's hash check, keeping it open
  across sequential reads.
- One proxy setting (`AZ_IOT_PROXY_*`) for both `copts.proxy` and the download; libcurl's
  proxy environment variables are overridden.
- A transfer that stalls (under 1 byte/s for 60 s) or cannot connect within 60 s fails, and so
  does one that sends more than the manifest's file size.
- Only HTTP and HTTPS URLs are fetched, redirects included.
- A file is restarted when a replacing workflow asks for another file mid-transfer.
- Each download replaces any file or link at its path (removed, then created exclusively, owner-only);
  the read-back refuses a link, a non-regular file, and a file larger than the manifest size.
- Requires libcurl with asynchronous DNS (`AsynchDNS` in `curl --version`); without it the
  sample exits.
- `install_fn` works in chunks (`SAMPLE_INSTALL_CHUNKS` calls per step), returning
  `AZ_IOT_SU_RESULT_IN_PROGRESS` between them.
- Device failure codes from 0x100 up, so they never read as an `AZ_IOT_SU_RESULT_*`
  value. A failed download reports `0x1000 + HTTP status`, `0x200 + CURLcode`, `0x103` (more
  data than the manifest size) or `0x104` (`SAMPLE_DOWNLOAD_PREFIX` + file id too long) in the
  low bits of the step's extended result code.
- The regular route: checks at startup, again after each applied update, and every
  `AZ_IOT_SU_POLL_INTERVAL_S` (default 4 hours).
  Each check waits at most half the interval (60 s cap). The sample exits if it cannot report
  an applied update as installed.
- Requires Azure Device Provisioning service, an Azure IoT Hub and Azure Device Update.

| Concern | Behavior |
|---|---|
| Connection, update check, manifest verification, status reports | **Real** |
| `download_fn` | **Real**, libcurl multi interface, to `SAMPLE_DOWNLOAD_PREFIX<file id>` |
| `read_file_fn` | **Real**, reads the downloaded file for the SHA-256 check |
| `install_fn` / `apply_fn` | **Simulated**, log only; applying the last step records the update as installed |
| `is_installed_fn` | "Already installed" for an update whose id equals the installed one |

The update's import manifest must declare compatibility `manufacturer`/`model` equal to
`AZ_IOT_SU_MANUFACTURER`/`AZ_IOT_SU_MODEL` (default `Contoso`/`SU-Sim`). Unlike
[simulated_regular](../simulated_regular/README.md), the payload can be any content: it is
downloaded and hashed for real.

## Configure

| Variable | Required | Meaning |
|---|---|---|
| `AZ_IOT_DPS_ID_SCOPE`, `AZ_IOT_DPS_REGISTRATION_ID` | yes | The device's DPS enrollment |
| `AZ_IOT_CLIENT_CERT`, `AZ_IOT_CLIENT_KEY`, `AZ_IOT_TRUSTED_CA` | yes | X.509 credential and trust bundle |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | DPS endpoint; default global |
| `AZ_IOT_SU_MANUFACTURER`, `AZ_IOT_SU_MODEL` | no | Compatibility properties |
| `AZ_IOT_SU_INSTALLED_PROVIDER` / `_NAME` / `_VERSION` | no | Installed update id; default `Contoso/SU-Sim/1.0.0` |
| `AZ_IOT_PROXY_HOST`, `AZ_IOT_PROXY_PORT` | no | HTTP proxy (port default 8080) for MQTT and the download |
| `AZ_IOT_PROXY_USERNAME`, `AZ_IOT_PROXY_PASSWORD` | no | Proxy Basic credentials |
| `SAMPLE_DOWNLOAD_PREFIX` | no | Path prefix of downloaded files; default `./su_download_` |
| `SAMPLE_INSTALL_CHUNKS` | no | `install_fn` calls per step; default 4 |
| `AZ_IOT_SU_POLL_INTERVAL_S` | no | Seconds between checks; default 14400. The provisioning service plans for one check per device every 4 hours. |
| `AZ_IOT_SU_LOG_LEVEL` | no | `trace`, `debug`, `info` (default), `warn`, `error`, `off` |

## Build and run

The target `az_iot_sample_software_update_http_download` is built when a crypto backend
(OpenSSL, or mbedTLS without it) is available and CMake finds libcurl. On Linux:

```bash
sudo apt-get install -y libssl-dev libcurl4-openssl-dev
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_software_update_http_download
./build/linux-gcc-debug/samples/software_update/az_iot_sample_http_download
```

With az_mqtt (`-DAZ_IOT_WITH_AZ_MQTT=ON`), target `az_iot_sample_software_update_http_download_az_mqtt` builds the same sample in
`az_mqtt/` next to that binary ([adapters](../../../README.md#build)).

## Where to look in `main.c`

| What | Where |
|---|---|
| Non-blocking download | `download_file()` |
| Hash read-back | `download_read_file()` |
| Chunked install | `download_install()` |
| Proxy for MQTT and download | `copts.proxy` and `CURLOPT_PROXY` in `main()` / `download_file()` |
| Limits and workspace | `su_opts.limits`, `g_su_workspace` |
| Check after an applied update | the main loop |
