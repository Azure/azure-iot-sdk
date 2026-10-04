// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device;
using Microsoft.Azure.Iot.Device.Unified.DirectMethods;
using System.Text.Json;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.DirectMethods;
using SetupSampleDevice;
using Microsoft.Azure.Iot.Device.Provisioning.Models;

internal class Program
{
    private const string MethodName = "testMethod";

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

        using ConnectionClient connectionClient = new ConnectionClient();

        ConnectionContext persistedConnectionContext = new()
        {
            AuthenticationProvider = authentication,
            ConnectionProfile = ConnectionProfile.Classic,
            DeviceId = deviceId,
            IotHubHostName = "someHostName",
            IssuedClientCertificates = null,
        };

        // User may attempt to connect directly to IoT Hub using credentials that were persisted during device reboot.
        // This call should return false if the connection to IoT Hub cannot be established without provisioning again. 
        // This call should not provision the device, though.
        if (!await connectionClient.TryConnectAsync(persistedConnectionContext, cts.Token))
        {
            // If the connection client cannot connect directly to IoT Hub (identity terminal error or retry policy expires),
            // then go through provisioning again
            ProvisioningSettings provisioningSettings = new(idScope);

            // During this call, the client may reprovision even if initial provisioning was successful if connection to IoT Hub
            // cannot be established.
            ConnectionContext currentConnectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: cts.Token);
        }

        // At this point in the sample, the device has connected to IoT Hub and only provisioned if it was necessary

        await connectionClient.DisconnectAsync();
    }
}