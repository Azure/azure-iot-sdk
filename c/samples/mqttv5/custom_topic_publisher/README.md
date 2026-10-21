<!-- Copyright (c) Microsoft. All rights reserved.
     Licensed under the MIT license. See LICENSE file in the project root for full license information. -->

# Custom topic publisher, mqttv5 only

Provisions through DPS, connects to an mqttv5 IoT Hub, publishes one message to a custom topic,
and closes.

## Sample features

- Hub generation: mqttv5 (MQTT 5) only. An assignment to an mqttv3 hub fails.
- Builds the topic from a template with `az_iot_mqttv5_custom_topic_client_format_topic()`, which
  replaces `{deviceId}` with the device's id. Provisioning again can change that id, so a device
  that publishes repeatedly builds the topic before each publish; `publish()` sends the topic as
  given and does not expand `{deviceId}`.
- Publishes at QoS 1 with a content type and one user property. The client adds no properties of
  its own.
- Custom topics are publish only.
- Registers both MQTT adapters (v3.1.1 and v5): the hub uses MQTT 5, but DPS always uses MQTT 3.1.1.
- Platforms: Linux and Windows.

## Service requirements

- A Device Provisioning Service (DPS) instance and an X.509 enrollment that assign the device to an
  **mqttv5** IoT Hub. DPS then returns the connection profile `mqttV5`.
- A topic template on the hub that matches the published topic, for example `contoso/{deviceId}/#`
  for the default `contoso/{deviceId}/telemetry`. Templates are set in the hub's
  `properties.mqttV5Settings.topicGroups[].topicTemplates` (ARM api-version `2026-10-01-preview`):

  ```json
  "mqttV5Settings": {
    "topicGroups": [
      { "topicGroupId": "contoso", "topicTemplates": [ "contoso/{deviceId}/#" ] }
    ]
  }
  ```

  The whole `topicGroups` list is replaced on every update, so include the groups you keep.
- A route that delivers the hub's device messages to an endpoint you read. Routed messages carry
  the published topic in the `mqtt-topic` property.

## Configure

The sample reads these environment variables:

| Variable | Required | Meaning |
| --- | --- | --- |
| `AZ_IOT_DPS_ID_SCOPE` | yes | DPS ID scope. |
| `AZ_IOT_DPS_REGISTRATION_ID` | yes | Registration ID; must equal the device certificate's common name. |
| `AZ_IOT_CLIENT_CERT` | yes | Device certificate chain (PEM file, leaf first). |
| `AZ_IOT_CLIENT_KEY` | yes | Device private key (PEM file). A key file is for development; see [Production keys](../../../docs/connecting.md#production-keys). |
| `AZ_IOT_TRUSTED_CA` | yes | CA bundle (PEM file) that validates the DPS and IoT Hub server certificates, e.g. `/etc/ssl/certs/ca-certificates.crt`. |
| `AZ_IOT_DPS_GLOBAL_ENDPOINT` | no | Provisioning endpoint. Default `global.azure-devices-provisioning.net`. |
| `AZ_IOT_CUSTOM_TOPIC_TEMPLATE` | no | Topic to publish to; `{deviceId}` is replaced with the device id. Default `contoso/{deviceId}/telemetry`. |

```sh
export AZ_IOT_DPS_ID_SCOPE='<id-scope>'
export AZ_IOT_DPS_REGISTRATION_ID='<registration-id>'
export AZ_IOT_CLIENT_CERT="$PWD/device-cert.pem"
export AZ_IOT_CLIENT_KEY="$PWD/device-key.pem"
export AZ_IOT_TRUSTED_CA='/etc/ssl/certs/ca-certificates.crt'
export AZ_IOT_CUSTOM_TOPIC_TEMPLATE='contoso/{deviceId}/telemetry'
```

PowerShell:

```powershell
$env:AZ_IOT_DPS_ID_SCOPE          = '<id-scope>'
$env:AZ_IOT_DPS_REGISTRATION_ID   = '<registration-id>'
$env:AZ_IOT_CLIENT_CERT           = "$PWD\device-cert.pem"
$env:AZ_IOT_CLIENT_KEY            = "$PWD\device-key.pem"
$env:AZ_IOT_TRUSTED_CA            = "$PWD\ca.pem"
$env:AZ_IOT_CUSTOM_TOPIC_TEMPLATE = 'contoso/{deviceId}/telemetry'
```

## Build and run

Build prerequisites (toolchain, CMake, OpenSSL) are listed in the
[samples overview](../../README.md#prerequisites). Run from the repository's `c/` directory.

Linux:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug --target az_iot_sample_mqttv5_custom_topic_publisher
./build/linux-gcc-debug/samples/mqttv5/az_iot_sample_custom_topic_publisher
```

With az_mqtt (`-DAZ_IOT_WITH_AZ_MQTT=ON`), target `az_iot_sample_mqttv5_custom_topic_publisher_az_mqtt` builds the same sample in
`az_mqtt/` next to that binary ([adapters](../../README.md#build)).

Windows (*Developer PowerShell for VS 2022*):

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target az_iot_sample_mqttv5_custom_topic_publisher
.\build\windows-msvc-debug\samples\mqttv5\Debug\az_iot_sample_custom_topic_publisher.exe
```

## How it ends

Exit code 0 when the hub acknowledges the publish; otherwise 1.

## Expected output

```text
Published to 'contoso/<device-id>/telemetry'.
```

SDK warnings and errors go to stderr. On an mqttv3 hub it prints
`This device is assigned to an MQTTv3 hub. This sample needs an mqttv5 hub.` and exits 1.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `Required env var <NAME> not set.`, exit 1 | A required variable is missing. |
| `Cannot build a topic from '<template>': AZ_IOT_ERR_INVALID_ARG` | The template has a `{` or `}` that is not part of `{deviceId}`. The variable is case-sensitive. |
| `Publish to '<topic>' not sent: AZ_IOT_ERR_INVALID_ARG` | The topic is invalid: `+` or `#`, a first level of `ih`, over 256 bytes or 15 levels, or not UTF-8. |
| `The hub refused '<topic>'.` | No topic template on the hub matches the topic. Shown only with an MQTT adapter that reports PUBACK reason codes, such as az_mqtt; the Paho adapter registered here does not yet. |
| `Publish to '<topic>' failed: AZ_IOT_ERR_MQTT` | The publish failed and the MQTT adapter did not report why. With the Paho adapter this includes a topic no template matches. |
| `Publish to '<topic>' failed: AZ_IOT_ERR_NOT_CONNECTED` | The connection ended before the hub acknowledged; the hub may also close the connection on a refused publish. |
| SDK line `dps register: errorCode=<code> errorMessage=<text>` | DPS refused the registration: no matching enrollment, the certificate's common name differs from `AZ_IOT_DPS_REGISTRATION_ID`, or the enrollment is disabled. |
| Repeated `paho: connect failed: ... TCP/TLS connect failure` | DPS or the hub is unreachable (network, proxy, `AZ_IOT_DPS_GLOBAL_ENDPOINT`), or server certificate validation fails (`AZ_IOT_TRUSTED_CA`). |

## Where to look in `main.c`

| What | Code |
| --- | --- |
| Create the client before `open()` | `az_iot_mqttv5_custom_topic_client_init()` |
| Build the topic for this device | `az_iot_mqttv5_custom_topic_client_format_topic()` |
| Publish and wait for completion | `az_iot_mqttv5_custom_topic_client_publish()`, `on_send_done()` |
