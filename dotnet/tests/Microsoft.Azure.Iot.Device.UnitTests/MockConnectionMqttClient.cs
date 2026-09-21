// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

using Microsoft.Azure.Iot.Device.Mqtt;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    /// <summary>
    /// A minimal <see cref="IMqttClient"/> whose every response is under the test's control.
    /// </summary>
    /// <remarks>
    /// This exists alongside <see cref="MockMqttClient"/> because tests for the connection layer itself need to drive
    /// failures that the gen2-flavoured mock deliberately smooths over: a refused CONNACK, a throwing connect, and a
    /// connected/disconnected flag that tracks those outcomes accurately.
    /// </remarks>
    internal sealed class MockConnectionMqttClient : IMqttClient
    {
        public event Func<MqttPublishReceivedEventArgs, Task> PublishReceivedAsync = null!;
        public event Func<MqttClientConnectedEventArgs, Task> ConnectedAsync = null!;
        public event Func<MqttConnect, Task<MqttConnect>> ConnectingAsync = null!;
        public event Func<MqttClientDisconnectedEventArgs, Task> DisconnectedAsync = null!;

        private int _connectAttemptCount;
        private volatile bool _isConnected;

        /// <summary>How the server answers a CONNECT. May throw to simulate a transport-level failure.</summary>
        public Func<MqttConnect, Task<MqttConnectAck>>? OnConnect { get; set; }

        public Func<MqttPublish, Task<MqttPublishAck>>? OnPublish { get; set; }

        public Func<MqttSubscribe, Task<MqttSubscribeAck>>? OnSubscribe { get; set; }

        public Func<MqttUnsubscribe, Task<MqttUnsubscribeAck>>? OnUnsubscribe { get; set; }

        /// <summary>How many CONNECTs have been attempted, including the ones the server refused.</summary>
        public int ConnectAttemptCount => Volatile.Read(ref _connectAttemptCount);

        public List<MqttDisconnect> DisconnectRequests { get; } = new();

        public bool IsDisposed { get; private set; }

        public bool IsConnected() => _isConnected;

        public async Task<MqttConnectAck> ConnectAsync(MqttConnect connect, CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();

            if (ConnectingAsync != null)
            {
                connect = await ConnectingAsync.Invoke(connect);
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

        public Task DisconnectAsync(MqttDisconnect disconnect, CancellationToken cancellationToken = default)
        {
            lock (DisconnectRequests)
            {
                DisconnectRequests.Add(disconnect);
            }

            bool wasConnected = _isConnected;
            _isConnected = false;

            if (wasConnected && DisconnectedAsync != null)
            {
                // The real client raises this from its own I/O loop rather than inline with the caller.
                _ = DisconnectedAsync.Invoke(new() { Reason = MqttDisconnectReason.NormalDisconnection });
            }

            return Task.CompletedTask;
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

        public async Task SimulatePublishReceivedAsync(MqttPublish publish)
        {
            if (PublishReceivedAsync != null)
            {
                await PublishReceivedAsync.Invoke(new MockMqttPublishReceivedEventArgs() { Publish = publish });
            }
        }

        public async Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default)
        {
            if (OnPublish != null)
            {
                return await OnPublish.Invoke(publish);
            }

            return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
        }

        public async Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default)
        {
            if (OnSubscribe != null)
            {
                return await OnSubscribe.Invoke(subscribe);
            }

            return MqttObjectHelpers.CreateSuccessfulSuback(subscribe);
        }

        public async Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default)
        {
            if (OnUnsubscribe != null)
            {
                return await OnUnsubscribe.Invoke(unsubscribe);
            }

            return MqttObjectHelpers.CreateSuccessfulUnsuback(unsubscribe);
        }

        public void Dispose()
        {
            IsDisposed = true;
        }
    }
}
