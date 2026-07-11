using Microsoft.Azure.Devices.Client.Mqtt;
using System.Globalization;

namespace Microsoft.Azure.Devices.Client.Telemetry
{
    /// <summary>
    /// A feature client for sending device-to-cloud telemetry.
    /// </summary>
    public class TelemetryClient
    {
        private ConnectionClient _connection;

        internal const string DeviceBoundMessagesTopicFormat = "devices/{0}/messages/devicebound/";

        public const string MessagePropertyCorrelationId = "$.cid";
        public const string MessagePropertyMessageId = "$.mid";
        public const string MessagePropertyContentType = "$.ct";
        public const string MessagePropertyContentEncoding = "$.ce";

        /// <summary>
        /// Construct a new <see cref="TelemetryClient"/> instance.
        /// </summary>
        /// <param name="connection">The connection client this feature client will use.</param>
        /// <remarks>
        /// The provided connection client does not need to be connected before this constructor is called. However, the provided connection client must be connected prior
        /// to using this feature client to send any telemetry.
        /// </remarks>
        public TelemetryClient(ConnectionClient connection)
        {
            _connection = connection;
        }


        /// <summary>
        /// Send device-to-cloud telemetry.
        /// </summary>
        /// <param name="message">The message to send.</param>
        /// <param name="cancellationToken">the cancellation token.</param>
        /// <exception cref="NotSupportedException">Thrown only if this method is called while the provided <see cref="ConnectionClient"/> is disconnected and not trying to reconnect.</exception>
        /// <exception cref="PublishRejectedException">Thrown if this telemetry message is rejected by IoT Hub for any reason.</exception>
        public async Task SendTelemetryAsync(OutgoingTelemetryMessage message, CancellationToken cancellationToken = default)
        {
            if (_connection.CurrentConnectionContext == null)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            if (_connection.CurrentConnectionContext.IsAzureEventGrid)
            {
                throw new NotImplementedException("d2c telemetry not implemented for AEG Hub in this SDK yet");
            }
            else
            {
                //TODO fill in content type, encoding, etc from message user properties
                var mqttMessage = new MqttPublish
                {
                    Topic = "devices/" + _connection.CurrentConnectionContext.DeviceId + "/messages/events/",
                    PayloadAsReadOnlySequence = message.PayloadAsReadOnlySequence,
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                };

                if (message.MessageId != null)
                {
                    mqttMessage.Topic += $"&{MessagePropertyMessageId}={message.MessageId}";
                }

                if (message.CorrelationId != null)
                {
                    mqttMessage.Topic += $"&{MessagePropertyCorrelationId}={message.CorrelationId}";
                }

                if (message.ContentType != null)
                {
                    mqttMessage.Topic += $"&{MessagePropertyContentType}={message.ContentType}";
                }

                if (message.ContentEncoding != null)
                {
                    mqttMessage.Topic += $"&{MessagePropertyContentEncoding}={message.ContentEncoding}";
                }

                foreach (var customUserPropertyKey in message.UserProperties.Keys)
                { 
                    mqttMessage.Topic += $"&{customUserPropertyKey}={message.UserProperties[customUserPropertyKey]}";
                }

                MqttPublishAck puback = await _connection.PublishAsync(mqttMessage, cancellationToken);

                PublishRejectedException.ThrowIfUnsuccessfulPuback(puback, "Failed to publish this telemetry because the MQTT broker rejected it.");
            }
        }
    }
}
