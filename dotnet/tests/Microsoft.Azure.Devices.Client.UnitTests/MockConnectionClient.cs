using Microsoft.Azure.Devices.Client.CertificateManagement;
using Microsoft.Azure.Devices.Client.Mqtt;

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

        public Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();

            if (OnPublishAttempt != null)
            {
                return OnPublishAttempt.Invoke(publish);
            }

            return Task.FromResult(new MqttPublishAck()
            { 
                ReasonCode = MqttPublishAckReasonCode.Success
            });
        }

        public Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();

            if (OnSubscribeAttempt != null)
            {
                return OnSubscribeAttempt.Invoke(subscribe);
            }

            var subackEntries = new List<MqttSubscribeAckItem>();
            foreach (var subscribeEntry in subscribe.TopicFilters)
            {
                if (subscribeEntry.QualityOfServiceLevel == MqttQualityOfServiceLevel.AtLeastOnce)
                {
                    subackEntries.Add(new MqttSubscribeAckItem()
                    {
                        ResultCode = MqttClientSubscribeResultCode.GrantedQoS1,
                    });
                }
                else if (subscribeEntry.QualityOfServiceLevel == MqttQualityOfServiceLevel.AtMostOnce)
                {
                    subackEntries.Add(new MqttSubscribeAckItem()
                    {
                        ResultCode = MqttClientSubscribeResultCode.GrantedQoS0,
                    });
                }
            }

            var defaultSuback = new MqttSubscribeAck()
            {
                Items = subackEntries
            };

            return Task.FromResult(defaultSuback);
        }

        public Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();

            if (OnUnsubscribeAttempt != null)
            {
                return OnUnsubscribeAttempt.Invoke(unsubscribe);
            }

            var unsubackEntries = new List<MqttUnsubscribeAckItem>();
            foreach (var unsubscribeTopic in unsubscribe.TopicFilters)
            {
                unsubackEntries.Add(new MqttUnsubscribeAckItem()
                {
                    ResultCode = MqttClientUnsubscribeResultCode.Success,
                    TopicFilter = unsubscribeTopic
                });
            }

            var defaultunsuback = new MqttUnsubscribeAck()
            {
                Items = unsubackEntries
            };

            return Task.FromResult(defaultunsuback);
        }

        public void Dispose()
        {

        }
    }
}
