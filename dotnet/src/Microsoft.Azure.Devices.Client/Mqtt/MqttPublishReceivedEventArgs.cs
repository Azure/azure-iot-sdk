namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public abstract class MqttPublishReceivedEventArgs : EventArgs
    {
        /// <summary>
        /// The MQTT publish that was received by this client.
        /// </summary>
        /// <remarks>
        /// If this publish is QoS 1 or higher, then it must be acknowledged by calling <see cref="AcknowledgeAsync(CancellationToken)"/>.
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

        // To be implemented by underlying MQTT client library
        public abstract Task AcknowledgeAsync(CancellationToken cancellationToken);
    }
}
