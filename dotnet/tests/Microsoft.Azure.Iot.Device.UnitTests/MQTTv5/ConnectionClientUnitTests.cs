// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.Telemetry;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Retry;
using Xunit;

namespace Microsoft.Azure.Iot.Device.UnitTests.MQTTv5
{
    public class ConnectionClientUnitTests
    {
        private static ConnectionContext GetMockConnectionContext()
        {
#pragma warning disable SYSLIB0026 // Type or member is obsolete (Mock certificate, don't need to load a real one using typical X509 certificate loader
            return new ConnectionContext()
            {
                AuthenticationProvider = new X509AuthenticationProvider(new System.Security.Cryptography.X509Certificates.X509Certificate2()),
                DeviceId = "someDeviceId",
                IotHubHostName = "someHostName",
                ConnectionProfile = IotHubConnectionType.Mqttv5
            };
#pragma warning restore SYSLIB0026 // Type or member is obsolete
        }

        // The password comes from the authentication provider, so this client's connect customization must not overwrite it.
        [Fact]
        public async Task ConnectSendsAuthenticationProviderPassword()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            byte[]? sentPassword = null;
            mockMqttClient.OnConnect = connect =>
            {
                sentPassword = connect.Password;
                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            await connectionClient.ConnectToHubAsync(
                new ConnectionContext()
                {
                    AuthenticationProvider = new SymmetricKeyAuthenticationProvider("someDeviceId", "0ne00000000", new byte[] { 1, 2, 3 }),
                    DeviceId = "someDeviceId",
                    IotHubHostName = "someHostName",
                    ConnectionProfile = IotHubConnectionType.Mqttv5,
                },
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.NotNull(sentPassword);
            Assert.StartsWith("SharedAccessSignature ", System.Text.Encoding.UTF8.GetString(sentPassword));
        }

        // Matches the C SDK: the MQTTv5 hub serves MQTT over WebSockets at "/mqtt" on port 443.
        [Fact]
        public async Task ConnectUsesMqttV5HubWebSocketUri()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            string? sentWebSocketUri = null;
            mockMqttClient.OnConnect = connect =>
            {
                sentWebSocketUri = connect.WebsocketUri;
                return Task.FromResult(new MqttConnectAck() { ResultCode = MqttConnectReasonCode.Success });
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken);

            Assert.Equal("wss://someHostName:443/mqtt", sentWebSocketUri);
        }

        // A publish attempt that fails only after the reconnect and its birth flow have already completed must still be
        // retried, rather than waiting for a connection-restored wakeup that already happened.
        [Fact]
        public async Task PublishRetriesWhenPresenceCompletesBeforeFailedAttemptReturns()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = new(new()
            {
                MqttClient = mockMqttClient,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            });
            TelemetryClient telemetryClient = new(connectionClient);

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken);

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

            int telemetryAttemptCount = 0;
            mockMqttClient.OnPublish = async publish =>
            {
                if (publish.Topic.EndsWith("/srv/telemetry", StringComparison.Ordinal)
                    && Interlocked.Increment(ref telemetryAttemptCount) == 1)
                {
                    firstPublishAttemptStarted.TrySetResult();
                    await releaseFirstPublishAttempt.Task;
                    throw new MqttClientNotConnectedException("mock client not connected exception");
                }

                return new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success };
            };

            Task pendingTelemetry = telemetryClient.SendTelemetryAsync(new Models.Telemetry.DeviceToCloudTelemetry(), TestContext.Current.CancellationToken);

            try
            {
                await firstPublishAttemptStarted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                // The reconnect and its birth flow must finish while the original publish attempt is still pending.
                await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new Exception("mock exception"))
                    .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
                await reconnectPresenceCompleted.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                releaseFirstPublishAttempt.TrySetResult();
                await pendingTelemetry.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

                Assert.Equal(1, presenceCompletionCount);
                Assert.Equal(2, telemetryAttemptCount);
            }
            finally
            {
                releaseFirstPublishAttempt.TrySetResult();
            }
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

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnect = async (connect) =>
            {
                return new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success };
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
                // With session resumed, the birth flow doesn't need to send the subscribe on the presence topic again
                Assert.Equal(4, mockMqttClient.SentMqttTrafficInOrder.Count);
            }
            else
            {
                Assert.Equal(5, mockMqttClient.SentMqttTrafficInOrder.Count);
            }

            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();

            // Verify that the telemetry sent by the telemetry client is the last piece of traffic "sent" in this test. It must be preceded by the intial device presence flow traffic and by the reconnection device presence flow traffic
            Assert.NotNull(lastTraffic.Publish);
            Assert.StartsWith("ih/", lastTraffic.Publish.Topic);
            Assert.EndsWith("/srv/telemetry", lastTraffic.Publish.Topic);
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

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnect = async (connect) =>
            {
                return new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success };
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
            await connectionClient.SubscribeAsync(featureClientLevelSubscribeRequest, throwOnErrorReasonCode: true, TestContext.Current.CancellationToken);

            if (isSessionResumed)
            {
                // With session resumed, the birth flow doesn't need to send the subscribe on the presence topic again
                Assert.Equal(4, mockMqttClient.SentMqttTrafficInOrder.Count);
            }
            else
            {
                Assert.Equal(5, mockMqttClient.SentMqttTrafficInOrder.Count);
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

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken);

            // Setup mock MQTT layer to lose connection when telemetry client sends a publish for the first time (subsequent retries will work normally)
            int retryCount = 0;
            mockMqttClient.OnConnect = async (connect) =>
            {
                return new MqttConnectAck() { IsSessionPresent = isSessionResumed, ResultCode = MqttConnectReasonCode.Success };
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
            await connectionClient.UnsubscribeAsync(featureClientLevelUnsubscribeRequest, throwOnErrorReasonCode: true, TestContext.Current.CancellationToken);

            if (isSessionResumed)
            {
                // With session resumed, the birth flow doesn't need to send the subscribe on the presence topic again
                Assert.Equal(4, mockMqttClient.SentMqttTrafficInOrder.Count);
            }
            else
            {
                Assert.Equal(5, mockMqttClient.SentMqttTrafficInOrder.Count);
            }

            var lastTraffic = mockMqttClient.SentMqttTrafficInOrder.Last();

            // Verify that the subscribe sent directly from this test is the last piece of traffic "sent" in this test. It must be preceded by the intial device presence flow traffic and by the reconnection device presence flow traffic
            Assert.NotNull(lastTraffic.Unsubscribe);
            Assert.Single(lastTraffic.Unsubscribe.TopicFilters);
            Assert.Equal(expectedTopicString, lastTraffic.Unsubscribe.TopicFilters.FirstOrDefault());
        }
    }
}
