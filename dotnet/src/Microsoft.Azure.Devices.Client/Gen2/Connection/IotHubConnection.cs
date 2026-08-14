using Google.Protobuf;
using Microsoft.Azure.Devices.Client.Gen2.Twin;
using Microsoft.Azure.Devices.Client.Models.Twin;
using Microsoft.Azure.Devices.Client.Mqtt;
using System.Diagnostics;
using System.Reflection;
using System.Text;
using System.Text.Json.Nodes;

namespace Microsoft.Azure.Devices.Client.Gen2.Connection
{
    internal class IotHubConnection //TODO maybe just move this code into connection client?
    {
         internal static async Task ConnectToAzureEventGridIotHubAsync(IMqttClient mqttClient, string hostname, string deviceId, X509AuthenticationProvider x509AuthenticationProvider, TwinPushOptions? twinPushOptions, CancellationToken cancellationToken = default)
        {
            
        }


    }
}
