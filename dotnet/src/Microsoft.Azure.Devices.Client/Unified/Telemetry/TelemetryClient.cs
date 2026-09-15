using Microsoft.Azure.Devices.Client.Exceptions;
using Microsoft.Azure.Devices.Client.Models.Telemetry;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.Unified.Connection;
using System.Diagnostics;
using System.Globalization;

namespace Microsoft.Azure.Devices.Client.Unified.Telemetry
{
    /// <summary>
    /// A feature client for sending device-to-cloud telemetry and receiving cloud-to-device telemetry.
    /// </summary>
    public class TelemetryClient : IDisposable
    {
        private bool _isDisposed = false;

        private const string ClassicTelemetryTopicFormat = "devices/{0}/messages/events/";
        internal const string DeviceBoundMessagesTopicFormat = "devices/{0}/messages/devicebound/";

        private IConnectionClient _connection;
        private Gen2.Telemetry.TelemetryClient _aegTelemetryClient;

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
            _aegTelemetryClient = new(new Stub(_connection));
            _connection.PublishReceivedAsync += HandleReceivedMqttPublish;
            _aegTelemetryClient.CloudToDeviceTelemetryReceivedAsync += DelegateGen2CloudToDeviceTelemetry;
        }

        private async Task DelegateGen2CloudToDeviceTelemetry(CloudToDeviceTelemetry telemetry)
        {
            if (CloudToDeviceTelemetryReceivedAsync != null)
            {
                //TODO do we even need to wait for this?
                await CloudToDeviceTelemetryReceivedAsync.Invoke(telemetry);
            }
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

            if (currentConnectionContext.ConnectionProfile == Provisioning.Models.ConnectionProfile.MqttV5)
            {
                await _aegTelemetryClient.SendTelemetryAsync(message, cancellationToken);
                return;
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

                // Puback is checked for non-success cases under this layer, so no need to check it here as well
                MqttPublishAck puback = await _connection.PublishAsync(mqttMessage, cancellationToken);
            }
        }

        private async Task HandleReceivedMqttPublish(MqttPublishReceivedEventArgs args)
        {
            if (!args.Publish.Topic.StartsWith("devices/") || !args.Publish.Topic.Contains("/messages/devicebound/"))
            {
                // The publish is not relevant to this client, so ignore it. This check needs to happen prior to checking the deviceId within the topic b/c deviceId is
                // not available until after provisioning finishes and this client may be setup prior to provisioning. This allows this client to ignore DPS
                // publishes without needing to know the deviceId.
                return;
            }

            var connectionContext = _connection.GetCurrentConnectionContext();

            if (connectionContext == null)
            {
                // Should never happen?
                Trace.TraceWarning("Cannot handle a received MQTT message while disconnected");
                return;
            }

            var expectedDeviceBoundMessagesTopic = string.Format(CultureInfo.InvariantCulture, DeviceBoundMessagesTopicFormat, connectionContext.DeviceId);

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

        /// <summary>
        /// Releases the unmanaged resources used by this client and optionally disposes of the managed resources.
        /// </summary>
        /// <param name="disposing">true to release both managed and unmanaged resources; false to releases only unmanaged resources.</param>
        public void Dispose(bool disposing)
        {
            _connection.PublishReceivedAsync -= HandleReceivedMqttPublish;
            _aegTelemetryClient.CloudToDeviceTelemetryReceivedAsync -= DelegateGen2CloudToDeviceTelemetry;
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
            _aegTelemetryClient.CloudToDeviceTelemetryReceivedAsync -= DelegateGen2CloudToDeviceTelemetry;
            _connection.Dispose();
            _isDisposed = true;
        }
    }
}
