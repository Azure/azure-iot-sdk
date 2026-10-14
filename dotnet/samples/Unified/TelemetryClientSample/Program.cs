// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication.Boot;
using Microsoft.Azure.Iot.Device.Authentication.Operational;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
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

        string deviceId = SampleConstants.LoadX509DeviceId();
        string idScope = SampleConstants.LoadIdScope();
        X509BootAuthenticationProvider bootAuthentication = SampleConstants.LoadX509BootAuthenticationProvider();
        X509OperationalAuthenticationProvider operationalAuthentication = new(bootAuthentication.ClientCertificate);

        using ConnectionClient connectionClient = new ConnectionClient();

        TelemetryClient telemetryClient = new TelemetryClient(connectionClient);

        ProvisioningSettings provisioningSettings = new(idScope);
        var connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, bootAuthentication, operationalAuthentication, cancellationToken: cts.Token);
        Console.WriteLine($"Device {deviceId} is now provisioned and connected to IoT Hub.");
        Console.WriteLine("Press 'Ctrl+C' to end the sample");

        while (!cts.Token.IsCancellationRequested)
        {
            DeviceToCloudTelemetry outgoingTelemetry = new()
            {
                Payload = Encoding.UTF8.GetBytes("Hello world!"),
                MessageId = Guid.NewGuid().ToString(),
            };

            outgoingTelemetry.UserProperties.Add("SomeCustomUserPropertyKey", "SomeCustomUserPropertyValue");

            Console.WriteLine($"Sending telemetry with message Id {outgoingTelemetry.MessageId}");
            try
            {
                await telemetryClient.SendTelemetryAsync(outgoingTelemetry, cts.Token);
                await Task.Delay(TimeSpan.FromSeconds(1), cts.Token);
            }
            catch (OperationCanceledException)
            {
                // Expected when user cancels the sample    
            }
        }

        await connectionClient.DisconnectAsync();
    }
}