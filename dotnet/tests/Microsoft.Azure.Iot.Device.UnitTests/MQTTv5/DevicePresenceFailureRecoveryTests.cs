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
        public async Task BirthAnnouncesInitialThenLatestObservedTwinVersions()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            using ConnectionClient connectionClient = new(options);
            using TwinClient twinClient = new(connectionClient, new Device.Models.Twin.TwinState() { DesiredVersion = 5, ReportedVersion = 3 });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            var push = new Device.Models.Twin.TwinPush()
            {
                Desired = new Device.Models.Twin.Section { Version = 7, Payload = ByteString.CopyFromUtf8("{}") },
                Reported = new Device.Models.Twin.Section { Version = 4, Payload = ByteString.CopyFromUtf8("{}") },
            };
            MqttPublish pushPublish = new()
            {
                Topic = "ih/someDeviceId/dev/twin",
                CorrelationData = connectionClient.CurrentConnectionNonce!.Value.ToByteArray(bigEndian: true),
                Payload = push.ToByteArray(),
            };
            pushPublish.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("twin-push:1")));
            await mockMqttClient.SimulatePublishReceivedAsync(pushPublish);

            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Birth[] births = mockMqttClient.SentMqttTrafficInOrder
                .Select(traffic => traffic.Publish).OfType<MqttPublish>()
                .Where(publish => publish.Topic.EndsWith("/srv/presence", StringComparison.Ordinal))
                .Select(publish => Birth.Parser.ParseFrom(publish.Payload))
                .ToArray();
            Assert.Equal(2, births.Length);
            Assert.Equal(5ul, births[0].DesiredVersion);
            Assert.Equal(3ul, births[0].ReportedVersion);
            Assert.Equal(7ul, births[1].DesiredVersion);
            Assert.Equal(4ul, births[1].ReportedVersion);
        }

        [Fact]
        public async Task BirthAsDifferentIdentityAnnouncesNoTwinVersions()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            using ConnectionClient connectionClient = new(options);
            using TwinClient twinClient = new(connectionClient, new Device.Models.Twin.TwinState() { DesiredVersion = 5, ReportedVersion = 3 });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await connectionClient.DisconnectAsync(null, TestContext.Current.CancellationToken);

#pragma warning disable SYSLIB0026 // Mock certificate
            ConnectionContext otherIdentity = new()
            {
                AuthenticationProvider = new X509AuthenticationProvider(new System.Security.Cryptography.X509Certificates.X509Certificate2(), null),
                DeviceId = "someOtherDeviceId",
                IotHubHostName = "someHostName",
                ConnectionProfile = IotHubConnectionType.Mqttv5,
            };
#pragma warning restore SYSLIB0026
            await connectionClient.ConnectToHubAsync(otherIdentity, TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Birth[] births = mockMqttClient.SentMqttTrafficInOrder
                .Select(traffic => traffic.Publish).OfType<MqttPublish>()
                .Where(publish => publish.Topic.EndsWith("/srv/presence", StringComparison.Ordinal))
                .Select(publish => Birth.Parser.ParseFrom(publish.Payload))
                .ToArray();
            Assert.Equal(2, births.Length);
            Assert.Equal(5ul, births[0].DesiredVersion);
            Assert.Equal(3ul, births[0].ReportedVersion);
            Assert.Equal(0ul, births[1].DesiredVersion);
            Assert.Equal(0ul, births[1].ReportedVersion);
            Assert.Equal(0ul, twinClient.GetTwinState().DesiredVersion);
            Assert.Equal(0ul, twinClient.GetTwinState().ReportedVersion);
        }

        [Fact]
        public async Task MissingExpectedTwinPushReconnectsWithEscalatingTimeout()
        {
            using MockConnectionMqttClient mockMqttClient = new()
            {
                AutoRespondToBirth = true,
                AutoBirthAck = new BirthAck() { DesiredVersion = 3, ReportedVersion = 2 },
            };
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            using ConnectionClient connectionClient = new(options);
            List<int> attempts = new();
            connectionClient.TwinPushTimeoutSchedule = attempt =>
            {
                lock (attempts)
                {
                    attempts.Add(attempt);
                }

                return attempt < 3 ? TimeSpan.FromMilliseconds(50) : Timeout.InfiniteTimeSpan;
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            await WaitForBirthCountAsync(mockMqttClient, 3);
            lock (attempts)
            {
                Assert.Equal(new[] { 1, 2, 3 }, attempts);
            }

            Assert.Equal(2, mockMqttClient.DisconnectRequests.Count);
        }

        [Fact]
        public async Task TwinPushBeforeBirthAckSatisfiesExpectedTwinPush()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            mockMqttClient.OnPublish = async publish =>
            {
                if (publish.Topic.EndsWith("/srv/presence", StringComparison.Ordinal))
                {
                    // IoT hub usually sends the twin push before the birth ack.
                    MqttPublish push = new()
                    {
                        Topic = "ih/someDeviceId/dev/twin",
                        CorrelationData = publish.CorrelationData,
                        Payload = new Device.Models.Twin.TwinPush().ToByteArray(),
                    };
                    push.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("twin-push:1")));
                    await mockMqttClient.SimulatePublishReceivedAsync(push);

                    MqttPublish birthAck = new()
                    {
                        Topic = "ih/someDeviceId/dev/presence",
                        CorrelationData = publish.CorrelationData,
                        Payload = new BirthAck() { DesiredVersion = 3, ReportedVersion = 2 }.ToByteArray(),
                    };
                    birthAck.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth-ack:1")));
                    _ = mockMqttClient.SimulatePublishReceivedAsync(birthAck);
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            using ConnectionClient connectionClient = new(options);
            TaskCompletionSource<int> scheduleRequested = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.TwinPushTimeoutSchedule = attempt =>
            {
                scheduleRequested.TrySetResult(attempt);
                return TimeSpan.FromMilliseconds(50);
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            Assert.Equal(1, await scheduleRequested.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken));

            // Give a (wrongly) missed twin push's timeout plenty of time to reconnect.
            await Task.Delay(TimeSpan.FromMilliseconds(500), TestContext.Current.CancellationToken);
            Assert.Empty(mockMqttClient.DisconnectRequests);
            Assert.Equal(1, mockMqttClient.ConnectAttemptCount);
        }

        [Theory]
        [InlineData("unsupported-type-version")]
        [InlineData("malformed-protobuf")]
        [InlineData("malformed-section")]
        [InlineData("stale-nonce")]
        public async Task UnusableTwinPushDoesNotSatisfyExpectedTwinPush(string defect)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            mockMqttClient.OnPublish = async publish =>
            {
                if (publish.Topic.EndsWith("/srv/presence", StringComparison.Ordinal))
                {
                    // The twin client ignores twin pushes that it can't apply, so they don't count as the expected push.
                    MqttPublish push = new()
                    {
                        Topic = "ih/someDeviceId/dev/twin",
                        CorrelationData = defect == "stale-nonce" ? Guid.NewGuid().ToByteArray(bigEndian: true) : publish.CorrelationData,
                        Payload = defect switch
                        {
                            "malformed-protobuf" => new byte[] { 0xFF, 0xFF, 0xFF },
                            "malformed-section" => new Device.Models.Twin.TwinPush()
                            {
                                Desired = new Device.Models.Twin.Section { Version = 3, Payload = ByteString.CopyFromUtf8("not json") },
                            }.ToByteArray(),
                            "stale-nonce" => new Device.Models.Twin.TwinPush()
                            {
                                Desired = new Device.Models.Twin.Section { Version = 3, Payload = ByteString.CopyFromUtf8("{}") },
                            }.ToByteArray(),
                            _ => new Device.Models.Twin.TwinPush().ToByteArray(),
                        },
                    };
                    push.UserProperties.Add(new("type", Encoding.UTF8.GetBytes(defect == "unsupported-type-version" ? "twin-push:2" : "twin-push:1")));
                    await mockMqttClient.SimulatePublishReceivedAsync(push);

                    MqttPublish birthAck = new()
                    {
                        Topic = "ih/someDeviceId/dev/presence",
                        CorrelationData = publish.CorrelationData,
                        Payload = new BirthAck() { DesiredVersion = 3, ReportedVersion = 2 }.ToByteArray(),
                    };
                    birthAck.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth-ack:1")));
                    _ = mockMqttClient.SimulatePublishReceivedAsync(birthAck);
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            using ConnectionClient connectionClient = new(options);
            connectionClient.TwinPushTimeoutSchedule = attempt => attempt == 1 ? TimeSpan.FromMilliseconds(50) : Timeout.InfiniteTimeSpan;

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            await WaitForBirthCountAsync(mockMqttClient, 2);
            Assert.Single(mockMqttClient.DisconnectRequests);
        }

        [Fact]
        public async Task EscalatedTwinPushTimeoutResetsOnceABirthNeedsNoTwinPush()
        {
            int birthCount = 0;
            using MockConnectionMqttClient mockMqttClient = new();
            mockMqttClient.OnPublish = async publish =>
            {
                if (publish.Topic.EndsWith("/srv/presence", StringComparison.Ordinal))
                {
                    // Only the second birth's versions match IoT hub's, so it is the only one that needs no twin push.
                    BirthAck ack = Interlocked.Increment(ref birthCount) == 2
                        ? new BirthAck()
                        : new BirthAck() { DesiredVersion = 3, ReportedVersion = 2 };
                    MqttPublish birthAck = new()
                    {
                        Topic = "ih/someDeviceId/dev/presence",
                        CorrelationData = publish.CorrelationData,
                        Payload = ack.ToByteArray(),
                    };
                    birthAck.UserProperties.Add(new("type", Encoding.UTF8.GetBytes("birth-ack:1")));
                    _ = mockMqttClient.SimulatePublishReceivedAsync(birthAck);
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            using ConnectionClient connectionClient = new(options);
            List<int> attempts = new();
            connectionClient.TwinPushTimeoutSchedule = attempt =>
            {
                lock (attempts)
                {
                    attempts.Add(attempt);
                    return attempts.Count == 1 ? TimeSpan.FromMilliseconds(50) : Timeout.InfiniteTimeSpan;
                }
            };
            int completedFlows = 0;
            TaskCompletionSource secondFlowCompleted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                if (Interlocked.Increment(ref completedFlows) == 2)
                {
                    secondFlowCompleted.TrySetResult();
                }

                return Task.CompletedTask;
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            // The first twin push times out, so the device reconnects with the next birth, which needs no twin push.
            await secondFlowCompleted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await WaitForBirthCountAsync(mockMqttClient, 3);
            using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
            timeout.CancelAfter(TimeSpan.FromSeconds(30));
            while (true)
            {
                lock (attempts)
                {
                    if (attempts.Count == 2)
                    {
                        Assert.Equal(new[] { 1, 1 }, attempts);
                        return;
                    }
                }

                await Task.Delay(TimeSpan.FromMilliseconds(20), timeout.Token);
            }
        }

        [Fact]
        public async Task NoTwinPushIsExpectedWhenBirthVersionsMatchBirthAck()
        {
            using MockConnectionMqttClient mockMqttClient = new()
            {
                AutoRespondToBirth = true,
                AutoBirthAck = new BirthAck() { DesiredVersion = 3, ReportedVersion = 2 },
            };
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            using ConnectionClient connectionClient = new(options);
            using TwinClient twinClient = new(connectionClient, new Device.Models.Twin.TwinState() { DesiredVersion = 3, ReportedVersion = 2 });
            bool scheduleRequested = false;
            connectionClient.TwinPushTimeoutSchedule = _ =>
            {
                scheduleRequested = true;
                return TimeSpan.Zero;
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await Task.Delay(TimeSpan.FromMilliseconds(200), TestContext.Current.CancellationToken);

            Assert.False(scheduleRequested);
            Assert.Empty(mockMqttClient.DisconnectRequests);
        }

        [Fact]
        public async Task BirthAckWithOlderTwinVersionThanAnnouncedReconnectsToAnnounceValidVersions()
        {
            using MockConnectionMqttClient mockMqttClient = new()
            {
                AutoRespondToBirth = true,
                AutoBirthAck = new BirthAck() { DesiredVersion = 2, ReportedVersion = 3 },
            };
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            using ConnectionClient connectionClient = new(options);
            using TwinClient twinClient = new(connectionClient, new Device.Models.Twin.TwinState() { DesiredVersion = 5, ReportedVersion = 3 });
            connectionClient.TwinPushTimeoutSchedule = _ => Timeout.InfiniteTimeSpan;

            // Connecting completes only once IoT hub acknowledges a birth with valid versions.
            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Birth[] births = mockMqttClient.SentMqttTrafficInOrder
                .Select(traffic => traffic.Publish).OfType<MqttPublish>()
                .Where(publish => publish.Topic.EndsWith("/srv/presence", StringComparison.Ordinal))
                .Select(publish => Birth.Parser.ParseFrom(publish.Payload))
                .ToArray();
            Assert.Equal(2, births.Length);
            Assert.Equal(5ul, births[0].DesiredVersion);
            Assert.Equal(3ul, births[0].ReportedVersion);
            Assert.Equal(0ul, births[1].DesiredVersion);
            Assert.Equal(3ul, births[1].ReportedVersion);
            Assert.Single(mockMqttClient.DisconnectRequests);
        }

        private static async Task WaitForBirthCountAsync(MockConnectionMqttClient mockMqttClient, int count)
        {
            using CancellationTokenSource timeout = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
            timeout.CancelAfter(TimeSpan.FromSeconds(30));
            while (true)
            {
                lock (mockMqttClient.SentMqttTrafficInOrder)
                {
                    if (mockMqttClient.SentMqttTrafficInOrder.Count(traffic =>
                        traffic.Publish?.Topic.EndsWith("/srv/presence", StringComparison.Ordinal) == true) >= count)
                    {
                        return;
                    }
                }

                await Task.Delay(TimeSpan.FromMilliseconds(20), timeout.Token);
            }
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
            // A SUBACK failure reason code means the subscription does not exist, so the birth flow disconnects.
            await RunInitialConnectBirthFailureTestAsync(
                birthSubscribeInjector: (subscribe, attempt) => attempt == 1
                    ? new MqttSubscribeAck()
                    {
                        Items = new List<MqttSubscribeAckItem>()
                        {
                            new MqttSubscribeAckItem()
                            {
                                ReasonCode = MqttClientSubscribeReasonCode.NotAuthorized,
                                TopicFilter = new(subscribe.TopicFilters.First().Topic, MqttQualityOfServiceLevel.AtLeastOnce),
                            }
                        }
                    }
                    : null,
                birthPublishInjector: null,
                cancellationToken: TestContext.Current.CancellationToken);
        }

        // The broker may grant a QoS other than the requested QoS 1. The subscription still exists, so the birth is sent
        // on this connection rather than reconnecting forever.
        [Theory]
        [InlineData(MqttClientSubscribeReasonCode.GrantedQoS0)]
        [InlineData(MqttClientSubscribeReasonCode.GrantedQoS2)]
        public async Task AnnounceDevicePresenceAcceptsAnyGrantedQoS(MqttClientSubscribeReasonCode grantedReasonCode)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            int disconnectCount = 0;
            mockMqttClient.OnDisconnect = disconnect =>
            {
                Interlocked.Increment(ref disconnectCount);
                return Task.CompletedTask;
            };
            mockMqttClient.OnSubscribe = subscribe => Task.FromResult(new MqttSubscribeAck()
            {
                Items = new List<MqttSubscribeAckItem>()
                {
                    new MqttSubscribeAckItem()
                    {
                        ReasonCode = grantedReasonCode,
                        TopicFilter = new(subscribe.TopicFilters.First().Topic, MqttQualityOfServiceLevel.AtLeastOnce),
                    }
                }
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(0, Volatile.Read(ref disconnectCount));
            Assert.Equal(1, mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Subscribe != null));
            Assert.Equal(1, mockMqttClient.SentMqttTrafficInOrder.Count(traffic =>
                traffic.Publish?.Topic.EndsWith("/srv/presence", StringComparison.Ordinal) == true));
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
                    await connectionClient.DisconnectAsync(null, TestContext.Current.CancellationToken);
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
