// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Google.Protobuf;
using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Exceptions;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.Twin;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Retry;
using System.Text;
using Xunit;
using MqttV5ConnectionClientOptions = Microsoft.Azure.Iot.Device.MQTTv5.Models.ConnectionClientOptions;
using Microsoft.Azure.Iot.Device.Unified.Models;

namespace Microsoft.Azure.Iot.Device.UnitTests.MQTTv5
{
    /// <summary>
    /// Tests for recovery and shutdown of the MQTTv5 device presence (birth) flow.
    /// </summary>
    /// <remarks>
    /// Every failure in the device presence (birth) flow is expected to tear down the connection with
    /// <c>DisconnectAsync(desireReconnection: true)</c>. The <c>MqttConnectionManager</c> then reconnects on its own and
    /// runs the birth flow again, so a single transient failure is expected to eventually recover into a successful
    /// announcement.
    ///
    /// These tests exercise that recovery on the very first connect attempt: the birth flow runs fire-and-forget as soon
    /// as the broker accepts the CONNECT, and the connection layer now marks the connection as desired before that first
    /// attempt, so a failure there disconnects and reconnects just like it would on an already-established connection.
    /// Because the connection client's <c>ConnectAsync</c> does not complete until device presence has been announced
    /// successfully, awaiting it is enough to prove that the client recovered from the injected failure.
    /// </remarks>
    public class DevicePresenceFailureRecoveryTests
    {
        private static ConnectionContext GetMockConnectionContext()
        {
#pragma warning disable SYSLIB0026 // Type or member is obsolete (Mock certificate, don't need to load a real one using typical X509 certificate loader
            return new ConnectionContext()
            {
                AuthenticationProvider = new X509AuthenticationProvider(new System.Security.Cryptography.X509Certificates.X509Certificate2(), null),
                DeviceId = "someDeviceId",
                IotHubHostName = "someHostName",
                ConnectionProfile = IotHubConnectionType.Mqttv5
            };
#pragma warning restore SYSLIB0026 // Type or member is obsolete
        }

        [Theory]
        [InlineData(false, true, true)]
        [InlineData(true, false, true)]
        [InlineData(true, true, false)]
        [InlineData(false, false, false)]
        public async Task UnifiedFeatureOptOutsKeepSharedMqttV5Subscription(
            bool enableTwin, bool enableDirectMethods, bool enableCloudToDeviceMessages)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                EnableTwin = enableTwin,
                EnableDirectMethods = enableDirectMethods,
                EnableCloudToDeviceMessages = enableCloudToDeviceMessages,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            using Device.Unified.Connection.ConnectionClient connectionClient = new(options);

            options.EnableTwin = !enableTwin;
            options.EnableDirectMethods = !enableDirectMethods;
            options.EnableCloudToDeviceMessages = !enableCloudToDeviceMessages;

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            MqttSubscribe[] subscribes = mockMqttClient.SentMqttTrafficInOrder
                .Select(traffic => traffic.Subscribe).OfType<MqttSubscribe>().ToArray();
            Assert.Equal(2, subscribes.Length);
            Assert.All(subscribes, subscribe => Assert.Equal("ih/someDeviceId/dev/#", Assert.Single(subscribe.TopicFilters).Topic));

            MqttPublish[] births = mockMqttClient.SentMqttTrafficInOrder
                .Select(traffic => traffic.Publish).OfType<MqttPublish>().ToArray();
            Assert.Equal(2, births.Length);
            Assert.All(births, publish =>
            {
                Birth birth = Birth.Parser.ParseFrom(publish.Payload);
                Assert.Equal(enableTwin, birth.PushDesired);
                Assert.Equal(enableTwin, birth.PushReported);
            });
        }

        [Theory]
        [InlineData(false, false)]
        [InlineData(false, true)]
        [InlineData(true, false)]
        [InlineData(true, true)]
        public async Task BirthHonorsSnapshottedTwinPushSettings(bool pushDesired, bool pushReported)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                ReceiveDesiredPropertyUpdates = pushDesired,
                ReceiveReportedPropertiesUponConnect = pushReported,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            using ConnectionClient connectionClient = new(options);
            using TwinClient twinClient = new(connectionClient);
            using Device.MQTTv5.DirectMethods.DirectMethodClient directMethodClient = new(connectionClient);
            options.ReceiveDesiredPropertyUpdates = !pushDesired;
            options.ReceiveReportedPropertiesUponConnect = !pushReported;

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            MqttSubscribe[] subscribes = mockMqttClient.SentMqttTrafficInOrder
                .Select(traffic => traffic.Subscribe).OfType<MqttSubscribe>().ToArray();
            Assert.Equal(2, subscribes.Length);
            Assert.All(subscribes, subscribe => Assert.Equal("ih/someDeviceId/dev/#", Assert.Single(subscribe.TopicFilters).Topic));

            MqttPublish[] births = mockMqttClient.SentMqttTrafficInOrder
                .Select(traffic => traffic.Publish).OfType<MqttPublish>().ToArray();
            Assert.Equal(2, births.Length);
            Assert.All(births, publish =>
            {
                Birth birth = Birth.Parser.ParseFrom(publish.Payload);
                Assert.Equal(pushDesired, birth.PushDesired);
                Assert.Equal(pushReported, birth.PushReported);
            });
        }

        [Fact]
        public async Task AnnounceDevicePresenceRecoversWhenSubackIsMalformed()
        {
            // A SUBACK with no items is malformed, so the birth flow disconnects and lets the manager reconnect.
            await RunInitialConnectBirthFailureTestAsync(
                birthSubscribeInjector: (subscribe, attempt) => attempt == 1
                    ? new MqttSubscribeAck() { Items = new List<MqttSubscribeAckItem>() }
                    : null,
                birthPublishInjector: null,
                cancellationToken: TestContext.Current.CancellationToken);
        }

        [Fact]
        public async Task AnnounceDevicePresenceRecoversWhenSubackReasonCodeIsUnsuccessful()
        {
            // The birth SUBSCRIBE is requested at QoS 1, so anything other than GrantedQoS1 is a failed subscribe.
            await RunInitialConnectBirthFailureTestAsync(
                birthSubscribeInjector: (subscribe, attempt) => attempt == 1
                    ? new MqttSubscribeAck()
                    {
                        Items = new List<MqttSubscribeAckItem>()
                        {
                            new MqttSubscribeAckItem()
                            {
                                ReasonCode = MqttClientSubscribeReasonCode.GrantedQoS0,
                                TopicFilter = new(subscribe.TopicFilters.First().Topic, MqttQualityOfServiceLevel.AtLeastOnce),
                            }
                        }
                    }
                    : null,
                birthPublishInjector: null,
                cancellationToken: TestContext.Current.CancellationToken);
        }

        [Fact]
        public async Task AnnounceDevicePresenceRecoversWhenSubscribeThrows()
        {
            // An exception raised while subscribing to the devicebound topic is caught and treated as a failed birth.
            await RunInitialConnectBirthFailureTestAsync(
                birthSubscribeInjector: (subscribe, attempt) => attempt == 1
                    ? throw new Exception("mock exception thrown while subscribing to devicebound topic")
                    : null,
                birthPublishInjector: null,
                cancellationToken: TestContext.Current.CancellationToken);
        }

        [Fact]
        public async Task AnnounceDevicePresenceRecoversWhenBirthPublishThrows()
        {
            // An exception raised while publishing the birth message is caught and treated as a failed birth.
            await RunInitialConnectBirthFailureTestAsync(
                birthSubscribeInjector: null,
                birthPublishInjector: (publish, attempt) => attempt == 1
                    ? throw new Exception("mock exception thrown while publishing birth message")
                    : null,
                cancellationToken: TestContext.Current.CancellationToken);
        }

        [Fact]
        public async Task AnnounceDevicePresenceRecoversWhenBirthPubackIsUnsuccessful()
        {
            // A non-success PUBACK on the birth message means the announcement did not land, so the birth flow disconnects.
            await RunInitialConnectBirthFailureTestAsync(
                birthSubscribeInjector: null,
                birthPublishInjector: (publish, attempt) => attempt == 1
                    ? new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.NoMatchingSubscribers }
                    : null,
                cancellationToken: TestContext.Current.CancellationToken);
        }

        [Fact]
        public async Task InitialConnectThrowsWhenPresenceRecoveryRetryIsExhausted()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            mockMqttClient.OnConnect = async (connect) =>
                new MqttConnectAck() { IsSessionPresent = false, ResultCode = MqttConnectReasonCode.Success };
            mockMqttClient.OnSubscribe = async (subscribe) =>
                new MqttSubscribeAck() { Items = new List<MqttSubscribeAckItem>() };

            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new NoRetry(),
            });

            await Assert.ThrowsAsync<DeviceException>(() =>
                connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken)
                    .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken));
        }

        [Theory]
        [InlineData(false, "subscribe", false)]
        [InlineData(false, "publish", false)]
        [InlineData(false, "birth-ack", false)]
        [InlineData(true, "subscribe", false)]
        [InlineData(true, "publish", false)]
        [InlineData(true, "birth-ack", false)]
        [InlineData(false, "subscribe", true)]
        [InlineData(false, "publish", true)]
        [InlineData(false, "birth-ack", true)]
        [InlineData(true, "subscribe", true)]
        [InlineData(true, "publish", true)]
        [InlineData(true, "birth-ack", true)]
        public async Task DisconnectAbandonsPendingPresenceWithoutResettingRecovery(bool useUnifiedClient, string pendingStage, bool recoverConnection)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClientOptions options = new() { MqttClient = mockMqttClient };
            AbstractConnectionClient connectionClient = useUnifiedClient
                ? new Device.Unified.Connection.ConnectionClient(options)
                : new ConnectionClient(new() { MqttClient = mockMqttClient });

            TaskCompletionSource stageStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource recoveryCompleted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource<MqttSubscribeAck> pendingSuback = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource<MqttPublishAck> pendingPuback = new(TaskCreationOptions.RunContinuationsAsynchronously);
            MqttSubscribe? presenceSubscribe = null;
            MqttPublish? birthPublish = null;

            try
            {
                await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken);
                mockMqttClient.AutoRespondToBirth = false;
                int presenceCompletions = 0;
                connectionClient.DevicePresenceFlowCompletedAsync += _ =>
                {
                    Interlocked.Increment(ref presenceCompletions);
                    recoveryCompleted.TrySetResult();
                    return Task.CompletedTask;
                };

                mockMqttClient.OnSubscribe = subscribe =>
                {
                    presenceSubscribe ??= subscribe;
                    if (pendingStage == "subscribe" && !stageStarted.Task.IsCompleted)
                    {
                        stageStarted.TrySetResult();
                        return pendingSuback.Task;
                    }

                    return Task.FromResult(MqttObjectHelpers.CreateSuccessfulSuback(subscribe));
                };
                mockMqttClient.OnPublish = publish =>
                {
                    bool isFirstPublish = birthPublish == null;
                    birthPublish ??= publish;
                    stageStarted.TrySetResult();
                    return pendingStage == "publish" && isFirstPublish
                        ? pendingPuback.Task
                        : Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
                };

                Task announcement = connectionClient.HandleConnectedToHubAsync(new()
                {
                    ConnectAck = new() { ResultCode = MqttConnectReasonCode.Success },
                });
                await stageStarted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                if (recoverConnection)
                {
                    mockMqttClient.AutoRespondToBirth = true;
                    await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy);
                    await recoveryCompleted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                }
                else
                {
                    await connectionClient.DisconnectAsync(TestContext.Current.CancellationToken);
                }

                await announcement.WaitAsync(TimeSpan.FromSeconds(5), TestContext.Current.CancellationToken);

                pendingSuback.TrySetResult(MqttObjectHelpers.CreateSuccessfulSuback(presenceSubscribe!));
                pendingPuback.TrySetResult(new() { ReasonCode = MqttPublishAckReasonCode.Success });
                if (birthPublish != null)
                {
                    MqttPublish lateBirthAck = new()
                    {
                        Topic = "ih/someDeviceId/dev/presence",
                        CorrelationData = birthPublish.CorrelationData,
                        Payload = new BirthAck().ToByteArray(),
                    };
                    lateBirthAck.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth-ack:1")));
                    await mockMqttClient.SimulatePublishReceivedAsync(lateBirthAck);
                }

                Assert.Equal(recoverConnection ? 1 : 0, presenceCompletions);
                Assert.Equal(recoverConnection ? 0 : 1, mockMqttClient.DisconnectRequests.Count);
                Assert.Equal(recoverConnection ? 2 : 1, mockMqttClient.ConnectAttemptCount);
                Assert.Equal(recoverConnection, mockMqttClient.IsConnected());
            }
            finally
            {
                pendingSuback.TrySetCanceled();
                pendingPuback.TrySetCanceled();
                connectionClient.Dispose();
            }
        }

        /// <summary>
        /// Injects a single birth-flow failure into the very first connect attempt and asserts that the client
        /// disconnects, reconnects on its own, and eventually announces device presence successfully so that
        /// <see cref="ConnectionClient.ConnectAsync"/> completes.
        /// </summary>
        /// <param name="birthSubscribeInjector">
        /// Optional override for the birth SUBSCRIBE. Receives the subscribe request and the 1-based attempt number.
        /// Return a SUBACK (or throw) to fail that attempt, or return null to send a normal successful SUBACK.
        /// </param>
        /// <param name="birthPublishInjector">
        /// Optional override for the birth PUBLISH. Receives the birth publish and the 1-based attempt number. Return a
        /// PUBACK (or throw) to fail that attempt, or return null to send a normal successful PUBACK.
        /// </param>
        private static async Task RunInitialConnectBirthFailureTestAsync(
            Func<MqttSubscribe, int, MqttSubscribeAck?>? birthSubscribeInjector,
            Func<MqttPublish, int, MqttPublishAck?>? birthPublishInjector,
            CancellationToken cancellationToken)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            int connectCount = 0;
            int disconnectCount = 0;
            int birthSubscribeCount = 0;
            int birthPublishCount = 0;

            // Sessions never resume, so every connect's birth flow re-sends the presence SUBSCRIBE that the failure
            // cases target.
            mockMqttClient.OnConnect = async (connect) =>
            {
                Interlocked.Increment(ref connectCount);
                return new MqttConnectAck() { IsSessionPresent = false, ResultCode = MqttConnectReasonCode.Success };
            };

            mockMqttClient.OnDisconnect = async (disconnect) =>
            {
                Interlocked.Increment(ref disconnectCount);
            };

            mockMqttClient.OnSubscribe = async (subscribe) =>
            {
                // Only the birth flow subscribes in this test, so every subscribe here is a device presence subscribe.
                int attempt = Interlocked.Increment(ref birthSubscribeCount);
                MqttSubscribeAck? injected = birthSubscribeInjector?.Invoke(subscribe, attempt);
                return injected ?? MqttObjectHelpers.CreateSuccessfulSuback(subscribe);
            };

            mockMqttClient.OnPublish = async (publish) =>
            {
                // Only the birth message is published in this test, so every publish here is a device presence publish.
                if (publish.Topic.EndsWith("/srv/presence") && birthPublishInjector != null)
                {
                    int attempt = Interlocked.Increment(ref birthPublishCount);
                    MqttPublishAck? injected = birthPublishInjector.Invoke(publish, attempt);
                    if (injected != null)
                    {
                        return injected;
                    }
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            // ConnectAsync only completes once the birth flow has successfully announced device presence, which requires
            // recovering from the injected failure on the first attempt. A timeout guards against a regression where the
            // failure is not retried (which would hang forever).
            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: cancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), cancellationToken);

            // The birth flow's failure handling must have disconnected at least once...
            Assert.True(disconnectCount >= 1, $"Expected at least one disconnect from the failed birth flow, but saw {disconnectCount}.");

            // ...and the manager must have reconnected on its own for the initial connect to eventually succeed.
            Assert.True(connectCount >= 2, $"Expected at least two connects (the initial attempt plus a reconnect after the failed birth flow), but saw {connectCount}.");

            // A successful birth flow ends by publishing the birth message on the presence topic, so that must be the
            // last traffic on the wire once device presence has been announced.
            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();
            Assert.NotNull(lastTraffic.Publish);
            Assert.EndsWith("/srv/presence", lastTraffic.Publish.Topic);
        }
    }
}
