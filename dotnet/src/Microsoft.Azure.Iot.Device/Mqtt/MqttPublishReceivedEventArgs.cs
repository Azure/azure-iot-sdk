// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public abstract class MqttPublishReceivedEventArgs : EventArgs
    {
        /// <summary>
        /// The MQTT publish that was received by this client.
        /// </summary>
        /// <remarks>
        /// The connection layer acknowledges QoS 1 or higher publishes before dispatching them to receive handlers.
        /// Receive handlers must not acknowledge the publish themselves.
        /// </remarks>
        public required MqttPublish Publish { get; set; }

        /// <summary>
        ///     Gets or sets the reason code which will be sent to the server in the ACK packet.
        /// </summary>
        public MqttPublishAckReasonCode ReasonCode { get; set; }

        /// <summary>
        ///     Gets or sets the reason string which will be sent to the server in the ACK packet.
        /// </summary>
        public string? ResponseReasonString { get; set; }

        /// <summary>
        ///     Gets or sets the user properties which will be sent to the server in the ACK packet etc.
        /// </summary>
        public List<MqttUserProperty> ResponseUserProperties { get; } = new();

        /// <summary>
        /// Acknowledges this publish using the underlying MQTT client. Called by the connection layer, not receive handlers.
        /// </summary>
        public abstract Task AcknowledgeAsync(CancellationToken cancellationToken);
    }
}
