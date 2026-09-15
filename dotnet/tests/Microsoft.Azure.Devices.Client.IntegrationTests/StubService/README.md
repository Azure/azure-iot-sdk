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

// ... provision the device under test onto this hub with the stub DPS below, which is the only way
// the SDK's connection clients expose connecting - ConnectAsync is internal ...

await hub.WaitForDeviceBirthAsync(deviceId);                                   // gen2 only
await hub.UpdateDesiredPropertiesAsync(deviceId, new JsonObject { ["fanSpeed"] = 43 });
StubDirectMethodResult result = await hub.InvokeDirectMethodAsync(deviceId, "reboot", payload);
```

Device-initiated traffic is surfaced as events: `TelemetryReceived`, `ReportedPropertiesReceived`,
`TwinGetReceived`, and `DeviceBirthReceived`. The stub's authoritative twin for a device is reachable
at any time through `GetDeviceState(deviceId)`.

## Dropping device connections

A real service disconnects devices without warning, and MQTT 5 lets it say why. The stub does the same, so
that a test can check how the SDK reacts to each disconnect reason code.

The stub is a client, not a broker, so it cannot close another client's session by itself. It borrows that
ability from `StubIotHubServiceOptions.ConnectionDropper`, an `IStubDeviceConnectionDropper`.
`MqttFaultInjectionClient` implements that interface by publishing a fault injection request to the broker -
see [Fault injection over MQTT](#fault-injection-over-mqtt) - and both `StubServiceTestEnvironment` and the
test harness in `StubIotHubServiceTests` wire it up already. Without a dropper, every drop call throws.

```csharp
// A specific reason code.
await hub.DropDeviceConnectionAsync(deviceId, MqttDisconnectReasonCode.ServerBusy);

// A random reason code, for a specific device or for a randomly chosen connected one.
MqttDisconnectReasonCode? code = await hub.DropDeviceConnectionWithRandomReasonCodeAsync(deviceId);
StubDeviceConnectionDroppedEventArgs? drop = await hub.DropRandomDeviceConnectionAsync();
```

`StubIotHubServiceOptions.RandomConnectionDrops` turns the same thing into a background loop that runs for as
long as the stub is started:

```csharp
await using var environment = await StubServiceTestEnvironment.StartAsync(
    IotHubGeneration.Gen2,
    configureHub: options => options.RandomConnectionDrops = new StubConnectionDropOptions
    {
        Enabled = true,
        MinInterval = TimeSpan.FromSeconds(1),
        MaxInterval = TimeSpan.FromSeconds(5),
        Probability = 0.5,
        ReasonCodes = StubDisconnectReasonCodes.All,
        RandomSeed = 1234,   // replay a failing run
        MaxDrops = 10,
    });
```

Each pass waits a random interval in `[MinInterval, MaxInterval]`, rolls `Probability`, and on a hit drops one
randomly chosen connected device with one randomly chosen code from `ReasonCodes`. Candidates are the devices
the stub serves and already knows about, optionally narrowed further by `StubConnectionDropOptions.DeviceIds`;
the stub never drops its own connection or the stub DPS's.

`ReasonCodes` defaults to `StubDisconnectReasonCodes.ServerInitiated`, which is every code MQTT 5 section
3.14.2.1 allows a server to send. `StubDisconnectReasonCodes.All` adds the client-only
`DisconnectWithWillMessage`, for checking that a device survives a code it should never be sent;
`StubDisconnectReasonCodes.ServerInitiatedErrors` drops `NormalDisconnection`.

Every drop, explicit or random, raises `DeviceConnectionDropped` and is appended to `ConnectionDropHistory`,
which is what a randomized test asserts against since neither the victim, the moment, nor the code is known up
front. A gen2 device is marked not dispatch-ready again, because it has to redo the presence handshake.

Note that the reason code does **not** reach a gen1 device. MQTT 3.1.1 has no server-to-client DISCONNECT
packet, so the broker can only close the socket, and the client reports `NormalDisconnection` however
deliberate the drop was. Only a gen2 (MQTT 5) device sees the code that was asked for. Assert against
`ConnectionDropHistory`, which records what was requested, rather than the device's disconnect arguments.

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

Provisioning is not optional in these tests. `ConnectionClient.ConnectAsync` is internal, so
`ProvisionAndConnectAsync` is the only way a test can get a device onto the stub hub, and every test in
this folder goes through the stub DPS to get there.

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

## Dropping device connections

The stub DPS drops connections the same way the stub hub does, and for the same reason: to see what the SDK
does when a registration is cut off partway through. The API mirrors the hub's, backed by the same
`StubConnectionDropEngine`.

```csharp
await using var environment = await StubServiceTestEnvironment.StartAsync(
    IotHubGeneration.Gen2,
    configureProvisioningService: options =>
    {
        options.AssigningPollResponses = 3;
        options.RandomConnectionDrops = new StubConnectionDropOptions
        {
            Enabled = true,
            MinInterval = TimeSpan.FromSeconds(1),
            MaxInterval = TimeSpan.FromSeconds(2),
            ReasonCodes = StubDisconnectReasonCodes.All,
            MaxDrops = 1,
            RandomSeed = 1234,
        };
    });

// Or explicitly, at a moment of the test's choosing.
await dps.DropDeviceConnectionAsync(deviceId, MqttDisconnectReasonCode.ServerBusy);
await dps.DropDeviceConnectionWithRandomReasonCodeAsync(deviceId);
await dps.DropRandomDeviceConnectionAsync();
```

`StubDeviceProvisioningServiceOptions.ConnectionDropper` supplies the ability, exactly as on the hub, and
`ForHub` copies the hub's dropper along with everything else it copies, so a stub pair built that way needs
no extra wiring. Drops raise `DeviceConnectionDropped` and land in `ConnectionDropHistory`.

Two things differ from the hub:

- **The only droppable device is the one being registered.** The SDK connects to DPS with the registration
  id as its MQTT client id, so `StubDeviceProvisioningServiceOptions.DeviceId` or `RegistrationId` is the
  only value the stub can match, and any other device id is rejected. This falls out of the same
  single-device limitation as the rest of the stub DPS.
- **Reason codes are a stub-side record only.** Provisioning is MQTT 3.1.1 for both generations, and 3.1.1
  has no server-to-client DISCONNECT packet, so the registering device only ever sees its socket close. The
  requested code is recorded in `ConnectionDropHistory`, which is the only dependable place to assert on it.

Timing matters more here than on the hub, because a registration is short. `AssigningPollResponses` is the
lever: each extra poll costs at least the SDK's two second `retry-after` floor, which is what keeps the
device on the DPS connection long enough for a drop to land mid registration. Note also that the random drop
loop starts with `StartAsync`, which is before the device connects.

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

# Fault injection over MQTT

Every fault the in-process broker can inject is triggered by an ordinary MQTT PUBLISH, never by a .NET call on
the broker object. `InProcessMqttBroker` exposes no fault-injecting method at all. The consequence is that a
fault can be injected from outside the process that hosts the broker - by another process, by a test written in
another language, or by hand.

The whole structure of a fault travels in the **payload**, as JSON, rather than in MQTT 5 user properties.
MQTT 3.1.1 has no user properties, and gen1 devices and all DPS traffic speak 3.1.1, so the same bytes have to
work over either protocol version.

## The control topics

| Topic | Direction | Contents |
|---|---|---|
| `$fault/req` | caller -> broker | An `MqttFaultInjectionRequest` as JSON |
| `$fault/res/{requestId}` | broker -> caller | An `MqttFaultInjectionResponse` as JSON |

A request is never forwarded to subscribers, so control traffic cannot be mistaken for device or service
traffic. A request carrying a `requestId` is answered on `$fault/res/{requestId}`, or on the request's own
`responseTopic` when it names one; a request with neither is still executed, and the broker simply stays
silent. Subscribe to `$fault/res/#` *before* publishing, since a fault can disconnect the caller itself.

## The request

```jsonc
{
  "requestId": "3f1c...",       // correlates the response; omit for fire and forget
  "fault": "disconnect",        // "disconnect" or "listClients"
  "clientId": "device-1",       // the victim; required by "disconnect"
  "reasonCode": 137,            // numeric MQTT 5 DISCONNECT code, 137 = ServerBusy, default 0
  "reasonString": "throttled",  // optional
  "delayMilliseconds": 250,     // optional; the response follows the fault, not the request
  "responseTopic": "..."        // optional override of $fault/res/{requestId}
}
```

`reasonCode` is the number rather than a name so that a caller in another language need not know MQTTnet's
enum. A code MQTT 5 does not define is rejected with an error response. Because the SDK connects a device using
its device id as the MQTT client id, `clientId` and the device id are the same string for a device under test.

| Fault | Effect |
|---|---|
| `disconnect` | Terminates `clientId`'s session with `reasonCode`, optionally after `delayMilliseconds` |
| `listClients` | Reports the currently connected client ids. Not a fault - it is how an out-of-process caller picks a victim for one |

## The response

```jsonc
{
  "requestId": "3f1c...",
  "fault": "disconnect",
  "succeeded": true,        // the request was understood and carried out
  "faultApplied": true,     // something was actually hit
  "clientIds": ["..."],     // listClients only
  "error": null             // why, when succeeded is false
}
```

The two booleans differ: a well-formed disconnect request naming a client that is not connected *succeeds*
without applying anything. A payload too malformed to parse cannot be correlated to a response either, so it is
logged and ignored rather than answered.

## Injecting a fault

From outside the process, anything that can publish will do:

```bash
mosquitto_pub -h 127.0.0.1 -p 1883 -t '$fault/req' \
    -m '{"fault":"disconnect","clientId":"device-1","reasonCode":137}'
```

From a test, `MqttFaultInjectionClient` wraps the same protocol and handles the response correlation. It is an
`IStubDeviceConnectionDropper`, which is how both stubs drop connections:

```csharp
await using var faultInjection = await MqttFaultInjectionClient.ForBrokerAsync(broker, MqttProtocolVersion.V500);

IReadOnlyList<string> clients = await faultInjection.GetConnectedClientIdsAsync();
bool dropped = await faultInjection.DisconnectClientAsync("device-1", MqttDisconnectReasonCode.ServerBusy);

// Or the raw contract, for anything the interface does not cover.
MqttFaultInjectionResponse response = await faultInjection.SendAsync(new MqttFaultInjectionRequest
{
    Fault = MqttFaultInjection.Faults.Disconnect,
    ClientId = "device-1",
    ReasonCode = (int)MqttDisconnectReasonCode.ServerBusy,
    DelayMilliseconds = 500,
});
```

`StubServiceTestEnvironment` and the harness in `StubIotHubServiceTests` build one already and hand it to both
stubs, deliberately on MQTT 3.1.1 for gen1 and MQTT 5 for gen2 so that both halves of the payload-only contract
stay exercised.

# Reference

## Files

| File | Contents |
|---|---|
| `StubIotHubService.cs` | Lifecycle, subscriptions, message routing, and the generation-agnostic public API |
| `StubIotHubService.Gen1.cs` | Classic hub protocol |
| `StubIotHubService.Gen2.cs` | Event Grid MQTT broker hub protocol |
| `StubIotHubService.ConnectionDrops.cs` | Explicit and random device connection drops |
| `StubIotHubServiceOptions.cs` | Configuration |
| `StubConnectionDropOptions.cs` | Random connection drop configuration and the reason code sets |
| `StubConnectionDropEngine.cs` | The drop machinery itself - history, the seeded random choices, and the background loop - shared by both stubs |
| `IStubDeviceConnectionDropper.cs` | The ability to close a device's session, which the stubs borrow from `MqttFaultInjectionClient` |
| `StubDeviceProvisioningService.cs` | The stub DPS: registration, polling, and hub assignment |
| `StubDeviceProvisioningService.ConnectionDrops.cs` | Explicit and random drops of the registering device's connection |
| `StubDeviceProvisioningServiceOptions.cs` | Stub DPS configuration |
| `StubDeviceState.cs` | Per-device authoritative twin, versioning, and JSON merge patch |
| `StubServiceModels.cs` | Event args and result types |
| `IotHubGeneration.cs` | The generation enum |
| `InProcessMqttBroker.cs` | A plaintext MQTT broker on a loopback port, for tests that want no external dependencies |
| `InProcessMqttBroker.FaultInjection.cs` | The broker's `$fault/req` handler - the only place a fault is actually injected |
| `MqttFaultInjection.cs` | The fault injection wire contract: topics, fault names, and the request and response payloads |
| `MqttFaultInjectionClient.cs` | Injects faults by publishing them, and is the `IStubDeviceConnectionDropper` both stubs use |
| `LocalBrokerMqttClient.cs` | Retargets the SDK's CONNECT packet at a local plaintext broker |
| `StubServiceTestEnvironment.cs` | A broker, a hub, and a DPS bound to it, for tests that just want a device to provision somewhere |
| `StubIotHubServiceTests.cs` | End-to-end tests of a real device client against the stub |
| `MqttFaultInjectionTests.cs` | Tests of the fault injection protocol itself, over both MQTT 3.1.1 and MQTT 5 |

The provisioning tests that use `StubServiceTestEnvironment` live with the client they exercise, in
`Gen2/ProvisioningIntegrationTests.cs` and `Unified/ProvisioningIntegrationTests.cs`.

## Running against a local broker

`StubIotHubServiceTests` shows the full wiring. `InProcessMqttBroker` starts an MQTTnet broker on a free
loopback port, and `LocalBrokerMqttClient` - supplied through `ConnectionClientOptions.MqttClient` - rewrites
the host and port on the SDK's CONNECT packet and drops the client certificate so that TLS is not negotiated.
Everything above the CONNECT packet is the unmodified SDK code path.
