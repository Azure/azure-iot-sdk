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
        cts.CancelAfter(TimeSpan.FromSeconds(20));

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
                DirectMethodRequestPayloadObject? directMethodRequestPayload = JsonSerializer.Deserialize<DirectMethodRequestPayloadObject>(args.Payload);

                if (directMethodRequestPayload == null)
                {
                    Console.WriteLine("Received an unexpected payload format");
                    return Task.FromResult(new DirectMethodResponse() { Status = 400 });
                }

                DirectMethodResponsePayloadObject directMethodResponsePayloadObject = new()
                {
                    SomeBooleanField = false,
                    SomeLongField = 10000000,
                };

                // Note that the user callback here doesn't know/doesn't care if the IoT hub was a classic IoT hub vs an AEG IoT hub. It works for both.
                return Task.FromResult(new DirectMethodResponse()
                {
                    Status = 200,
                    Payload = JsonSerializer.SerializeToUtf8Bytes(directMethodResponsePayloadObject),
                });
            }
            else
            {
                // Undefined method was invoked, so return "not found"
                return Task.FromResult(new DirectMethodResponse() { Status = 404 });
            }
        };
        directMethodClient.DirectMethodInvokedAsync += HandleDirectMethodAsync;

        ProvisioningSettings provisioningSettings = new(idScope);
        await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication);
        Console.WriteLine("Device is connected and now waiting for direct method invocations...");

        await Task.Delay(-1, cts.Token);

        directMethodClient.DirectMethodInvokedAsync -= HandleDirectMethodAsync;
        await connectionClient.DisconnectAsync();
    }
}