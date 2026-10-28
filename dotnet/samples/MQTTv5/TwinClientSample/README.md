# MQTTv5 twin sample

Keeps a device's copy of its twin in sync with an IoT Hub that uses MQTTv5, and persists that copy to disk so that it
survives restarts. The sample:

- Applies each desired properties update to its local copy, then acknowledges it by reporting the desired version it
  applied (`appliedDesiredVersion`).
- Reports `lastStartedAt` each time it starts.
- Saves the twin to `twin-{deviceId}.json` next to the sample's binaries, and restores it on the next run.

## Prerequisites

- The .NET 10 SDK.
- Run [SetupSampleDevice](../../SetupSampleDevice/) as described in the [samples README](../../README.md)
  to create an X.509 enrollment and save the sample credentials. DPS must be linked to an IoT Hub that uses MQTTv5.

## Run

From the `dotnet\samples` directory:

```powershell
dotnet run --project MQTTv5\TwinClientSample
```

The sample runs until you press Ctrl+C or its 10-minute timeout expires, then prints the persisted twin.

To see persistence at work:

1. Run the sample, update the device's desired properties from the service side, and watch the device apply them.
2. Stop the sample and update the desired properties again.
3. Run the sample again. The device tells IoT Hub which versions it already holds, so IoT Hub only pushes the desired
   properties because they changed while the device was offline.

Delete `twin-{deviceId}.json` to start over. The device then holds no versions, so IoT Hub pushes the whole twin.

## Persisted twin

[PersistedTwin.cs](./PersistedTwin.cs) holds the device's copy of its twin:

| Field | Meaning |
| --- | --- |
| `DesiredVersion`, `Desired` | The latest desired properties this device has applied, and their version. |
| `ReportedVersion`, `Reported` | IoT Hub's latest known reported properties, and their version. |
| `PendingReported` | Reported properties patches that IoT Hub hasn't applied yet, in the order they were made. |

A version of `0` means the device doesn't hold that section in full, since IoT Hub's versions are never `0`. Each change
is written to a temporary file that then replaces the twin file, so a crash mid-write never leaves a corrupt twin. An
unreadable file is ignored, which is always safe because IoT Hub then pushes the whole twin.

The SDK doesn't persist the twin. Persistence is the application's responsibility.

## Best practices demonstrated

### Connecting

- **Choose the push options once.** `ReceiveDesiredPropertyUpdates` and `ReceiveReportedPropertiesUponConnect` are
  set on `ConnectionClientOptions` and can't change while the connection client exists.
- **Pass the versions the device already holds.** `InitialDesiredPropertiesVersion` and
  `InitialReportedPropertiesVersion` come from the persisted twin. IoT Hub only pushes the sections whose versions
  differ, rather than the device fetching the whole twin each time it starts. If a persisted version is newer than
  IoT Hub's (for example, because the device's twin was recreated), the SDK discards it.
- **Set handlers before connecting.** IoT Hub may push the twin as soon as the device connects, so
  `DesiredPatchReceived` and `TwinPushReceived` are subscribed before `ProvisionAndConnectAsync`.

### Desired properties

- **Apply patches as JSON merge patches** to the local copy, and record the new version.
- **Replace the local copy with each twin push.** `TwinPushReceived` carries a whole section, which replaces the
  local one.
- **Rely on the SDK for ordering.** `DesiredPatchReceived` is only raised for the next desired version after the
  one the device holds. If the SDK detects a missed update, it fetches the desired properties and raises
  `TwinPushReceived` with them instead, so the application never applies patches out of order.
- **Acknowledge what was applied** by reporting `appliedDesiredVersion`, so the service side can tell which
  desired version the device is acting on.

### Reported properties

- **Persist each patch before sending it.** A patch is added to `PendingReported` before
  `UpdateReportedPropertiesAsync` is called, so it isn't lost if the device stops before IoT Hub applies it.
- **Send patches whether or not the device is connected.** The SDK holds patches until the device connects and IoT
  Hub acknowledges its presence. Patches left over from the previous run are queued before connecting, so they are
  sent before newer ones. Re-sending a patch that IoT Hub may already have applied is safe, because applying the
  same merge patch again leaves the same reported properties.
- **Keep the persisted order and the send order the same.** The SDK queues each patch before
  `UpdateReportedPropertiesAsync` returns, so the sample records and sends each patch under one lock.
- **Let the SDK make patches conditional.** The SDK sends one patch at a time, conditional on IoT Hub's version of
  the reported properties, and combines patches made while another is in flight. It also re-sends patches after
  timeouts, version mismatches and reconnects. When a patch completes, the sample applies it and every earlier
  pending patch to its copy of the reported properties, since IoT Hub applied them in order.
- **Resynchronize after a rejection.** If IoT Hub rejects a patch, the device no longer knows whether its copy
  matches IoT Hub's. The sample sets its reported version to `0` and fetches the reported properties. If that fails,
  IoT Hub pushes them on the next connection because the device holds no version.
- **Keep patches when shutting down.** Patches whose sends are cancelled when the sample stops stay in the
  persisted twin and are sent again on the next run. The sample awaits every outstanding send before disconnecting,
  so none are left running unobserved.
