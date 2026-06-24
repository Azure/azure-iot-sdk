# Azure IoT C# SDK — Internal API Review

**Repo:** `azure-iot-sdk-net` · **Language:** C#

# 1. Context

IoT Hub is deploying a new service implementation that uses **MQTT v5** with a different protocol exchange than the current IoT Hub (MQTT v3.1.1). Both services expose the same device features — twin, direct methods, telemetry, C2D — but the wire format differs. File Upload is also supported when connected to Classic Azure IoT Hub.

This SDK is a **new C# client** that:

- Works transparently with **both the current IoT Hub and the new IoT Hub** from a single binary.
- Discovers the target hub flavor at **DPS provisioning time** — this is not a user-facing knob.
- Exposes a **feature-client oriented API** (`TelemetryClient`, `TwinClient`, etc.) rather than a monolithic service client.
- Provides a **pluggable MQTT abstraction** so platforms can bring their own MQTT stack (MQTTnet implementation ships as default).

---

## 2. Requirements

- This SDK **MUST** be compatible with the current IoT Hub (MQTT v3.1.1) and the new IoT Hub (MQTT v5). Protocol differences are hidden from the application where possible.
  - Any features only present in the new IoT/AEG Hub **MUST** be available in the SDK API. 
  - The SDK **SHOULD** attempt to mimic the new IoT Hub behavior when users connect to the current IoT Hub if it is reasonably trivial.
    - For example, the new IoT Hub's GetTwin API allows for user to specify which of desired/reported sections they want. The current IoT Hub does not support this, but SDK **SHOULD** filter those sections out from the response received from the service.
    - Another example, the new IoT Hub's direct method flow now includes receiving a probe message that allows users to reject a direct method ahead of time. The SDK **MUST NOT** try to mimic this behavior when connected to the current IoT Hub.
- This SDK **MUST** support x509 authentication when connecting to DPS and to both the current IoT Hub and new IoT Hub
- This SDK's API **SHOULD** assume a device application starts by provisioning through DPS. The SDK **MAY** support connecting directly to IoT Hub without first provisioning.
- By default, this SDK **MUST** support TLS 1.3 and TLS 1.2.
  - This SDK **SHOULD** support the user customizing TLS level logic only by providing their own MQTT client
  - This SDK **SHOULD NOT** support directly setting TLS-level configurations (for instance, connectionClient.SetPreferredTlsVersion(...))
- This SDK **MUST** support automatic re-connection with exponential backoff + jitter retry policy that can be configured by user
- This SDK **SHOULD** use a "feature client" pattern for its API surface
  - `ConnectionClient` for lifecycle; separate `TelemetryClient`, `TwinClient`, `DirectMethodsClient`, `FileUploadClient` for messaging/features. Users compose only the features they need.
- This SDK **MUST** include a default MQTT client
  - This SDK **MAY** support a user-provided MQTT client
- For file upload APIs specifically, This SDK **MUST** include a default HTTP client
  - This SDK **MAY** support a user-provided HTTP client
- The SDK API **SHOULD** align with the APIs exposed by other Azure IoT SDK APIs, including C.
  - For example, both SDKs **should** have the same set of feature clients


### Future Requirements

- This SDK **MUST** provide a feature client supports subscribing and/or publishing to arbitrary MQTT topics. 
  — This feature **MUST** be available only when connecting to the new IoT/AEG Hub
- This SDK **SHOULD** provide a feature client that supports communicating Azure Device Update information to and from IoT Hub
  - This SDK **MUST NOT** handle the actual device update flows that happen after receiving Azure Device Update information from IoT Hub


### Non-Requirements

- This SDK **MUST NOT** include any IoT Edge support and/or module client APIs
- This SDK **MUST NOT** include support for using device connection strings
- This SDK **MUST NOT** include support for using SAS token, symmetric key, or TPM-base authentication when connecting to any of DPS, the current IoT Hub, or the new IoT Hub
- This SDK **MUST NOT** include support for using HTTP and/or AMQP transport protocol APIs
  - However, the SDK **MUST** include support for using the HTTP file upload APIs when connecting to the current IoT hub since those APIs are not available over MQTT when connected to the current IoT Hub.

## 3. Architecture

The SDK is layered as per the below diagram:

![layering diagram](./sdk%20layering.png)

The fundamental principle here is that all connection management is done by the `ConnectionClient` and all IoT Hub features are delegated to specific feature clients that use that connection.

## What are all the "Feature Clients"? 

- ConnectionClient
  - Provision through DPS
  - Connect to IoT hub (regardless of current IoT hub vs new IoT Hub)
  - All retry/reconnection logic
  - Support for sending certificate signing request to DPS when provisioning
  - Support for sending certificate signing request to IoT hub after provisioning 

- TelemetryClient
  - Publish device to cloud telemetry
  - Receive cloud to device telemetry

- TwinClient
  - Get twin
  - Update reported properties
  - Receive desired property updates

- DirectMethodsClient
  - Receive and respond to direct method invocations
  - Receive and respond to direct method probe requests (AEG SDK client only)

- FileUploadClient
  - Get file upload SAS URI
  - Complete file upload SAS URI

- CustomTopicTelemetryClient (AEG SDK client only, future requirement)
  - Publish to custom MQTT topic
  - Subscribe to custom topic

# 4. API Surface

## Code snippets

<details>
  <summary>ConnectionClient - With default MQTT client</summary>

```csharp
string idScope = Environment.GetEnvironmentVariable("DPS_ID_SCOPE");
string pcks12CertificatePath = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PATH");
string pcks12CertificatePassword = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PASSWORD");
X509AuthenticationProvider authentication = new(X509CertificateLoader.LoadPkcs12FromFile(pcks12CertificatePath, pcks12CertificatePassword));

ConnectionClient connectionClient = new ConnectionClient();
ProvisioningSettings provisioningSettings = new(idScope);
await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication); 

// Connected to IoT hub, can now use feature clients to do twin/methods/telemetry/etc.
```

</details>

<details>
  <summary>ConnectionClient - With user-provided MQTT client</summary>

```csharp
string idScope = Environment.GetEnvironmentVariable("DPS_ID_SCOPE");
string pcks12CertificatePath = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PATH");
string pcks12CertificatePassword = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PASSWORD");
X509AuthenticationProvider authentication = new(X509CertificateLoader.LoadPkcs12FromFile(pcks12CertificatePath, pcks12CertificatePassword));

IMqttClient mqttClient;
if (EnableMqttLogs)
{
    mqttClient = new MqttNetClient(new MQTTnet.MqttClientFactory().CreateMqttClient(MqttNetTraceLogger.CreateTraceLogger()));
    Trace.Listeners.Add(new ConsoleTraceListener());
}
else
{
    mqttClient = new MqttNetClient(new MQTTnet.MqttClientFactory().CreateMqttClient());
}

ConnectionClient connectionClient = new ConnectionClient(mqttClient);

ProvisioningSettings provisioningSettings = new(idScope);
Console.WriteLine("Provisioning and connecting to IoT hub using the provided MQTT client");
await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication);
```

</details>

<details>
  <summary>TelemetryClient</summary>

```csharp
ConnectionClient connectionClient = new ConnectionClient();

TelemetryClient telemetryClient = new TelemetryClient(connectionClient);

Func<CloudToDeviceMessage, Task<CompletionType>> HandleReceivedCloudToDeviceTelemetryAsync = async (args) =>
{
    Console.WriteLine($"Received cloud to device telemetry with message Id {args.MessageId}");
    return CompletionType.Complete;
};

telemetryClient.CloudToDeviceMessageReceivedAsync += HandleReceivedCloudToDeviceTelemetryAsync;

ProvisioningSettings provisioningSettings = new(idScope);
await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication);

while (true)
{
    OutgoingTelemetryMessage outgoingTelemetry = new()
    {
        Payload = Encoding.UTF8.GetBytes("Hello world!"),
        MessageId = Guid.NewGuid().ToString(),
    };

    await telemetryClient.SendTelemetryAsync(outgoingTelemetry);
    await Task.Delay(TimeSpan.FromSeconds(1));
}
```

</details>

<details>
  <summary>TwinClient</summary>

```csharp
Twin? currentTwin = null;
ConnectionClient connectionClient = new ConnectionClient();
TwinClient twinClient = new TwinClient(connectionClient);

Action<DesiredPropertyUpdateReceivedEventArgs> HandleDesiredPropertiesUpdateAsync = async (args) =>
{
    Console.WriteLine($"Received desired property update");
    currentTwin.DesiredVersion = args.DesiredPropertiesVersion;
    currentTwin.Desired = args.DesiredProperties;

    ReportedPatchRequest reportedPatch = new()
    {
        ReportedProperties = args.DesiredProperties, // Echo back the desired properties as the current reported properties
        IfMatch = 1
    };

    ReportedPatchResponse patchResponse = await twinClient.UpdateReportedPropertiesAsync(reportedPatch);
    currentTwin.ReportedVersion = patchResponse.Version;
    if (patchResponse.Result == Result.Ok)
    {
        currentTwin.Reported = args.DesiredProperties;
        currentTwin.ReportedVersion = args.DesiredPropertiesVersion;
    }
};

twinClient.DesiredPropertyUpdateReceived += HandleDesiredPropertiesUpdateAsync;

ProvisioningSettings provisioningSettings = new(idScope);
TwinPushOptions twinPushOptions = new()
{
    ReceiveDesiredPropertyUpdates = true,
    ReceiveReportedPropertiesUponConnect = true,
};

var connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, twinPushOptions);
currentTwin = connectionContext.InitialTwinPush;
```

</details>

<details>
  <summary>DirectMethodsClient</summary>

```csharp
ConnectionClient connectionClient = new();
DirectMethodClient directMethodClient = new(connectionClient);

Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>> HandleDirectMethodAsync = (request) =>
{
    Console.WriteLine($"Received direct method with name {request.MethodName}");
    if (request.MethodName.Equals("testMethod"))
    {
        DirectMethodRequestPayloadObject directMethodRequestPayload = JsonSerializer.Deserialize<DirectMethodRequestPayloadObject>(request.Payload);

        DirectMethodResponsePayloadObject directMethodResponsePayloadObject = new()
        {
            SomeBooleanField = false,
            SomeLongField = 10000000,
        };

        return Task.FromResult(new DirectMethodResponse()
        {
            Status = 200,
            Payload = JsonSerializer.SerializeToUtf8Bytes(directMethodResponsePayloadObject),
        });
    }
    else
    {
        // Undefined method was invoked, so return "not found"
        return Task.FromResult(new DirectMethodResponse() { Status = 404 });
    }
};
directMethodClient.DirectMethodInvokedAsync += HandleDirectMethodAsync;

ProvisioningSettings provisioningSettings = new(idScope);
await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication); 
```

</details>

<details>
  <summary>ConnectionClient - Certificate management snippet</summary>

```csharp
string idScope = Environment.GetEnvironmentVariable("DPS_ID_SCOPE");
string registrationId = Environment.GetEnvironmentVariable("DPS_REGISTRATION_ID");

// These credentials are only used when provisioning. When connecting to IoT Hub, this sample uses the certificate that was signing by DPS during provisioning
string pcks12CertificatePath = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PATH");
string pcks12CertificatePassword = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PASSWORD");
X509AuthenticationProvider authentication = new(X509CertificateLoader.LoadPkcs12FromFile(pcks12CertificatePath, pcks12CertificatePassword));

ConnectionClient connectionClient = new();

// Create initial certificate signing request for DPS to fulfill while provisioning
var (csrBase64, privateKey) = GenerateCsr(registrationId, CsrAlgorithm.RSA);
SavePrivateKey(privateKey, PrivateKeyPath);

// Provision and connect to IoT hub using the certificates signed by DPS. Save those certificates signed by DPS locally
ProvisioningSettings provisioningSettings = new(idScope);
ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(csrBase64, provisioningSettings, authentication);
File.WriteAllText(InitialSignedCertificatesPath, ConvertToPem(connectionContext.IssuedClientCertificateChain)); // Save the DPS-issued certificate locally

// Create a new certificate signing request to send to IoT Hub this time
(csrBase64, privateKey) = GenerateCsr(registrationId, CsrAlgorithm.RSA);
var certificateSigningRequest = new CertificateSigningRequest(registrationId, csrBase64, null, "*");
CertificateSigningOperation pendingCsr = await connectionClient.SendCertificateSigningRequestAsync(certificateSigningRequest);
await pendingCsr.Accepted;
CertificateSigningResponse certificateSigningResponse = await pendingCsr.Completed;

// Save the newly signed certificates locally
File.WriteAllText(RenewedSignedCertificatesPath, ConvertToPem(certificateSigningResponse.Certificates)); // Save the IoT hub-renewed issued certificate locally

// Upon getting the newly signed certificate, disconnect from IoT Hub and then reconnect with that new certificate
await connectionClient.DisconnectAsync();
X509AuthenticationProvider newX509AuthenticationProvider = new(CreateX509CertificateFromKeyAndCert(RenewedSignedCertificatesPath, PrivateKeyPath));
await connectionClient.ConnectAsync(connectionContext, newX509AuthenticationProvider);
```

</details>

## Connection Client and Feature Clients

<details>
  <summary>ConnectionClient</summary>

```csharp
public class ConnectionClient
{
    /// <summary>
    /// An event that executes each time this client is connected to either Device Provisioning Service or IoT hub.
    /// </summary>
    public event Action<MqttClientConnectedEventArgs>? ConnectedAsync;

    /// <summary>
    /// An event that executes each time this client is disconnected from either Device Provisioning Service or IoT hub.
    /// </summary>
    public event Action<MqttClientDisconnectedEventArgs>? DisconnectedAsync;

    /// <summary>
    /// Construct a new <see cref="ConnectionClient"/>
    /// </summary>
    /// <param name="mqttClient">The MQTT client to use. If null, a default MQTT client will be created for you.</param>
    public ConnectionClient(IMqttClient? mqttClient = null);

    /// <summary>
    /// Provision this device with the provided credentials using Device Provisioning Service, then connect this device to the IoT hub it was provisioned to.
    /// </summary>
    /// <param name="provisioningSettings">The mandatory and optional provisioning-specific fields</param>
    /// <param name="authentication">The x509 authentication to use when connecting to both Device Provisioning Service and IoT hub.</param>
    /// <param name="twinOptions">The optional flags to control twin updates to this device from IoT hub.</param>
    /// <param name="cancellationToken">The cancellation token.</param>
    /// <returns>The received twin push upon connecting to IoT hub if any part of the twin was configured to be pushed in <see cref="TwinPushOptions"/>.</returns>
    public async Task<ConnectionContext> ProvisionAndConnectAsync(ProvisioningSettings provisioningSettings, X509AuthenticationProvider authentication, TwinPushOptions? twinOptions = default, CancellationToken cancellationToken = default);

    /// <summary>
    /// Disconnect this device from IoT hub.
    /// </summary>
    /// <param name="cancellationToken">The cancellation token.</param>
    public async Task DisconnectAsync(CancellationToken cancellationToken = default);

    /// <summary>
    /// Override the default exponential-backoff with jitter retry algorithm used when attempting to re-connect over MQTT
    /// </summary>
    /// <param name="retryPolicy">The custom retry policy to use.</param>
    public void SetRetryPolicy(IRetryPolicy retryPolicy);

    /// <summary>
    /// Send a certificate signing request to IoT hub
    /// </summary>
    /// <param name="request">The certificates to have IoT hub sign.</param>
    /// <param name="cancellationToken">The cancellation token</param>
    /// <returns>A set of tasks. One that completes when IoT hub accepts the request (and starts signing), one that completes when IoT hub completes the signing, and one that completes if any step in the process fails.</returns>
    public async Task<CertificateSigningOperation> SendCertificateSigningRequestAsync(CertificateSigningRequest request, CancellationToken cancellationToken = default);

    /// <summary>
    /// Connect directly to IoT Hub
    /// </summary>
    /// <param name="connectionContext">The details about which IoT hub host to connect to, and which device Id to connect as.</param>
    /// <param name="authentication">The authentication to use when connecting.</param>
    /// <param name="twinPushOptions">The options around receiving a twin push upon connecting.</param>
    /// <param name="cancellationToken">Cancellation token.</param>
    /// <returns>The initial twin of the device if a twin push was configured via <see cref="TwinPushOptions"/></returns>
    public async Task<Twin.Twin> ConnectAsync(ConnectionContext connectionContext, X509AuthenticationProvider authentication, TwinPushOptions? twinPushOptions = default, CancellationToken cancellationToken = default);
}
```

</details>

<details>
  <summary>TelemetryClient</summary>

```csharp
public class TelemetryClient
{
    /// <summary>
    /// An event that executes every time this device receives cloud-to-device telemetry. Once received, the application must decide how to complete that telemetry.
    /// </summary>
    public event Func<CloudToDeviceMessage, Task<CompletionType>>? CloudToDeviceMessageReceivedAsync;

    public TelemetryClient(ConnectionClient connection);

    /// <summary>
    /// Send device-to-cloud telemetry.
    /// </summary>
    /// <param name="message">The message to send.</param>
    /// <param name="cancellationToken">the cancellation token.</param>
    public async Task SendTelemetryAsync(OutgoingTelemetryMessage message, CancellationToken cancellationToken = default);
}
```
</details>

<details>
  <summary>TwinClient</summary>

```csharp
public class TwinClient
{
    /// <summary>
    /// An event that executes whenever this client receives a desired properties update from IoT hub.
    /// </summary>
    public event Action<DesiredPropertyUpdateReceivedEventArgs>? DesiredPropertyUpdateReceived;

    /// <summary>
    /// An event that executes whenever this client receives a Twin push message from IoT hub.
    /// </summary>
    /// <remarks>
    /// This feature is only supported by IoT hubs using Azure Event Grid. Older IoT hubs will never encounter this event.
    /// </remarks>
    public event Action<TwinPushReceivedEventArgs>? TwinPushReceived;

    public TwinClient(IConnectionClient connection);

    /// <summary>
    /// Get the full twin, or some conditional set of the twin properties.
    /// </summary>
    /// <param name="getReported">Request the service to include the reported properties in the returned twin.</param>
    /// <param name="getDesired">Request the service to include the desired properties in the returned twin.</param>
    /// <param name="ifNotMatchReported">
    /// If provided, and the version matches or exceeds the version of the reported properties held by the service, then the 
    /// service will not include the reported properties in the returned twin.
    /// </param>
    /// <param name="ifNotMatchDesired">
    /// If provided, and the version matches or exceeds the version of the desired properties held by the service, then the 
    /// service will not include the desired properties in the returned twin.
    /// </param>
    /// <param name="cancellationToken">The cancellation token.</param>
    /// <returns>The returned twin.</returns>
    /// <remarks>
    /// Only IoT hubs that use Azure Event Grid will actually respond to the getReported/getDesired/ifNotMatch flags as this feature is unsupported in older IoT hubs. 
    /// However, this SDK will parse the twin that the service returns to filter out unrequested sections to mimic the behavior of Azure Event Grid IoT hubs.
    /// </remarks>
    public Task<TwinGetResponseWrapper> GetTwinAsync(bool getReported = true, bool getDesired = true, ulong ifNotMatchReported = 0, ulong ifNotMatchDesired = 0,  CancellationToken cancellationToken = default);

    /// <summary>
    /// Update this device's reported properties.
    /// </summary>
    /// <param name="patch">The patch of the reported properties to send to IoT hub</param>
    /// <param name="cancellationToken">The cancellation token.</param>
    /// <returns>Whether IoT hub accepted this patch.</returns>
    public Task<ReportedPatchResponse> UpdateReportedPropertiesAsync(ReportedPatchWrapper patch, CancellationToken cancellationToken = default);
}

```
</details>

<details>
  <summary>DirectMethodsClient</summary>

```csharp
public class DirectMethodClient
{
    /// <summary>
    /// An event that executes whenever this device receives a direct method request from IoT hub. After executing the direct method, the device must
    /// provide a direct method response.
    /// </summary>
    public event Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>>? DirectMethodInvokedAsync;

    /// <summary>
    /// An event that executes whenever this device receives a direct method probe from IoT hub. This event is a precursor to receiving the direct method itself 
    /// and allows your application to decide whether it is ready to receive this direct method or not.
    /// </summary>
    /// <remarks>This feature is only supported by IoT hubs that use Azure Event Grid. Older IoT hubs will never send this probe.</remarks>
    public event Func<DirectMethodRequestProbeReceivedEventArgs, Task<ProbeAck>>? DirectMethodProbeReceivedAsync;

    public DirectMethodClient(ConnectionClient connection);
}
```
</details>

<details>
  <summary>FileUploadClient</summary>

```csharp
public class FileUploadClient
{
    public FileUploadClient(ConnectionClient connection);

    /// <summary>
    /// Get a SAS URI that can be used to upload a file to a configured Azure Storage account.
    /// </summary>
    /// <param name="request">The request for the SAS URI.</param>
    /// <param name="cancellationToken">The cancellation token.</param>
    /// <returns>The SAS URI.</returns>
    public Task<FileUploadSasUriResponse> GetFileUploadSasUriAsync(FileUploadSasUriRequest request, CancellationToken cancellationToken = default);

    /// <summary>
    /// Signal to IoT hub that the SAS URI retrieved with <see cref="GetFileUploadSasUri(FileUploadSasUriRequest, CancellationToken)"/> is no longer needed 
    /// and should be released.
    /// </summary>
    /// <param name="completion">The notification that includes the SAS URI from <see cref="FileUploadSasUriResponse"/>.</param>
    /// <param name="cancellationToken">the cancellation token</param>
    public Task CompleteFileUploadSasUriAsync(FileUploadCompletionNotification completion, CancellationToken cancellationToken = default);
}
```
</details>

### Sample code

[Samples](https://github.com/Azure/azure-iot-sdk-net/tree/main/samples)

# Open questions

- Will AEG hub somehow support more than just "complete" as a response to a c2d message over MQTT? If not, we should probably remove the complete/abandon/reject concept from the SDK completely since the SDK is MQTT-only

# Appendix

## Definitions

- "Service client"
  - Refers to the previously standard pattern where an SDK would expose a client object that's purpose was to interact with a specific service (IoT hub, for instance). That client typically exposed the same set of APIs that the cloud service itself exposed.
  - This term was previously used to describe the clients that offered meta-device operations like registering device identies. It will not be used this way in this doc, though.
- "Feature client"
  - Refers to a newer pattern wherein the SDK may expose client objects that contain the set of a APIs needed to interact with a specific feature (Twin, for instance). This client may communicate with multiple cloud services if that feature spans multiple cloud services. For example, IoT hub and DPS both use twin concepts.
- "Device Client"
  - Previously used to refer to the client that held all of the device-specific APIs that IoT hub offered (for example, sending device to cloud telemetry). This terminology won't be used in this new SDK, though.

# Important links
- [Hub side design docs introduction](https://eng.ms/docs/cloud-ai-platform/microsoft-specialized-clouds-msc/msc-edge/digital-operations/azure-iot-hub/iot-hub-team-docs/gateway/rfcs/aeg/general)
- [Hub side direct method design doc](https://eng.ms/docs/cloud-ai-platform/microsoft-specialized-clouds-msc/msc-edge/digital-operations/azure-iot-hub/iot-hub-team-docs/gateway/rfcs/aeg/dm)
- [Hub side twin design doc](https://eng.ms/docs/cloud-ai-platform/microsoft-specialized-clouds-msc/msc-edge/digital-operations/azure-iot-hub/iot-hub-team-docs/gateway/rfcs/aeg/twin)
- [Hub side device presence doc](https://eng.ms/docs/cloud-ai-platform/microsoft-specialized-clouds-msc/msc-edge/digital-operations/azure-iot-hub/iot-hub-team-docs/gateway/rfcs/aeg/presence)
- [Jesus PM design doc](https://microsoft-my.sharepoint-df.com/:w:/p/jesusbar/cQq8Ul2G9oVyRpB8fS3XrYewEgUCIry0d9EzLYCxY4lPpXwGFA?isSPOFile=1&ovuser=72f988bf-86f1-41af-91ab-2d7cd011db47%2Ctimtay%40microsoft.com&wdExp=TEAMS-TREATMENT&web=1&TeamsCID=fd21fdd7-c1ca-4f18-b09e-ba2a56c6199e&clickparams=eyJBcHBOYW1lIjoiVGVhbXMtRGVza3RvcCIsIkFwcFZlcnNpb24iOiI0OS8yNjA0MzAxOTIwNyJ9&linkOpenTime=1779147318200)
- [Custom topic design doc](https://microsoft.sharepoint.com/:w:/t/DigitalOperations/cQpWzBHzdpWFR4xKGrCrZkWOEgUCja3H0GE_an-KHswmk9oIlg?isSPOFile=1&ovuser=72f988bf-86f1-41af-91ab-2d7cd011db47%2Ctimtay%40microsoft.com&wdExp=TEAMS-TREATMENT&web=1&TeamsCID=28d597b8-c456-41f6-a4e7-55c0bccbcf18&clickparams=eyJBcHBOYW1lIjoiVGVhbXMtRGVza3RvcCIsIkFwcFZlcnNpb24iOiI0OS8yNjA0MzAxOTIxNiJ9)