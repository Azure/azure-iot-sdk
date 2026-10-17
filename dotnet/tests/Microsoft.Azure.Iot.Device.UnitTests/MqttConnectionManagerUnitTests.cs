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

        /// <summary>
        /// An operation meant for an endpoint this layer is not targeting must be rejected as not connected, without ever
        /// reaching the underlying client, so that it cannot be sent over another endpoint's connection.
        /// </summary>
        [Fact]
        public async Task OperationMeantForAnotherEndpointIsRejectedWithoutReachingClient()
        {
            using MockConnectionMqttClient mqttClient = new();
            int operationsThatReachedClient = 0;
            mqttClient.OnPublish = _ =>
            {
                Interlocked.Increment(ref operationsThatReachedClient);
                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };
            mqttClient.OnSubscribe = subscribe =>
            {
                Interlocked.Increment(ref operationsThatReachedClient);
                return Task.FromResult(MqttObjectHelpers.CreateSuccessfulSuback(subscribe));
            };
            mqttClient.OnUnsubscribe = unsubscribe =>
            {
                Interlocked.Increment(ref operationsThatReachedClient);
                return Task.FromResult(MqttObjectHelpers.CreateSuccessfulUnsuback(unsubscribe));
            };
            using MqttConnectionManager connectionManager = new(mqttClient, TimeSpan.FromSeconds(30), new RecordingRetryPolicy());

            await connectionManager.SwitchEndpointAsync(ConnectionEndpoint.DeviceProvisioningService, TestContext.Current.CancellationToken);

            await Assert.ThrowsAsync<MqttClientNotConnectedException>(
                () => connectionManager.PublishAsync(ConnectionEndpoint.IotHub, new MqttPublish() { Topic = "devices/d/messages/events/" }, TestContext.Current.CancellationToken));
            await Assert.ThrowsAsync<MqttClientNotConnectedException>(
                () => connectionManager.SubscribeAsync(ConnectionEndpoint.IotHub, new MqttSubscribe("$iothub/twin/res/#", MqttQualityOfServiceLevel.AtMostOnce), TestContext.Current.CancellationToken));
            await Assert.ThrowsAsync<MqttClientNotConnectedException>(
                () => connectionManager.UnsubscribeAsync(ConnectionEndpoint.IotHub, new MqttUnsubscribe("$iothub/twin/res/#"), TestContext.Current.CancellationToken));

            Assert.Equal(0, Volatile.Read(ref operationsThatReachedClient));

            // An operation meant for the endpoint this layer targets is sent as usual.
            await connectionManager.PublishAsync(ConnectionEndpoint.DeviceProvisioningService, new MqttPublish() { Topic = "$dps/registrations/PUT/iotdps-register/?$rid=1" }, TestContext.Current.CancellationToken);
            Assert.Equal(1, Volatile.Read(ref operationsThatReachedClient));
        }

        /// <summary>
        /// An operation that was admitted while IoT hub was the target, and is still in flight when the target changes to
        /// Device Provisioning Service, must finish before the switch completes. Otherwise it could reach the underlying
        /// client after the caller of the switch connected that client to DPS.
        /// </summary>
        [Fact]
        public async Task SwitchingEndpointWaitsForOperationsMeantForPreviousEndpoint()
        {
            using MockConnectionMqttClient mqttClient = new();
            TaskCompletionSource hubPublishReachedClient = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource releaseHubPublish = new(TaskCreationOptions.RunContinuationsAsynchronously);
            int publishesThatReachedClient = 0;
            mqttClient.OnPublish = async _ =>
            {
                if (Interlocked.Increment(ref publishesThatReachedClient) == 1)
                {
                    hubPublishReachedClient.TrySetResult();
                    await releaseHubPublish.Task;
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };
            using MqttConnectionManager connectionManager = new(mqttClient, TimeSpan.FromSeconds(30), new RecordingRetryPolicy());

            await connectionManager.SwitchEndpointAsync(ConnectionEndpoint.IotHub, TestContext.Current.CancellationToken);

            Task<MqttPublishAck> hubPublish = connectionManager.PublishAsync(ConnectionEndpoint.IotHub, new MqttPublish() { Topic = "devices/d/messages/events/" }, TestContext.Current.CancellationToken);
            await hubPublishReachedClient.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Task switchToDps = connectionManager.SwitchEndpointAsync(ConnectionEndpoint.DeviceProvisioningService, TestContext.Current.CancellationToken);

            // The target has already changed, so no further hub operation is admitted while the switch waits...
            Assert.Equal(ConnectionEndpoint.DeviceProvisioningService, connectionManager.ConnectionEndpoint);
            await Assert.ThrowsAsync<MqttClientNotConnectedException>(
                () => connectionManager.PublishAsync(ConnectionEndpoint.IotHub, new MqttPublish() { Topic = "devices/d/messages/events/" }, TestContext.Current.CancellationToken));

            // ...but the switch does not complete while the hub operation admitted before it is still in flight.
            await Task.Delay(TimeSpan.FromMilliseconds(500), TestContext.Current.CancellationToken);
            Assert.False(switchToDps.IsCompleted, "Switching endpoints must wait for operations meant for the previous endpoint.");

            releaseHubPublish.TrySetResult();
            await hubPublish.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            await switchToDps.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            Assert.Equal(1, Volatile.Read(ref publishesThatReachedClient));
        }

        /// <summary>
        /// A caller that gives up on waiting for in-flight operations is released, and the switch it started still stands.
        /// </summary>
        [Fact]
        public async Task SwitchingEndpointWhileWaitingForPreviousOperationsCanBeCancelled()
        {
            using MockConnectionMqttClient mqttClient = new();
            TaskCompletionSource hubPublishReachedClient = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource releaseHubPublish = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mqttClient.OnPublish = async _ =>
            {
                hubPublishReachedClient.TrySetResult();
                await releaseHubPublish.Task;
                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };
            using MqttConnectionManager connectionManager = new(mqttClient, TimeSpan.FromSeconds(30), new RecordingRetryPolicy());

            await connectionManager.SwitchEndpointAsync(ConnectionEndpoint.IotHub, TestContext.Current.CancellationToken);
            Task<MqttPublishAck> hubPublish = connectionManager.PublishAsync(ConnectionEndpoint.IotHub, new MqttPublish() { Topic = "devices/d/messages/events/" }, TestContext.Current.CancellationToken);
            await hubPublishReachedClient.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            using CancellationTokenSource cts = new();
            Task switchToDps = connectionManager.SwitchEndpointAsync(ConnectionEndpoint.DeviceProvisioningService, cts.Token);
            cts.Cancel();

            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => switchToDps);
            Assert.Equal(ConnectionEndpoint.DeviceProvisioningService, connectionManager.ConnectionEndpoint);

            releaseHubPublish.TrySetResult();
            await hubPublish.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            // With nothing left in flight, a later switch completes straight away.
            await connectionManager.SwitchEndpointAsync(ConnectionEndpoint.DeviceProvisioningService, TestContext.Current.CancellationToken)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
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
