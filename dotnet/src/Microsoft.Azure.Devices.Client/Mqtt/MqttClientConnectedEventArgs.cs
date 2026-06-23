
namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public class MqttClientConnectedEventArgs : EventArgs
    {
        /// <summary>
        ///     Gets the authentication result.
        ///     <remarks>MQTT 5.0.0+ feature.</remarks>
        /// </summary>
        public MqttClientConnectAck ConnectAck { get; set; }
    }
}
