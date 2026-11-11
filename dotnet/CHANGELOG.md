# .NET release notes

These capability snapshots describe the published packages, not changes made since release.
See the [capability matrix](README.md#key-features) for the version/API-set comparison.
Both packages target .NET 10.0 and combine device connectivity and DPS provisioning in
`Microsoft.Azure.Iot.Device`; neither includes service clients.

Source and sample links below use the repository commits recorded in the published NuGet packages.
The GitHub release entries provide package announcements; their tags should not be used in place of
these package-specific source links.

## 2.0.0-preview4

Published November 3, 2026:
[NuGet package](https://www.nuget.org/packages/Microsoft.Azure.Iot.Device/2.0.0-preview4),
[GitHub release](https://github.com/Azure/azure-iot-sdk/releases/tag/net/2.0.0-preview4),
[source](https://github.com/Azure/azure-iot-sdk/tree/c2568eae58cd1235ea2e961eb29e77b02dfe70f9/dotnet).

This prerelease includes the Unified API for MQTTv3 and MQTTv5 hubs and the MQTTv5-specific API for
MQTTv5 hubs only. All preview APIs are subject to change. Installing this package does not add
MQTTv5 capabilities to an MQTTv3 hub.

- Both API sets support DPS provisioning, X.509 authentication, device-to-cloud telemetry, twins,
  direct-method invocations, custom MQTT clients, and automatic reconnection/re-provisioning.
- The MQTTv5 API includes **custom-topic publishing** through `CustomTopicsClient.PublishAsync`.
  Custom-topic subscriptions and receiving are **not supported**. The caller must inspect the returned
  PUBACK reason code for broker rejection.
- Both API sets expose MQTTv5 twin pushes. Desired-property push mode and reported-property pushes on
  connection are opt-in (`ReceiveDesiredPropertyUpdates` and `ReceiveReportedPropertiesUponConnect`,
  both defaulting to `false`). Application-defined direct-method probes require the MQTTv5 API.
- Symmetric key / SAS authentication, cloud-to-device messages, IoT Hub certificate signing, and the
  file-upload SAS URI/completion APIs are available only with the Unified API connected to an MQTTv3 hub.
  MQTTv5 hubs require X.509 authentication. DPS certificate issuance is supported separately from hub signing.
- AMQPS, HTTPS provisioning/messaging transport, TPM provisioning, and software updates via DPS
  are not supported.

Use the release-pinned
[Unified samples](https://github.com/Azure/azure-iot-sdk/tree/c2568eae58cd1235ea2e961eb29e77b02dfe70f9/dotnet/samples/Unified)
or [MQTTv5 samples](https://github.com/Azure/azure-iot-sdk/tree/c2568eae58cd1235ea2e961eb29e77b02dfe70f9/dotnet/samples/MQTTv5),
including the [custom-topic publishing sample](https://github.com/Azure/azure-iot-sdk/tree/c2568eae58cd1235ea2e961eb29e77b02dfe70f9/dotnet/samples/MQTTv5/CustomTopicSample).
The samples reference the SDK project in that source revision; applications using NuGet should pin
`Microsoft.Azure.Iot.Device` to `2.0.0-preview4`.

## 1.2.0

Published November 3, 2026:
[NuGet package](https://www.nuget.org/packages/Microsoft.Azure.Iot.Device/1.2.0),
[GitHub release](https://github.com/Azure/azure-iot-sdk/releases/tag/net/1.2.0),
[source](https://github.com/Azure/azure-iot-sdk/tree/fa63f8e548f217472471cefb97a1cf0908f7f846/dotnet).

This stable release contains the Unified API for MQTTv3 IoT Hubs only.

- Supports DPS provisioning, X.509 and symmetric key / SAS authentication, device-to-cloud telemetry,
  cloud-to-device messages, twins, and direct-method invocation handling.
- Supports DPS certificate issuance and IoT Hub certificate signing where the service enables them,
  custom MQTT clients, and automatic reconnection/re-provisioning.
- Includes file-upload SAS URI and completion APIs over HTTPS. Applications perform the actual upload
  with a storage client; this does not provide an HTTPS provisioning or messaging transport.
- Does not include the MQTTv5 API set, MQTTv5 hub connections, MQTTv5 twin pushes, application-defined
  direct-method probes, or custom-topic publishing/subscriptions.
- AMQPS, HTTPS provisioning/messaging transport, TPM provisioning, and software updates via DPS
  are not supported.

Use the release-pinned
[Unified samples](https://github.com/Azure/azure-iot-sdk/tree/fa63f8e548f217472471cefb97a1cf0908f7f846/dotnet/samples/Unified).
They reference the SDK project in that source revision; applications using NuGet should pin
`Microsoft.Azure.Iot.Device` to `1.2.0`. MQTTv5 examples from the preview branch do not apply to this package.
