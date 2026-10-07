// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device;
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

        string customPublishingTopic = $"";
        string customReceivingTopic = $"";


        var subackReasonCode = await customTopicsClient.SubscribeAsync(customReceivingTopic, MqttQualityOfServiceLevel.AtLeastOnce, cts.Token);
        if (subackReasonCode != MqttClientSubscribeReasonCode.GrantedQoS0 && subackReasonCode != MqttClientSubscribeReasonCode.GrantedQoS1 && subackReasonCode != MqttClientSubscribeReasonCode.GrantedQoS2)
        {
            Console.WriteLine($"IoT Hub did not accept the custom topic subscription. Reason code: {subackReasonCode}");
            return;
        }

        Console.WriteLine($"Device {deviceId} is now subscribed to topic {customReceivingTopic}.");

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