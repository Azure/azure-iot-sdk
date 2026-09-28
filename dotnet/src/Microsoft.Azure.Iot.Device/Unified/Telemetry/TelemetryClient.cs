// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Unified.Connection;

namespace Microsoft.Azure.Iot.Device.Unified.Telemetry
{
    /// <summary>
    /// A feature client for sending device-to-cloud telemetry.
    /// </summary>
    public class TelemetryClient : IDisposable
    {
        private bool _isDisposed = false;

        private const string ClassicTelemetryTopicFormat = "devices/{0}/messages/events/";

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
        public async Task SendTelemetryAsync(DeviceToCloudTelemetry message, CancellationToken cancellationToken = default)
        {
            ObjectDisposedException.ThrowIf(_isDisposed, this);

            var currentConnectionContext = _connection.GetCurrentConnectionContext();
            if (currentConnectionContext == null)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            string deviceId = currentConnectionContext.DeviceId;

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

            // Puback is checked for non-success cases under this layer, so no need to check it here as well
            MqttPublishAck puback = await _connection.PublishAsync(mqttMessage, cancellationToken);
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            if (disposing)
            {
                _connection.Dispose();
            }

            _isDisposed = true;
        }

        /// <summary>
        /// Releases the unmanaged resources and disposes of the managed resources used by this client 
        /// </summary>
        public void Dispose()
        {
            _connection.Dispose();
            _isDisposed = true;
        }
    }
}
