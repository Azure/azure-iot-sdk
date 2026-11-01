// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.Mqtt;
using System.Text;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    /// <summary>
    /// An <see cref="IMqttClient"/> whose every response is under the test's control.
    /// </summary>
    /// <remarks>
    /// By default every operation succeeds. Tests can drive failures such as a refused CONNACK or a throwing connect,
    /// and the connected/disconnected flag tracks those outcomes accurately.
    /// </remarks>
    internal sealed class MockConnectionMqttClient : IMqttClient
    {
        public event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync = null!;
        public event Func<MqttClientConnectedEventArgs, Task> ConnectedAsync = null!;
        public event Func<MqttConnect, CancellationToken, Task<MqttConnect>> ConnectingAsync = null!;
        public event Func<MqttClientDisconnectedEventArgs, Task> DisconnectedAsync = null!;

        private int _connectAttemptCount;
        private volatile bool _isConnected;

        /// <summary>How the server answers a CONNECT. May throw to simulate a transport-level failure.</summary>
        public Func<MqttConnect, Task<MqttConnectAck>>? OnConnect { get; set; }

        public Func<MqttPublish, Task<MqttPublishAck>>? OnPublish { get; set; }

        public Func<MqttSubscribe, Task<MqttSubscribeAck>>? OnSubscribe { get; set; }

        public Func<MqttUnsubscribe, Task<MqttUnsubscribeAck>>? OnUnsubscribe { get; set; }

        /// <summary>Invoked on every client-initiated DISCONNECT, after it has been recorded.</summary>
        public Func<MqttDisconnect, Task>? OnDisconnect { get; set; }

        /// <summary>
        /// When set, a publish to an MQTTv5 birth topic (<c>ih/{deviceId}/srv/presence</c>) is answered with a simulated
        /// birth-ack.
        /// </summary>
        public bool AutoRespondToBirth { get; set; }

        /// <summary>
        /// When cleared, a client-initiated DISCONNECT does not raise the disconnect callback, so that a test can raise it
        /// later (see <see cref="SimulateSpuriousDisconnectCallbackAsync"/>) to reproduce MQTTnet raising it late.
        /// </summary>
        public bool RaiseDisconnectCallbackOnClientDisconnect { get; set; } = true;

        /// <summary>The birth-ack that <see cref="AutoRespondToBirth"/> answers each birth with.</summary>
        public BirthAck AutoBirthAck { get; set; } = new();

        /// <summary>How many CONNECTs have been attempted, including the ones the server refused.</summary>
        public int ConnectAttemptCount => Volatile.Read(ref _connectAttemptCount);

        public List<MqttDisconnect> DisconnectRequests { get; } = new();

        /// <summary>
        /// The publishes/subscribes/unsubscribes that were "sent on the wire", in order. An operation whose handler throws
        /// is not recorded.
        /// </summary>
        public List<MockMqttOutgoingTraffic> SentMqttTrafficInOrder { get; } = new();

        public bool IsDisposed { get; private set; }

        public bool IsConnected() => _isConnected;

        public async Task<MqttConnectAck> ConnectAsync(MqttConnect connect, CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();

            if (ConnectingAsync != null)
            {
                foreach (Func<MqttConnect, CancellationToken, Task<MqttConnect>> handler in ConnectingAsync.GetInvocationList())
                {
                    connect = await handler(connect, cancellationToken);
                }
            }

            Interlocked.Increment(ref _connectAttemptCount);

            MqttConnectAck connack = OnConnect != null
                ? await OnConnect.Invoke(connect)
                : new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success };

            // A refused CONNACK leaves no session behind, so the client must not report itself as connected.
            if (connack.ResultCode == MqttConnectReasonCode.Success)
            {
                _isConnected = true;

                if (ConnectedAsync != null)
                {
                    await ConnectedAsync.Invoke(new() { ConnectAck = connack });
                }
            }

            return connack;
        }

        public async Task DisconnectAsync(MqttDisconnect disconnect, CancellationToken cancellationToken = default)
        {
            lock (DisconnectRequests)
            {
                DisconnectRequests.Add(disconnect);
            }

            bool wasConnected = _isConnected;
            _isConnected = false;

            if (OnDisconnect != null)
            {
                await OnDisconnect.Invoke(disconnect);
            }

            if (wasConnected && RaiseDisconnectCallbackOnClientDisconnect && DisconnectedAsync != null)
            {
                // The real client raises this from its own I/O loop rather than inline with the caller.
                _ = DisconnectedAsync.Invoke(new() { Reason = MqttDisconnectReason.NormalDisconnection });
            }
        }

        /// <summary>
        /// Simulate the server hanging up. Awaiting this also awaits whatever reconnection it provokes.
        /// </summary>
        public async Task SimulateServerDisconnectAsync(MqttDisconnectReason reason, Exception? cause = null)
        {
            _isConnected = false;

            if (DisconnectedAsync != null)
            {
                await DisconnectedAsync.Invoke(new()
                {
                    Exception = cause,
                    Reason = reason,
                    ReasonString = reason.ToString(),
                });
            }
        }

        /// <summary>
        /// Raise the disconnect callback without actually disconnecting, reproducing MQTTnet's habit of raising it
        /// more times than there were real disconnects.
        /// </summary>
        public async Task SimulateSpuriousDisconnectCallbackAsync(MqttDisconnectReason reason)
        {
            if (DisconnectedAsync != null)
            {
                await DisconnectedAsync.Invoke(new() { Reason = reason, ReasonString = reason.ToString() });
            }
        }

        public Task SimulatePublishReceivedAsync(MqttPublish publish)
        {
            return SimulatePublishReceivedEventAsync(new MockMqttPublishReceivedEventArgs() { Publish = publish });
        }

        public async Task SimulatePublishReceivedEventAsync(MqttPublishReceivedEventArgs args)
        {
            if (PublishReceivedAsync != null)
            {
                await PublishReceivedAsync.Invoke(args);
            }
        }

        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();

            if (AutoRespondToBirth)
            {
                SimulateBirthAckIfBirthPublish(publish);
            }

            MqttPublishAck puback = OnPublish != null
                ? await OnPublish.Invoke(publish).WaitAsync(cancellationToken)
                : new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };

            RecordSentTraffic(new(publish));
            return puback;
        }

        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();

            MqttSubscribeAck suback = OnSubscribe != null
                ? await OnSubscribe.Invoke(subscribe).WaitAsync(cancellationToken)
                : MqttObjectHelpers.CreateSuccessfulSuback(subscribe);

            RecordSentTraffic(new(subscribe));
            return suback;
        }

        public async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            MqttUnsubscribeAck unsuback = OnUnsubscribe != null
                ? await OnUnsubscribe.Invoke(unsubscribe)
                : MqttObjectHelpers.CreateSuccessfulUnsuback(unsubscribe);

            RecordSentTraffic(new(unsubscribe));
            return unsuback;
        }

        private void RecordSentTraffic(MockMqttOutgoingTraffic traffic)
        {
            lock (SentMqttTrafficInOrder)
            {
                SentMqttTrafficInOrder.Add(traffic);
            }
        }

        private void SimulateBirthAckIfBirthPublish(MqttPublish publish)
        {
            if (!publish.Topic.StartsWith("ih/") || !publish.Topic.EndsWith("/srv/presence"))
            {
                return;
            }

            string deviceId = publish.Topic.Split('/')[1];
            MqttPublish birthAck = new()
            {
                Topic = $"ih/{deviceId}/dev/presence",
                Payload = AutoBirthAck.ToByteArray(),
                CorrelationData = publish.CorrelationData,
            };
            birthAck.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth-ack:1")));
            _ = SimulatePublishReceivedAsync(birthAck);
        }

        public void Dispose()
        {
            IsDisposed = true;
        }
    }
}
