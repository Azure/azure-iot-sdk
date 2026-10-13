// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Retry;
using System.Net.Sockets;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests
{
    public class MqttConnectionManagerUnitTests
    {
        // Each simulated connect attempt takes this long to fail, so the "Disconnected" callbacks of the failed attempts
        // have ample time to try starting reconnection loops of their own while the initial connect is still retrying.
        private static readonly TimeSpan s_connectAttemptDuration = TimeSpan.FromSeconds(1);

        private static readonly TimeSpan s_testTimeout = TimeSpan.FromSeconds(60);

        [Fact]
        public async Task InitialConnectRetriesDoNotStartSecondReconnectionLoop()
        {
            const int failingAttempts = 3;

            using SlowFailingMqttClient mqttClient = new(failingAttempts, s_connectAttemptDuration);
            RecordingRetryPolicy retryPolicy = new();
            using MqttConnectionManager connectionManager = new(mqttClient, TimeSpan.FromSeconds(30), retryPolicy);

            using CancellationTokenSource cts = new(s_testTimeout);
            MqttConnectAck connectAck = await connectionManager.ConnectAsync(new MqttConnect() { HostName = "some-hub.azure-devices.net" }, cts.Token);

            Assert.Equal(MqttConnectReasonCode.Success, connectAck.ResultCode);

            // Give any callbacks that were queued behind the initial connect time to (incorrectly) start reconnecting.
            await Task.Delay(TimeSpan.FromSeconds(1));

            // The initial connect does not consult the policy on attempt 1, so the policy should have been consulted
            // exactly once for each of the later attempts, and only by the initial connect's loop.
            uint[] consultedAttempts = retryPolicy.ConsultedAttempts;
            for (int i = 1; i < consultedAttempts.Length; i++)
            {
                Assert.True(consultedAttempts[i] > consultedAttempts[i - 1], $"Retry attempt numbers were not strictly increasing: {string.Join(", ", consultedAttempts)}");
            }

            Assert.Equal(new uint[] { 2, 3, 4 }, consultedAttempts);
            Assert.Equal(failingAttempts + 1, mqttClient.ConnectAttempts);
            Assert.True(mqttClient.IsConnected());
        }

        private class RecordingRetryPolicy : IRetryPolicy
        {
            private readonly List<uint> _consultedAttempts = new();

            public uint[] ConsultedAttempts
            {
                get
                {
                    lock (_consultedAttempts)
                    {
                        return _consultedAttempts.ToArray();
                    }
                }
            }

            public RetryGuidance GetRetryGuidance(uint currentRetryCount, Exception? lastException, ConnectionEndpoint connectionEndpoint, out TimeSpan retryDelay)
            {
                lock (_consultedAttempts)
                {
                    _consultedAttempts.Add(currentRetryCount);
                }

                retryDelay = TimeSpan.FromMilliseconds(100);
                return RetryGuidance.Retry;
            }
        }

        // Mimics MQTTnet: a failed connect attempt raises the "Disconnected" event on a background task and then throws.
        private class SlowFailingMqttClient : IMqttClient
        {
            private readonly int _failingAttempts;
            private readonly TimeSpan _attemptDuration;
            private int _connectAttempts;
            private volatile bool _isConnected;

            public SlowFailingMqttClient(int failingAttempts, TimeSpan attemptDuration)
            {
                _failingAttempts = failingAttempts;
                _attemptDuration = attemptDuration;
            }

            public int ConnectAttempts => Volatile.Read(ref _connectAttempts);

#pragma warning disable CS0067 // Not every event is raised by this fake
            public event Func<MqttPublishReceivedEventArgs, Task>? PublishReceivedAsync;
            public event Func<MqttClientConnectedEventArgs, Task>? ConnectedAsync;
            public event Func<MqttConnect, Task<MqttConnect>>? ConnectingAsync;
            public event Func<MqttClientDisconnectedEventArgs, Task>? DisconnectedAsync;
#pragma warning restore CS0067

            public async Task<MqttConnectAck> ConnectAsync(MqttConnect connect, CancellationToken cancellationToken = default)
            {
                int attempt = Interlocked.Increment(ref _connectAttempts);

                await Task.Delay(_attemptDuration, cancellationToken);

                if (attempt <= _failingAttempts)
                {
                    SocketException failure = new((int)SocketError.HostNotFound);
                    Func<MqttClientDisconnectedEventArgs, Task>? handlers = DisconnectedAsync;
                    if (handlers != null)
                    {
                        _ = Task.Run(() => handlers.Invoke(new MqttClientDisconnectedEventArgs()
                        {
                            Exception = failure,
                            Reason = MqttDisconnectReason.ImplementationSpecificError,
                        }));
                    }

                    throw failure;
                }

                _isConnected = true;
                return new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success };
            }

            public Task DisconnectAsync(MqttDisconnect disconnect, CancellationToken cancellationToken = default)
            {
                _isConnected = false;
                return Task.CompletedTask;
            }

            public bool IsConnected() => _isConnected;

            public Task<MqttPublishAck> PublishAsync(MqttPublish publish, CancellationToken cancellationToken = default) => throw new NotSupportedException();

            public Task<MqttSubscribeAck> SubscribeAsync(MqttSubscribe subscribe, CancellationToken cancellationToken = default) => throw new NotSupportedException();

            public Task<MqttUnsubscribeAck> UnsubscribeAsync(MqttUnsubscribe unsubscribe, CancellationToken cancellationToken = default) => throw new NotSupportedException();

            public void Dispose()
            {
            }
        }
    }
}
