
namespace Microsoft.Azure.Devices.Client.Mqtt
{
    public interface IMqttClient //TODO disposal of payload carrying object types?
    {
        event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync;

        event Func<MqttClientConnectedEventArgs, Task> ConnectedAsync;

        event Func<MqttClientDisconnectedEventArgs, Task> DisconnectedAsync;

        Task<MqttConnectAck> ConnectAsync(MqttConnect connect, CancellationToken cancellationToken = default);

        Task DisconnectAsync(MqttDisconnect disconnect, CancellationToken cancellationToken = default);

        Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default);

        Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe mqttSubscribe, CancellationToken cancellationToken = default);

        Task<MqttUnsubscribeAck> UnsubscribeAsync(string topic, List<MqttUserProperty> userProperties, CancellationToken cancellationToken = default);
    }
}
