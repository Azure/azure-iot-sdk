# TPM authentication sample

This sample provisions a device that attests with a [TPM](https://learn.microsoft.com/azure/iot-dps/concepts-tpm-attestation)
through the Device Provisioning Service (DPS), connects it to the IoT Hub it is assigned and sends one telemetry message.

The SDK talks to the TPM only through the `ITpmSecurityProvider` interface, so it has no dependency on any particular TPM
library. This sample implements that interface in [TssTpmSecurityProvider.cs](./TssTpmSecurityProvider.cs) with the
[Microsoft.TSS](https://www.nuget.org/packages/Microsoft.TSS) library, and passes it to a `TpmAuthenticationProvider`:

```csharp
using TssTpmSecurityProvider tpm = TssTpmSecurityProvider.CreateForPlatformTpm();
TpmAuthenticationProvider authentication = new(registrationId, idScope, tpm);

// DPS only supports TPM attestation over HTTPS. The device still connects to its IoT Hub over MQTT.
ProvisioningSettings provisioningSettings = new(idScope) { Transport = ProvisioningTransport.Http };

using ConnectionClient connectionClient = new();
await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication);
```

During provisioning, DPS sends the device a key encrypted to its TPM's endorsement key. The TPM imports that key and
persists it, and every SAS token the device presents to DPS and to IoT Hub is signed by the TPM with that key. The key
never leaves the TPM.

## Limitations

- DPS only supports TPM attestation over HTTPS, so `ProvisioningSettings.Transport` must be `ProvisioningTransport.Http`.
  `ConnectionClient` throws an `ArgumentException` otherwise.
- TPM authentication uses SAS tokens, so it only works with MQTTv3 IoT Hubs.
- TPM devices have no secondary credentials to fall back to.

## Prerequisites

- A DPS instance linked to an IoT Hub.
- A TPM 2.0, either:
  - the platform TPM. On Windows this is reached through TBS and usually requires running as administrator. On Linux it
    is reached through `/dev/tpmrm0` or `/dev/tpm0` and requires access to that device.
  - the TSS.MSR TPM simulator, which runs on Windows. Download `Simulator.exe` and `Tpm.dll` from the
    [azure-iot-sdk-csharp repo](https://github.com/Azure/azure-iot-sdk-csharp/tree/main/security/tpm/samples/SecurityProviderTpmSimulator/TpmSimulator)
    into the same directory and run `Simulator.exe`. It listens on `127.0.0.1:2321`.

## Registering the device

DPS identifies a TPM device by its TPM's endorsement key, so the device's individual enrollment is created from it. On
the device, from the `dotnet/samples` directory, run the [SetupSampleDevice](../../SetupSampleDevice/) sample with
`--tpm` to register the platform TPM, or `--tpm-simulator` to register a running TPM simulator:

```bash
dotnet run --project SetupSampleDevice --property WarningLevel=0 -- --tpm-simulator
```

It reads the endorsement key, creates the enrollment and saves the device's registration ID and your DPS ID scope in the
samples directory, where this sample loads them from. To use an enrollment you created yourself, set
`SAMPLE_TPM_REGISTRATION_ID` to its registration ID and `DPS_ID_SCOPE` to your DPS ID scope instead.

## Running the sample

From the `dotnet/samples` directory, run against the platform TPM with:

```bash
dotnet run --project Unified/TpmAuthenticationSample --property WarningLevel=0
```

or against the TPM simulator with:

```bash
dotnet run --project Unified/TpmAuthenticationSample --property WarningLevel=0 -- --simulator
```

Use the TPM that was registered: an enrollment for the platform TPM's endorsement key does not accept the simulator, or
the other way around.
