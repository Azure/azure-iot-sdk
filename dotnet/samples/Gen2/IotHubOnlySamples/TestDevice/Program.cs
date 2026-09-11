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

    private static Task<DirectMethodProbeAck> DirectMethodProbeReceivedAsync(DirectMethodRequestProbeReceivedEventArgs arg)
    {
        Console.WriteLine("Received direct method probe for method with name " + arg.MethodName + " and response timeout seconds " + arg.ResponseTimeoutSeconds + ". Responding with positive probe ack");
        return Task.FromResult(DirectMethodProbeAck.Accepted());
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