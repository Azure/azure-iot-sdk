<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Logging

The SDK logs through one process-wide sink and logs nothing until you install one. This page
covers how to capture logs and what to collect when reporting an issue.

## Enable logging

To stderr:

```c
az_iot_log_sink sink = az_iot_log_stderr_sink(AZ_IOT_LOG_LEVEL_INFO);
az_iot_log_set_global_sink(&sink);
```

To a rotating file (Windows, Linux, macOS):

```c
static az_iot_log_file_sink file_sink; /* must outlive its use */
az_iot_log_sink sink;
if (az_iot_log_file_sink_open(
        &file_sink, AZ_IOT_LOG_FILE_DEFAULT_NAME, NULL, AZ_IOT_LOG_LEVEL_INFO, &sink)
    == AZ_IOT_OK)
{
  az_iot_log_set_global_sink(&sink);
}
/* ... */
az_iot_log_set_global_sink(NULL);
az_iot_log_file_sink_close(&file_sink);
```

- Name the file `azure-iot-sdk-c.log` (`AZ_IOT_LOG_FILE_DEFAULT_NAME`), so support can ask for it
  by name.
- The file is appended to and rotated at 1 MiB. Up to 3 older files are kept as `<path>.1`
  (newest) to `<path>.3`. Change this with `az_iot_log_file_sink_options`.
- Each line is flushed as it is written. On POSIX a new file is created owner-only (0600).
- Set the sink before creating any client. Do not change it while a client runs.
- Samples write `azure-iot-sdk-c.log` in the working directory when `AZ_IOT_SAMPLE_LOG_TO_FILE` is
  set, except `authentication/hsm_sign_callback`, `authentication/custom_provider_template` and
  the ESP32 sample.
- For your own sink, pass a callback, a context and a minimum level in an `az_iot_log_sink`.
  It may be called from MQTT adapter threads.

## Line format

The built-in sinks write:

```text
2026-10-03T18:16:17.408Z [INFO ] [t:10294] connection_client.c:3756: connection: open: sdk=1.0.0-preview route=dps ...
```

| Field | Meaning |
| --- | --- |
| `2026-10-03T18:16:17.408Z` | UTC wall-clock time, milliseconds. A device without a set clock shows 1970. |
| `[INFO ]` | Level. |
| `[t:10294]` | OS thread id (Windows, Linux). MQTT adapter threads differ from the application's. |
| `connection_client.c:3756` | Source file and line. |
| `connection: ...` | Message, starting with its component. |

A message longer than `AZ_IOT_LOG_MESSAGE_MAX` (384) is cut and ends in `...`.

## Levels

| Level | Use | Contains |
| --- | --- | --- |
| `ERROR`, `WARN` | Always on | Failures, and state changes caused by a failure, with the reason and the service or transport error code. |
| `INFO` | Default; enough for most support cases | Configuration at `open()`, every connection state change, each scheduled retry and its delay, the DPS assignment. |
| `DEBUG` | Reproducing an issue | Protocol steps, TLS material paths. |
| `TRACE` | Only when asked | Message bodies, such as software update payloads, and Paho's trace (`AZ_IOT_PAHO_TRACE`). Review before sharing. |

For TLS or MQTT connection issues, set `AZ_IOT_PAHO_TRACE=protocol` and the sink to `TRACE`. See
[Environment variables](client-configuration.md#environment-variables).

## Components

Every SDK message starts with `<component>: `. Filter by it.

| Prefix | Area |
| --- | --- |
| `connection` | Connection client: open, state changes, retries, MQTT acks |
| `dps` | Provisioning: registration, assignment |
| `cert`, `cert_pem` | Issued certificate chains, PEM certificate provider |
| `su` | Software updates |
| `paho` | Paho MQTT adapter and TLS |
| `mqttv3_telemetry`, `mqttv3_twin`, `mqttv3_direct_method`, `mqttv3_file_upload`, `c2d` | mqttv3 feature clients |
| `mqttv5_telemetry`, `mqttv5_twin`, `mqttv5_direct_method` | mqttv5 feature clients |

`app` is reserved for applications. Start your own messages with `app: ` (for example
`AZ_IOT_LOG_INFO("app: firmware 2.1 started")`) so they can be told apart from the SDK's.
`c/eng/check-log-prefixes.sh` enforces the list in CI.

## What logs contain

- Logs contain identifiers: hub and DPS host names, device id, registration id, ID scope, model
  id, file paths.
- At `DEBUG` and above, the SDK does not log keys, SAS tokens or proxy credentials. It removes
  the query, which may hold a PIN, from key URIs.
- `TRACE` adds message bodies and Paho's own trace. Review it before sharing.
- Do not log secrets in your own messages. Review any log before you share it.

## What to collect for support

Always include:

- SDK version (`sdk=` on the `connection: open:` line), OS and MQTT adapter.
- `azure-iot-sdk-c.log` and its rotated files, at `INFO` or lower, covering the issue. A log
  that starts at the `connection: open:` line is best.
- The UTC time the issue was seen.

Then, by symptom:

| Symptom | Level | Also |
| --- | --- | --- |
| Device does not connect or keeps reconnecting | `DEBUG` | `AZ_IOT_PAHO_TRACE=protocol` with `TRACE` for TLS failures. |
| Provisioning fails | `DEBUG` | Lines with the `dps` prefix carry the service error code. |
| Twin, direct method, telemetry or cloud-to-device issue | `DEBUG` | The relevant `mqttv3_*` or `mqttv5_*` component. |
| Software update fails | `DEBUG` | `TRACE` only if asked for the update payload. |
