namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public abstract class MqttPublishReceivedEventArgs : EventArgs
    {
        public MqttPublish Publish { get; set; }

        /// <summary>
        ///     Gets the client identifier.
        ///     Hint: This identifier needs to be unique over all used clients / devices on the broker to avoid connection issues.
        /// </summary>
        //public string ClientId { get; set; } probably not needed? How to route responses back to service client, though?

        /// <summary>
        ///     Gets or sets the reason code which will be sent to the server in the ACK packet.
        /// </summary>
        public MqttPublishReceivedReasonCode ReasonCode { get; set; }

        /// <summary>
        ///     Gets or sets the reason string which will be sent to the server in the ACK packet.
        /// </summary>
        public string ResponseReasonString { get; set; }

        /// <summary>
        ///     Gets or sets the user properties which will be sent to the server in the ACK packet etc.
        /// </summary>
        public List<MqttUserProperty> ResponseUserProperties { get; }

        // To be implemented by underlying MQTT client library
        public abstract Task AcknowledgeAsync(CancellationToken cancellationToken);
    }
}
