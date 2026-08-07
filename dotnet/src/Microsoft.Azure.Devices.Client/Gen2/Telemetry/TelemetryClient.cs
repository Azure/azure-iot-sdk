using Microsoft.Azure.Devices.Client.Exceptions;
using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Models.Telemetry;
using Microsoft.Azure.Devices.Client.Mqtt;
using System.Globalization;
using System.Text;

namespace Microsoft.Azure.Devices.Client.Gen2.Telemetry
{
    /// <summary>
    /// A feature client for sending device-to-cloud telemetry and receiving cloud-to-device telemetry.
    /// </summary>
    public class TelemetryClient : IDisposable
    {
        internal const string DeviceBoundMessagesTopicFormat = "devices/{0}/messages/devicebound/";

        private const string NewTelemetryTopicFormat = "ih/{0}/srv/telemetry";

        private IConnectionClient _connection;

        public const string MessagePropertyCorrelationId = "$.cid";
        public const string MessagePropertyMessageId = "$.mid";
        public const string MessagePropertyContentType = "$.ct";
        public const string MessagePropertyContentEncoding = "$.ce";

        public event Func<CloudToDeviceTelemetry, Task>? CloudToDeviceTelemetryReceivedAsync;

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
            _connection.MqttClient.PublishReceivedAsync += HandleReceivedMqttPublish;
        }

        internal TelemetryClient(Unified.Connection.IConnectionClient connection)
        {
            _connection = new ConnectionClient(connection);
            _connection.MqttClient.PublishReceivedAsync += HandleReceivedMqttPublish;
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

            MqttPublishAck aegPuback = await _connection.MqttClient.PublishAsync(mqttMessage, cancellationToken);

            PublishRejectedException.ThrowIfUnsuccessfulPuback(aegPuback, "Failed to publish this telemetry because the MQTT broker rejected it.");
        }

        private async Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (_connection.GetCurrentConnectionContext()!.IsAzureEventGrid)
            {
                throw new NotSupportedException("c2d is not supported when connected to AEG Hub currently");
            }
            else
            {
                //TODO it is a bit awkward to build this now instead of in the constructor, but the deviceId isn't present until after hub connection :/
                var expectedDeviceBoundMessagesTopic = string.Format(CultureInfo.InvariantCulture, DeviceBoundMessagesTopicFormat, _connection.GetCurrentConnectionContext()!.DeviceId);

                if (args.Publish.Topic.StartsWith(expectedDeviceBoundMessagesTopic))
                {
                    if (CloudToDeviceTelemetryReceivedAsync != null)
                    {
                        var receivedCloudToDeviceMessage = new CloudToDeviceTelemetry(args.Publish.Payload);

                        // devices/{device-id}/messages/devicebound/{property-bag}
                        string[] topicSegments = args.Publish.Topic.Split("/", StringSplitOptions.RemoveEmptyEntries);

                        //TODO is there a case where there is no property bag b/c no correlation id + no message id + no user properties?

                        // for example: "%24.mid=febd6d71-df05-474f-8a9b-fe90318c7eb8&%24.to=%2Fdevices%2Fb98a1b81-6b6b-4762-ae77-610fb8d6d3cd%2Fmessages%2FdeviceBound"
                        string propertyBag = topicSegments[4];

                        string[] keyValuePairs = propertyBag.Split('&');
                        foreach (var keyValuePair in keyValuePairs)
                        {
                            string key = Uri.UnescapeDataString(keyValuePair.Split("=")[0]);
                            string value = Uri.UnescapeDataString(keyValuePair.Split("=")[1]);

                            if (key.Equals(TelemetryClient.MessagePropertyMessageId))
                            {
                                receivedCloudToDeviceMessage.MessageId = value;
                            }
                            else if (key.Equals(TelemetryClient.MessagePropertyCorrelationId))
                            {
                                receivedCloudToDeviceMessage.CorrelationId = value;
                            }
                            else if (key.Equals(TelemetryClient.MessagePropertyContentType))
                            {
                                receivedCloudToDeviceMessage.ContentType = value;
                            }
                            else if (key.Equals(TelemetryClient.MessagePropertyContentEncoding))
                            {
                                receivedCloudToDeviceMessage.ContentEncoding = value;
                            }
                            else
                            {
                                receivedCloudToDeviceMessage.UserProperties.Add(key, value);
                            }
                        }

                        await CloudToDeviceTelemetryReceivedAsync.Invoke(receivedCloudToDeviceMessage);

                        await args.AcknowledgeAsync(CancellationToken.None);
                    }
                }
            }
        }

        public void Dispose()
        {
            _connection.MqttClient.PublishReceivedAsync -= HandleReceivedMqttPublish;
        }
    }
}
