
namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public class MqttClientDisconnectedEventArgs : EventArgs
    {
        public Exception? Exception { get; set; }

        /// <summary>
        ///     Gets or sets the reason.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public MqttClientDisconnectReason Reason { get; set; }

        public string? ReasonString { get; set; }

        public List<MqttUserProperty> UserProperties { get; set; } = new();
    }
}
