// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device;
using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.Twin;
using SetupSampleDevice;
using System.Text.Json.Nodes;

/// <summary>
/// Demonstrates keeping a device's copy of its twin in sync with IoT hub, and persisting it between runs.
/// </summary>
/// <remarks>
/// <para>
/// Run this sample, update the device's desired properties from the service side, stop the sample, update them again, then
/// run the sample again. On the second run, the device tells IoT hub which versions of its twin it already holds, so IoT hub
/// only pushes the sections that changed while the device was offline. Delete the persisted twin file to start over.
/// </para>
/// <para>
/// The device applies each desired properties update and acknowledges it in its reported properties by reporting the
/// version of the desired properties it has applied.
/// </para>
/// </remarks>
internal class Program
{
    private static async Task Main(string[] args)
    {
        using CancellationTokenSource cts = new CancellationTokenSource();
        cts.CancelAfter(TimeSpan.FromMinutes(10));

        // Cancel sample on key press
        Console.CancelKeyPress += (sender, eventArgs) =>
        {
            cts.Cancel();
            eventArgs.Cancel = true;
        };

        string deviceId = SampleConstants.LoadDeviceId();
        string idScope = SampleConstants.LoadIdScope();
        X509AuthenticationProvider authentication = SampleConstants.LoadAuthenticationProvider();

        // The SDK doesn't persist the twin, so this application does.
        string twinPath = Path.Combine(AppContext.BaseDirectory, $"twin-{deviceId}.json");
        PersistedTwin twin = PersistedTwin.Load(twinPath);
        Console.WriteLine($"Loaded this device's twin from {twinPath} (desired version {twin.DesiredVersion}, reported version {twin.ReportedVersion})");

        Microsoft.Azure.Iot.Device.MQTTv5.Models.ConnectionClientOptions options = new()
        {
            // Ask IoT hub to push the desired properties upon connecting if they changed while the device was offline, and then
            // to send each desired properties update as it happens. These can't be changed once the connection client is created.
            ReceiveDesiredPropertyUpdates = true,

            // Ask IoT hub to push the reported properties upon connecting if this device doesn't hold their latest version.
            ReceiveReportedPropertiesUponConnect = true,

            // Tell IoT hub which versions this device already holds, so it doesn't push sections that haven't changed. If a
            // persisted version is newer than IoT hub's (for example, because the twin was recreated), the SDK discards it.
            InitialDesiredPropertiesVersion = twin.DesiredVersion,
            InitialReportedPropertiesVersion = twin.ReportedVersion,
        };

        using ConnectionClient connectionClient = new ConnectionClient(options);
        using TwinClient twinClient = new TwinClient(connectionClient);

        // Sends a reported properties patch that was recorded in the persisted twin, and records IoT hub's response to it.
        async Task SendReportedPatchAsync(PersistedTwin.PendingPatch pending)
        {
            try
            {
                // The SDK makes the patch conditional on IoT hub's version of the reported properties, combines it with
                // other patches made while one is in flight, and keeps it across reconnects until IoT hub responds.
                ReportedPatchResponse response = await twinClient.UpdateReportedPropertiesAsync(new() { ReportedProperties = pending.Patch }, cts.Token);
                if (response.Result == Result.Ok)
                {
                    twin.CompleteReportedPatch(pending, response.Version);
                    Console.WriteLine($"IoT hub applied the reported properties patch {pending.Patch.ToJsonString()} (reported version {response.Version})");
                    return;
                }

                Console.WriteLine($"IoT hub rejected the reported properties patch {pending.Patch.ToJsonString()}: {response.Result}");
                twin.RejectReportedPatch(pending);
                await ResyncReportedPropertiesAsync();
            }
            catch (OperationCanceledException) when (cts.IsCancellationRequested)
            {
                // The sample is stopping. The patch stays in the persisted twin and is sent again on the next run. IoT hub may
                // already have applied it, but applying the same merge patch again leaves the same reported properties.
            }
            catch (Exception e)
            {
                Console.WriteLine($"Failed to send the reported properties patch {pending.Patch.ToJsonString()}: {e.Message}");
                twin.RejectReportedPatch(pending);
                await ResyncReportedPropertiesAsync();
            }
        }

        // Pending patches are sent in the order they were recorded, which is the order the SDK sends them to IoT hub in.
        object sendLock = new();
        bool isConnected = false;
        void SendPendingReportedPatches()
        {
            lock (sendLock)
            {
                if (!isConnected)
                {
                    return;
                }

                foreach (PersistedTwin.PendingPatch pending in twin.GetPendingReportedPatches().Where(pending => !pending.IsSent))
                {
                    pending.IsSent = true;

                    // The SDK queues the patch before this call returns, so patches are queued in this order.
                    _ = SendReportedPatchAsync(pending);
                }
            }
        }

        // Records a reported properties patch in the persisted twin before sending it, so that it isn't lost if the
        // application stops before IoT hub applies it.
        void UpdateReportedProperties(JsonObject patch)
        {
            twin.AddPendingReportedPatch(patch);
            SendPendingReportedPatches();
        }

        // Fetches IoT hub's copy of the reported properties when this device no longer knows whether its copy matches.
        async Task ResyncReportedPropertiesAsync()
        {
            try
            {
                DeviceTwin current = await twinClient.GetTwinAsync(getReported: true, getDesired: false, cancellationToken: cts.Token);
                if (current.Reported != null && current.ReportedVersion is ulong reportedVersion)
                {
                    twin.ApplySnapshot(desired: false, reportedVersion, current.Reported);
                    Console.WriteLine($"Fetched the reported properties from IoT hub (reported version {reportedVersion})");
                }
            }
            catch (Exception e) when (e is DeviceException or OperationCanceledException)
            {
                // The reported version stays 0, so IoT hub pushes the reported properties the next time this device connects.
                Console.WriteLine($"Failed to fetch the reported properties. They will be pushed by IoT hub upon reconnecting: {e.Message}");
            }
        }

        // The SDK only raises this for the next version of the desired properties that this device holds. If it misses an
        // update, it fetches the full desired properties and raises TwinPushReceived with them instead.
        void HandleDesiredPatch(DesiredPatchReceivedEventArgs args)
        {
            Console.WriteLine($"Received desired properties patch {args.DesiredProperties.ToJsonString()} (desired version {args.DesiredPropertiesVersion})");
            twin.ApplyDesiredPatch(args.DesiredPropertiesVersion, args.DesiredProperties);

            // Some application-level processing based on what desired properties changed

            UpdateReportedProperties(new JsonObject { ["appliedDesiredVersion"] = args.DesiredPropertiesVersion });
        }

        // IoT hub pushes the full desired and/or reported properties upon connecting if this device's versions are out of date.
        // The SDK also raises this with the full desired properties when it resynchronizes them.
        void HandleTwinPush(TwinPushReceivedEventArgs args)
        {
            if (args.Reported != null)
            {
                Console.WriteLine($"Received the full reported properties (reported version {args.Reported.PropertiesVersion})");
                twin.ApplySnapshot(desired: false, args.Reported.PropertiesVersion, args.Reported.Properties);
            }

            if (args.Desired != null)
            {
                Console.WriteLine($"Received the full desired properties (desired version {args.Desired.PropertiesVersion})");
                twin.ApplySnapshot(desired: true, args.Desired.PropertiesVersion, args.Desired.Properties);

                // Some application-level processing based on the new desired properties

                UpdateReportedProperties(new JsonObject { ["appliedDesiredVersion"] = args.Desired.PropertiesVersion });
            }
        }

        // Set handlers before connecting, since IoT hub may push the twin as soon as the device connects.
        twinClient.DesiredPatchReceived += HandleDesiredPatch;
        twinClient.TwinPushReceived += HandleTwinPush;

        ProvisioningSettings provisioningSettings = new(idScope);

        await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: cts.Token);
        Console.WriteLine($"Device {deviceId} is now provisioned and connected to IoT Hub. Now listening for desired property patches");

        // Send the reported properties patches that IoT hub hadn't applied when the sample last stopped, along with any made
        // while connecting.
        lock (sendLock)
        {
            isConnected = true;
        }

        SendPendingReportedPatches();

        UpdateReportedProperties(new JsonObject { ["lastStartedAt"] = DateTimeOffset.UtcNow });

        try
        {
            Console.WriteLine("Press 'Ctrl+C' to end the sample");
            await Task.Delay(-1, cts.Token);
        }
        catch (OperationCanceledException)
        {
            Console.WriteLine("Sample timeout has completed. Shutting down the sample...");
        }

        twinClient.DesiredPatchReceived -= HandleDesiredPatch;
        twinClient.TwinPushReceived -= HandleTwinPush;
        await connectionClient.DisconnectAsync();

        Console.WriteLine($"This device's twin, as persisted to {twinPath}:");
        Console.WriteLine(twin.ToJsonString());
    }
}