using Microsoft.Azure.Devices.Client;
using Microsoft.Azure.Devices.Client.Telemetry;
using Microsoft.Azure.Devices.Client.Twin;
using SetupSampleDevice;
using System.Security.Cryptography.X509Certificates;
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

        TelemetryClient telemetryClient = new TelemetryClient(connectionClient);

        ProvisioningSettings provisioningSettings = new(idScope);
        var connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication);
        Console.WriteLine($"Device {deviceId} is now provisioned and connected to IoT Hub.");
        Console.WriteLine("Press 'Ctrl+C' to end the sample");

        while (!cts.Token.IsCancellationRequested)
        {
            OutgoingTelemetryMessage outgoingTelemetry = new()
            {
                Payload = Encoding.UTF8.GetBytes("Hello world!"),
                MessageId = Guid.NewGuid().ToString(),
            };

            outgoingTelemetry.UserProperties.Add("SomeCustomUserPropertyKey", "SomeCustomUserPropertyValue");

            Console.WriteLine($"Sending telemetry with message Id {outgoingTelemetry.MessageId}");
            await telemetryClient.SendTelemetryAsync(outgoingTelemetry);
            await Task.Delay(TimeSpan.FromSeconds(1));
        }

        await connectionClient.DisconnectAsync();
    }
}