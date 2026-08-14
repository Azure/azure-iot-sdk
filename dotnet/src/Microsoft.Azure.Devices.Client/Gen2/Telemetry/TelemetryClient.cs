using Microsoft.Azure.Devices.Client.Exceptions;
using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Models.Telemetry;
using Microsoft.Azure.Devices.Client.Mqtt;
using System.Diagnostics;
using System.Globalization;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Gen2.Telemetry
{
    /// <summary>
    /// A feature client for sending device-to-cloud telemetry and receiving cloud-to-device telemetry.
    /// </summary>
    public class TelemetryClient : IDisposable
    {
        private bool _isDisposed = false;

        internal const string DeviceBoundMessagesTopicFormat = "devices/{0}/messages/devicebound/";

        private const string NewTelemetryTopicFormat = "ih/{0}/srv/telemetry";

        private IConnectionClient _connection;

        public const string MessagePropertyCorrelationId = "$.cid";
        public const string MessagePropertyMessageId = "$.mid";
        public const string MessagePropertyContentType = "$.ct";
        public const string MessagePropertyContentEncoding = "$.ce";

        public event Func<CloudToDeviceTelemetry, Task>? CloudToDeviceTelemetryReceivedAsync; //Not supported yet

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
            _connection.PublishReceivedAsync += HandleReceivedMqttPublish;
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

        private async Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            Trace.TraceError("c2d is not supported when connected to AEG Hub currently");
        }

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;
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
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;
            _connection.Dispose();

            _isDisposed = true;
        }
    }
}
