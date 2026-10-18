// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Unified.Connection;
using Microsoft.Azure.Iot.Device.Unified.Telemetry;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Xunit;
using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.Retry;

namespace Microsoft.Azure.Iot.Device.UnitTests.Unified
{
    public class ConnectionClientUnitTests
    {
        private static ConnectionContext GetMockConnectionContext(bool isMQTTv5)
        {
#pragma warning disable SYSLIB0026 // Type or member is obsolete (Mock certificate, don't need to load a real one using typical X509 certificate loader
            return new ConnectionContext()
            {
                AuthenticationProvider = new X509AuthenticationProvider(new System.Security.Cryptography.X509Certificates.X509Certificate2()),
                DeviceId = "someDeviceId",
                IotHubHostName = "someHostName",
                ConnectionProfile = isMQTTv5 ? IotHubConnectionType.Mqttv5 : IotHubConnectionType.Mqttv3,
            };
#pragma warning restore SYSLIB0026 // Type or member is obsolete
        }

        [Fact]
        public async Task ProvisionAndConnectUsesSeededConnectionContext()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            ConnectionContext connectionContext = GetMockConnectionContext(false);
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
            }, connectionContext);

            // The seeded assignment connects, so this device connects directly using it rather than provisioning.
            ConnectionContext result = await connectionClient.ProvisionAndConnectAsync(
                new ProvisioningSettings("someIdScope"),
                connectionContext.AuthenticationProvider,
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.Same(connectionContext, result);
            Assert.Same(connectionContext, connectionClient.GetCurrentConnectionContext());
        }

        [Theory]
        [InlineData(true)]
        [InlineData(false)]
        public async Task ConnectionClientReannouncesBirthBeforeContinuingPublish(bool isSessionResumed)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            TelemetryClient telemetryClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnect = connect =>
            {
                return Task.FromResult(new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success });
            };
            mockMqttClient.OnPublish = async (publish) =>
            {
                if (retryCount == 0)
                {
                    _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new Exception("mock exception"));
                    retryCount++;
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            await telemetryClient.SendTelemetryAsync(new Models.Telemetry.DeviceToCloudTelemetry(), TestContext.Current.CancellationToken);

            if (isSessionResumed)
            {
                // With session resumed, the reconnect flow doesn't need to send the subscribe on the direct methods/telemetry/twin topics
                Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count);
            }
            else
            {
                Assert.Equal(3, mockMqttClient.SentMqttTrafficInOrder.Count);
            }

            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();

            // Verify that the telemetry sent by the telemetry client is the last piece of traffic "sent" in this test. It must be preceded by the intial device presence flow traffic and by the reconnection device presence flow traffic
            Assert.NotNull(lastTraffic.Publish);
            Assert.StartsWith("devices/", lastTraffic.Publish.Topic);
            Assert.EndsWith("/messages/events/", lastTraffic.Publish.Topic);
        }

        [Fact]
        public async Task PublishRetriesWhenPresenceCompletesBeforeFailedAttemptReturns()
        {
            using MockConnectionMqttClient mockMqttClient = new();
            using TestConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), TestContext.Current.CancellationToken);

            TaskCompletionSource firstPublishAttemptStarted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource releaseFirstPublishAttempt = new(TaskCreationOptions.RunContinuationsAsynchronously);
            TaskCompletionSource reconnectPresenceCompleted = new(TaskCreationOptions.RunContinuationsAsynchronously);
            int presenceCompletionCount = 0;
            connectionClient.DevicePresenceFlowCompletedAsync += args =>
            {
                Interlocked.Increment(ref presenceCompletionCount);
                reconnectPresenceCompleted.TrySetResult();
                return Task.CompletedTask;
            };

            int publishAttemptCount = 0;
            mockMqttClient.OnPublish = async publish =>
            {
                if (Interlocked.Increment(ref publishAttemptCount) == 1)
                {
                    firstPublishAttemptStarted.TrySetResult();
                    await releaseFirstPublishAttempt.Task;
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            Task<MqttPublishAck> pendingPublish = connectionClient.PublishAsync(
                new MqttPublish() { Topic = "test/topic" },
                TestContext.Current.CancellationToken);

            try
            {
                await firstPublishAttemptStarted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                // Keep this on the same endpoint: the reconnect and presence flow must finish while the original publish is still pending.
                await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new Exception("mock exception"))
                    .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                await reconnectPresenceCompleted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                releaseFirstPublishAttempt.TrySetResult();
                await pendingPublish.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                Assert.Equal(1, presenceCompletionCount);
                Assert.Equal(2, publishAttemptCount);
            }
            finally
            {
                releaseFirstPublishAttempt.TrySetResult();
            }
        }

        [Theory]
        [InlineData(true)]
        [InlineData(false)]
        public async Task ConnectionClientReannouncesBirthBeforeContinuingSubscribe(bool isSessionResumed)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnect = connect =>
            {
                return Task.FromResult(new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success });
            };

            string expectedTopicString = Guid.NewGuid().ToString();

            mockMqttClient.OnSubscribe = async (subscribe) =>
            {
                if (retryCount == 0 && subscribe.TopicFilters.FirstOrDefault()!.Topic.Equals(expectedTopicString))
                {
                    _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new Exception("mock exception"));
                    retryCount++;
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                return MqttObjectHelpers.CreateSuccessfulSuback(subscribe);
            };

            MqttSubscribe featureClientLevelSubscribeRequest = new(expectedTopicString, MqttQualityOfServiceLevel.AtLeastOnce);
            await connectionClient.SubscribeAsync(featureClientLevelSubscribeRequest, TestContext.Current.CancellationToken);

            if (isSessionResumed)
            {
                // With session resumed, the reconnect flow doesn't need to send the subscribe on the direct methods/telemetry/twin topics
                Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count);

            }
            else
            {
                Assert.Equal(3, mockMqttClient.SentMqttTrafficInOrder.Count);
            }

            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();

            // Verify that the subscribe sent directly from this test is the last piece of traffic "sent" in this test. It must be preceded by the intial device presence flow traffic and by the reconnection device presence flow traffic
            Assert.NotNull(lastTraffic.Subscribe);
            Assert.Single(lastTraffic.Subscribe.TopicFilters);
            Assert.Equal(expectedTopicString, lastTraffic.Subscribe.TopicFilters.FirstOrDefault()!.Topic);
        }

        [Theory]
        [InlineData(true)]
        [InlineData(false)]
        public async Task ConnectionClientReannouncesBirthBeforeContinuingUnsubscribe(bool isSessionResumed)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient
            });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(false), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnect = connect =>
            {
                return Task.FromResult(new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success });
            };

            string expectedTopicString = Guid.NewGuid().ToString();

            mockMqttClient.OnUnsubscribe = async (unsubscribe) =>
            {
                if (retryCount == 0 && unsubscribe.TopicFilters.FirstOrDefault()!.Equals(expectedTopicString))
                {
                    _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new Exception("mock exception"));
                    retryCount++;
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                return MqttObjectHelpers.CreateSuccessfulUnsuback(unsubscribe);
            };

            MqttUnsubscribe featureClientLevelUnsubscribeRequest = new(expectedTopicString);
            await connectionClient.UnsubscribeAsync(featureClientLevelUnsubscribeRequest, TestContext.Current.CancellationToken);

            if (isSessionResumed)
            {
                // With session resumed, the reconnect flow doesn't need to send the subscribe on the direct methods/telemetry/twin topics
                Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count);

            }
            else
            {
                Assert.Equal(3, mockMqttClient.SentMqttTrafficInOrder.Count);
            }

            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();

            // Verify that the subscribe sent directly from this test is the last piece of traffic "sent" in this test. It must be preceded by the intial device presence flow traffic and by the reconnection device presence flow traffic
            Assert.NotNull(lastTraffic.Unsubscribe);
            Assert.Single(lastTraffic.Unsubscribe.TopicFilters);
            Assert.Equal(expectedTopicString, lastTraffic.Unsubscribe.TopicFilters.FirstOrDefault()!);
        }
    }
}
