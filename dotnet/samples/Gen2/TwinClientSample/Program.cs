// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Twin;
using Microsoft.Azure.Iot.Device.Gen2.Connection;
using Microsoft.Azure.Iot.Device.Gen2.Twin;
using SetupSampleDevice;
using System.Text.Json;

internal class Program
{
    static DeviceTwin? currentTwin = null;

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

        TwinPushOptions twinPushOptions = new()
        {
            ReceiveDesiredPropertyUpdates = true,
            ReceiveReportedPropertiesUponConnect = true,
        };

        using ConnectionClient connectionClient = new ConnectionClient(null, twinPushOptions);

        using TwinClient twinClient = new TwinClient(connectionClient);

        Action<DesiredPatchReceivedEventArgs> HandleDesiredPropertiesUpdateAsync = async (args) =>
        {
            if (currentTwin == null)
            {
                // The full twin wasn't received yet, so disregard any piecemeal updates that came before it 
                return;
            }

            Console.WriteLine($"Received desired property update");
            currentTwin.DesiredVersion = args.DesiredPropertiesVersion;
            currentTwin.Desired = args.DesiredProperties;

            // Some application-level processing based on what desired properties changed

            var reportedProperties = args.DesiredProperties; // Echo back the desired properties as the current reported properties

            Console.WriteLine($"Responding to desired patch by sending a reported patch");
            ReportedPatchRequest reportedPatch = new()
            {
                ReportedProperties = reportedProperties,
                IfMatch = 0,
            };
            ReportedPatchResponse patchResponse = await twinClient.UpdateReportedPropertiesAsync(reportedPatch);
            currentTwin.ReportedVersion = patchResponse.Version;
            if (patchResponse.Result == Result.Ok)
            {
                currentTwin.Reported = args.DesiredProperties;
                currentTwin.ReportedVersion = args.DesiredPropertiesVersion;
            }

            Console.WriteLine($"The current twin is now: {JsonSerializer.Serialize(currentTwin)}");
        };

        Action<TwinPushReceivedEventArgs> HandleTwinPushAsync = async (args) =>
        {
            Console.WriteLine("Received a twin push from IoT Hub. Updating the local reference of this device's twin to match");

            if (currentTwin == null)
            {
                currentTwin = new();
            }

            if (args.Reported != null)
            {
                currentTwin.Reported = args.Reported.Properties;
                currentTwin.ReportedVersion = args.Reported.PropertiesVersion;
            }

            if (args.Desired != null)
            {
                currentTwin.Desired = args.Desired.Properties;
                currentTwin.DesiredVersion = args.Desired.PropertiesVersion;
            }
        };


        twinClient.DesiredPatchReceived += HandleDesiredPropertiesUpdateAsync;
        twinClient.TwinPushReceived += HandleTwinPushAsync;

        ProvisioningSettings provisioningSettings = new(idScope);

        var connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cts.Token);
        Console.WriteLine($"Device {deviceId} is now provisioned and connected to IoT Hub. Now listening for desired property patches");

        currentTwin = await twinClient.GetTwinAsync(true, true, 0, 0, cts.Token);
        Console.WriteLine($"The current twin is: {JsonSerializer.Serialize(currentTwin)}");

        try
        {
            Console.WriteLine("Press 'Ctrl+C' to end the sample");
            await Task.Delay(-1, cts.Token);
        }
        catch (OperationCanceledException)
        {
            Console.WriteLine("Sample timeout has completed. Shutting down the sample...");
        }

        twinClient.DesiredPatchReceived -= HandleDesiredPropertiesUpdateAsync;
        await connectionClient.DisconnectAsync();
    }
}