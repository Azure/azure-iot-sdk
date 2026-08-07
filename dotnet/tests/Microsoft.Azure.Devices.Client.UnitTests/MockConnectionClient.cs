using Microsoft.Azure.Devices.Client.CertificateManagement;
using Microsoft.Azure.Devices.Client.Mqtt;
using Microsoft.Azure.Devices.Client.Unified.Connection;

namespace Microsoft.Azure.Devices.Client.UnitTests
{
    public class MockConnectionClient : IConnectionClient
    {
        // The stubs that allow a user of this mock to see/control how publish/subscribe/unsubscribes happen at the MQTT level
        public event Func<MqttPublish, Task<MqttPublishAck>>? OnPublishAttempt;
        public event Func<MqttSubscribe, Task<MqttSubscribeAck>>? OnSubscribeAttempt;
        public event Func<MqttUnsubscribe, Task<MqttUnsubscribeAck>>? OnUnsubscribeAttempt;

        // The actual IConnection client
        public event Func<MqttPublishReceivedEventArgs, Task>? ApplicationMessageReceivedAsync;
        public event Action<MqttClientConnectedEventArgs>? ConnectedAsync;
        public event Action<MqttClientDisconnectedEventArgs>? DisconnectedAsync;

        private ConnectionContext? _currentConnectionContext;

        public IMqttClient MqttClient => throw new NotImplementedException(); //TODO need a mock IMqttClient

        public ConnectionContext? GetCurrentConnectionContext()
        {
            return _currentConnectionContext;
        }

        public void SetCurrentConnectionContext(ConnectionContext context)
        {
            _currentConnectionContext = context;
        }

        public async Task SimulateReceivePublishAsync(MqttPublish msg, ushort packetId = default)
        {
            MockMqttPublishReceivedEventArgs msgReceivedArgs = new()
            {
                Publish = msg,
            };

            if (ApplicationMessageReceivedAsync != null)
            {
                await ApplicationMessageReceivedAsync.Invoke(msgReceivedArgs);
            }
        }


        public Task<CertificateSigningOperation> SendCertificateSigningRequestAsync(CertificateSigningRequest request, CancellationToken cancellationToken = default)
        {
            throw new NotImplementedException("No unit test needs this yet");
        }

        public void Dispose()
        {

        }
    }
}
