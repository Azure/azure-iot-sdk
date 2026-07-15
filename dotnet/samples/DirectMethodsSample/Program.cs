using DirectMethodsClientSample;
using Microsoft.Azure.Devices.Client;
using Microsoft.Azure.Devices.Client.DirectMethods;
using SetupSampleDevice;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;

internal class Program
{
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

        using DirectMethodClient directMethodClient = new DirectMethodClient(connectionClient);
        Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>> HandleDirectMethodAsync = (args) =>
        {
            Console.WriteLine($"Received direct method with name {args.MethodName}");
            if (args.MethodName.Equals("testMethod"))
            {
                DirectMethodRequestPayloadObject? directMethodRequestPayload = null;
                try
                {
                    directMethodRequestPayload = JsonSerializer.Deserialize<DirectMethodRequestPayloadObject>(args.Payload);
                }
                catch (JsonException)
                {
                    Console.WriteLine("Received an unexpected payload format. Responding to direct method request with 400 response");
                    return Task.FromResult(new DirectMethodResponse() { Status = 400 });
                }

                if (directMethodRequestPayload == null)
                {
                    Console.WriteLine("Received an unexpected payload format. Responding to direct method request with 400 response");
                    return Task.FromResult(new DirectMethodResponse() { Status = 400 });
                }

                DirectMethodResponsePayloadObject directMethodResponsePayloadObject = new()
                {
                    SomeBooleanField = false,
                    SomeLongField = 10000000,
                };

                Console.WriteLine($"Responding to the direct method request with a 200 response");

                return Task.FromResult(new DirectMethodResponse()
                {
                    Status = 200,
                    Payload = JsonSerializer.SerializeToUtf8Bytes(directMethodResponsePayloadObject),
                });
            }
            else
            {
                Console.WriteLine($"Received a direct method request with an unexpected method name {args.MethodName}. Responding to direct method request with 404 response");
                return Task.FromResult(new DirectMethodResponse() { Status = 404 });
            }
        };
        directMethodClient.DirectMethodInvokedAsync += HandleDirectMethodAsync;

        ProvisioningSettings provisioningSettings = new(idScope);
        await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication);
        Console.WriteLine($"Device {deviceId} is now provisioned and connected to IoT Hub. Now waiting for direct method invocations...");

        try
        {
            Console.WriteLine("Press 'Ctrl+C' to end the sample");
            await Task.Delay(-1, cts.Token);
        }
        catch (OperationCanceledException)
        {
            Console.WriteLine("Sample timeout has completed. Shutting down the sample...");
        }

        directMethodClient.DirectMethodInvokedAsync -= HandleDirectMethodAsync;
        await connectionClient.DisconnectAsync();
    }
}