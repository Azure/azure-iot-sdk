using Microsoft.Azure.Devices.Client;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter;
using Microsoft.Azure.Devices.Client.Unified.Connection;
using SetupSampleDevice;

internal class Program
{
    private static async Task Main(string[] args)
    {
        string deviceId = SampleConstants.LoadDeviceId();
        string idScope = SampleConstants.LoadIdScope();
        X509AuthenticationProvider authentication = SampleConstants.LoadAuthenticationProvider();

        // This MQTT client interface allows users to bring their own MQTT client implementation
        // This SDK includes a single implementation of the MQTT client interface using MQTTnet as the client library
        IMqttClient mqttClient = new MqttNetClient();

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

        Console.WriteLine("Shutting down sample...");

        await connectionClient.DisconnectAsync();
    }
}