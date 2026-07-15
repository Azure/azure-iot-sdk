using Microsoft.Azure.Devices.Client;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter;
using SetupSampleDevice;
using System.Diagnostics;

internal class Program
{
    private const bool EnableMqttLogs = true;

    private static async Task Main(string[] args)
    {
        string deviceId = SampleConstants.LoadDeviceId();
        string idScope = SampleConstants.LoadIdScope();
        X509AuthenticationProvider authentication = SampleConstants.LoadAuthenticationProvider();

        // This MQTT client interface allows users to bring their own MQTT client implementation
        IMqttClient mqttClient;
        if (EnableMqttLogs)
        {
            // This SDK includes a single implementation of the MQTT client interface using MQTTnet as the client library
            mqttClient = new MqttNetClient(new MQTTnet.MqttClientFactory().CreateMqttClient(MqttNetTraceLogger.CreateTraceLogger()));
            Trace.Listeners.Add(new ConsoleTraceListener());
        }
        else
        {
            mqttClient = new MqttNetClient(new MQTTnet.MqttClientFactory().CreateMqttClient());
        }

        ConnectionClientOptions connectionClientOptions = new()
        {
            MqttClient = mqttClient,
        };

        // This connection client will dispose the underlying mqtt client when it is disposed
        using ConnectionClient connectionClient = new ConnectionClient(connectionClientOptions);

        ProvisioningSettings provisioningSettings = new(idScope);
        Console.WriteLine("Provisioning and connecting to IoT hub using the provided MQTT client");
        await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication);
        Console.WriteLine($"Device {deviceId} is now provisioned and connected to IoT Hub.");

        await Task.Delay(TimeSpan.FromSeconds(1));

        await connectionClient.DisconnectAsync();
    }
}