namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public sealed class MqttConnectingFailedException : Exception
    {
        public MqttConnectingFailedException(string message, MqttConnectAck connack)
            : base(message)
        {
            ConnectAck = connack;
        }

        public MqttConnectAck ConnectAck { get; }

        public MqttConnectResultCode ResultCode => ConnectAck?.ResultCode ?? MqttConnectResultCode.UnspecifiedError;
    }
}
