# Azure IoT SDK for .NET

## Contents

This folder contains the source code, tests, and samples for the Azure IoT SDK for .NET

This SDK is intended for use by IoT devices that use Azure IoT Hub and Azure Device Provisioning Service

This SDK does not include any service client code. You can find that [here instead](https://github.com/Azure/azure-iot-sdk-csharp/tree/main/iothub/service)

## NuGet packages

| Package Name                                          | Release Version                                           |
| ---                                                   | ---                                                       |
| Microsoft.Azure.Iot.Device                            | [![NuGet][iothub-device-release]][iothub-device-nuget]    |

## Choose a version

Both releases target **.NET 10.0** and include device connectivity and DPS provisioning in the single
`Microsoft.Azure.Iot.Device` package. They do not include service clients.

| Version | Use when | Documentation and samples |
| --- | --- | --- |
| [1.2.0](https://www.nuget.org/packages/Microsoft.Azure.Iot.Device/1.2.0) | Connecting to an MQTTv3 IoT Hub with the Unified API set | [Release notes](CHANGELOG.md#120), [release source][stable-source], [release samples][stable-samples] |
| [2.0.0-preview4](https://www.nuget.org/packages/Microsoft.Azure.Iot.Device/2.0.0-preview4) | Evaluating an MQTTv5 IoT Hub, or using the Unified API set across MQTTv3 and MQTTv5 hubs | [Release notes](CHANGELOG.md#200-preview4), [release source][preview-source], [Unified samples][preview-unified-samples], [MQTTv5 samples][preview-mqttv5-samples] |

Pin the package version explicitly:

```shell
dotnet add package Microsoft.Azure.Iot.Device --version 1.2.0
```

For the preview, replace `1.2.0` with `2.0.0-preview4`. The capability matrix below describes these
published versions, not unreleased branch changes. Release source and sample links are pinned to the
commits recorded in the NuGet packages' repository metadata. Local samples reference the local SDK
project; use the pinned samples when working with a published package.

## High-level Design notes

Unlike previous incarnations of the Azure IoT Hub and Device Provisioning Service SDKs, this version is designed such that

 - A single ```ConnectionClient``` instance handles all provisioning + connection logic
 - Separate "feature clients" (such as ```TelemetryClient```) use that ```ConnectionClient``` to perform twin/direct methods/telemetry operations

## Capturing logs

The SDK writes diagnostic messages through `System.Diagnostics.Trace`, including connection, provisioning,
retry, and feature-client diagnostics. Register a `TraceListener` before creating clients to send these messages
to a console, file, or another destination. No additional logging package is required.

For example, capture informational messages, warnings, and errors in a file:

```csharp
using System.Diagnostics;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Models;

using var listener = new TextWriterTraceListener("iot-sdk.log")
{
    Filter = new EventTypeFilter(SourceLevels.Information),
    TraceOutputOptions = TraceOptions.DateTime,
};
Trace.Listeners.Add(listener);
Trace.AutoFlush = true;

try
{
    using var connectionClient = new ConnectionClient(new ConnectionClientOptions
    {
        EnableMqttLogging = true,
    });

    // Provision, connect, and use feature clients here.
}
finally
{
    Trace.Flush();
    Trace.Listeners.Remove(listener);
}
```

Add a `ConsoleTraceListener` to capture messages in the console instead of, or in addition to, a file.
Use `SourceLevels.Warning` to capture only warnings and errors, or `SourceLevels.All` to accept every severity.
`Trace.Listeners` and `Trace.AutoFlush` are process-wide settings: listeners can also receive trace messages
from your application and other libraries, not just this SDK. Keep listeners alive until clients have been
disconnected and disposed, then flush, remove, and dispose the listeners you added.

`ConnectionClientOptions.EnableMqttLogging` defaults to `false`. Setting it to `true` adds low-level MQTTnet
diagnostics to the same trace output when using the SDK's default MQTT client; it does not enable or disable
the SDK's own diagnostics. MQTTnet messages are currently emitted at trace `Information` level, regardless of
their original MQTTnet severity. If you supply your own `MqttClient`, configure its logging separately.

Treat captured logs as potentially sensitive, especially with MQTT logging enabled: they can contain device
identifiers, hostnames, topics, and message contents. Restrict access, limit retention, and review or redact
logs before sharing them. For production applications, use an appropriate rotation and retention policy.
`Trace` calls require the `TRACE` compilation symbol, which standard .NET SDK projects define in both Debug
and Release builds; retain it if you customize compilation constants.

See the dedicated [logging sample](samples/Unified/LoggingSample/) for a runnable example that captures
console and file logs while provisioning a device and sending telemetry.

## MQTTv5 Public Preview Notice

> Please note that all preview APIs are subject to change prior to their formal release

**2.0.0-preview4** supports MQTTv5 IoT Hubs, which are also in preview. MQTTv5 hub capabilities are not
available merely by installing the preview package while continuing to use an MQTTv3 hub.

The preview package includes two API sets:

- **Unified** supports either hub type, selected from the DPS assignment. Core telemetry, twin, and
  direct-method APIs work across both, with the hub-specific limitations listed below. MQTTv5 twin
  push options are also exposed by this API set.
- **MQTTv5** targets MQTTv5 hubs only and adds custom-topic publishing and direct-method probe handling.

The `main` branch contains the MQTTv3-only SDK; MQTTv5 implementation and samples live in
`releases/public-preview`. For published-version examples, use the pinned links under
[Choose a version](#choose-a-version), rather than assuming either branch matches a released package.

## Key features

**Supported** means available in 1.2.0. **Preview** means implemented in 2.0.0-preview4, whose APIs
remain subject to change. **No** means unsupported in that version/API set, not a delivery commitment.

| Feature | 1.2.0 Unified / MQTTv3 | 2.0.0-preview4 Unified / MQTTv3 | 2.0.0-preview4 Unified / MQTTv5 | 2.0.0-preview4 MQTTv5 API |
| --- | --- | --- | --- | --- |
| Device provisioning through DPS | Supported | Preview | Preview | Preview |
| X.509 authentication | Supported | Preview | Preview | Preview |
| Symmetric key / SAS authentication | Supported | Preview | No | No |
| MQTTv3 connection to IoT Hub | Supported | Preview | No | No |
| MQTTv5 connection to IoT Hub | No | No | Preview | Preview |
| Device-to-cloud telemetry | Supported | Preview | Preview | Preview |
| Cloud-to-device messages | Supported | Preview | No | No |
| Twin GET, reported-property updates, desired-property patches | Supported | Preview | Preview (opt in to patches) | Preview (opt in to patches) |
| MQTTv5 twin pushes | No | No | Preview | Preview |
| Direct-method invocation handling | Supported | Preview | Preview | Preview |
| Application-defined direct-method probe handling | No | No | No | Preview |
| Custom-topic publishing | No | No | No | Preview |
| Custom-topic subscriptions / receiving | No | No | No | No |
| Certificate issuance through DPS | Supported | Preview | Preview | Preview |
| Certificate signing through IoT Hub | Supported | Preview | No | No |
| File-upload SAS URI and completion APIs | Supported | Preview | No | No |
| Bring-your-own MQTT client (`IMqttClient`) | Supported | Preview | Preview | Preview |
| Retry, reconnection, and re-provisioning | Supported | Preview | Preview | Preview |
| AMQPS transport | No | No | No | No |
| HTTPS transport for provisioning / messaging | No | No | No | No |
| TPM authentication through DPS | No | No | No | No |
| Software updates via DPS | No | No | No | No |

### Capability limitations

- **Custom topics:** `MQTTv5.CustomTopics.CustomTopicsClient.PublishAsync` publishes to application-defined
  topics on an MQTTv5 hub. It does not expose custom-topic subscriptions or receiving. Check the returned
  `MqttPublishAck.ReasonCode` for broker rejection; a failed PUBACK is returned, not thrown.
  See the [preview4 custom-topic publishing sample][custom-topic-sample].
- **Twins:** In preview4, both API sets expose MQTTv5 twin push behavior. Desired-property push mode is
  off by default; set `ReceiveDesiredPropertyUpdates = true` for ongoing desired-property patches.
  `ReceiveReportedPropertiesUponConnect` independently controls reported-property pushes on connection
  and also defaults to `false`. These options do not change MQTTv3 desired-property patch delivery.
- **Direct methods:** The Unified API handles invocations on either hub type, but application-defined
  MQTTv5 probe handling requires the MQTTv5 API set.
- **Certificates:** DPS certificate issuance and IoT Hub certificate signing are separate capabilities.
  Hub-side signing is available only on MQTTv3 hubs in these releases and requires service-side support.
- **File upload:** `Unified.FileUpload.FileUploadClient` requests an upload SAS URI and reports completion
  over HTTPS for MQTTv3 hubs. The application uploads the file using a storage client. This is not an
  HTTPS provisioning or messaging transport.

[iothub-device-release]: https://img.shields.io/nuget/v/Microsoft.Azure.Iot.Device.svg?style=plastic
[iothub-device-nuget]: https://www.nuget.org/packages/Microsoft.Azure.Iot.Device/
[stable-source]: https://github.com/Azure/azure-iot-sdk/tree/fa63f8e548f217472471cefb97a1cf0908f7f846/dotnet
[stable-samples]: https://github.com/Azure/azure-iot-sdk/tree/fa63f8e548f217472471cefb97a1cf0908f7f846/dotnet/samples/Unified
[preview-source]: https://github.com/Azure/azure-iot-sdk/tree/c2568eae58cd1235ea2e961eb29e77b02dfe70f9/dotnet
[preview-unified-samples]: https://github.com/Azure/azure-iot-sdk/tree/c2568eae58cd1235ea2e961eb29e77b02dfe70f9/dotnet/samples/Unified
[preview-mqttv5-samples]: https://github.com/Azure/azure-iot-sdk/tree/c2568eae58cd1235ea2e961eb29e77b02dfe70f9/dotnet/samples/MQTTv5
[custom-topic-sample]: https://github.com/Azure/azure-iot-sdk/tree/c2568eae58cd1235ea2e961eb29e77b02dfe70f9/dotnet/samples/MQTTv5/CustomTopicSample