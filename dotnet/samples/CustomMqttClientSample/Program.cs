using Microsoft.Azure.Devices.Client;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.MQTTnetAdapter;
using System.Diagnostics;
using System.Security.Cryptography.X509Certificates;

internal class Program
{
    private const bool EnableMqttLogs = true;

    private static async Task Main(string[] args)
    {
        string idScope = Environment.GetEnvironmentVariable("DPS_ID_SCOPE") ?? throw new Exception("");
        string pcks12CertificatePath = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PATH") ?? throw new Exception("");
        string pcks12CertificatePassword = Environment.GetEnvironmentVariable("X509_CERTIFICATE_PASSWORD") ?? throw new Exception("");
        X509AuthenticationProvider authentication = new(X509CertificateLoader.LoadPkcs12FromFile(pcks12CertificatePath, pcks12CertificatePassword));

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

        await Task.Delay(TimeSpan.FromSeconds(1));

        await connectionClient.DisconnectAsync();
    }
}