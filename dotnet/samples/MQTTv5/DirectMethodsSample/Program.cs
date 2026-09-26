// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using DirectMethodsClientSample;
using Google.Protobuf;
using Microsoft.Azure.Iot.Device;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.DirectMethods;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.DirectMethods;
using SetupSampleDevice;
using System.Text.Json;

internal class Program
{
    private const string MethodName = "testMethod";

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
        Func<DirectMethodRequestProbeReceivedEventArgs, Task<DirectMethodProbeAck>> HandleDirectMethodProbeAsync = async (args) =>
        {
            if (args.MethodName.Equals(MethodName))
            {
                Console.WriteLine($"Received direct method probe for the expected method '{args.MethodName}'. Responding to IoT Hub that this device is ready for it.");
                return DirectMethodProbeAck.Accepted();
            }

            Console.WriteLine($"Received direct method probe for an unknown method '{args.MethodName}'. Rejecting it.");
            return DirectMethodProbeAck.Rejected(RejectedReason.MethodNotFound);
        };

        Func<DirectMethodRequestReceivedEventArgs, Task<DirectMethodResponse>> HandleDirectMethodAsync = (args) =>
        {
            Console.WriteLine($"Received direct method with name {args.MethodName}");
            if (args.MethodName.Equals(MethodName))
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

        directMethodClient.DirectMethodProbeReceivedAsync += HandleDirectMethodProbeAsync;
        directMethodClient.DirectMethodInvokedAsync += HandleDirectMethodAsync;

        ProvisioningSettings provisioningSettings = new(idScope);
        await connectionClient.ProvisionAndConnectAsync(provisioningSettings, authentication, cancellationToken: cts.Token);
        Console.WriteLine($"Device {deviceId} is now provisioned and connected to IoT Hub. Now waiting for direct method invocations with direct method name '{MethodName}'...");

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