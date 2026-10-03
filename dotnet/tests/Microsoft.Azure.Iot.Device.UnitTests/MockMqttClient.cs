// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Text;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class MockMqttClient : IMqttClient
    {
        //IMqtt client interface callbacks that will be used by the connection client
        public event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync = null!;
        public event Func<MqttClientConnectedEventArgs, Task> ConnectedAsync = null!;
        public event Func<MqttConnect, Task<MqttConnect>> ConnectingAsync = null!;
        public event Func<MqttClientDisconnectedEventArgs, Task> DisconnectedAsync = null!;


        // Mock-specific callbacks that are used by unit tests to control how the MQTT client should respond to connects/publishes/subscribes. By default, this mock just returns a basic "OK" response on all operations
        public event Func<MqttConnect, Task<MqttConnectAck>>? OnConnectAttempt;
        public event Func<MqttDisconnect, Task>? OnDisconnectAttempt;
        public event Func<MqttPublish, Task<MqttPublishAck>>? OnPublishAttempt;
        public event Func<MqttSubscribe, Task<MqttSubscribeAck>>? OnSubscribeAttempt;
        public event Func<MqttUnsubscribe, Task<MqttUnsubscribeAck>>? OnUnsubscribeAttempt;


        // The list of publishes/subscribes/unsubscribes as they were "sent on the wire" after any necessary reconnection
        public List<MockMqttOutgoingTraffic> SentMqttTrafficInOrder = new();

        private bool _isMQTTv5;

        public MockMqttClient(bool isMQTTv5)
        {
            _isMQTTv5 = isMQTTv5;
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

                if (ConnectedAsync != null)
                {
                    await ConnectedAsync.Invoke(new()
                    {
                        ConnectAck = connack,
                    });
                }

                return connack;
            }

            var defaultConnack = new MqttConnectAck()
            {
                ResultCode = MqttConnectReasonCode.Success
            };

            if (ConnectedAsync != null)
            {
                await ConnectedAsync.Invoke(new()
                {
                    ConnectAck = defaultConnack,
                });
            }

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
            GC.SuppressFinalize(this);
        }

        public bool IsConnected() => _isConnected;

        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            if (_isMQTTv5)
            {
                // Respond to birth message by simulating birth ack message
                if (publish.Topic.StartsWith("ih", StringComparison.Ordinal) && publish.Topic.EndsWith("srv/presence", StringComparison.Ordinal)) // A bit of an assumption, but I don't expect tests to use this topic suffix elsewhere
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

            if (OnPublishAttempt != null)
            {
                try
                {
                    var puback = await OnPublishAttempt.Invoke(publish);
                    SentMqttTrafficInOrder.Add(new(publish)); // Only note this as outgoing traffic if the publish attempt returns without throwing
                    return puback;
                }
                catch (MqttClientNotConnectedException)
                {
                    throw;
                }
            }

            SentMqttTrafficInOrder.Add(new(publish));
            return new MqttPublishAck()
            {
                ReasonCode = MqttPublishAckReasonCode.Success
            };
        }

        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            if (OnSubscribeAttempt != null)
            {
                var suback = await OnSubscribeAttempt.Invoke(subscribe);
                SentMqttTrafficInOrder.Add(new(subscribe)); // Only note this as outgoing traffic if the subscribe attempt returns without throwing
                return suback;
            }

            var defaultSuback = MqttObjectHelpers.CreateSuccessfulSuback(subscribe);

            SentMqttTrafficInOrder.Add(new(subscribe));
            return defaultSuback;
        }

        public async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            if (OnUnsubscribeAttempt != null)
            {
                var unsuback = await OnUnsubscribeAttempt.Invoke(unsubscribe);
                SentMqttTrafficInOrder.Add(new(unsubscribe)); // Only note this as outgoing traffic if the unsubscribe attempt returns without throwing
                return unsuback;
            }

            var defaultUnsuback = MqttObjectHelpers.CreateSuccessfulUnsuback(unsubscribe);
            SentMqttTrafficInOrder.Add(new(unsubscribe));
            return defaultUnsuback;
        }
    }
}
