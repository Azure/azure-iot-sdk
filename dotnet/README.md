# Azure IoT SDK for .NET

## Contents

This folder contains the source code, tests, and samples for the Azure IoT SDK for .NET

This SDK is intended for use by IoT devices that use Azure IoT Hub and Azure Device Provisioning Service

This SDK does not include any service client code. You can find that [here instead](https://github.com/Azure/azure-iot-sdk-csharp/tree/main/iothub/service)

## NuGet packages

| Package Name                                          | Release Version                                           |
| ---                                                   | ---                                                       |
| Microsoft.Azure.Iot.Device                            | [![NuGet][iothub-device-release]][iothub-device-nuget]    |

## High-level Design notes

Unlike previous incarnations of the Azure IoT Hub and Device Provisioning Service SDKs, this version is designed such that

 - A single ```ConnectionClient``` instance handles all provisioning + connection logic
 - Separate "feature clients" (such as ```TelemetryClient```) use that ```ConnectionClient``` to perform twin/direct methods/telemetry operations

## Boot and operational authentication

`ProvisionAndConnectAsync` requires two explicitly supplied authentication providers:
the **boot** provider authenticates with Device Provisioning Service (DPS), and the
**operational** provider authenticates with IoT Hub. The providers can use different
credentials and TLS callbacks. The roles have separate abstract base classes:
`BootAuthenticationProvider` exposes DPS authentication and registration Id lookup,
while `OperationalAuthenticationProvider` exposes Hub authentication and issued-certificate
handling. `ProvisionAndConnectAsync` accepts these role-specific types, so a provider
cannot accidentally be supplied for the wrong endpoint.

| Credential | Boot provider (DPS) | Operational provider (IoT Hub) |
| --- | --- | --- |
| X.509 | `X509BootAuthenticationProvider` | `X509OperationalAuthenticationProvider` |
| Symmetric key | `SymmetricKeyBootAuthenticationProvider` | `SymmetricKeyOperationalAuthenticationProvider` |
| Issued X.509 only | — | `IssuedCertificateOperationalAuthenticationProvider` |

```csharp
// X.509 boot + X.509 operational using existing certificates.
var boot = new X509BootAuthenticationProvider(bootCertificate);
var operational = new X509OperationalAuthenticationProvider(operationalCertificate);
ConnectionContext context = await connectionClient.ProvisionAndConnectAsync(
    provisioningSettings, boot, operational, cancellationToken: cancellationToken);
```

For symmetric-key boot, use
`new SymmetricKeyBootAuthenticationProvider(registrationId, idScope, bootKey)`.
For symmetric-key operational authentication, use
`new SymmetricKeyOperationalAuthenticationProvider(operationalKey)`; the assigned hub hostname
and device Id are supplied by the connection client on each connection attempt.
Each key is Base64-encoded and must match the corresponding DPS enrollment or Hub
device identity. A standard symmetric-key DPS enrollment assigns the same device
key to Hub, so pass that key to both providers in that configuration.

For X.509 operational authentication with a certificate issued during provisioning:

```csharp
var boot = new SymmetricKeyBootAuthenticationProvider(registrationId, idScope, bootKey);
var operational = new IssuedCertificateOperationalAuthenticationProvider(csrPrivateKey);
provisioningSettings.CertificateSigningRequest = new(csrPrivateKey, base64Csr);
ConnectionContext context = await connectionClient.ProvisionAndConnectAsync(
    provisioningSettings, boot, operational, cancellationToken: cancellationToken);
```

The boot provider can instead be an `X509BootAuthenticationProvider` holding the boot
certificate. `IssuedCertificateOperationalAuthenticationProvider` only ever presents issued
certificates; it accepts either the CSR private key or an asynchronous callback that returns
the issued certificate with its private key, and turns each issued chain into a certificate
inside `GetCredentialsAsync`. Issued-certificate-only providers fail if no certificate is
available; they never fall back to a boot certificate or SAS token. To present an existing
operational certificate until one is issued, pass that certificate along with the CSR private
key or callback to `X509OperationalAuthenticationProvider` instead.

Only the operational provider receives certificates issued by DPS or Hub.
`ConnectionContext.OperationalAuthenticationProvider` is used on every Hub
connection, including reconnects and restored assignments. When calling
`ProvisionAndConnectAsync` with a cached context, the supplied operational provider
replaces the context's provider. Automatic reprovisioning retains the boot provider
and uses the current operational provider for the new Hub assignment.

**Migration:** replace the former general-purpose provider classes with the
corresponding boot and operational classes, and derive custom providers from the
appropriate role-specific abstract base. Supply both providers to `ProvisionAndConnectAsync`, rename
`ConnectionContext.AuthenticationProvider` to `OperationalAuthenticationProvider`,
and replace `SymmetricKeyWithIssuedCertificateAuthenticationProvider` with a
symmetric-key boot provider and an X.509 operational provider. Hub-only connections
need only the operational provider in their connection context.

## MQTTv5 Public Preview Notice

> Please note that all preview APIs are subject to change prior to their formal release

This SDK is currently previewing v2.0.0 versions which are compatible with new MQTTv5 supporting Azure IoT Hub (which is also currently in preview). These new MQTTv5 IoT Hubs will include new features that will not be available in the current MQTTv3 IoT Hubs.

The behavior of these two kinds of IoT Hubs (MQTTv3 and MQTTv5) is different enough that this preview SDK version includes two API sets (each with their own set of samples). 

 - The **Unified** API set allows for writing device code that works as similarly as possible regardless of the type of IoT Hub it is connected to by taking a lowest-common-denominator of the supported features. 
   - Samples can be found in our preview branch [here](https://github.com/Azure/azure-iot-sdk/tree/releases/public-preview/dotnet/samples/Unified)
 - The **MQTTv5** API set allows for writing device code that specifically targets this new kind of IoT hub. See the supported features section below for more details.
   - Samples can be found in our preview branch [here](https://github.com/Azure/azure-iot-sdk/tree/releases/public-preview/dotnet/samples/MQTTv5)
 - All the SDK code is shipped in a single NuGet package. No more managing IoT Hub + DPS packages separately

## Key features

:heavy_check_mark: feature available  :heavy_multiplication_x: not supported, but planned to be supported  :heavy_minus_sign: not supported

### v1.0.0 supported features

|Feature|Support|Description|
|---|---|---|
| Device provisioning      | :heavy_check_mark: | Provision your device through an Azure Device Provisioning Service enrollment
| X509 Authentication      | :heavy_check_mark: | 
| MQTTv3 connection to IoT Hub       | :heavy_check_mark: | After provisioning device, connect to IoT Hub via MQTTv3 connection
| MQTTv5 connection to IoT Hub       | :heavy_minus_sign: | This feature will only be supported in v2.0.0
| Device-to-cloud telemetry       | :heavy_check_mark: | 
| Device twin       | :heavy_check_mark: | 
| Direct methods       | :heavy_check_mark: | 
| Certificate management       | :heavy_check_mark: | ```ConnectionClient``` class allows for you to ask the Device Provisioning Service to sign certificates for your device to use when connecting to IoT Hub. The same class also allows you to ask IoT Hub to sign certificates that can be used to connect to IoT Hub with as well.
| Bring-your-own MQTT client       | :heavy_check_mark: | This library defines an ```IMqttClient``` interface that allows you to swap out our default MQTTnet MQTT client for your own, if needed 
| Retry + reconnection logic       | :heavy_check_mark: | ```ConnectionClient``` includes retry by default and even handles re-provisioning the device if necessary
| AMQPS connection support       | :heavy_minus_sign: |
| HTTPS connection support       | :heavy_minus_sign:* | We are considering adding support for performing provisioning over HTTPS
| TPM authentication support       | :heavy_minus_sign:* | We are considering adding support for using TPM authentication
| Software updates via DPS | :heavy_minus_sign: |

### v2.0.0-preview Unified API set supported features

|Feature|Support|Description|
|---|---|---|
| Device provisioning      | :heavy_check_mark: | Provision your device through an Azure Device Provisioning Service enrollment
| X509 Authentication      | :heavy_check_mark: | 
| MQTTv3 connection to IoT Hub       | :heavy_check_mark: | When provisioned to an MQTTv3 IoT Hub, the ```ConnectionClient``` will automatically do this
| MQTTv5 connection to IoT Hub       | :heavy_check_mark: | When provisioned to an MQTTv5 IoT Hub, the ```ConnectionClient``` will automatically do this 
| Device-to-cloud telemetry       | :heavy_check_mark: | 
| Device twin       | :heavy_check_mark:* | *Some twin features (such as twin pushes) are only available in MQTTv5 Hubs, so they are not included in this API set 
| Direct methods       | :heavy_check_mark:* | *Direct method probe handling support is exclusive to MQTTv5 hubs, so it is omitted from this API set
| Custom topic support       | :heavy_minus_sign: | This feature will only be supported via the MQTTv5 API set
| Certificate management       | :heavy_check_mark:* | Only available on MQTTv3 Hubs currently
| Bring-your-own MQTT client       | :heavy_check_mark: | This library defines an ```IMqttClient``` interface that allows you to swap out our default MQTTnet MQTT client for your own, if needed 
| Retry + reconnection logic       | :heavy_check_mark: | ```ConnectionClient``` includes retry by default and even handles re-provisioning the device if necessary
| AMQPS connection support       | :heavy_minus_sign: |
| HTTPS connection support       | :heavy_minus_sign:* | We are considering adding support for performing provisioning over HTTPS
| TPM authentication support       | :heavy_minus_sign:* | We are considering adding support for using TPM authentication
| Software updates via DPS | :heavy_minus_sign: |

### v2.0.0-preview MQTTv5 API set supported features

|Feature|Support|Description|
|---|---|---|
| Device provisioning      | :heavy_check_mark: | Provision your device through an Azure Device Provisioning Service enrollment
| X509 Authentication      | :heavy_check_mark: | 
| MQTTv3 connection to IoT Hub       | :heavy_minus_sign: | 
| MQTTv5 connection to IoT Hub       | :heavy_check_mark: | 
| Device-to-cloud telemetry       | :heavy_check_mark: | 
| Device twin       | :heavy_check_mark: | 
| Direct methods       | :heavy_check_mark: |
| Custom topic support       | :heavy_multiplication_x: | This feature will allow you to publish/receive publishes from custom MQTT topics. It will be exclusive to MQTTv5 IoT Hubs
| Certificate management       | :heavy_multiplication_x: |
| Bring-your-own MQTT client       | :heavy_check_mark: | This library defines an ```IMqttClient``` interface that allows you to swap out our default MQTTnet MQTT client for your own, if needed 
| Retry + reconnection logic       | :heavy_check_mark: | ```ConnectionClient``` includes retry by default and even handles re-provisioning the device if necessary
| AMQPS connection support       | :heavy_minus_sign: |
| HTTPS connection support       | :heavy_minus_sign:* | We are considering adding support for performing provisioning over HTTPS
| TPM authentication support       | :heavy_minus_sign:* | We are considering adding support for using TPM authentication
| Software updates via DPS | :heavy_minus_sign: |


[iothub-device-release]: https://img.shields.io/nuget/v/Microsoft.Azure.Iot.Device.svg?style=plastic
[iothub-device-nuget]: https://www.nuget.org/packages/Microsoft.Azure.Iot.Device/