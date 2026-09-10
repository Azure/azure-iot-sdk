# .NET IoT SDK Samples

This directory contains all the samples for using the .NET IoT SDK in this repo.

## Pre-requisites

- A Device Provisioning Service
- An IoT Hub linked to that Device Provisioning Service

### How to create a sample device

Most of these samples assume that a sample device is already enrolled with your Device Provisioning Service, but the [SetupSampleDevice](./SetupSampleDevice/) sample allows you to create that sample device, enroll it with DPS, and locally save all the credentials needed to run the other samples in this directory.

Before running this sample, you simply need to enter your service-side DPS + Hub credentials [here](./SetupSampleDevice/Program.cs) like:

```csharp
    // Fake credentials for demonstration purposes only
    public static string DpsConnectionString = "HostName=Some-Fake-DPS-Hostname.azure-devices-provisioning.net;SharedAccessKeyName=provisioningserviceowner;SharedAccessKey=XXXXXXXXXXX";
    public static string DpsIdScope = "0ne12345678";
```

Once you edit the above section, run the sample from this directory with:

```bash
dotnet run --project SetupSampleDevice --property WarningLevel=0
```

This sample can be run again to delete the previous credentials and create a new device and new credentials.

### How to run the device samples

By running the above sample, you should see a set of credential files saved at the root of this samples directory like "deviceId.txt", "certificate.pfx", and more. Once you have those, all you need to do is run a command from this directory like:

```bash
dotnet run --project DirectMethodsSample  --property WarningLevel=0
```

### Connecting to preview nuget feed

In this folder, there is a nuget.config file that can be used to connect and authenticate with the nuget feed that this project pushes preview bits to. The only requirement is to have a github account and to create a PAT (classic) with "read:packages" permissions and then fill in the ```<packageSourceCredentials>``` section of the provided nuget.config file with those credentials
