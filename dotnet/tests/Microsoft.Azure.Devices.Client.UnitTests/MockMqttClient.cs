using Google.Protobuf;
using Microsoft.Azure.Devices.Client.Gen2.Connection;
using Microsoft.Azure.Devices.Client.Mqtt;
using System.Text;

namespace Microsoft.Azure.Devices.Client.UnitTests
{
    public class MockMqttClient : IMqttClient
    {
        //IMqtt client interface callbacks that will be used by the connection client
        public event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync;
        public event Func<MqttClientConnectedEventArgs, Task> ConnectedAsync;
        public event Func<MqttConnect, Task<MqttConnect>> ConnectingAsync;
        public event Func<MqttClientDisconnectedEventArgs, Task> DisconnectedAsync;


        // Mock-specific callbacks that are used by unit tests to control how the MQTT client should respond to connects/publishes/subscribes. By default, this mock just returns a basic "OK" response on all operations
        public event Func<MqttConnect, Task<MqttConnectAck>>? OnConnectAttempt;
        public event Func<MqttDisconnect, Task>? OnDisconnectAttempt;
        public event Func<MqttPublish, Task<MqttPublishAck>>? OnPublishAttempt;
        public event Func<MqttSubscribe, Task<MqttSubscribeAck>>? OnSubscribeAttempt;
        public event Func<MqttUnsubscribe, Task<MqttUnsubscribeAck>>? OnUnsubscribeAttempt;
        public event Func<MqttPublish, Task>? OnPublishAcknowledged;


        private bool _isGen2;

        public MockMqttClient(bool isGen2)
        {
            _isGen2 = isGen2;
        }

        public async Task SimulateNewMessageAsync(MqttPublish publish)
        {
            await PublishReceivedAsync.Invoke(new MockMqttPublishReceivedEventArgs()
            { 
                Publish = publish,
            });
        }

        public async Task SimulateServerInitiatedDisconnectAsync(Exception cause, MqttDisconnectReason reason = MqttDisconnectReason.ImplementationSpecificError)
        {
            _isConnected = false;

            if (DisconnectedAsync != null)
            {
                await DisconnectedAsync.Invoke(new MqttClientDisconnectedEventArgs()
                { 
                    Exception = cause,
                    Reason = reason,
                });
            }
        }

        private bool _isConnected = false;

        public async Task<MqttConnectAck> ConnectAsync(MqttConnect connect, CancellationToken cancellationToken = default)
        {
            _isConnected = true;

            if (ConnectingAsync != null)
            {
                connect = await ConnectingAsync(connect);
            }

            if (OnConnectAttempt != null)
            {
                var connack = await OnConnectAttempt.Invoke(connect);

                ConnectedAsync?.Invoke(new()
                {
                    ConnectAck = connack,
                });
            }

            var defaultConnack = new MqttConnectAck()
            {
                ResultCode = MqttConnectResultCode.Success
            };

            ConnectedAsync?.Invoke(new()
            {
                ConnectAck = defaultConnack,
            });

            return defaultConnack;
        }

        public async Task DisconnectAsync(MqttDisconnect disconnect, CancellationToken cancellationToken = default)
        {
            _isConnected = false;

            if (OnDisconnectAttempt != null)
            {
                await OnDisconnectAttempt(disconnect);
            }

            DisconnectedAsync?.Invoke(new() 
            { 
                Reason = MqttDisconnectReason.NormalDisconnection,
            });
        }

        public void Dispose()
        {
            
        }

        public bool IsConnected() => _isConnected;

        public Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            if (_isGen2)
            {
                // Respond to birth message by simulating birth ack message
                if (publish.Topic.StartsWith("ih") && publish.Topic.EndsWith("srv/presence")) // A bit of an assumption, but I don't expect tests to use this topic suffix elsewhere
                {
                    string deviceId = publish.Topic.Split("/")[1];
                    var simulatedBirthAck = new MqttPublish()
                    {
                        Topic = $"ih/{deviceId}/dev/presence",
                        Payload = new BirthAck().ToByteArray(),
                        CorrelationData = publish.CorrelationData,
                    };
                    simulatedBirthAck.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth-ack:1")));
                    _ = SimulateNewMessageAsync(simulatedBirthAck);

                    //TODO send mock twin push as well
                }
            }

            return Task.FromResult(new MqttPublishAck()
            {
                ReasonCode = MqttPublishAckReasonCode.Success
            });
        }

        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            if (OnSubscribeAttempt != null)
            {
                return await OnSubscribeAttempt.Invoke(subscribe);
            }

            List<MqttSubscribeAckItem> subackItems = new();
            foreach (var subscribeItem in subscribe.TopicFilters)
            {
                subackItems.Add(new MqttSubscribeAckItem()
                {
                    ResultCode = subscribeItem.QualityOfServiceLevel == MqttQualityOfServiceLevel.ExactlyOnce ? MqttClientSubscribeResultCode.GrantedQoS2 : subscribeItem.QualityOfServiceLevel == MqttQualityOfServiceLevel.AtLeastOnce ? MqttClientSubscribeResultCode.GrantedQoS1 : MqttClientSubscribeResultCode.GrantedQoS0,
                    TopicFilter = new(subscribeItem.Topic, subscribeItem.QualityOfServiceLevel)
                });
            }
            return new MqttSubscribeAck()
            {
                Items = subackItems,
            };
        }

        public Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            List<MqttUnsubscribeAckItem> unsubackItems = new();
            foreach (var unsubscribeItem in unsubscribe.TopicFilters)
            {
                unsubackItems.Add(new MqttUnsubscribeAckItem()
                {
                    ResultCode = MqttClientUnsubscribeResultCode.Success,
                    TopicFilter = unsubscribeItem,
                });
            }

            return Task.FromResult(new MqttUnsubscribeAck()
            {
                Items = unsubackItems,
            });
        }
    }
}
