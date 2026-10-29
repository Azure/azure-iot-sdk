# MQTTv5 twin sample

Keeps a device's copy of its twin in sync with an IoT Hub that uses MQTTv5, and persists that copy to disk so that it
survives restarts. The sample:

- Applies each desired properties update to its local copy, then acknowledges it by reporting the desired version it
  applied (`appliedDesiredVersion`).
- Reports `lastStartedAt` each time it starts.
- Saves the twin to `twin-{deviceId}.json` next to the sample's binaries, and restores it on the next run.

## Why `ConcurrentTwinClient` rather than `TwinClient`

The SDK offers two MQTTv5 twin clients. This sample uses `ConcurrentTwinClient`, and most devices should too.

`TwinClient` just mirrors IoT Hub's twin API. It sends each request once and raises each message as it arrives. The
application then has to do all of the following itself to keep its twin in sync:

- **Ordering.** Desired properties updates may be raised out of version order, more than once, or not at all if the
  device was disconnected when IoT Hub sent them.
- **Retries.** Requests aren't queued or retried, so a reported properties patch fails if the connection drops before
  IoT Hub responds.
- **Conditional patches.** Patches are only conditional on IoT Hub's version of the reported properties if the caller
  supplies that version.
- **Versions held.** It doesn't track which twin versions the device holds, so it can't tell IoT Hub upon connecting
  to skip pushing sections that haven't changed.
- **Persistence.** It keeps no copy of the twin to persist.

`ConcurrentTwinClient` implements the device-side behavior that the twin spec recommends, built on a `TwinClient` of
its own. It:

- Announces the versions the device holds upon each connection.
- Delivers desired updates in version order, and resyncs if one is missed.
- Queues reported properties patches, sending them one at a time across reconnects, conditional on IoT Hub's version.
- Raises a `TwinState` snapshot to persist each time the device's copy of its twin changes.

The persistence and best practices below all depend on that behavior. It is also safe to use from multiple threads.
Use `TwinClient` only if your application implements this behavior itself, or just needs one-off twin requests.

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

The SDK keeps the device's copy of its twin as a `TwinState`, but doesn't persist it. The sample saves each snapshot
raised by `ConcurrentTwinClient.TwinStateChanged`, and passes the saved one to the `ConcurrentTwinClient` constructor on the next run:

| Field | Meaning |
| --- | --- |
| `DesiredVersion`, `Desired` | The latest desired properties the device received, and their version. |
| `ReportedVersion`, `Reported` | The reported properties IoT Hub has applied, and their version. |
| `PendingReportedPatches` | Reported properties patches that IoT Hub hasn't applied yet, in the order they were made. |

A version of `0` means the device doesn't hold that section in full, since IoT Hub's versions are never `0`. Each
snapshot is written to a temporary file that then replaces the twin file, so a crash mid-write never leaves a corrupt
twin. An unreadable file is ignored, which is always safe because IoT Hub then pushes the whole twin.

## Best practices demonstrated

### Connecting

- **Choose the push options once.** `ReceiveDesiredPropertyUpdates` and `ReceiveReportedPropertiesUponConnect` are
  set on `ConnectionClientOptions` and can't change while the connection client exists.
- **Start from the persisted twin.** Passing it to the `ConcurrentTwinClient` constructor tells IoT Hub which versions the
  device already holds, so IoT Hub only pushes the sections whose versions differ, rather than the device fetching the
  whole twin each time it starts. If a persisted version is newer than IoT Hub's (for example, because the device's
  twin was recreated), the SDK discards it.
- **Set handlers before connecting.** IoT Hub may push the twin as soon as the device connects, so every handler is
  subscribed before `ProvisionAndConnectAsync`.

### Desired properties

- **Rely on the SDK for ordering.** `DesiredPatchReceived` is only raised for the next desired version after the
  one the device holds. If the SDK detects a missed update, it fetches the desired properties and raises
  `TwinPushReceived` with them instead, so the application never applies patches out of order. The SDK applies
  each patch and push to its `TwinState`.
- **Acknowledge what was applied** by reporting `appliedDesiredVersion`, so the service side can tell which
  desired version the device is acting on.

### Reported properties

- **Don't block event handlers.** Handlers call `EnqueueReportedPropertiesUpdate`, which returns immediately, rather
  than awaiting `UpdateReportedPropertiesAsync`. Outcomes are logged from `ReportedPatchCompleted`.
- **Persist each patch before it's sent.** `TwinStateChanged` is raised with the patch in `PendingReportedPatches`
  before `EnqueueReportedPropertiesUpdate` returns, so it isn't lost if the device stops before IoT Hub applies it.
  Re-sending a patch that IoT Hub may already have applied is safe, because applying the same merge patch again
  leaves the same reported properties.
- **Update reported properties whether or not the device is connected.** The SDK holds patches until the device
  connects and IoT Hub acknowledges its presence, and sends patches restored from the persisted twin before newer
  ones.
- **Let the SDK make patches conditional.** The SDK sends one patch at a time, conditional on IoT Hub's version of
  the reported properties, and combines patches made while another is in flight. It also re-sends patches after
  timeouts, version mismatches and reconnects. A rejected patch needs no cleanup, since the SDK only adds patches to
  its copy of the reported properties once IoT Hub applies them.
- **Expect patches to be combined.** The SDK sends one patch at a time. Patches queued while one is in flight, or
  while the device isn't connected, are combined into one JSON merge patch, where later values win. For example, on
  startup the patches restored from the persisted twin, `lastStartedAt` and any `appliedDesiredVersion`
  acknowledgements may all go out in one send. As a result:
  - `ReportedPatchCompleted` is raised once per send, and its `Patch` is the combined patch.
  - If several `appliedDesiredVersion` acknowledgements are combined, IoT Hub only receives the latest, and the
    reported version advances once for the whole send. The service side shouldn't expect one reported version per
    desired update.
  - Patches that can't be combined into one merge patch (such as one that removes a property followed by one that
    sets that property's children) are sent one after another.
