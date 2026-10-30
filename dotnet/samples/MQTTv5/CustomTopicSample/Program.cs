// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device;
using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.CustomTopics;
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

        string deviceId = SampleConstants.LoadDeviceId();
        string idScope = SampleConstants.LoadIdScope();
        X509AuthenticationProvider authentication = SampleConstants.LoadAuthenticationProvider();

        using ConnectionClient connectionClient = new ConnectionClient();

        CustomTopicsClient customTopicsClient = new CustomTopicsClient(connectionClient);

        ProvisioningSettings provisioningSettings = new(idScope);
        var connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: cts.Token);
        Console.WriteLine($"Device {deviceId} is now provisioned and connected to IoT Hub.");
        Console.WriteLine("Press 'Ctrl+C' to end the sample");

        // "{deviceId}" (IConnectionClient.DeviceIdPlaceholder) is replaced with this device's current ID each time the
        // SDK sends an operation, so this topic keeps working if the device is re-provisioned with a different device ID.
        string customPublishingTopic = $"foo/{IConnectionClient.DeviceIdPlaceholder}";

        while (!cts.Token.IsCancellationRequested)
        {
            try
            {
                Console.WriteLine($"Sending telemetry to custom topic {customPublishingTopic}");
                MqttPublish mqttPublish = new()
                {
                    Topic = customPublishingTopic,
                    Payload = Encoding.UTF8.GetBytes("Hello world!"),
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                };
                await customTopicsClient.PublishAsync(mqttPublish, cts.Token);
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