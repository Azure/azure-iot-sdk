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

        public MqttConnectReasonCode ResultCode => ConnectAck?.ResultCode ?? MqttConnectReasonCode.UnspecifiedError;
    }
}
