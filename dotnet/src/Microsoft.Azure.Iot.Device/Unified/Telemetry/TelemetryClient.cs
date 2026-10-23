// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Models.Telemetry;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Unified.Connection;
using System.Diagnostics;
using System.Globalization;

namespace Microsoft.Azure.Iot.Device.Unified.Telemetry
{
    /// <summary>
    /// A feature client for sending device-to-cloud telemetry and receiving cloud-to-device telemetry.
    /// </summary>
    public class TelemetryClient : IDisposable
    {
        private bool _isDisposed = false;

        private const string ClassicTelemetryTopic = "devices/" + IConnectionClient.DeviceIdPlaceholder + "/messages/events/";
        internal const string DeviceBoundMessagesTopicFormat = "devices/" + IConnectionClient.DeviceIdPlaceholder + "/messages/devicebound/";

        private IConnectionClient _connection;

        public const string MessagePropertyCorrelationId = "$.cid";
        public const string MessagePropertyMessageId = "$.mid";
        public const string MessagePropertyContentType = "$.ct";
        public const string MessagePropertyContentEncoding = "$.ce";

        private event Func<CloudToDeviceTelemetry, Task>? _cloudToDeviceTelemetryReceivedAsync;

        /// <summary>
        /// An event that executes whenever this device receives a cloud-to-device message from IoT hub.
        /// </summary>
        /// <exception cref="InvalidOperationException">Thrown when adding a handler if the connection client opted out of cloud-to-device messages (see <see cref="ConnectionClientOptions.EnableCloudToDeviceMessages"/>).</exception>
        public event Func<CloudToDeviceTelemetry, Task>? CloudToDeviceTelemetryReceivedAsync
        {
            add
            {
                if (!_connection.AreCloudToDeviceMessagesEnabled)
                {
                    throw new InvalidOperationException($"The connection client opted out of cloud-to-device messages. Set {nameof(ConnectionClientOptions)}.{nameof(ConnectionClientOptions.EnableCloudToDeviceMessages)} to true to receive them.");
                }

                _cloudToDeviceTelemetryReceivedAsync += value;
            }
            remove
            {
                _cloudToDeviceTelemetryReceivedAsync -= value;
            }
        }

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
            if (currentConnectionContext == null || !_connection.IsConnectionTargetSet)
            {
                throw new NotSupportedException("Must be connected before calling this method.");
            }

            if (message.Payload != null && message.Payload.Length > 255000) //Leaving some buffer b/c classic hub message size calc is not strictly about payload size
            {
                throw new MessageTooLargeException("This telemetry message is too large to be accepted by IoT Hub. It will not be sent.");
            }

            //TODO fill in content type, encoding, etc from message user properties
            // The connection client substitutes this device's current ID for the placeholder in this topic when it sends
            // this publish, so the publish still goes to the right topic if the device ID changes while it is pending.
            var mqttMessage = new MqttPublish
            {
                Topic = ClassicTelemetryTopic,
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

            var expectedDeviceBoundMessagesTopic = DeviceBoundMessagesTopicFormat.Replace(IConnectionClient.DeviceIdPlaceholder, connectionContext.DeviceId, StringComparison.Ordinal);

            if (args.Publish.Topic.StartsWith(expectedDeviceBoundMessagesTopic))
            {
                if (_cloudToDeviceTelemetryReceivedAsync != null)
                {
                    var receivedCloudToDeviceMessage = new CloudToDeviceTelemetry(args.Publish.Payload);

                    // devices/{device-id}/messages/devicebound/{property-bag}
                    string[] topicSegments = args.Publish.Topic.Split("/", StringSplitOptions.RemoveEmptyEntries);

                    //TODO is there a case where there is no property bag b/c no correlation id + no message id + no user properties?

                    // for example: "%24.mid=febd6d71-df05-474f-8a9b-fe90318c7eb8&%24.to=%2Fdevices%2Fb98a1b81-6b6b-4762-ae77-610fb8d6d3cd%2Fmessages%2FdeviceBound"
                    // Note that not all c2d messages have a property bag
                    if (topicSegments.Length == 5)
                    {
                        string propertyBag = topicSegments[4];

                        string[] keyValuePairs = propertyBag.Split('&');
                        foreach (var keyValuePair in keyValuePairs)
                        {
                            string[] parts = keyValuePair.Split('=', 2);
                            string key = Uri.UnescapeDataString(parts[0]);
                            string value = parts.Length > 1 ? Uri.UnescapeDataString(parts[1]) : "";

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
                    }

                    await _cloudToDeviceTelemetryReceivedAsync.Invoke(receivedCloudToDeviceMessage);

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
