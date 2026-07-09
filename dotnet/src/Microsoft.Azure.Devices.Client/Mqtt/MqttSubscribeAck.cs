

namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public class MqttSubscribeAck
    {
        /// <summary>
        ///     Gets the result for every topic filter item.
        /// </summary>
        public required IReadOnlyCollection<MqttSubscribeAckItem> Items { get; set; }

        /// <summary>
        ///     Gets the reason string.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public string? ReasonString { get; set; }

        /// <summary>
        ///     Gets the user properties which were part of the SUBACK packet.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public IReadOnlyCollection<MqttUserProperty> UserProperties { get; set; } = new List<MqttUserProperty>();
    }
}
