using Microsoft.Azure.Devices.Client.Gen2.Telemetry;
using Microsoft.Azure.Devices.Client.Models;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.Unified.Twin;
using System.Reflection;
using System.Runtime.InteropServices;

namespace Microsoft.Azure.Devices.Client.Unified.Connection
{
    internal class IotHubConnection //TODO maybe just move this code into connection client?
    {
        internal const string ClassicHubApiVersion = "2025-08-01-preview";

        internal static async Task ConnectToAzureEventGridIotHubAsync(IMqttClient mqttClient, string hostname, string deviceId, X509AuthenticationProvider x509AuthenticationProvider, CancellationToken cancellationToken = default)
        {
            await Gen2.Connection.IotHubConnection.ConnectToAzureEventGridIotHubAsync(mqttClient, hostname, deviceId, x509AuthenticationProvider, null, cancellationToken);
        }

        internal static async Task ConnectToClassicIotHubAsync(IMqttClient mqttClient, string hostname, string deviceId, X509AuthenticationProvider x509AuthenticationProvider, CancellationToken cancellationToken = default)
        {
            string clientId = deviceId;
            //TODO what is the latest Hub API version?
            string username = $"{hostname}/{clientId}/?api-version={ClassicHubApiVersion}&DeviceClientType={Uri.EscapeDataString(GetUserAgentString())}";

            MqttConnect connectPacket = new MqttConnect()
            {
                HostName = hostname,
                TcpPort = 8883,
                WebsocketPort = 443,
                WebsocketUri = $"wss://{hostname}/$iothub/websocket",
                ClientCertificate = x509AuthenticationProvider.ClientCertificate,
                CleanSession = true, //TODO user configurable value
                Username = username,
                Password = Array.Empty<byte>(),
                ClientId = clientId,
                ProtocolVersion = MqttProtocolVersion.V311
            };

            var connack = await mqttClient.ConnectAsync(connectPacket, cancellationToken);

            ConnectRejectedException.ThrowIfUnsuccessfulConnack(connack, "Connection to IoT Hub was rejected.");

            //TODO feels a bit weird to do these subs outside of the method client/twin client, and it forces the user to construct their direct method/twin clients
            //and set their callbacks before connecting, but not sure what other approach works when AEG style hub mandates subscriptions as part of the connect birth message

            //TODO check for previous connack isSessionPresent flag before firing off all these subscriptions?
            MqttSubscribe mqttSubscribe = new();
            var expectedQos = MqttQualityOfServiceLevel.AtMostOnce;
            mqttSubscribe.TopicFilters.Add(new(string.Format(TelemetryClient.DeviceBoundMessagesTopicFormat + "#", deviceId), expectedQos));
            mqttSubscribe.TopicFilters.Add(new(TwinClient.ClassicTwinResponseTopic + "#", expectedQos));
            mqttSubscribe.TopicFilters.Add(new(TwinClient.ClassicTwinDesiredPropertiesPatchTopic + "#", expectedQos));
            mqttSubscribe.TopicFilters.Add(new(DirectMethods.Unified.DirectMethodClient.ClassicDirectMethodsRequestTopic + "#", expectedQos));
            var suback = await mqttClient.SubscribeAsync(mqttSubscribe, cancellationToken);

            foreach (var topicSuback in suback.Items)
            {
                if (topicSuback.ResultCode != MqttClientSubscribeResultCode.GrantedQoS0)
                {
                    throw new Exception("TODO");
                }
            }
        }

        private static string GetUserAgentString()
        {
            const string name = "Microsoft.Azure.Devices.Client";

            string runtime = RuntimeInformation.FrameworkDescription.Trim();
            string operatingSystem = RuntimeInformation.OSDescription.Trim();
            string processorArchitecture = RuntimeInformation.ProcessArchitecture.ToString().Trim();

            string userAgent = $"{name}/{GetPackageVersion()} ({runtime}; {operatingSystem}; {processorArchitecture})";

            return userAgent;
        }

        private static string GetPackageVersion()
        {
            return typeof(IotHubConnection).GetTypeInfo().Assembly.GetName().Version!.ToString(3);
        }
    }
}
