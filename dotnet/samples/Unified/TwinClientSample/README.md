# Twin sample

Keeps a device's copy of its twin in sync with IoT Hub, and persists that copy to disk so that it survives restarts. The
sample:

- Applies each desired properties update to its local copy, then acknowledges it by reporting the desired version it
  applied (`appliedDesiredVersion`).
- Reports `lastStartedAt` each time it starts.
- Saves the twin to `twin-{deviceId}.json` next to the sample's binaries, and restores it on the next run.

The SDK orders desired updates, holds and combines reported properties patches, and re-sends them across reconnects, so
the application doesn't need to handle any of that itself. See [How the SDK keeps the twin in sync](#how-the-sdk-keeps-the-twin-in-sync)
for the guarantees this gives.

## Prerequisites

- The .NET 10 SDK.
- Run [SetupSampleDevice](../../SetupSampleDevice/) as described in the [samples README](../../README.md)
  to create an X.509 enrollment and save the sample credentials.

## Run

From the `dotnet\samples` directory:

```powershell
dotnet run --project Unified\TwinClientSample
```

The sample runs until you press Ctrl+C or its 10-minute timeout expires, then prints the persisted twin.

To see persistence at work:

1. Run the sample, update the device's desired properties from the service side, and watch the device apply them.
2. Stop the sample and update the desired properties again.
3. Run the sample again. The device starts from the twin it persisted, so the SDK only raises the desired properties,
   because they changed while the device was offline.

Delete `twin-{deviceId}.json` to start over. The device then holds no versions, so the SDK raises the whole twin.

## Persisted twin

The SDK keeps the device's copy of its twin as a `TwinState`, but doesn't persist it. The sample saves each snapshot
raised by `TwinClient.TwinStateChanged`, and passes the saved one to the `TwinClient` constructor on the next run:

| Field | Meaning |
| --- | --- |
| `DesiredVersion`, `Desired` | The latest desired properties the device received, and their version. |
| `ReportedVersion`, `Reported` | The reported properties IoT Hub has applied, and their version. |
| `PendingReportedPatches` | Reported properties patches that IoT Hub hasn't applied yet, in the order they were made. |

A version of `0` means the device doesn't hold that section in full, since IoT Hub's versions are never `0`. Each
snapshot is written to a temporary file that then replaces the twin file, so a crash mid-write never leaves a corrupt
twin. An unreadable file is ignored, which is always safe because the SDK then raises the whole twin upon connecting.

## Best practices demonstrated

- **Set handlers before connecting**, since the twin may be raised as soon as the device connects.
- **Rely on the SDK for ordering.** `DesiredPatchReceived` is only raised for the next desired version. If the SDK
  detects a missed update, it fetches the desired properties and raises `TwinPushReceived` instead.
- **Don't block event handlers.** Handlers call `EnqueueReportedPropertiesUpdate` rather than awaiting
  `UpdateReportedPropertiesAsync`, and outcomes are logged from `ReportedPatchCompleted`.
- **Update reported properties whether or not the device is connected.** The SDK holds patches, in order, and sends
  them once the device has connected and synchronized its twin.
- **Expect patches to be combined.** Patches queued while one is in flight, or while the device isn't connected, are
  combined into one JSON merge patch, so `ReportedPatchCompleted` is raised once per send.

## How the SDK keeps the twin in sync

- **The SDK gets the twin upon each connection.** IoT Hub doesn't push the twin upon connecting or say which versions
  it holds, so the SDK gets the full twin each time the device connects, and raises `TwinPushReceived` with the
  sections whose versions differ from the device's. Reported properties patches wait for that response, which adds a
  round trip before they are sent, and the whole twin is transferred on each connection.
- **Reported properties patches are at-least-once.** IoT Hub can't apply a patch conditionally, so if the response to a
  patch is lost (for example, because the connection dropped), the SDK sends it again and IoT Hub may apply it twice.
  Applying the same merge patch twice leaves the same reported properties, but the reported version advances twice.
  The SDK picks up the resulting reported properties on the next connection.
- **Twin requests use QoS 0.** If IoT Hub doesn't respond, the SDK re-sends the request after a defensive timeout of
  5 to 15 minutes. If IoT Hub throttles or fails the get twin request made upon connecting, reported properties
  patches wait until it is re-sent.
