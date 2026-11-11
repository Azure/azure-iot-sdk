# .NET IoT SDK Samples

This directory contains all the samples for using the .NET IoT SDK in this repo.

## Match samples to your SDK version

Local samples reference the SDK project in this checkout, not a published NuGet package. For
**1.2.0**, use the [release-pinned Unified samples](https://github.com/Azure/azure-iot-sdk/tree/fa63f8e548f217472471cefb97a1cf0908f7f846/dotnet/samples/Unified)
with an MQTTv3 IoT Hub. For **2.0.0-preview4**, use the
[release-pinned Unified samples](https://github.com/Azure/azure-iot-sdk/tree/c2568eae58cd1235ea2e961eb29e77b02dfe70f9/dotnet/samples/Unified)
for either hub type, or the [MQTTv5 samples](https://github.com/Azure/azure-iot-sdk/tree/c2568eae58cd1235ea2e961eb29e77b02dfe70f9/dotnet/samples/MQTTv5)
for an MQTTv5 hub. MQTTv5 hubs require X.509 authentication.

The preview4 [custom-topic sample](https://github.com/Azure/azure-iot-sdk/tree/c2568eae58cd1235ea2e961eb29e77b02dfe70f9/dotnet/samples/MQTTv5/CustomTopicSample)
demonstrates publishing only; custom-topic subscriptions and receiving are not supported.
See the [capability matrix](../README.md#key-features) for
version-specific limitations. Samples added after these releases may use APIs absent from those packages.

## Pre-requisites

- A Device Provisioning Service
- An IoT Hub linked to that Device Provisioning Service

### How to create a sample device

Most of these samples assume that a sample device is already enrolled with your Device Provisioning Service, but the [SetupSampleDevice](./SetupSampleDevice/) sample allows you to create that sample device, enroll it with DPS, and locally save all the credentials needed to run the other samples in this directory. It creates two individual enrollments: one that attests with an X.509 certificate, which most samples use, and one that attests with a symmetric key, which the [AuthenticationSample](./Unified/AuthenticationSample/) uses for its symmetric key scenarios.

Before running this sample, you simply need to enter your service-side DPS + Hub credentials [here](./SetupSampleDevice/Program.cs) like:

```csharp
    // Fake credentials for demonstration purposes only
    public static string DpsConnectionString = "HostName=Some-Fake-DPS-Hostname.azure-devices-provisioning.net;SharedAccessKeyName=provisioningserviceowner;SharedAccessKey=XXXXXXXXXXX";
    public static string DpsIdScope = "0ne12345678";
    public static string IotHubHostName = "some-fake-hub.azure-devices.net";
```

Once you edit the above section, run the sample from this directory with:

```bash
dotnet run --project SetupSampleDevice --property WarningLevel=0
```

This sample can be run again to delete the previous credentials and create a new device and new credentials.

### How to run the device samples

By running the above sample, you should see a set of credential files saved at the root of this samples directory like "deviceId.txt", "certificate.pfx", and more. Once you have those, all you need to do is run a command from this directory like:

```bash
dotnet run --project Unified/DirectMethodsSample --property WarningLevel=0
```

To see each way a device can authenticate with DPS and IoT Hub (X.509 certificates, symmetric keys, and certificates issued by DPS and renewed by IoT Hub), see the [AuthenticationSample](./Unified/AuthenticationSample/).

To capture SDK diagnostics and optional MQTTnet logs to the console and a file, see the
[LoggingSample](./Unified/LoggingSample/).

To keep a device's twin in sync with IoT Hub and persist it between runs, see the
[TwinClientSample](./Unified/TwinClientSample/).
