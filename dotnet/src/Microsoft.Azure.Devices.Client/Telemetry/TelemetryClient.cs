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

        public TelemetryClient(ConnectionClient connection)
        {
            _connection = connection;
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
            if (_connection.CurrentConnectionContext == null)
            {
                throw new NotSupportedException("Must open the connection before sending telemetry");
            }

            if (_connection.CurrentConnectionContext.IsAzureEventGrid)
            {
                throw new NotImplementedException("d2c telemetry not implemented in this SDK yet");
            }
            else
            {
                //TODO fill in content type, encoding, etc from message user properties
                var mqttMessage = new MqttPublish
                {
                    Topic = "devices/" + _connection.CurrentConnectionContext.DeviceId + "/messages/events/",
                    PayloadAsByteArray = message.Payload,
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                };

                await _connection.PublishAsync(mqttMessage, cancellationToken);
            }
        }

        private async void HandleConnectionAsync(MqttClientConnectedEventArgs args)
        {
        }

        private async void HandleDisconnectionAsync(MqttClientDisconnectedEventArgs args)
        {
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
