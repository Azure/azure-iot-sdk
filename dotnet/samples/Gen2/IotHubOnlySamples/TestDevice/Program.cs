using Google.Protobuf;
using Microsoft.Azure.Devices.Client;
using System.Diagnostics;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using Microsoft.Azure.Devices.Client.Gen2.Twin;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Gen2.DirectMethods;
using Microsoft.Azure.Devices.Client.Models.DirectMethods;
using Microsoft.Azure.Devices.Client.Models.Twin;

internal class Program
{
    const string Pcks12CertificatePath = "../../../../certificate.pfx";
    const string Pcks12CertificatePassword = "Some dummy certificate password";

    const bool EnableMqttLogs = true;

    // This sample skips DPS entirely and connects directly to IoT hub.
    private static async Task Main(string[] args)
    {
        using CancellationTokenSource cts = new CancellationTokenSource();
        cts.CancelAfter(TimeSpan.FromHours(1));

        if (!File.Exists("../../../../deviceId.txt"))
        {
            throw new Exception("Run the SetupDevice sample first to generate a test device + credentials");
        }

        string deviceId = File.ReadAllText("../../../../deviceId.txt");
        string hostName = File.ReadAllText("../../../../hostname.txt");

        X509AuthenticationProvider authentication = new(X509CertificateLoader.LoadPkcs12FromFile(Pcks12CertificatePath, Pcks12CertificatePassword));

        // Remove this to eliminate all SDK + MQTT level logs
        Trace.Listeners.Add(new ConsoleTraceListener());

        ConnectionClientOptions options = new()
        {
            EnableMqttLogging = EnableMqttLogs,
        };
        ConnectionClient connectionClient = new ConnectionClient(options);

        ConnectionContext connectionContext = new()
        {
            DeviceId = deviceId,
            IotHubHostName = hostName,
            AuthenticationProvider = authentication,
        };

        TwinClient twinClient = new(connectionClient);
        twinClient.DesiredPatchReceived += DesiredPatchReceived;
        twinClient.TwinPushReceived += TwinPushReceived;

        DirectMethodClient directMethodClient = new(connectionClient);
        directMethodClient.DirectMethodProbeReceivedAsync += DirectMethodProbeReceivedAsync;
        directMethodClient.DirectMethodInvokedAsync += DirectMethodInvokedAsync;

        await connectionClient.ConnectAsync(connectionContext, null, cancellationToken: cts.Token);

        Console.WriteLine($"Connected to IoT hub as device with Id {deviceId}. Now listening for twin/direct method messages");
        await Task.Delay(-1, cts.Token);
    }

    private static Task<DirectMethodResponse> DirectMethodInvokedAsync(DirectMethodRequestReceivedEventArgs arg)
    {
        if (arg.Payload != null)
        {
            // Assumes the payload was a utf8 encoded string of some sort
            Console.WriteLine("Received direct method invocation with name " + arg.MethodName + " and payload " + Encoding.UTF8.GetString(arg.Payload));
        }
        else
        {
            Console.WriteLine("Received direct method invocation with name " + arg.MethodName + " and no payload");
        }

        Console.WriteLine("Sending direct method response with status 200");
        return Task.FromResult(new DirectMethodResponse()
        {
            Status = 200,
            Payload = JsonSerializer.SerializeToUtf8Bytes(new
            {
                message = "Some response payload"
            }),
        });
    }

    private static Task<ProbeAck> DirectMethodProbeReceivedAsync(DirectMethodRequestProbeReceivedEventArgs arg)
    {
        Console.WriteLine("Received direct method probe for method with name " + arg.MethodName + " and response timeout seconds " + arg.ResponseTimeoutSeconds + ". Responding with positive probe ack");
        return Task.FromResult(new ProbeAck()
        { 
            Ready = new Ready()
            { 
                ReadyId = ByteString.CopyFrom(Guid.NewGuid().ToByteArray()),
            }
        });
    }

    private static void DesiredPatchReceived(DesiredPatchReceivedEventArgs args)
    {
        Console.WriteLine("Received desired patch with version " + args.DesiredPropertiesVersion + " and properties: " + JsonSerializer.Serialize(args.DesiredProperties));
    }

    private static void TwinPushReceived(TwinPushReceivedEventArgs args)
    {
        if (args.Desired != null && args.Reported != null)
        {
            Console.WriteLine("Received twin push with desired version " + args.Desired + " and desired properties: " + JsonSerializer.Serialize(args.Desired.Properties) + " and reported version " + args.Reported.PropertiesVersion + " and reported properties " + JsonSerializer.Serialize(args.Reported.Properties));
        }
        else if (args.Desired != null)
        {
            Console.WriteLine("Received twin push with desired version " + args.Desired + " and desired properties: " + JsonSerializer.Serialize(args.Desired.Properties));
        }
        else if (args.Reported != null)
        {
            Console.WriteLine("Received twin push with reported version " + args.Reported.PropertiesVersion + " and reported properties " + JsonSerializer.Serialize(args.Reported.Properties));
        }
        else
        {
            Console.WriteLine("Received twin push with no reported section or desired section");
        }
    }
}