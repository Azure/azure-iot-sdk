namespace Microsoft.Azure.Iot.Device.Mqtt
{
    public class MqttConnectAck
    {
        /// <summary>
        ///     Gets a value indicating whether a session was already available or not.
        /// </summary>
        public bool IsSessionPresent { get; set; }

        public uint? MaximumPacketSize { get; set; }

        /// <summary>
        ///     Gets the reason string.
        ///     MQTTv5 only.
        /// </summary>
        public string? ReasonString { get; set; }

        public ushort? ReceiveMaximum { get; set; }

        /// <summary>
        ///     Gets the response information.
        ///     MQTTv5 only.
        /// </summary>
        public string? ResponseInformation { get; set; }

        /// <summary>
        ///     Gets the result code.
        /// </summary>
        public MqttConnectReasonCode ResultCode { get; set; }

        /// <summary>
        ///     MQTTv5 only.
        ///     Gets the keep alive interval which was chosen by the server instead of the
        ///     keep alive interval from the client CONNECT packet.
        ///     A value of 0 indicates that the feature is not used.
        /// </summary>
        public ushort ServerKeepAlive { get; set; }

        public uint? SessionExpiryInterval { get; set; }

        /// <summary>
        ///     Gets the user properties.
        ///     In MQTT 5, user properties are basic UTF-8 string key-value pairs that you can append to almost every type of MQTT
        ///     packet.
        ///     As long as you don’t exceed the maximum message size, you can use an unlimited number of user properties to add
        ///     metadata to MQTT messages and pass information between publisher, broker, and subscriber.
        ///     The feature is very similar to the HTTP header concept.
        ///     MQTTv5 only.
        /// </summary>
        public List<MqttUserProperty> UserProperties { get; set; } = new();
    }
}
