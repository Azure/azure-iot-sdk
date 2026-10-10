// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using SetupSampleDevice;
using System.Text;

internal class Program
{
    private static async Task Main(string[] args)
    {
        using CancellationTokenSource cts = new CancellationTokenSource();
        cts.CancelAfter(TimeSpan.FromSeconds(20));

        // Cancel sample on key press
        Console.CancelKeyPress += (sender, eventArgs) =>
        {
            cts.Cancel();
            eventArgs.Cancel = true;
        };

        // The device should always have access to its provisioning credentials
        string deviceId = SampleConstants.LoadDeviceId();
        string idScope = SampleConstants.LoadIdScope();
        X509AuthenticationProvider authentication = SampleConstants.LoadAuthenticationProvider();
        ProvisioningSettings provisioningSettings = new(idScope);

        ConnectionClient connectionClient;
        ConnectionContext? persistedProvisioningResult = null;
        string? provisionedHostName = SampleConstants.LoadIotHubHostName();

        if (!string.IsNullOrEmpty(provisionedHostName))
        {
            // If this sample has been run before, it will load the persisted provisioning result
            persistedProvisioningResult = new()
            {
                AuthenticationProvider = authentication,
                DeviceId = SampleConstants.LoadDeviceId(),
                IotHubHostName = provisionedHostName,
                ConnectionProfile = SampleConstants.LoadConnectionProfile(),
            };

            connectionClient = new ConnectionClient(connectionContext: persistedProvisioningResult);
        }
        else
        {
            // If no provisioning result was persisted, just create the default connection client
            connectionClient = new ConnectionClient();
        }

        // This will attempt to connect directly to IoT Hub if any persisted credentials were loaded above. If no credentials
        // were loaded, or those credentials failed to open a connection to IoT Hub, then this call will fall back to provisioning the device
        // again
        Console.WriteLine("Establishing connection with IoT Hub credentials, but provisioning if they are not present or no longer work...");
        var connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, forceProvisioning: false, cancellationToken: cts.Token);
        Console.WriteLine("Connected to IoT Hub.");

        // Persist the newest provisioning result so that the next time you run the sample, it connects directly to IoT Hub without provisioning
        Console.WriteLine("Saving device credentials to disk so that this sample can be run again using them rather than provisioning");
        SampleConstants.SaveIotHubHostName(connectionContext.IotHubHostName);
        SampleConstants.SaveDeviceId(connectionContext.DeviceId);
        SampleConstants.SaveConnectionProfile(connectionContext.ConnectionProfile);

        Console.WriteLine("Closing the connection client");
        await connectionClient.DisconnectAsync();
    }
}