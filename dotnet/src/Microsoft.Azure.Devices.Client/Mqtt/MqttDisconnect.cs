
namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public class MqttDisconnect
    {
        /// <summary>
        ///     Gets or sets the reason code.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public MqttClientDisconnectOptionsReason Reason { get; set; }

        /// <summary>
        ///     Gets or sets the reason string.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public string ReasonString { get; set; }

        /// <summary>
        ///     Gets or sets the session expiry interval.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public uint SessionExpiryInterval { get; set; }

        /// <summary>
        ///     Gets or sets the user properties.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public List<MqttUserProperty> UserProperties { get; set; }
    }
}
