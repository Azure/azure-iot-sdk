// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Exceptions;
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

        [Theory]
        [InlineData(0u, 0u, false)]
        [InlineData(0u, 0u, true)]
        [InlineData(3600u, 3600u, false)]
        [InlineData(3600u, 3600u, true)]
        [InlineData(3600u, 0u, true)]
        [InlineData(0u, 3600u, true)]
        public async Task DisconnectExpiryRespectsTheFinalMqtt5ConnectPacket(
            uint proposedExpiry, uint actualExpiry, bool desireReconnection)
        {
            using MockConnectionMqttClient mqttClient = new() { RaiseDisconnectCallbackOnClientDisconnect = false };
            using MqttConnectionManager connectionManager = new(mqttClient, TimeSpan.FromSeconds(30), new RecordingRetryPolicy());
            connectionManager.ConnectingAsync += (packet, _) => Task.FromResult(new MqttConnect()
            {
                HostName = packet.HostName,
                ProtocolVersion = MqttProtocolVersion.V500,
                SessionExpiryInterval = actualExpiry,
            });
            await connectionManager.ConnectAsync(new MqttConnect()
            {
                HostName = "some-hub.azure-devices.net",
                ProtocolVersion = MqttProtocolVersion.V500,
                SessionExpiryInterval = proposedExpiry,
            }, TestContext.Current.CancellationToken);
            MqttDisconnect options = new() { ReasonString = "test shutdown" };

            await connectionManager.DisconnectAsync(desireReconnection, options, TestContext.Current.CancellationToken);

            MqttDisconnect sentDisconnect = Assert.Single(mqttClient.DisconnectRequests);
            Assert.Equal(actualExpiry == 0 ? 0u : 3600u, sentDisconnect.SessionExpiryInterval);
            Assert.Equal(options.Reason, sentDisconnect.Reason);
            Assert.Equal(options.ReasonString, sentDisconnect.ReasonString);
            Assert.Same(options.UserProperties, sentDisconnect.UserProperties);
            Assert.Equal(3600u, options.SessionExpiryInterval);
        }

        [Theory]
        [InlineData("unknown/topic", MqttQualityOfServiceLevel.AtMostOnce, 0)]
        [InlineData("unknown/topic", MqttQualityOfServiceLevel.AtLeastOnce, 1)]
        [InlineData("unknown/topic", MqttQualityOfServiceLevel.ExactlyOnce, 1)]
        [InlineData("$dps/registrations/res/200/?$rid=1", MqttQualityOfServiceLevel.AtLeastOnce, 1)]
        [InlineData("$iothub/credentials/res/200/?$rid=1", MqttQualityOfServiceLevel.AtLeastOnce, 1)]
        [InlineData("devices/device/messages/devicebound/", MqttQualityOfServiceLevel.AtLeastOnce, 1)]
        public async Task ReceivedPublishesAreAcknowledgedWithoutReceiveHandlers(
            string topic, MqttQualityOfServiceLevel qos, int expectedAcknowledgements)
        {
            using MockConnectionMqttClient mqttClient = new();
            using MqttConnectionManager connectionManager = new(mqttClient, TimeSpan.FromSeconds(30), new RecordingRetryPolicy());
            MockMqttPublishReceivedEventArgs args = new()
            {
                Publish = new() { Topic = topic, QualityOfServiceLevel = qos },
            };

            await mqttClient.SimulatePublishReceivedEventAsync(args);

            Assert.Equal(expectedAcknowledgements, args.AcknowledgeCount);
        }

        [Fact]
        public async Task PublishIsAcknowledgedBeforeReceiveHandlersEvenWhenTheyFail()
        {
            using MockConnectionMqttClient mqttClient = new();
            using MqttConnectionManager connectionManager = new(mqttClient, TimeSpan.FromSeconds(30), new RecordingRetryPolicy());
            MockMqttPublishReceivedEventArgs args = new()
            {
                Publish = new() { Topic = "unknown/topic", QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce },
            };
            TaskCompletionSource handlerStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource releaseHandler = new(TaskCreationOptions.RunContinuationsAsynchronously);
            bool laterHandlerRan = false;
            connectionManager.PublishReceivedAsync += async _ =>
            {
                handlerStarted.TrySetResult();
                await releaseHandler.Task;
                throw new InvalidOperationException("Handler failed.");
            };
            connectionManager.PublishReceivedAsync += _ =>
            {
                laterHandlerRan = true;
                return Task.CompletedTask;
            };

            Task receiveTask = mqttClient.SimulatePublishReceivedEventAsync(args);
            try
            {
                await handlerStarted.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
                Assert.Equal(1, args.AcknowledgeCount);
                Assert.False(receiveTask.IsCompleted);
            }
            finally
            {
                releaseHandler.TrySetResult();
                await receiveTask.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            }

            Assert.True(laterHandlerRan);
            Assert.Equal(1, args.AcknowledgeCount);
        }

        [Fact]
        public async Task AcknowledgementFailureDoesNotPreventPublishDelivery()
        {
            using MockConnectionMqttClient mqttClient = new();
            using MqttConnectionManager connectionManager = new(mqttClient, TimeSpan.FromSeconds(30), new RecordingRetryPolicy());
            MockMqttPublishReceivedEventArgs args = new()
            {
                Publish = new() { Topic = "unknown/topic", QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce },
                AcknowledgeFails = true,
            };
            MqttPublishReceivedEventArgs? received = null;
            connectionManager.PublishReceivedAsync += publish =>
            {
                received = publish;
                return Task.CompletedTask;
            };

            await mqttClient.SimulatePublishReceivedEventAsync(args);

            Assert.Same(args, received);
            Assert.Equal(1, args.AcknowledgeCount);
        }

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

        /// <summary>
        /// Disconnect callbacks from the underlying client that do not correspond to a connection that came up and was
        /// lost -- one for a connect attempt that never connected, one while still connected, or a second one for the same
        /// loss -- must not be reported.
        /// </summary>
        [Fact]
        public async Task DisconnectedIsReportedOnlyOncePerLostConnection()
        {
            using MockConnectionMqttClient mqttClient = new();
            using MqttConnectionManager connectionManager = new(mqttClient, TimeSpan.FromSeconds(30), new RecordingRetryPolicy());
            int disconnectedCount = 0;
            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            int connectedCount = 0;
            connectionManager.DisconnectedAsync += _ =>
            {
                Interlocked.Increment(ref disconnectedCount);
                return Task.CompletedTask;
            };
            connectionManager.ConnectedAsync += _ =>
            {
                if (Interlocked.Increment(ref connectedCount) == 2)
                {
                    reconnected.TrySetResult();
                }

                return Task.CompletedTask;
            };

            // A callback for a connect attempt that never connected.
            await mqttClient.SimulateSpuriousDisconnectCallbackAsync(MqttDisconnectReason.ImplementationSpecificError);
            Assert.Equal(0, Volatile.Read(ref disconnectedCount));

            await connectionManager.ConnectAsync(new MqttConnect() { HostName = "some-hub.azure-devices.net" }, TestContext.Current.CancellationToken);

            // A callback while the connection is still up.
            await mqttClient.SimulateSpuriousDisconnectCallbackAsync(MqttDisconnectReason.ImplementationSpecificError);
            Assert.Equal(0, Volatile.Read(ref disconnectedCount));

            TaskCompletionSource<MqttConnectAck> reconnect = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mqttClient.OnConnect = _ => reconnect.Task;
            await mqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            Assert.Equal(1, Volatile.Read(ref disconnectedCount));

            // A second callback for the same loss, while reconnection is still pending.
            await mqttClient.SimulateSpuriousDisconnectCallbackAsync(MqttDisconnectReason.ServerBusy);
            Assert.Equal(1, Volatile.Read(ref disconnectedCount));

            reconnect.TrySetResult(new() { ResultCode = MqttConnectReasonCode.Success });
            await reconnected.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);
            Assert.Equal(1, Volatile.Read(ref disconnectedCount));
        }

        /// <summary>
        /// When the underlying client's disconnect callback for a lost connection only arrives after the next connection
        /// is up, the loss must still be reported, exactly once and before the next connection is.
        /// </summary>
        [Fact]
        public async Task LateDisconnectCallbackIsReportedOnceBeforeNextConnection()
        {
            using MockConnectionMqttClient mqttClient = new();
            using MqttConnectionManager connectionManager = new(mqttClient, TimeSpan.FromSeconds(30), new RecordingRetryPolicy());
            List<string> events = new();
            connectionManager.ConnectedAsync += _ =>
            {
                lock (events) { events.Add("connected"); }
                return Task.CompletedTask;
            };
            connectionManager.DisconnectedAsync += _ =>
            {
                lock (events) { events.Add("disconnected"); }
                return Task.CompletedTask;
            };

            await connectionManager.ConnectAsync(new MqttConnect() { HostName = "some-hub.azure-devices.net" }, TestContext.Current.CancellationToken);

            // The underlying client reconnects before raising the disconnect callback for the connection it lost...
            await mqttClient.ConnectAsync(new MqttConnect() { HostName = "some-hub.azure-devices.net" }, TestContext.Current.CancellationToken);

            // ...which then arrives late.
            await mqttClient.SimulateSpuriousDisconnectCallbackAsync(MqttDisconnectReason.ImplementationSpecificError);

            lock (events)
            {
                Assert.Equal(new[] { "connected", "disconnected", "connected" }, events);
            }
        }

        /// <summary>
        /// A spurious disconnect callback must not make an operation that failed for another reason look as if its
        /// connection was lost, which would have it sent again rather than reported as a contained error.
        /// </summary>
        [Fact]
        public async Task SpuriousDisconnectCallbackDoesNotMakeFailedOperationLookInterrupted()
        {
            using MockConnectionMqttClient mqttClient = new();
            mqttClient.OnPublish = async _ =>
            {
                await mqttClient.SimulateSpuriousDisconnectCallbackAsync(MqttDisconnectReason.ImplementationSpecificError);
                throw new TimeoutException("mock ack timeout");
            };
            using MqttConnectionManager connectionManager = new(mqttClient, TimeSpan.FromSeconds(30), new RecordingRetryPolicy());
            await connectionManager.SwitchEndpointAsync(ConnectionEndpoint.IotHub, TestContext.Current.CancellationToken);
            await connectionManager.ConnectAsync(new MqttConnect() { HostName = "some-hub.azure-devices.net" }, TestContext.Current.CancellationToken);

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                () => connectionManager.PublishAsync(
                    ConnectionEndpoint.IotHub,
                    new MqttPublish() { Topic = "devices/d/messages/events/", QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce },
                    TestContext.Current.CancellationToken));

            Assert.True(exception.IsContained);
            Assert.IsType<TimeoutException>(exception.InnerException);
        }

        /// <summary>
        /// Handlers of a connection loss must finish before reconnection starts, so that work they abandon for the lost
        /// connection (such as pending certificate signing requests) cannot include work started on the next one.
        /// </summary>
        [Fact]
        public async Task ReconnectionStartsOnlyAfterDisconnectedHandlersFinish()
        {
            using MockConnectionMqttClient mqttClient = new();
            using MqttConnectionManager connectionManager = new(mqttClient, TimeSpan.FromSeconds(30), new RecordingRetryPolicy());
            await connectionManager.ConnectAsync(new MqttConnect() { HostName = "some-hub.azure-devices.net" }, TestContext.Current.CancellationToken);
            TaskCompletionSource<int> connectAttemptsAfterHandler = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionManager.DisconnectedAsync += async _ =>
            {
                // Longer than the retry policy's delay, so a reconnection running alongside this handler would attempt
                // to connect before it finishes.
                await Task.Delay(TimeSpan.FromMilliseconds(500));
                connectAttemptsAfterHandler.TrySetResult(mqttClient.ConnectAttemptCount);
            };
            connectionManager.DisconnectedAsync += _ => Task.CompletedTask;

            await mqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy);

            Assert.Equal(1, await connectAttemptsAfterHandler.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));
        }

        /// <summary>
        /// A spurious disconnect callback while the connection is active must not make an acknowledgement timeout look
        /// like a connection loss.
        /// </summary>
        [Fact]
        public async Task SpuriousDisconnectCallbackDoesNotMisclassifyAcknowledgementTimeout()
        {
            using MockConnectionMqttClient mqttClient = new();
            TaskCompletionSource publishReachedClient = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource timeOutAcknowledgement = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mqttClient.OnPublish = async _ =>
            {
                publishReachedClient.TrySetResult();
                await timeOutAcknowledgement.Task;
                throw new TimeoutException("The mock acknowledgement timed out.");
            };
            using MqttConnectionManager connectionManager = new(mqttClient, TimeSpan.FromSeconds(30), new RecordingRetryPolicy());

            await connectionManager.ConnectAsync(new MqttConnect() { HostName = "some-hub.azure-devices.net" }, TestContext.Current.CancellationToken);
            await connectionManager.SwitchEndpointAsync(ConnectionEndpoint.IotHub, TestContext.Current.CancellationToken);

            Task<MqttPublishAck> publish = connectionManager.PublishAsync(
                ConnectionEndpoint.IotHub,
                new MqttPublish() { Topic = "devices/d/messages/events/", QualityOfServiceLevel = MqttQualityOfServiceLevel.AtLeastOnce },
                TestContext.Current.CancellationToken);
            await publishReachedClient.Task.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken);

            await mqttClient.SimulateSpuriousDisconnectCallbackAsync(MqttDisconnectReason.ImplementationSpecificError);
            timeOutAcknowledgement.TrySetResult();

            DeviceException exception = await Assert.ThrowsAsync<DeviceException>(
                () => publish.WaitAsync(s_testTimeout, TestContext.Current.CancellationToken));
            Assert.IsType<TimeoutException>(exception.InnerException);
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
            public event Func<MqttConnect, CancellationToken, Task<MqttConnect>>? ConnectingAsync;
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
