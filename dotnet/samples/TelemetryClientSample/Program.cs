using Microsoft.Azure.Devices.Client;
using Microsoft.Azure.Devices.Client.Provisioning;
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
        CloudToDeviceTelemetryClient cloudToDeviceTelemetryClient = new CloudToDeviceTelemetryClient(connectionClient);

        cloudToDeviceTelemetryClient.CloudToDeviceTelemetryReceivedAsync += async (args) =>
        {
            Console.WriteLine($"Received a cloud to device message with message id {args.MessageId}");
        };

        ProvisioningSettings provisioningSettings = new(idScope);
        var connectionContext = await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: cts.Token);
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