<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# C SDK samples

## Layout

| Folder | Serves | Use when |
| --- | --- | --- |
| [unified/](unified/) | MQTTv3 (MQTT v3.1.1) **or** MQTTv5 (MQTT v5), whichever DPS assigns | Default. The device does not control which hub it is provisioned to. |
| [mqttv5/](mqttv5/) | MQTTv5 only | The device is known to be on an MQTTv5 hub. |
| [authentication/](authentication/) | Either generation | Certificate providers, CSR enrollment, non-extractable keys. See its [README](authentication/README.md). |
| [software_update/](software_update/) | Either generation | Software Update agent. See [pc/simulated_onboarding](software_update/pc/simulated_onboarding/README.md), [pc/simulated_regular](software_update/pc/simulated_regular/README.md) and [esp32](software_update/esp32/README.md). |
| [common/](common/) | — | Shared helpers (`sample_utils`, certificate provider, CSR backends). |

There is no MQTTv3-only group: the unified samples cover MQTTv3 hubs. The
MQTTv3 IoT Hub only features, C2D and file upload, are in `unified/` and report MQTTv5 hubs.

## How a unified sample works

The hub generation comes from the `connectionProfile` DPS returns with the
assignment, and it can change while a device runs: a device moved to another
hub re-provisions and may land on the other generation. A feature client pins
its generation at `init()`; DPS assigning the other one stops the connection
with `AZ_IOT_ERR_CONNECTION_PROFILE_MISMATCH` before that hub is reached, and
the state event carries the assigned profile.

The unified samples, except `connect_first`, `c2d_receiver` and `file_upload` (below):

1. Registers **both** Paho adapters (`az_iot_paho_factory_create_v3_1_1()` and
   `az_iot_paho_factory_create_v5()`).
2. Builds its `az_iot_mqttv3_*` feature clients **before** `open()`, assuming
   MQTTv3 (what DPS assigns when it names no profile).
3. On a profile mismatch -- first connect, or a later move in either
   direction -- destroys them, builds the `az_iot_mqttv5_*` or `az_iot_mqttv3_*`
   ones for the profile the event carries, then calls `close()` and `open()`.
   In-flight operations of the old clients are lost.

[unified/connect_first](unified/connect_first/main.c) shows the conservative
alternative: open with no feature client, read the profile with
`az_iot_connection_client_get_hub_profile()` once `CONNECTED`, then build. It
handles later moves the same way. `unified/file_upload` builds this way too,
since its client needs the assigned hub at `init()`.

The MQTTv5 samples build theirs before `open()` and do not rebuild: an assignment
to an MQTTv3 hub fails. A profile this SDK does not know fails the connection
with `AZ_IOT_ERR_CONNECTION_PROFILE_UNSUPPORTED`.

[unified/telemetry](unified/telemetry/main.c) is the shortest example; read it
first.

## Prerequisites

- A DPS instance, a linked IoT Hub, and an X.509 enrollment for the device.
- Device certificate, private key and trusted CA as PEM files.

## Configuration

The unified and MQTTv5 samples read these; the authentication samples add or
replace some (see [authentication/README.md](authentication/README.md)):

| Variable | Required | Meaning |
| --- | --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | yes | DPS ID scope |
| `AZ_IOT_DPS_REGISTRATION_ID` | yes | Registration ID |
| `AZ_IOT_CLIENT_CERT` | yes | Device certificate (PEM path) |
| `AZ_IOT_CLIENT_KEY` | yes | Device private key (PEM path) |
| `AZ_IOT_TRUSTED_CA` | yes | Trusted CA (PEM path) |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | DPS endpoint; default `global.azure-devices-provisioning.net` |

If a required variable is unset, the sample names it and exits non-zero.
Per-sample extras are documented in each sample's header comment.

Development only: while DPS does not return `connectionProfile`, the assignment
resolves to MQTTv3. Set `AZ_IOT_DPS_CONNECTION_PROFILE_OVERRIDE=mqttV5` (or
`classic`) to supply one; a value sent by DPS always wins.

## Build and run

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
./build/linux-gcc-debug/samples/unified/az_iot_sample_telemetry
```

Samples need `AZ_IOT_WITH_PAHO=ON` (the default). As in the .NET SDK, a sample
has the same name in both folders: `<folder>/<name>` builds CMake target
`az_iot_sample_<folder>_<name>` into `<build>/samples/<folder>/az_iot_sample_<name>`.
Authentication samples land in `<build>/samples/authentication/`.

## Samples

| Sample | What it shows |
| --- | --- |
| [unified/telemetry](unified/telemetry/) | The unified shape: build before `open()`, rebuild on reassignment, send every 5 s for ~60 s. |
| [unified/connect_first](unified/connect_first/) | The conservative shape: build after `CONNECTED`, from the profile read then. |
| [unified/twin_get_patch](unified/twin_get_patch/) | Twin GET and reported PATCH on every connect, desired updates. MQTTv5 returns sections separately and reports a patch verdict. |
| [unified/direct_method_responder](unified/direct_method_responder/) | Inline direct-method answers. MQTTv3 routes every name to one handler; MQTTv5 declares methods and probes first. |
| [unified/direct_method_slow_responder](unified/direct_method_slow_responder/) | Answering after the handler returned, against the device's timeout (MQTTv3) or the caller's (MQTTv5). |
| [unified/c2d_receiver](unified/c2d_receiver/) | Cloud-to-device messages. MQTTv3 IoT Hub only; on MQTTv5 it says so and exits non-zero. |
| [unified/file_upload](unified/file_upload/) | SAS-URI request, blob PUT via libcurl, completion notification. MQTTv3 only; on MQTTv5 it says so and exits non-zero. One-shot. |
| [unified/websockets](unified/websockets/) | unified/telemetry over MQTT-over-WebSockets (443). |
| [unified/proxy](unified/proxy/) | unified/telemetry through an HTTP CONNECT proxy. |
| [mqttv5/telemetry](mqttv5/telemetry/) | Telemetry on MQTTv5. |
| [mqttv5/twin_get_patch](mqttv5/twin_get_patch/) | Twin on MQTTv5. |
| [mqttv5/direct_method_responder](mqttv5/direct_method_responder/) | Direct methods on MQTTv5, with a probe handler. |
| [mqttv5/direct_method_slow_responder](mqttv5/direct_method_slow_responder/) | Deferred direct-method answers on MQTTv5. |
| [authentication](authentication/) | Certificate providers, CSR enrollment, operational certificates, key custody. |
| [software_update/pc/simulated_onboarding](software_update/pc/simulated_onboarding/) | Software Update on the onboarding route, for a day-0 device; no IoT Hub needed. Simulated install. |
| [software_update/pc/simulated_regular](software_update/pc/simulated_regular/) | Software Update on the regular route, polled, for a registered device. Simulated install. |
| [software_update/esp32](software_update/esp32/) | Software Update with a real OTA install on an ESP32 (ESP-IDF). Onboarding route until registered, regular after. |
