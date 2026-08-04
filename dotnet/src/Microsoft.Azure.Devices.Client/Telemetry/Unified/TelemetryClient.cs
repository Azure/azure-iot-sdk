using Microsoft.Azure.Devices.Client.Connection.Models;
using Microsoft.Azure.Devices.Client.Connection.Unified;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.Telemetry.Models;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Telemetry.Unified
{
    /// <summary>
    /// A feature client for sending device-to-cloud telemetry.
    /// </summary>
    public class TelemetryClient
    {
        private const string ClassicTelemetryTopicFormat = "devices/{0}/messages/events/";
        private const string NewTelemetryTopicFormat = "ih/{0}/srv/telemetry";

        private IConnectionClient _connection;

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
        public TelemetryClient(IConnectionClient connection)
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
        /// <exception cref="MessageTooLargeException">Thrown if the message's payload's size exceeds the supported limits of IoT hub.</exception>
        public async Task SendTelemetryAsync(OutgoingTelemetryMessage message, CancellationToken cancellationToken = default)
        {
            var currentConnectionContext = _connection.GetCurrentConnectionContext();
            if (currentConnectionContext == null)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            string deviceId = currentConnectionContext.DeviceId;

            if (currentConnectionContext.IsAzureEventGrid)
            {
                // TODO do we even need to pre-empt like this with AEG? Maybe AEG sends back a proper error code on the publish that we can translate to this exception.
                // Needs manual testing once AEG hub is more available
                if (message.Payload != null && message.Payload.Length > 255000)
                {
                    throw new MessageTooLargeException("This telemetry message is too large to be accepted by IoT Hub. It will not be sent.");
                }

                // AEG (MQTT5) hubs use a flat device->service telemetry topic and native MQTT 5
                // message properties, instead of the classic MQTTv3 property-bag topic string.
                // The AEG namespace routes this topic to the hub's routing backend, which converts
                // and delivers it to configured routing endpoints (e.g. Event Hubs).
                var mqttMessage = new MqttPublish
                {
                    Topic = string.Format(NewTelemetryTopicFormat, deviceId),
                    PayloadAsReadOnlySequence = message.PayloadAsReadOnlySequence,
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,

                    // Telemetry payloads in the AEG flow are UTF-8 text/JSON; this surfaces as
                    // content-encoding: utf-8 on the routed message.
                    PayloadFormatIndicator = MqttPayloadFormatIndicator.CharacterData,
                };

                if (message.ContentType != null)
                {
                    mqttMessage.ContentType = message.ContentType;
                }

                if (message.CorrelationId != null)
                {
                    mqttMessage.CorrelationData = Encoding.UTF8.GetBytes(message.CorrelationId);
                }

                // MQTT 5 has no native message-id or content-encoding field, so carry them as user
                // properties (reusing the classic $.mid / $.ce keys for naming consistency).
                if (message.MessageId != null)
                {
                    mqttMessage.UserProperties.Add(new MqttUserProperty(MessagePropertyMessageId, message.MessageId));
                }

                if (message.ContentEncoding != null)
                {
                    mqttMessage.UserProperties.Add(new MqttUserProperty(MessagePropertyContentEncoding, message.ContentEncoding));
                }

                foreach (var customUserPropertyKey in message.UserProperties.Keys)
                {
                    mqttMessage.UserProperties.Add(new MqttUserProperty(customUserPropertyKey, message.UserProperties[customUserPropertyKey]));
                }

                MqttPublishAck aegPuback = await _connection.PublishAsync(mqttMessage, cancellationToken);

                PublishRejectedException.ThrowIfUnsuccessfulPuback(aegPuback, "Failed to publish this telemetry because the MQTT broker rejected it.");
            }
            else
            {
                if (message.Payload != null && message.Payload.Length > 255000) //Leaving some buffer b/c classic hub message size calc is not strictly about payload size
                {
                    throw new MessageTooLargeException("This telemetry message is too large to be accepted by IoT Hub. It will not be sent.");
                }

                //TODO fill in content type, encoding, etc from message user properties
                var mqttMessage = new MqttPublish
                {
                    Topic = string.Format(ClassicTelemetryTopicFormat, deviceId),
                    PayloadAsReadOnlySequence = message.PayloadAsReadOnlySequence,
                    QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce,
                };

                // When publishing to MQTTv3 Hub, the topic string includes all the system properties (correlation id, message id, etc.)
                // and all the custom user properties. The values of all these properties must be URL encoded. The user property keys should
                // also be URL encoded, but the system properties' keys should not be URL encoded.
                if (message.MessageId != null)
                {
                    mqttMessage.Topic += $"&{MessagePropertyMessageId}={Uri.EscapeDataString(message.MessageId)}";
                }

                if (message.CorrelationId != null)
                {
                    mqttMessage.Topic += $"&{MessagePropertyCorrelationId}={Uri.EscapeDataString(message.CorrelationId)}";
                }

                if (message.ContentType != null)
                {
                    mqttMessage.Topic += $"&{MessagePropertyContentType}={Uri.EscapeDataString(message.ContentType)}";
                }

                if (message.ContentEncoding != null)
                {
                    mqttMessage.Topic += $"&{MessagePropertyContentEncoding}={Uri.EscapeDataString(message.ContentEncoding)}";
                }

                foreach (var customUserPropertyKey in message.UserProperties.Keys)
                { 
                    mqttMessage.Topic += $"&{Uri.EscapeDataString(customUserPropertyKey)}={Uri.EscapeDataString(message.UserProperties[customUserPropertyKey])}";
                }

                MqttPublishAck puback = await _connection.PublishAsync(mqttMessage, cancellationToken);

                PublishRejectedException.ThrowIfUnsuccessfulPuback(puback, "Failed to publish this telemetry because the MQTT broker rejected it.");
            }
        }
    }
}
