using Microsoft.Azure.Devices.Client.CertificateManagement;
using Microsoft.Azure.Devices.Client.Mqtt;
using System.Globalization;

namespace Microsoft.Azure.Devices.Client.Telemetry
{
    public class TelemetryClient
    {
        private ConnectionClient _connection;

        internal const string DeviceBoundMessagesTopicFormat = "devices/{0}/messages/devicebound/";

        private static readonly Dictionary<string, string> s_toSystemPropertiesMap = new()
        {
            { IotHubWirePropertyNames.AbsoluteExpiryTime, MessageSystemPropertyNames.ExpiryTimeUtc },
            { IotHubWirePropertyNames.ConnectionDeviceId, MessageSystemPropertyNames.ConnectionDeviceId },
            { IotHubWirePropertyNames.ConnectionModuleId, MessageSystemPropertyNames.ConnectionModuleId },
            { IotHubWirePropertyNames.ContentEncoding, MessageSystemPropertyNames.ContentEncoding },
            { IotHubWirePropertyNames.ContentType, MessageSystemPropertyNames.ContentType },
            { IotHubWirePropertyNames.CorrelationId, MessageSystemPropertyNames.CorrelationId },
            { IotHubWirePropertyNames.CreationTimeUtc, MessageSystemPropertyNames.CreationTimeUtc },
            { IotHubWirePropertyNames.InterfaceId, MessageSystemPropertyNames.InterfaceId },
            { IotHubWirePropertyNames.MessageId, MessageSystemPropertyNames.MessageId },
            { IotHubWirePropertyNames.MessageSchema, MessageSystemPropertyNames.MessageSchema },
            { IotHubWirePropertyNames.MqttDiagIdKey, MessageSystemPropertyNames.DiagId },
            { IotHubWirePropertyNames.MqttDiagCorrelationContextKey, MessageSystemPropertyNames.DiagCorrelationContext },
            { IotHubWirePropertyNames.OutputName, MessageSystemPropertyNames.OutputName },
            { IotHubWirePropertyNames.To, MessageSystemPropertyNames.To },
            { IotHubWirePropertyNames.UserId, MessageSystemPropertyNames.UserId },
        };

        /// <summary>
        /// An event that executes every time this device receives cloud-to-device telemetry. Once received, the application must decide how to complete that telemetry.
        /// </summary>
        public event Func<CloudToDeviceMessage, Task<CompletionType>>? CloudToDeviceMessageReceivedAsync;

        public TelemetryClient(ConnectionClient connection)
        {
            _connection = connection;
            _connection.ApplicationMessageReceivedAsync += HandleReceivedMqttPublish;
            _connection.DisconnectedAsync += HandleDisconnectionAsync;
            _connection.ConnectedAsync += HandleConnectionAsync;
        }

        /// <summary>
        /// Send device-to-cloud telemetry.
        /// </summary>
        /// <param name="message">The message to send.</param>
        /// <param name="cancellationToken">the cancellation token.</param>
        public async Task SendTelemetryAsync(OutgoingTelemetryMessage message, CancellationToken cancellationToken = default)
        {
            //TODO fill in content type, encoding, etc from message user properties
            var mqttMessage = new MqttPublish
            {
                Topic = "someTelemetryTopic",
                PayloadAsByteArray = message.Payload
            };

            await _connection.PublishAsync(mqttMessage, cancellationToken);
        }

        private async void HandleConnectionAsync(MqttClientConnectedEventArgs args)
        {
        }

        private async void HandleDisconnectionAsync(MqttClientDisconnectedEventArgs args)
        {
        }

        private async Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (_connection.CurrentConnectionContext!.IsAzureEventGrid)
            {

            }
            else
            {
                //TODO it is a bit awkward to build this now instead of in the constructor, but the deviceId isn't present until after hub connection :/
                var expectedDeviceBoundMessagesTopic = string.Format(CultureInfo.InvariantCulture, DeviceBoundMessagesTopicFormat, _connection.CurrentConnectionContext!.DeviceId);

                if (args.Publish.Topic.StartsWith(expectedDeviceBoundMessagesTopic))
                {
                    if (CloudToDeviceMessageReceivedAsync != null)
                    {
                        var receivedCloudToDeviceMessage = new CloudToDeviceMessage(args.Publish.PayloadAsByteArray);

                        // Device bound messages could be in 2 formats, depending on whether it is going to the device, or to a module endpoint
                        // Format 1 - going to the device - devices/{deviceId}/messages/devicebound/{properties}/
                        // Format 2 - going to module endpoint - devices/{deviceId}/modules/{moduleId/endpoints/{endpointId}/{properties}/
                        // So choose the right format to deserialize properties.
                        string[] topicSegments = args.Publish.Topic.Split("/", StringSplitOptions.RemoveEmptyEntries);
                        string propertiesSegment = topicSegments.Length > 6 ? topicSegments[6] : topicSegments[4];

                        Dictionary<string, string> properties = UrlEncodedDictionarySerializer.Deserialize(propertiesSegment, 0);
                        foreach (KeyValuePair<string, string> property in properties)
                        {
                            if (s_toSystemPropertiesMap.TryGetValue(property.Key, out string propertyName))
                            {
                                receivedCloudToDeviceMessage.SystemProperties[propertyName] = ConvertToSystemProperty(property);
                            }
                            else
                            {
                                receivedCloudToDeviceMessage.Properties[property.Key] = property.Value;
                            }
                        }

                        var completionType = await CloudToDeviceMessageReceivedAsync.Invoke(receivedCloudToDeviceMessage);
                        //TODO delayed ack support
                        if (completionType == CompletionType.Complete)
                        {
                            await args.AcknowledgeAsync(CancellationToken.None);
                        }
                    }
                }
            }
        }

        private static object ConvertToSystemProperty(KeyValuePair<string, string> property)
        {
            if (string.IsNullOrEmpty(property.Value))
            {
                return property.Value;
            }

            if (property.Key == IotHubWirePropertyNames.AbsoluteExpiryTime
                || property.Key == IotHubWirePropertyNames.CreationTimeUtc)
            {
                return DateTime.ParseExact(property.Value, "o", CultureInfo.InvariantCulture);
            }

            return property.Value;
        }
    }
}
