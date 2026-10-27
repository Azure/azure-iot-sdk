// Copyright (c) Microsoft. All rights reserved. Licensed under the MIT license.
// See LICENSE file in the project root for full license information.

using Microsoft.Azure.Iot.Device.Authentication;
using Microsoft.Azure.Iot.Device.MQTTv5.Connection;
using Microsoft.Azure.Iot.Device.MQTTv5.Telemetry;
using Microsoft.Azure.Iot.Device.Models;
using Microsoft.Azure.Iot.Device.Mqtt;
using Microsoft.Azure.Iot.Device.Retry;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using Xunit;
using MqttV5ConnectionClientOptions = Microsoft.Azure.Iot.Device.MQTTv5.Models.ConnectionClientOptions;
using Microsoft.Azure.Iot.Device.Unified.Models;

namespace Microsoft.Azure.Iot.Device.UnitTests.MQTTv5
{
    public class ConnectionClientUnitTests
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
        [InlineData(false)]
        [InlineData(true)]
        public async Task PresenceAnnouncementAwaitsEveryCompletionHandlerEvenWhenOneThrows(bool useUnifiedClient)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            ConnectionClientOptions options = new() { MqttClient = mockMqttClient };
            AbstractConnectionClient connectionClient = useUnifiedClient
                ? new Device.Unified.Connection.ConnectionClient(options)
                : new ConnectionClient(new() { MqttClient = mockMqttClient });
            TaskCompletionSource firstHandlerCanFinish = new(TaskCreationOptions.RunContinuationsAsynchronously);

            try
            {
                await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken);
                InvalidOperationException failure = new("first handler failed");
                bool secondHandlerRan = false;
                connectionClient.DevicePresenceFlowCompletedAsync += async _ =>
                {
                    await firstHandlerCanFinish.Task;
                    throw failure;
                };
                connectionClient.DevicePresenceFlowCompletedAsync += _ =>
                {
                    secondHandlerRan = true;
                    return Task.CompletedTask;
                };

                Task announcement = connectionClient.HandleConnectedToHubAsync(new()
                {
                    ConnectAck = new() { ResultCode = MqttConnectReasonCode.Success },
                });
                Assert.False(announcement.IsCompleted);
                Assert.False(secondHandlerRan);
                firstHandlerCanFinish.SetResult();

                Assert.Same(failure, await Assert.ThrowsAsync<InvalidOperationException>(
                    () => announcement.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken)));
                Assert.True(secondHandlerRan);
            }
            finally
            {
                firstHandlerCanFinish.TrySetResult();
                connectionClient.Dispose();
            }
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
                    AuthenticationProvider = new SymmetricKeyAuthenticationProvider("someDeviceId", "0ne00000000", new byte[] { 1, 2, 3 }, null),
                    DeviceId = "someDeviceId",
                    IotHubHostName = "someHostName",
                    ConnectionProfile = IotHubConnectionType.Mqttv5,
                },
                cancellationToken: TestContext.Current.CancellationToken);

            Assert.NotNull(sentPassword);
            Assert.StartsWith("SharedAccessSignature ", System.Text.Encoding.UTF8.GetString(sentPassword));
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task RejectedPrimaryCertificateFallsBackToSecondaryAndKeepsItForReconnect(bool useUnifiedClient)
        {
            using RSA key = RSA.Create(2048);
            CertificateRequest request = new("CN=someDeviceId", key, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            using X509Certificate2 primaryCertificate = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
            using X509Certificate2 secondaryCertificate = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(1));
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            AbstractConnectionClient connectionClient = useUnifiedClient
                ? new Device.Unified.Connection.ConnectionClient(new() { MqttClient = mockMqttClient })
                : new ConnectionClient(new() { MqttClient = mockMqttClient });
            using IDisposable connectionClientLifetime = (IDisposable)connectionClient;

            List<(X509Certificate2? Certificate, MqttProtocolVersion ProtocolVersion, string? Username)> connectAttempts = new();
            mockMqttClient.OnConnect = connect =>
            {
                lock (connectAttempts)
                {
                    connectAttempts.Add((connect.ClientCertificate, connect.ProtocolVersion, connect.Username));
                }

                return Task.FromResult(new MqttConnectAck()
                {
                    ResultCode = ReferenceEquals(connect.ClientCertificate, secondaryCertificate)
                        ? MqttConnectReasonCode.Success
                        : MqttConnectReasonCode.NotAuthorized,
                });
            };

            await connectionClient.ConnectToHubAsync(new()
            {
                AuthenticationProvider = new X509AuthenticationProvider(primaryCertificate, secondaryCertificate),
                DeviceId = "someDeviceId",
                IotHubHostName = "someHostName",
                ConnectionProfile = IotHubConnectionType.Mqttv5,
            }, TestContext.Current.CancellationToken).WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(new[] { primaryCertificate, secondaryCertificate, secondaryCertificate },
                connectAttempts.Select(attempt => attempt.Certificate));
            Assert.All(connectAttempts, attempt => Assert.Equal(MqttProtocolVersion.V500, attempt.ProtocolVersion));
            Assert.All(connectAttempts, attempt => Assert.StartsWith("correlationId=", attempt.Username));
            Assert.Equal(3, connectAttempts.Select(attempt => attempt.Username).Distinct().Count());
            Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count(traffic =>
                traffic.Publish?.Topic == "ih/someDeviceId/srv/presence"));
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

        [Fact]
        public async Task BirthMessageUsesProtobufContentType()
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            using ConnectionClient connectionClient = new(new() { MqttClient = mockMqttClient });

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), cancellationToken: TestContext.Current.CancellationToken);

            MqttPublish birthMessage = Assert.Single(mockMqttClient.SentMqttTrafficInOrder,
                traffic => traffic.Publish?.Topic.EndsWith("/srv/presence", StringComparison.Ordinal) == true).Publish!;
            Assert.Equal("application/protobuf", birthMessage.ContentType);
        }

        [Theory]
        [InlineData(false, false, 3600u)]
        [InlineData(true, false, 0u)]
        [InlineData(false, true, 3600u)]
        [InlineData(true, true, 0u)]
        [InlineData(false, false, 0u)]
        [InlineData(false, false, uint.MaxValue)]
        public async Task ReconnectUsesSnapshottedSessionSettings(bool useUnifiedClient, bool useCleanSession, uint sessionExpiryInterval)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                UseCleanSession = useCleanSession,
                SessionExpiryInterval = sessionExpiryInterval,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            ConnectionClientOptions unifiedOptions = new()
            {
                MqttClient = mockMqttClient,
                UseCleanSession = useCleanSession,
                ConnectionRetryPolicy = options.ConnectionRetryPolicy,
            };
            AbstractConnectionClient connectionClient = useUnifiedClient
                ? new Device.Unified.Connection.ConnectionClient(unifiedOptions)
                : new ConnectionClient(options);
            using IDisposable connectionClientLifetime = (IDisposable)connectionClient;

            options.UseCleanSession = !useCleanSession;
            options.SessionExpiryInterval = sessionExpiryInterval == 0 ? 3600u : 0u;
            unifiedOptions.UseCleanSession = !useCleanSession;

            List<(bool CleanStart, uint SessionExpiryInterval)> connectSettings = new();
            mockMqttClient.OnConnect = connect =>
            {
                lock (connectSettings)
                {
                    connectSettings.Add((connect.CleanStart, connect.SessionExpiryInterval));
                }

                return Task.FromResult(new MqttConnectAck()
                {
                    IsSessionPresent = !connect.CleanStart && connect.SessionExpiryInterval != 0,
                    ResultCode = MqttConnectReasonCode.Success,
                });
            };

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken);

            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(new[] { (true, sessionExpiryInterval), (useCleanSession, sessionExpiryInterval) }, connectSettings);
            Assert.Equal(useCleanSession || sessionExpiryInterval == 0 ? 2 : 1,
                mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Subscribe != null));
            Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Publish?.Topic.EndsWith("/srv/presence", StringComparison.Ordinal) == true));
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task FailedResumeAttemptStartsCleanSessionWithConfiguredExpiry(bool useUnifiedClient)
        {
            using MockConnectionMqttClient mockMqttClient = new() { AutoRespondToBirth = true };
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                SessionExpiryInterval = 3600,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            AbstractConnectionClient connectionClient = useUnifiedClient
                ? new Device.Unified.Connection.ConnectionClient(new()
                {
                    MqttClient = mockMqttClient,
                    ConnectionRetryPolicy = options.ConnectionRetryPolicy,
                })
                : new ConnectionClient(options);
            using IDisposable connectionClientLifetime = (IDisposable)connectionClient;

            await connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken);

            List<(bool CleanStart, uint SessionExpiryInterval)> connectSettings = new();
            mockMqttClient.OnConnect = connect =>
            {
                lock (connectSettings)
                {
                    connectSettings.Add((connect.CleanStart, connect.SessionExpiryInterval));
                    if (connectSettings.Count == 1)
                    {
                        throw new TimeoutException("mock CONNACK lost");
                    }
                }

                return Task.FromResult(new MqttConnectAck() { IsSessionPresent = !connect.CleanStart, ResultCode = MqttConnectReasonCode.Success });
            };

            TaskCompletionSource reconnected = new(TaskCreationOptions.RunContinuationsAsynchronously);
            connectionClient.DevicePresenceFlowCompletedAsync += _ =>
            {
                reconnected.TrySetResult();
                return Task.CompletedTask;
            };

            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await reconnected.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            uint expectedExpiry = useUnifiedClient ? 0u : 3600u;
            Assert.Equal(new[] { (false, expectedExpiry), (true, expectedExpiry) }, connectSettings);
            Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Subscribe != null));
        }

        [Theory]
        [InlineData(false)]
        [InlineData(true)]
        public async Task ConnectionLostBeforeBirthAckStartsCleanSession(bool useUnifiedClient)
        {
            using MockConnectionMqttClient mockMqttClient = new();
            MqttV5ConnectionClientOptions options = new()
            {
                MqttClient = mockMqttClient,
                SessionExpiryInterval = 3600,
                ConnectionRetryPolicy = new ExponentialBackoffRetryPolicy(uint.MaxValue, TimeSpan.Zero, useJitter: false),
            };
            AbstractConnectionClient connectionClient = useUnifiedClient
                ? new Device.Unified.Connection.ConnectionClient(new()
                {
                    MqttClient = mockMqttClient,
                    ConnectionRetryPolicy = options.ConnectionRetryPolicy,
                })
                : new ConnectionClient(options);
            using IDisposable connectionClientLifetime = (IDisposable)connectionClient;

            List<bool> cleanStartFlags = new();
            mockMqttClient.OnConnect = connect =>
            {
                lock (cleanStartFlags)
                {
                    cleanStartFlags.Add(connect.CleanStart);
                }

                return Task.FromResult(new MqttConnectAck() { IsSessionPresent = !connect.CleanStart, ResultCode = MqttConnectReasonCode.Success });
            };
            TaskCompletionSource birthPublished = new(TaskCreationOptions.RunContinuationsAsynchronously);
            mockMqttClient.OnPublish = _ =>
            {
                birthPublished.TrySetResult();
                return Task.FromResult(new MqttPublishAck() { ReasonCode = MqttPublishAckReasonCode.Success });
            };

            Task connecting = connectionClient.ConnectToHubAsync(GetMockConnectionContext(), TestContext.Current.CancellationToken);
            await birthPublished.Task.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            Assert.False(connecting.IsCompleted);

            mockMqttClient.AutoRespondToBirth = true;
            await mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ServerBusy)
                .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);
            await connecting.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

            Assert.Equal(new[] { true, true }, cleanStartFlags);
            Assert.Equal(2, mockMqttClient.SentMqttTrafficInOrder.Count(traffic => traffic.Subscribe != null));
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
                    retryCount++;
                    _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new IOException("mock connection reset"));
                    throw new IOException("mock connection reset");
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
                    retryCount++;
                    _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new IOException("mock connection reset"));
                    throw new IOException("mock connection reset");
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
                    retryCount++;
                    _ = mockMqttClient.SimulateServerDisconnectAsync(MqttDisconnectReason.ImplementationSpecificError, new IOException("mock connection reset"));
                    throw new IOException("mock connection reset");
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
