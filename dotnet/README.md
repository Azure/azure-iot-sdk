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

## Authentication

`ProvisionAndConnectAsync` takes a single `AuthenticationProvider` that authenticates the
device with both Device Provisioning Service (DPS) and IoT Hub. The abstract
`AuthenticationProvider` class exposes the registration Id, DPS credentials
(`GetProvisioningCredentialsAsync`), IoT Hub credentials (`GetIotHubCredentialsAsync`) and a
`HasCertificateSigningHandler` flag that reports whether the provider was given a way to turn
certificates issued by DPS or IoT Hub into an `X509Certificate2`. Derive from it to build a
custom provider.

| Credential | Provider | Constructors |
| --- | --- | --- |
| X.509 | `X509AuthenticationProvider` | `(certificate, chain?)`, `(certificate, privateKey)`, `(certificate, callback)` |
| Symmetric key | `SymmetricKeyAuthenticationProvider` | `(registrationId, idScope, key)`, `(registrationId, idScope, key, privateKey)`, `(registrationId, idScope, key, callback)` |

Every constructor also accepts optional remote and local TLS certificate validation callbacks.
The `privateKey` and `callback` constructors set `HasCertificateSigningHandler` to `true`: the
private key is attached to the issued leaf certificate, while the callback receives the issued
Base64 certificate chain and must return an `X509Certificate2` with its private key.

```csharp
var authentication = new X509AuthenticationProvider(deviceCertificate);
ConnectionContext context = await connectionClient.ProvisionAndConnectAsync(
    provisioningSettings, authentication, cancellationToken: cancellationToken);
```

For symmetric keys, use `new SymmetricKeyAuthenticationProvider(registrationId, idScope, deviceKey)`.
The key is Base64-encoded and is used for both the DPS enrollment and the assigned Hub device
identity; the hub hostname and device Id are supplied by the connection client on each
connection attempt.

To have DPS issue a certificate during provisioning, pass a `ProvisioningCertificateSigningRequest`
to the `ProvisionAndConnectAsync` overload that takes one. The provider must have been
constructed with the CSR private key or a conversion callback; otherwise that overload throws
`ArgumentException`:

```csharp
var authentication = new SymmetricKeyAuthenticationProvider(registrationId, idScope, deviceKey, csrPrivateKey);
ConnectionContext context = await connectionClient.ProvisionAndConnectAsync(
    provisioningSettings, authentication, new ProvisioningCertificateSigningRequest(base64Csr),
    cancellationToken: cancellationToken);
```

Once a certificate is issued (by DPS or Hub), a provider with a certificate signing handler
presents it to IoT Hub instead of the symmetric key or the original X.509 certificate. DPS
always receives the original credential. A provider without a handler ignores issued
certificates, and `SendCertificateSigningRequestAsync` throws `InvalidOperationException` when
the connection's provider has no handler.

`ConnectionContext.AuthenticationProvider` is used on every Hub connection, including
reconnects, restored assignments and automatic reprovisioning. When calling
`ProvisionAndConnectAsync` with a cached context, the supplied provider replaces the context's
provider.

**Migration:** `BootAuthenticationProvider`, `OperationalAuthenticationProvider` and all their
`X509*`, `SymmetricKey*` and `*CertificateSigningOperationalAuthenticationProvider` subclasses
were replaced by `AuthenticationProvider`, `X509AuthenticationProvider` and
`SymmetricKeyAuthenticationProvider`. Pass one provider to `ProvisionAndConnectAsync`, rename
`ConnectionContext.OperationalAuthenticationProvider` to `AuthenticationProvider`, and replace
`GetCredentialsAsync` overrides with `GetProvisioningCredentialsAsync` and
`GetIotHubCredentialsAsync`. DPS and Hub can no longer use different credentials or TLS callbacks,
and `X509AuthenticationProvider` always requires a client certificate (the former issued-only
constructors were removed). The CSR requirement is now checked at runtime using
`HasCertificateSigningHandler` rather than at compile time.

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