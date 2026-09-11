# Stub IoT hub service

A stand-in for the IoT hub service that runs inside the test process. It lets device-side tests exercise
real service round trips - get twin, reported-property patches, desired-property patches, direct methods,
telemetry, cloud-to-device messages - without provisioning a hub or depending on the cloud.

The stub is an **MQTT client, not a broker**. It attaches to the same broker the devices under test connect
to, subscribes to the service-bound topics, and publishes the responses a real hub would publish.

```
+------------------+          +------------+          +---------------------+
|  device client   | <------> |   broker   | <------> |  StubIotHubService  |
| (the SDK itself) |   MQTT   |            |   MQTT   |   (this folder)     |
+------------------+          +------------+          +---------------------+
```

## Choosing a generation

`StubIotHubServiceOptions.Generation` selects which hub the stub imitates. The two generations speak
genuinely different protocols, so this is not a cosmetic switch.

| | `IotHubGeneration.Gen1` (classic hub) | `IotHubGeneration.Gen2` (hub on the Event Grid MQTT broker) |
|---|---|---|
| MQTT version | 3.1.1 | 5 |
| Topics | `$iothub/...`, `devices/{deviceId}/...` | `ih/{deviceId}/srv/{feature}` device-to-service, `ih/{deviceId}/dev/{feature}` service-to-device |
| Message dispatch | topic shape | the `type` user property, formatted `{name}:{schemaVersion}` |
| Correlation | the `$rid` topic query parameter | MQTT 5 correlation data, a 16-byte big-endian GUID |
| Payloads | JSON | protobuf (`common/Protos/*.proto`) |
| Presence | none | `birth` / `birth-ack` handshake keyed on a per-connect-attempt nonce |
| Twin versioning | `$version` in the payload or topic | `version` fields in the protobuf messages, with optimistic concurrency on reported patches |
| Direct methods | single request/response | `probe` -> `probe-ack` -> `exec` -> `result`, with `abandon` |

Gen2 behavior is modeled on the design documents in the repository's `iot hub service design docs` folder
(device presence, device twin, and direct methods over the Event Grid MQTT broker) and cross-checked against
the `common/Protos` schemas that the SDK itself compiles against.

## Usage

```csharp
await using var hub = new StubIotHubService(new StubIotHubServiceOptions
{
    Generation = IotHubGeneration.Gen2,
    BrokerHostName = "localhost",
    BrokerPort = 1883,
});

// Device state can be seeded before StartAsync, since nothing goes on the wire until then.
hub.GetDeviceState(deviceId).ReplaceDesiredProperties(new JsonObject { ["fanSpeed"] = 42 });

await hub.StartAsync();

// ... connect the device under test to the same broker ...

await hub.WaitForDeviceBirthAsync(deviceId);                                   // gen2 only
await hub.UpdateDesiredPropertiesAsync(deviceId, new JsonObject { ["fanSpeed"] = 43 });
StubDirectMethodResult result = await hub.InvokeDirectMethodAsync(deviceId, "reboot", payload);
```

Device-initiated traffic is surfaced as events: `TelemetryReceived`, `ReportedPropertiesReceived`,
`TwinGetReceived`, and `DeviceBirthReceived`. The stub's authoritative twin for a device is reachable
at any time through `GetDeviceState(deviceId)`.

## Limitations

- **Gen1 device attribution.** Classic hub topics such as `$iothub/twin/GET/?$rid=...` carry no device id;
  a real hub knows the device from the MQTT session, which a second client on the broker cannot see. Set
  `StubIotHubServiceOptions.DeviceIdFilter` when serving gen1 so that the stub can attribute this traffic.
  If it is unset and exactly one device is known, that device is assumed.
- **Gen2 desired-patch version probes.** The design document describes a `desired-patch` with an absent
  payload as a version probe. `DesiredPatch.payload` in `common/Protos/twin.proto` is not `optional`, so
  the absence cannot be expressed and the probe is not implemented.
- **Gen2 cloud-to-device messages and file upload** are not implemented, matching the SDK, which does not
  support them for gen2 either.
- **No authentication or authorization.** The stub trusts whatever reaches it. It is a protocol stand-in,
  not a security model.

# Stub device provisioning service

`StubDeviceProvisioningService` is the companion stand-in for DPS. It is also an MQTT client on the same
broker, it always assigns the device to the stub hub, and it decides which *generation* of hub the device
believes it was assigned to.

```csharp
await using var hub = new StubIotHubService(new StubIotHubServiceOptions { Generation = IotHubGeneration.Gen2, ... });
await using var dps = StubDeviceProvisioningService.ForHub(hub, options => options.DeviceId = deviceId);

await hub.StartAsync();
await dps.StartAsync();

// The SDK now runs its real provisioning flow and lands on the stub hub, speaking gen2.
ConnectionContext context = await connectionClient.ProvisionAndConnectAsync(
    new ProvisioningSettings(idScope),
    new X509AuthenticationProvider(certificate));
```

`ForHub` copies the hub's `Generation`, broker endpoint, and `HubHostName` into the DPS options before
running the `configure` callback, so the two stubs cannot disagree about which protocol the device should
end up speaking. Construct `StubDeviceProvisioningService` directly if you want them to disagree - for
instance to test a device that is assigned to a gen2 hub that is not actually there.

## The registration flow

The stub implements the `$dps/registrations/...` protocol that `Provisioning/ProvisioningConnection.cs`
expects, which is MQTT 3.1.1 for **both** generations - only the assigned hub differs.

1. The device publishes to `$dps/registrations/PUT/iotdps-register/?$rid={n}`.
2. The stub answers `202` with status `assigning` and an operation id. The SDK requires the *first*
   response to be `assigning`.
3. The device polls `$dps/registrations/GET/iotdps-get-operationstatus/?$rid={n}&operationId={id}`.
   `StubDeviceProvisioningServiceOptions.AssigningPollResponses` controls how many polls are answered
   `assigning` before the registration completes; the default of `0` assigns on the first poll.
4. The stub answers `200` with status `assigned`, `assignedHub` set to the stub hub's host name, and
   `connectionProfile` set from the configured generation - `classic` for gen1, `mqttV5` for gen2. That
   last value is what the SDK reads to choose between the classic and Event Grid protocols for the
   subsequent hub connection.

On assignment the stub also pre-registers the device with the hub it is bound to, so that a gen1 hub can
attribute the device's subsequent topic traffic without `DeviceIdFilter` being set explicitly. The
`DeviceProvisioned` event reports each assignment.

## Limitations

- **The device id must be configured.** The DPS topics carry no registration id, and a stub sharing a
  broker cannot see another client's CONNECT packet, so `StubDeviceProvisioningServiceOptions.DeviceId`
  (or `RegistrationId`) has to be set. This is the same class of limitation as the gen1 hub's
  `DeviceIdFilter`.
- **One device at a time.** The SDK's response handler does not correlate on `$rid` - it treats the first
  response as the registration result and later ones as poll results - so the stub must not share a broker
  with other devices' DPS traffic.
- **No attestation.** Certificates, symmetric keys, and TPM are not validated; every registration succeeds.
- **`retry-after` costs at least two seconds per poll**, because the SDK clamps it to a two second floor.

# Reference

## Files

| File | Contents |
|---|---|
| `StubIotHubService.cs` | Lifecycle, subscriptions, message routing, and the generation-agnostic public API |
| `StubIotHubService.Gen1.cs` | Classic hub protocol |
| `StubIotHubService.Gen2.cs` | Event Grid MQTT broker hub protocol |
| `StubIotHubServiceOptions.cs` | Configuration |
| `StubDeviceProvisioningService.cs` | The stub DPS: registration, polling, and hub assignment |
| `StubDeviceProvisioningServiceOptions.cs` | Stub DPS configuration |
| `StubDeviceState.cs` | Per-device authoritative twin, versioning, and JSON merge patch |
| `StubServiceModels.cs` | Event args and result types |
| `IotHubGeneration.cs` | The generation enum |
| `InProcessMqttBroker.cs` | A plaintext MQTT broker on a loopback port, for tests that want no external dependencies |
| `LocalBrokerMqttClient.cs` | Retargets the SDK's CONNECT packet at a local plaintext broker |
| `StubServiceTestEnvironment.cs` | A broker, a hub, and a DPS bound to it, for tests that just want a device to provision somewhere |
| `StubIotHubServiceTests.cs` | End-to-end tests of a real device client against the stub |

The provisioning tests that use `StubServiceTestEnvironment` live with the client they exercise, in
`Gen2/ProvisioningIntegrationTests.cs` and `Unified/ProvisioningIntegrationTests.cs`.

## Running against a local broker

`StubIotHubServiceTests` shows the full wiring. `InProcessMqttBroker` starts an MQTTnet broker on a free
loopback port, and `LocalBrokerMqttClient` - supplied through `ConnectionClientOptions.MqttClient` - rewrites
the host and port on the SDK's CONNECT packet and drops the client certificate so that TLS is not negotiated.
Everything above the CONNECT packet is the unmodified SDK code path.
