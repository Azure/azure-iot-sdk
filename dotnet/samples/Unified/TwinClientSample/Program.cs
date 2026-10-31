// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using SetupSampleDevice;
using System.Text.Json;
using System.Text.Json.Nodes;
using TwinClient = Microsoft.Azure.Iot.Device.Unified.Twin.TwinClient;

/// <summary>
/// Demonstrates keeping a device's copy of its twin in sync with IoT hub, and persisting it between runs, whether the IoT
/// hub uses MQTTv3 or MQTTv5.
/// </summary>
/// <remarks>
/// <para>
/// Run this sample, update the device's desired properties from the service side, stop the sample, update them again, then
/// run the sample again. On the second run, the device starts from the twin it persisted, so the SDK only raises the sections
/// that changed while the device was offline. Delete the persisted twin file to start over.
/// </para>
/// <para>
/// The device applies each desired properties update and acknowledges it in its reported properties by reporting the
/// version of the desired properties it has applied.
/// </para>
/// </remarks>
internal class Program
{
    private static readonly JsonSerializerOptions s_serializerOptions = new() { WriteIndented = true };

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

        // The SDK keeps this device's copy of its twin, but doesn't persist it, so this application does.
        string twinPath = Path.Combine(AppContext.BaseDirectory, $"twin-{deviceId}.json");
        TwinState? persistedTwin = LoadTwin(twinPath);
        Console.WriteLine($"Loaded this device's twin from {twinPath} (desired version {persistedTwin?.DesiredVersion ?? 0}, reported version {persistedTwin?.ReportedVersion ?? 0})");

        using ConnectionClient connectionClient = new ConnectionClient();

        // Starting from the persisted twin means the SDK only raises the sections that changed while the device was offline,
        // and sends the reported properties patches that IoT hub hadn't applied when the sample stopped.
        using TwinClient twinClient = new TwinClient(connectionClient, persistedTwin);

        // The SDK raises each change to this device's copy of its twin (including each reported properties patch it is yet to
        // send) one at a time, in order, so saving each one keeps the persisted twin up to date.
        twinClient.TwinStateChanged += twin => SaveTwin(twinPath, twin);

        // The SDK sends reported properties patches in order, keeps them across reconnects until IoT hub responds, and only
        // adds them to its copy of the reported properties once IoT hub applies them, so a rejected patch needs no cleanup.
        //
        // The SDK sends one patch at a time. Patches queued while one is in flight (or while the device isn't connected) are
        // combined into one JSON merge patch and sent together, so this is raised once for all of them, and args.Patch is the
        // combined patch rather than any one of them.
        twinClient.ReportedPatchCompleted += args =>
        {
            if (args.Response?.Result == Result.Ok)
            {
                Console.WriteLine($"IoT hub applied the reported properties patch {args.Patch.ToJsonString()} (reported version {args.Response.Version})");
            }
            else
            {
                Console.WriteLine($"IoT hub didn't apply the reported properties patch {args.Patch.ToJsonString()}: {args.Response?.Result.ToString() ?? args.Error?.Message}");
            }
        };

        // The SDK only raises this for the next version of the desired properties that this device holds. If it misses an
        // update, it fetches the full desired properties and raises TwinPushReceived with them instead.
        twinClient.DesiredPatchReceived += args =>
        {
            Console.WriteLine($"Received desired properties patch {args.DesiredProperties.ToJsonString()} (desired version {args.DesiredPropertiesVersion})");

            // Some application-level processing based on what desired properties changed

            // Queue the patch rather than awaiting IoT hub's response, since event handlers must not block the SDK. If desired
            // patches arrive faster than IoT hub responds, the SDK combines these acknowledgements, so only the latest is sent.
            twinClient.EnqueueReportedPropertiesUpdate(new JsonObject { ["appliedDesiredVersion"] = args.DesiredPropertiesVersion });
        };

        // Raised with the full desired and/or reported properties upon connecting if this device's versions are out of date.
        // On an MQTTv3 IoT hub, the SDK gets the twin upon each connection to find out. The SDK also raises this with the full
        // desired properties when it resynchronizes them.
        twinClient.TwinPushReceived += args =>
        {
            if (args.Reported != null)
            {
                Console.WriteLine($"Received the full reported properties (reported version {args.Reported.PropertiesVersion})");
            }

            if (args.Desired != null)
            {
                Console.WriteLine($"Received the full desired properties (desired version {args.Desired.PropertiesVersion})");

                // Some application-level processing based on the new desired properties

                twinClient.EnqueueReportedPropertiesUpdate(new JsonObject { ["appliedDesiredVersion"] = args.Desired.PropertiesVersion });
            }
        };

        // The device doesn't need to be connected to update its reported properties. The SDK sends the patch once it connects,
        // combined with any patches restored from the persisted twin and any acknowledgements queued before then.
        twinClient.EnqueueReportedPropertiesUpdate(new JsonObject { ["lastStartedAt"] = DateTimeOffset.UtcNow });

        ProvisioningSettings provisioningSettings = new(idScope);

        ConnectionContext connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: cts.Token);
        Console.WriteLine($"Device {deviceId} is now provisioned and connected to IoT Hub using {connectionContext.ConnectionProfile}. Now listening for desired property patches");

        try
        {
            Console.WriteLine("Press 'Ctrl+C' to end the sample");
            await Task.Delay(-1, cts.Token);
        }
        catch (OperationCanceledException)
        {
            Console.WriteLine("Sample timeout has completed. Shutting down the sample...");
        }

        // Any reported properties patch that IoT hub hasn't applied yet stays in the persisted twin, and is sent on the next run.
        // IoT hub may already have applied it, but applying the same merge patch again leaves the same reported properties.
        await connectionClient.DisconnectAsync();

        Console.WriteLine($"This device's twin, as persisted to {twinPath}:");
        Console.WriteLine(JsonSerializer.Serialize(twinClient.GetTwinState(), s_serializerOptions));
    }

    // Loads the twin persisted at the given path, or returns null if this device holds no copy of its twin.
    private static TwinState? LoadTwin(string path)
    {
        if (!File.Exists(path))
        {
            return null;
        }

        try
        {
            return JsonSerializer.Deserialize<TwinState>(File.ReadAllText(path));
        }
        catch (JsonException e)
        {
            // Holding no copy is always safe: the SDK then raises the whole twin upon connecting.
            Console.WriteLine($"Ignoring the unreadable persisted twin at {path}: {e.Message}");
            return null;
        }
    }

    // Writes to a temporary file first, so that a crash mid-write never leaves a corrupt twin.
    private static void SaveTwin(string path, TwinState twin)
    {
        string temporaryPath = path + ".tmp";
        File.WriteAllText(temporaryPath, JsonSerializer.Serialize(twin, s_serializerOptions));
        File.Move(temporaryPath, path, overwrite: true);
    }
}
